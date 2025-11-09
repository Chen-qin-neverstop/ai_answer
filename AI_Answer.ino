#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "esp_http_server.h"
#include "esp_err.h"
#include "esp_timer.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include <math.h>
#include <time.h>
#include <AudioFileSourceHTTPStream.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include "AudioHTTPSStream.h"
#include <SPIFFS.h>
#include <AudioFileSourceSPIFFS.h>
#include <vector>
#include <functional>
#include <algorithm>
#include <ctype.h>
#include <cstring>
#include <stdlib.h>

// ==================== I2S驱动兼容性处理 ====================
// ESP32-S3 Arduino 3.x 使用新版I2S驱动API
// 新API与ESP8266Audio库兼容,可以同时使用录音和播放功能
//
#define ENABLE_MICROPHONE 1  // 设为1启用麦克风,设为0禁用

#if ENABLE_MICROPHONE
  #include <driver/i2s_std.h>
  #include <driver/gpio.h>
#endif

// 简单的 HTTPS 诊断工具：打印响应码和前 200 字节（用于调试Edge TTS等服务）
void httpsDiagnostic(const String &url) {
  Serial.println(F("🔍 [TTS] 进行 HTTPS 连接诊断..."));
  // 从 URL 中提取主机名
  String host;
  int idx = url.indexOf("//");
  if (idx >= 0) {
    int start = idx + 2;
    int slash = url.indexOf('/', start);
    if (slash > 0) host = url.substring(start, slash);
    else host = url.substring(start);
  } else {
    Serial.println(F("✗ [Diag] 无法解析 URL 中的主机名"));
    return;
  }

  IPAddress ip;
  Serial.printf("↪ [Diag] 正在解析主机: %s\n", host.c_str());
  if (WiFi.hostByName(host.c_str(), ip)) {
    Serial.printf("↪ [Diag] DNS 解析成功: %s -> %s\n", host.c_str(), ip.toString().c_str());
  } else {
    Serial.printf("✗ [Diag] DNS 解析失败: %s\n", host.c_str());
  }

  // TCP 连接测试到 443
  uint16_t port = 443;
  Serial.printf("↪ [Diag] 尝试 TCP 连接到 %s:%d ...\n", host.c_str(), port);
  WiFiClient tcpClient;
  tcpClient.setTimeout(5);
  bool connected = tcpClient.connect(host.c_str(), port);
  if (connected) {
    Serial.println(F("✓ [Diag] TCP 连接成功 (端口 443 开放)"));
    tcpClient.stop();
  } else {
    Serial.println(F("✗ [Diag] TCP 连接失败（connection refused / timeout）"));
  }

  // 最后尝试 HTTPClient 请求以获取应用层信息
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(5000);
  http.setUserAgent("ESP32-Diagnostic/1.0");
  Serial.println(F("↪ [Diag] 使用 HTTPClient 发起请求以获取更多信息..."));
  if (!http.begin(client, url)) {
    Serial.println(F("✗ [Diag] HTTP begin 失败 (可能 TLS/底层无法建立连接)"));
    return;
  }
  int code = http.GET();
  Serial.printf("↪ [Diag] HTTP 响应码: %d\n", code);
  if (code > 0) {
    int len = http.getSize();
    Serial.printf("↪ [Diag] Content-Length: %d\n", len);
    String payload = http.getString();
    Serial.print(F("↪ [Diag] 响应前200字节: "));
    if (payload.length() > 200) payload = payload.substring(0, 200);
    Serial.println(payload);
  } else {
    Serial.printf("✗ [Diag] HTTP 请求失败: %s\n", http.errorToString(code).c_str());
  }
  http.end();
}

// ==================== 本地配置(密钥)加载 ====================
// 优先包含本地未提交的 config_local.h；无则回退到示例 config_example.h
#if __has_include("config_local.h")
#  include "config_local.h"
#else
#  include "config_example.h"
#  warning "Using config_example.h. Copy it to config_local.h and fill real secrets."
#endif

// WiFi配置: 在 config_local.h / config_example.h 中定义 ssid/password

// ==================== AI API 配置 ====================
// ✅ 更新说明：通义千问现已支持Base64图片！(使用OpenAI兼容模式)
// 支持三种API:
//   1. "openai" - OpenAI GPT-4 Vision (需要国际网络)
//   2. "qwen"   - 通义千问VL (国内可用,推荐!)
//   3. "custom" - 自定义OpenAI兼容API

// 选择使用的API类型 (根据网络环境选择)
const char* API_TYPE = "qwen";  // 通义千问 - 国内推荐!

// OpenAI GPT-4 Vision 配置
const char* OPENAI_ENDPOINT = "https://api.openai.com/v1/chat/completions";
const char* OPENAI_MODEL = "gpt-4-vision-preview";

// ✅ 通义千问 Vision 配置 (OpenAI兼容模式,支持Base64!)
const char* QWEN_ENDPOINT = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
const char* QWEN_MODEL = "qwen-vl-plus";  // 可根据需要调整型号

// 自定义API配置（如果使用其他兼容OpenAI格式的API）
const char* CUSTOM_ENDPOINT = "https://your-custom-endpoint/v1/chat/completions";
const char* CUSTOM_MODEL = "your-model-name";

// 提示词配置（更中性、明确的任务指令，要求较长输出）
const char* VISION_PROMPT = "请用中文以客观、中立的口吻详细描述这张图片的内容，包含：人物外观（性别/年龄段/穿着/表情/姿态）、场景（室内/室外/环境光）、颜色与材质、画面中显著物体及其位置、可能的动作与线索。请分要点说明，至少输出 300 字，不要做身份推断或生成与图像无关的内容。";

// ==================== TTS 提供商选择 ====================
// 可选: "google" 谷歌翻译TTS, "edge" 微软Edge TTS, "baidu" 百度TTS
const char* TTS_PROVIDER = "baidu";

// 百度TTS相关配置（建议放在config_local.h）
#ifndef BAIDU_TTS_PROXY_URL
#define BAIDU_TTS_PROXY_URL "http://192.168.1.100:3000/baidu_tts" // 示例: 你的本地/局域网代理地址
#endif
// 如果希望设备直接获取 token 并直连百度TTS，请在 config_local.h 中定义以下两项：
// #define BAIDU_API_KEY "你的百度语音合成 API Key"
// #define BAIDU_SECRET_KEY "你的百度语音合成 Secret Key"
#ifndef BAIDU_API_KEY
#define BAIDU_API_KEY ""
#endif
#ifndef BAIDU_SECRET_KEY
#define BAIDU_SECRET_KEY ""
#endif

// Baidu token 缓存
static String baidu_access_token = "";
static unsigned long baidu_token_expires_ms = 0;

static String aliyun_asr_token = "";
static long long aliyun_token_expire_unix = 0;

// 可选代理: 如果设备无法直接访问外部TTS（网络/防火墙问题），
// 可以在本地或VPS上运行一个简单的HTTP代理，将真实TTS请求由代理发出并返回音频。
// 例: "http://192.168.1.100:3000/tts_proxy" 或 "http://your-vps:3000/tts_proxy"
// 置为空字符串表示不使用代理。
const char* TTS_PROXY_URL = "";

#ifndef CUSTOM_WAKE_WORD
#define CUSTOM_WAKE_WORD "你好小智"
#endif

#ifndef WAKE_ACK_TEXT
#define WAKE_ACK_TEXT "唤醒成功，请说出指令。"
#endif

#ifndef WAKE_ACK_ENABLED
#define WAKE_ACK_ENABLED 1
#endif

static String wakeWord = String(CUSTOM_WAKE_WORD);
static String wakeResponse = String(WAKE_ACK_TEXT);
static bool voiceWakeBusy = false;

// ==================== 阿里云ASR配置 ====================
#ifndef ALIYUN_ASR_ACCESS_KEY_ID
#define ALIYUN_ASR_ACCESS_KEY_ID ""
#endif
#ifndef ALIYUN_ASR_ACCESS_KEY_SECRET
#define ALIYUN_ASR_ACCESS_KEY_SECRET ""
#endif
#ifndef ALIYUN_ASR_APP_KEY
#define ALIYUN_ASR_APP_KEY ""
#endif
#ifndef ALIYUN_ASR_REGION
#define ALIYUN_ASR_REGION "cn-shanghai"
#endif

// ==================== ASR服务选择 ====================
#define ASR_PROVIDER_BAIDU  0
#define ASR_PROVIDER_ALIYUN 1
#define ASR_PROVIDER        ASR_PROVIDER_ALIYUN  // 选择ASR服务提供商

// ==================== I2S 音频输出配置 ====================
#define I2S_BCLK_PIN    21
#define I2S_LRC_PIN     42
#define I2S_DOUT_PIN    41
#define I2S_NUM         I2S_NUM_0

#define AUDIO_SAMPLE_RATE     16000
#define AUDIO_BITS_PER_SAMPLE 16
#define AUDIO_CHANNELS        1

// ==================== I2S 麦克风输入配置 ====================
#define MIC_I2S_BCLK_PIN    47  // 根据硬件调整
#define MIC_I2S_LRC_PIN     45  // 根据硬件调整
#define MIC_I2S_DIN_PIN     48  // 根据硬件调整
#define MIC_I2S_NUM         I2S_NUM_1

#define MIC_SAMPLE_RATE      16000
#define MIC_BITS_PER_SAMPLE  16
#define MIC_CHANNELS         1
#ifndef VOICE_COMMAND_SECONDS
#define VOICE_COMMAND_SECONDS 5
#endif

#ifndef WAKE_LISTEN_SECONDS
#define WAKE_LISTEN_SECONDS 2
#endif

#ifndef WAKE_TIMEOUT_MS
#define WAKE_TIMEOUT_MS (60000UL)
#endif

#ifndef AUTO_WAKE_ENABLED
#define AUTO_WAKE_ENABLED 1
#endif

#ifndef AUTO_WAKE_RETRY_DELAY_MS
#define AUTO_WAKE_RETRY_DELAY_MS (2000UL)
#endif

constexpr size_t MIC_BYTES_PER_SAMPLE = MIC_BITS_PER_SAMPLE / 8;
constexpr size_t MIC_BYTES_PER_SECOND = static_cast<size_t>(MIC_SAMPLE_RATE) * MIC_BYTES_PER_SAMPLE * MIC_CHANNELS;
constexpr size_t VOICE_COMMAND_BUFFER_BYTES = MIC_BYTES_PER_SECOND * VOICE_COMMAND_SECONDS;
constexpr size_t WAKE_WORD_BUFFER_BYTES = MIC_BYTES_PER_SECOND * WAKE_LISTEN_SECONDS;

// 触发按钮配置
#define TRIGGER_BUTTON_PIN 0
#define VOICE_BUTTON_PIN 1  // 新增语音输入按钮
bool lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;
bool lastVoiceButtonState = HIGH;
unsigned long lastVoiceDebounceTime = 0;
unsigned long voiceButtonPressStart = 0;

// 果云ESP32-S3 CAM引脚定义
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM  4
#define SIOC_GPIO_NUM  5

#define Y9_GPIO_NUM    16
#define Y8_GPIO_NUM    17
#define Y7_GPIO_NUM    18
#define Y6_GPIO_NUM    12
#define Y5_GPIO_NUM    10
#define Y4_GPIO_NUM    8
#define Y3_GPIO_NUM    9
#define Y2_GPIO_NUM    11
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM  7
#define PCLK_GPIO_NUM  13

#define LED_GPIO_NUM   2

// ==================== 函数前置声明 ====================
// 这些函数在文件后面定义,但在前面的代码中会被调用,需要先声明
String encodeBase64(const uint8_t* data, size_t length);
bool fetchBaiduTokenIfNeeded();
bool playBeepTone(int frequency, int durationMs);
bool performVoiceAnalysis();
void performVisionAnalysis();
void outputToSerial(String aiResponse);
bool downloadMP3ToSPIFFS(const String& url, const String& filepath);
bool playMP3FromSPIFFS(const String& filepath);

httpd_handle_t camera_httpd = NULL;

void setupCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 10;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("摄像头初始化失败，错误码: 0x%x\n", err);
    Serial.println("请检查摄像头连接及PSRAM配置");
    return;
  }

  Serial.println("摄像头初始化成功");

  sensor_t* s = esp_camera_sensor_get();
  if (s != nullptr) {
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
    s->set_whitebal(s, 1);
    s->set_gain_ctrl(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, 0);
    s->set_aec2(s, 0);
    s->set_agc_gain(s, 0);
    s->set_gainceiling(s, (gainceiling_t)0);
    s->set_bpc(s, 0);
    s->set_wpc(s, 1);
    s->set_raw_gma(s, 1);
    s->set_lenc(s, 1);
    s->set_hmirror(s, 0);
    s->set_vflip(s, 0);
    s->set_dcw(s, 1);
    s->set_colorbar(s, 0);
  }
}

// ==================== 麦克风初始化(新I2S API) ====================
// 使用ESP32-S3新版I2S标准模式驱动,与ESP8266Audio库兼容

#if ENABLE_MICROPHONE

// 全局I2S句柄
static i2s_chan_handle_t mic_rx_handle = NULL;

bool initMicrophoneI2S() {
  if (mic_rx_handle != NULL) {
    Serial.println("ℹ️ 麦克风I2S已初始化,跳过重复安装");
    return true;
  }

  // 1. 创建I2S通道配置
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = false;
  chan_cfg.dma_desc_num = 4;
  chan_cfg.dma_frame_num = 1024;
  
  esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &mic_rx_handle);
  if (err != ESP_OK) {
    Serial.printf("❌ 创建I2S RX通道失败: %d\n", err);
    return false;
  }

  // 2. 配置I2S标准模式
  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)MIC_I2S_BCLK_PIN,
      .ws   = (gpio_num_t)MIC_I2S_LRC_PIN,
      .dout = I2S_GPIO_UNUSED,
      .din  = (gpio_num_t)MIC_I2S_DIN_PIN,
      .invert_flags = {
        .mclk_inv = false,
        .bclk_inv = false,
        .ws_inv   = false,
      },
    },
  };

  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

  err = i2s_channel_init_std_mode(mic_rx_handle, &std_cfg);
  if (err != ESP_OK) {
    Serial.printf("❌ 初始化I2S标准模式失败: %d\n", err);
    i2s_del_channel(mic_rx_handle);
    mic_rx_handle = NULL;
    return false;
  }

  // 3. 启用I2S通道
  err = i2s_channel_enable(mic_rx_handle);
  if (err != ESP_OK) {
    Serial.printf("❌ 启用I2S通道失败: %d\n", err);
    i2s_del_channel(mic_rx_handle);
    mic_rx_handle = NULL;
    return false;
  }

  Serial.println("✓ 麦克风I2S驱动已初始化(新API)");
  return true;
}

void deinitMicrophoneI2S() {
  if (mic_rx_handle == NULL) {
    return;
  }

  i2s_channel_disable(mic_rx_handle);
  i2s_del_channel(mic_rx_handle);
  mic_rx_handle = NULL;
  Serial.println("✓ 麦克风I2S驱动已卸载");
}

#else

// 麦克风功能已禁用
static i2s_chan_handle_t mic_rx_handle = NULL;

bool initMicrophoneI2S() {
  Serial.println("⚠️ 麦克风功能已禁用(ENABLE_MICROPHONE=0)");
  return false;
}

void deinitMicrophoneI2S() {
  // 空函数
}

#endif

// 空函数保持向后兼容
void setupMicrophone() {
#if ENABLE_MICROPHONE
  Serial.println("ℹ️ 麦克风采用按需初始化策略");
#else
  Serial.println("⚠️ 麦克风功能已禁用(ENABLE_MICROPHONE=0)");
  Serial.println("ℹ️ 如需启用,请修改代码中的 ENABLE_MICROPHONE 为 1");
#endif
}

// ==================== 录音函数 (新I2S API) ====================
static void logPcmStatistics(const int16_t* samples, size_t sampleCount) {
  if (!samples || sampleCount == 0) {
    Serial.println("🔍 [音频] 无PCM数据统计");
    return;
  }

  int16_t minSample = 32767;
  int16_t maxSample = -32768;
  uint64_t sumAbs = 0;
  size_t zeroCount = 0;

  for (size_t i = 0; i < sampleCount; ++i) {
    int16_t s = samples[i];
    if (s < minSample) {
      minSample = s;
    }
    if (s > maxSample) {
      maxSample = s;
    }
    if (s == 0) {
      ++zeroCount;
    }
    sumAbs += static_cast<uint16_t>(abs(s));
  }

  float avgAbs = sampleCount ? static_cast<float>(sumAbs) / sampleCount : 0.0f;
  float zeroRatio = sampleCount ? (static_cast<float>(zeroCount) * 100.0f / sampleCount) : 0.0f;

  Serial.printf("🔍 [音频] min=%d max=%d avg|x|=%.1f 零占比=%.1f%% 样本=%u\n",
                minSample,
                maxSample,
                avgAbs,
                zeroRatio,
                static_cast<unsigned>(sampleCount));

  Serial.print("🔍 [音频] 前20采样: ");
  size_t preview = std::min<size_t>(20, sampleCount);
  for (size_t i = 0; i < preview; ++i) {
    Serial.printf("%d ", samples[i]);
  }
  Serial.println();
}

size_t recordAudio(uint8_t* buffer, size_t bufferSize, int durationSeconds) {
#if !ENABLE_MICROPHONE
  Serial.println("❌ 录音功能已禁用(ENABLE_MICROPHONE=0)");
  Serial.println("ℹ️ 如需启用,请修改代码中的 ENABLE_MICROPHONE 为 1");
  return 0;
#else
  if (!buffer) {
    Serial.println("❌ 录音失败：缓冲区指针无效");
    return 0;
  }

  const size_t targetBytes = MIC_BYTES_PER_SECOND * durationSeconds;
  if (targetBytes == 0) {
    Serial.println("⚠️ 录音目标长度为0，直接返回");
    return 0;
  }

  if (targetBytes > bufferSize) {
    Serial.printf("❌ 缓冲区大小不足：需要 %u 字节，实际 %u 字节\n",
                  static_cast<unsigned>(targetBytes), static_cast<unsigned>(bufferSize));
    return 0;
  }

  // 录音前初始化I2S驱动
  if (!initMicrophoneI2S()) {
    Serial.println("❌ 麦克风I2S初始化失败");
    return 0;
  }

  Serial.printf("🎤 开始录音 %d 秒（目标 %u 字节）...\n", durationSeconds, static_cast<unsigned>(targetBytes));

  size_t bytesRead = 0;
  unsigned long startTime = millis();
  const unsigned long maxDurationMs = durationSeconds * 1300UL;

  while (bytesRead < targetBytes) {
    size_t chunkSize = std::min<size_t>(4096, targetBytes - bytesRead);
    size_t bytesCaptured = 0;
    
    // 使用新I2S API读取数据
    esp_err_t result = i2s_channel_read(mic_rx_handle,
                                        buffer + bytesRead,
                                        chunkSize,
                                        &bytesCaptured,
                                        1000);

    if (result != ESP_OK) {
      Serial.printf("❌ I2S读取失败: %d\n", result);
      break;
    }

    if (bytesCaptured == 0) {
      if (millis() - startTime > maxDurationMs) {
        Serial.println("⚠️ 录音超时，未捕获到新的音频数据");
        break;
      }
      continue;
    }

    bytesRead += bytesCaptured;
  }

  Serial.printf("✓ 录音完成，实际捕获 %u / %u 字节\n",
                static_cast<unsigned>(bytesRead), static_cast<unsigned>(targetBytes));

  // 录音后卸载I2S驱动,释放资源
  deinitMicrophoneI2S();
  
  return bytesRead;
#endif
}

// ==================== 语音识别工具函数 ====================
static String percentEncode(const String& value) {
  const char* hex = "0123456789ABCDEF";
  String result;
  result.reserve(value.length() * 3);
  for (size_t i = 0; i < value.length(); ++i) {
    uint8_t c = static_cast<uint8_t>(value[i]);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      result += static_cast<char>(c);
    } else {
      result += '%';
      result += hex[(c >> 4) & 0x0F];
      result += hex[c & 0x0F];
    }
  }
  return result;
}

static String hmacSha1Base64(const String& key, const String& data) {
  unsigned char hmacResult[20];
  unsigned char base64Result[64];
  size_t base64Len = 0;

  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
  if (info == nullptr) {
    return "";
  }

  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  if (mbedtls_md_setup(&ctx, info, 1) != 0) {
    mbedtls_md_free(&ctx);
    return "";
  }

  mbedtls_md_hmac_starts(&ctx,
                          reinterpret_cast<const unsigned char*>(key.c_str()),
                          key.length());
  mbedtls_md_hmac_update(&ctx,
                          reinterpret_cast<const unsigned char*>(data.c_str()),
                          data.length());
  mbedtls_md_hmac_finish(&ctx, hmacResult);
  mbedtls_md_free(&ctx);

  if (mbedtls_base64_encode(base64Result, sizeof(base64Result), &base64Len, hmacResult,
                            sizeof(hmacResult)) != 0) {
    return "";
  }
  base64Result[base64Len] = '\0';
  return String(reinterpret_cast<char*>(base64Result));
}

static String getGmtTimestamp(time_t t) {
  struct tm timeinfo;
  gmtime_r(&t, &timeinfo);
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buffer);
}

static String createAliyunSignatureNonce() {
  uint32_t part1 = static_cast<uint32_t>(random(0x7FFFFFFF));
  uint32_t part2 = static_cast<uint32_t>(millis());
  return String(part1) + String(part2);
}

static bool equalsIgnoreCase(const char* a, const char* b) {
  if (!a || !b) {
    return false;
  }
  while (*a && *b) {
    if (tolower(static_cast<unsigned char>(*a)) != tolower(static_cast<unsigned char>(*b))) {
      return false;
    }
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

static bool isAliyunMetaKey(const char* key) {
  if (!key) {
    return false;
  }

  static const char* metaKeys[] = {
      "header",      "headers",    "namespace",   "name",      "status",
      "code",        "code_desc",  "message_id",  "request_id", "task_id",
      "trace_id",    "event_id",   "app_key",     "token",      "token_id",
      "biz_id",      "log",        "session_id",  "result_code", "message"};

  for (const char* meta : metaKeys) {
    if (equalsIgnoreCase(key, meta)) {
      return true;
    }
  }
  return false;
}

static String extractAliyunAsrText(JsonVariantConst node);

static String extractAliyunFromArray(JsonArrayConst arr) {
  for (JsonVariantConst child : arr) {
    String text = extractAliyunAsrText(child);
    if (text.length() > 0) {
      return text;
    }
  }
  return "";
}

// 尝试在阿里云ASR的多层响应结构中提取识别文本
static String extractAliyunAsrText(JsonVariantConst node) {
  if (node.isNull()) {
    return "";
  }

  if (node.is<const char*>()) {
    const char* raw = node.as<const char*>();
    if (!raw || raw[0] == '\0') {
      return "";
    }

    String text = raw;
    text.trim();
    if (text.length() == 0) {
      return "";
    }

    // 忽略明显的标识/哈希值
    bool looksHash = true;
    if (text.length() == 32) {
      for (size_t i = 0; i < text.length(); ++i) {
        char c = text.charAt(i);
        if (!isxdigit(static_cast<unsigned char>(c))) {
          looksHash = false;
          break;
        }
      }
    } else if (text.length() == 36) {
      // 形如 UUID: 8-4-4-4-12
      const int hyphenPos[] = {8, 13, 18, 23};
      looksHash = true;
      for (int i = 0; i < text.length(); ++i) {
        char c = text.charAt(i);
        if ((i == hyphenPos[0] || i == hyphenPos[1] || i == hyphenPos[2] || i == hyphenPos[3])) {
          if (c != '-') {
            looksHash = false;
            break;
          }
        } else if (!isxdigit(static_cast<unsigned char>(c))) {
          looksHash = false;
          break;
        }
      }
    } else {
      looksHash = false;
    }

    if (looksHash) {
      return "";
    }

    return text;
  }

  if (node.is<JsonArrayConst>()) {
    return extractAliyunFromArray(node.as<JsonArrayConst>());
  }

  if (node.is<JsonObjectConst>()) {
    JsonObjectConst obj = node.as<JsonObjectConst>();

    // 常见字段优先检查
    const char* candidateKeys[] = {
        "payload",           "result",         "Result",       "text",
        "Text",              "transcription",  "Transcript",   "transcript",
        "detokenized_result", "nbest",         "NBest",        "details",
        "Details",           "sentences",      "Sentences",    "sentence",
        "Sentence",          "display_text",   "DisplayText",  "alternatives",
        "Alternatives",      "final_result",   "FinalResult",  "best_transcription",
        "best_result",       "Utterance"};

    for (const char* key : candidateKeys) {
      if (obj.containsKey(key)) {
        String text = extractAliyunAsrText(obj[key]);
        if (text.length() > 0) {
          return text;
        }
      }
    }

    // 兜底：遍历所有字段
    for (JsonPairConst kv : obj) {
      const char* key = kv.key().c_str();
      if (isAliyunMetaKey(key)) {
        continue;
      }
      String text = extractAliyunAsrText(kv.value());
      if (text.length() > 0) {
        return text;
      }
    }
  }

  return "";
}

static bool fetchAliyunTokenIfNeeded() {
  time_t now = time(nullptr);
  if (!aliyun_asr_token.isEmpty() && aliyun_token_expire_unix > 0) {
    if (aliyun_token_expire_unix - now > 60) {
      return true;
    }
  }

  if (String(ALIYUN_ASR_ACCESS_KEY_ID).length() == 0 ||
    String(ALIYUN_ASR_ACCESS_KEY_SECRET).length() == 0 ||
    String(ALIYUN_ASR_APP_KEY).length() == 0) {
    Serial.println("❌ 阿里云ASR密钥未配置，请检查 config_local.h");
    return false;
  }

  if (now < 1000) {
    Serial.println("❌ 系统时间尚未同步，无法生成阿里云签名");
    return false;
  }

  String timestamp = getGmtTimestamp(now);
  String nonce = createAliyunSignatureNonce();

  std::vector<std::pair<String, String>> params = {
      {"AccessKeyId", String(ALIYUN_ASR_ACCESS_KEY_ID)},
      {"Action", "CreateToken"},
      {"Format", "JSON"},
      {"RegionId", String(ALIYUN_ASR_REGION)},
      {"SignatureMethod", "HMAC-SHA1"},
      {"SignatureNonce", nonce},
      {"SignatureVersion", "1.0"},
      {"Timestamp", timestamp},
      {"Version", "2019-02-28"}};

  std::sort(params.begin(), params.end(),
            [](const std::pair<String, String>& a, const std::pair<String, String>& b) {
              return a.first < b.first;
            });

  String canonicalQuery;
  for (size_t i = 0; i < params.size(); ++i) {
    canonicalQuery += percentEncode(params[i].first);
    canonicalQuery += "=";
    canonicalQuery += percentEncode(params[i].second);
    if (i + 1 < params.size()) {
      canonicalQuery += "&";
    }
  }

  String stringToSign = "GET&%2F&" + percentEncode(canonicalQuery);
  String signature = hmacSha1Base64(String(ALIYUN_ASR_ACCESS_KEY_SECRET) + "&", stringToSign);
  if (signature.length() == 0) {
    Serial.println("❌ 生成阿里云ASR签名失败");
    return false;
  }

  String requestUrl = "https://nls-meta." + String(ALIYUN_ASR_REGION) +
                      ".aliyuncs.com/?" + canonicalQuery + "&Signature=" + percentEncode(signature);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);

  if (!http.begin(client, requestUrl)) {
    Serial.println("❌ 初始化阿里云ASR token请求失败");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.printf("❌ 阿里云ASR token请求失败: %d (%s)\n", httpCode, http.errorToString(httpCode).c_str());
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.printf("❌ 解析阿里云ASR token响应失败: %s\n", error.c_str());
    return false;
  }

  if (!doc.containsKey("Token")) {
    Serial.printf("❌ 阿里云ASR token响应异常: %s\n", payload.c_str());
    return false;
  }

  aliyun_asr_token = doc["Token"]["Id"].as<String>();
  long long expireUnix = doc["Token"]["ExpireTime"] | 0LL;
  if (expireUnix == 0) {
    expireUnix = now + 3600;  // 默认缓存1小时
  }
  aliyun_token_expire_unix = expireUnix;
  Serial.println("✓ 阿里云ASR token获取成功");
  return true;
}

static String recognizeSpeechWithAliyun(const uint8_t* audioData, size_t audioSize) {
  if (!fetchAliyunTokenIfNeeded()) {
    return "";
  }

  if (aliyun_asr_token.isEmpty()) {
    Serial.println("❌ 阿里云ASR token为空");
    return "";
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(30000);

  String url = "https://nls-gateway-" + String(ALIYUN_ASR_REGION) +
               ".aliyuncs.com/stream/v1/asr?appkey=" + String(ALIYUN_ASR_APP_KEY) +
               "&format=pcm&sample_rate=" + String(MIC_SAMPLE_RATE) +
               "&enable_punctuation_prediction=true&enable_inverse_text_normalization=true";

  if (!http.begin(client, url)) {
    Serial.println("❌ 初始化阿里云ASR请求失败");
    return "";
  }

  http.addHeader("Content-Type", "application/octet-stream");
  http.addHeader("X-NLS-Token", aliyun_asr_token);

  const int16_t* pcmSamples = reinterpret_cast<const int16_t*>(audioData);
  size_t pcmSampleCount = audioSize / sizeof(int16_t);
  logPcmStatistics(pcmSamples, pcmSampleCount);

  int httpCode = http.POST(const_cast<uint8_t*>(audioData), audioSize);
  String response = "";

  if (httpCode > 0) {
    response = http.getString();
    Serial.printf("阿里云ASR响应码: %d\n", httpCode);

    if (httpCode == 200) {
      DynamicJsonDocument respDoc(4096);
      DeserializationError error = deserializeJson(respDoc, response);
      if (!error) {
        String text = extractAliyunAsrText(respDoc.as<JsonVariantConst>());

        if (text.length() > 0) {
          text.trim();
          Serial.printf("✓ 阿里云ASR识别结果: %s\n", text.c_str());
          http.end();
          return text;
        }

        if (respDoc.containsKey("error_code") || respDoc.containsKey("error_message")) {
          Serial.printf("❌ 阿里云ASR错误: %s\n", response.c_str());
        } else {
          Serial.println("ℹ️ 阿里云ASR返回空结果");
          Serial.printf("↪ 原始响应: %s\n", response.c_str());
        }
      } else {
        Serial.printf("❌ 阿里云ASR JSON解析失败: %s\n", error.c_str());
      }
    } else {
      Serial.printf("❌ 阿里云ASR HTTP错误: %s\n", response.c_str());
    }
  } else {
    Serial.printf("❌ 阿里云ASR请求失败: %d (%s)\n", httpCode, http.errorToString(httpCode).c_str());
  }

  http.end();
  return "";
}

static String recognizeSpeechWithBaidu(const uint8_t* audioData, size_t audioSize) {
  if (!fetchBaiduTokenIfNeeded()) {
    Serial.println("❌ 无法获取百度token");
    return "";
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(30000);

  const String url = "https://vop.baidu.com/server_api";
  if (!http.begin(client, url)) {
    Serial.println("❌ HTTP begin失败");
    return "";
  }

  http.addHeader("Content-Type", "application/json");

  String base64Audio = encodeBase64(audioData, audioSize);
  if (base64Audio.length() == 0) {
    Serial.println("❌ Base64编码音频失败");
    http.end();
    return "";
  }

  String requestBody;
  requestBody.reserve(base64Audio.length() + 256);
  requestBody += '{';
  requestBody += "\"format\":\"pcm\",";
  requestBody += "\"rate\":";
  requestBody += String(MIC_SAMPLE_RATE);
  requestBody += ',';
  requestBody += "\"channel\":";
  requestBody += String(MIC_CHANNELS);
  requestBody += ',';
  requestBody += "\"cuid\":\"ESP32CAM001\",";
  requestBody += "\"token\":\"";
  requestBody += baidu_access_token;
  requestBody += "\",";
  requestBody += "\"speech\":\"";
  requestBody += base64Audio;
  requestBody += "\",";
  requestBody += "\"len\":";
  requestBody += String(audioSize);
  requestBody += '}';

  Serial.printf("发送百度语音识别请求，大小: %u bytes\n", static_cast<unsigned>(requestBody.length()));

  int httpCode = http.POST(requestBody);
  String response = "";

  if (httpCode > 0) {
    response = http.getString();
    Serial.printf("百度ASR响应码: %d\n", httpCode);

    DynamicJsonDocument respDoc(4096);
    DeserializationError error = deserializeJson(respDoc, response);
    if (!error) {
      if (respDoc.containsKey("result") && respDoc["result"].size() > 0) {
        response = respDoc["result"][0].as<String>();
        Serial.printf("✓ 百度ASR识别结果: %s\n", response.c_str());
      } else if (respDoc.containsKey("err_msg")) {
        Serial.printf("❌ 百度ASR错误: %s\n", respDoc["err_msg"].as<const char*>());
        response = "";
      }
    } else {
      Serial.printf("❌ 百度ASR JSON解析失败: %s\n", error.c_str());
      response = "";
    }
  } else {
    Serial.printf("❌ 百度ASR请求失败: %d (%s)\n", httpCode, http.errorToString(httpCode).c_str());
  }

  http.end();
  base64Audio = "";
  requestBody = "";
  return response;
}

// ==================== 语音识别函数 ====================
String recognizeSpeech(const uint8_t* audioData, size_t audioSize) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi未连接，无法进行语音识别");
    return "";
  }

  if (audioSize == 0) {
    Serial.println("⚠️ 音频长度为0，跳过语音识别");
    return "";
  }

  Serial.printf("🎙️ 开始语音识别，音频长度: %u 字节\n", static_cast<unsigned>(audioSize));

  if (ASR_PROVIDER == ASR_PROVIDER_ALIYUN) {
    return recognizeSpeechWithAliyun(audioData, audioSize);
  }

  Serial.println("❌ 当前ASR提供商未实现，请检查配置");
  return "";
}

// ==================== 生成提示词 ====================
String generatePromptFromSpeech(String speechText) {
  Serial.printf("💡 生成提示词基于语音: %s\n", speechText.c_str());
  
  // 简单处理：将语音识别结果作为视觉AI的提示词
  // 可以根据需要扩展为更复杂的逻辑
  if (speechText.length() == 0) {
    return VISION_PROMPT; // 默认提示词
  }
  
  // 直接使用用户的语音命令作为提示词,让AI根据用户的命令分析图片
  String customPrompt = "用户命令：" + speechText + "\n\n请根据用户的命令结合当前图片，针对该命令做出反应";
  return customPrompt;
}

// ==================== 语音唤醒监听 ====================
bool listenForWakeWord(const String& targetWord, unsigned long timeoutMs) {
#if !ENABLE_MICROPHONE
  Serial.println("❌ [唤醒] 语音唤醒功能已禁用(ENABLE_MICROPHONE=0)");
  return false;
#else
  if (targetWord.isEmpty()) {
    Serial.println("❌ [唤醒] 唤醒词为空，跳过监听");
    return false;
  }

  const size_t wakeBufferSize = WAKE_WORD_BUFFER_BYTES;
  uint8_t* audioBuffer = (uint8_t*)malloc(wakeBufferSize);
  if (!audioBuffer) {
    Serial.println("❌ [唤醒] 分配监听缓冲区失败");
    return false;
  }

  Serial.printf("👂 [唤醒] 进入语音唤醒模式，唤醒词: %s\n", targetWord.c_str());
  Serial.printf("👂 [唤醒] 每次监听 %d 秒，超时时间: %lu ms\n", WAKE_LISTEN_SECONDS, timeoutMs);

  unsigned long start = millis();
  int attempt = 1;
  bool detected = false;

  String normalizedWake = targetWord;
  normalizedWake.toLowerCase();

  while (true) {
    if (timeoutMs > 0 && (millis() - start) > timeoutMs) {
      Serial.println("⌛ [唤醒] 超时，未检测到唤醒词");
      break;
    }

    Serial.printf("🎧 [唤醒] 第 %d 次监听...\n", attempt);
    size_t recordedBytes = recordAudio(audioBuffer, wakeBufferSize, WAKE_LISTEN_SECONDS);
    if (recordedBytes == 0) {
      Serial.println("⚠️ [唤醒] 本次录音失败或无数据");
      break;  // 录音失败直接退出,不再继续
    }

    String speechText = recognizeSpeech(audioBuffer, recordedBytes);
    if (speechText.isEmpty()) {
      Serial.println("ℹ️ [唤醒] 未识别到有效语音，继续监听");
      attempt++;
      continue;
    }

    Serial.printf("🗣️ [唤醒] 识别内容: %s\n", speechText.c_str());

    String normalizedSpeech = speechText;
    normalizedSpeech.toLowerCase();
    if (normalizedSpeech.indexOf(normalizedWake) != -1) {
      Serial.println("✅ [唤醒] 检测到唤醒词！");
      detected = true;
      break;
    }

    attempt++;
  }

  free(audioBuffer);
  return detected;
#endif
}

bool performVoiceWakeFlow() {
#if !ENABLE_MICROPHONE
  Serial.println("❌ 语音唤醒功能已禁用(ENABLE_MICROPHONE=0)");
  Serial.println("ℹ️ 如需启用,请修改代码中的 ENABLE_MICROPHONE 为 1");
  return false;
#else
  if (voiceWakeBusy) {
    Serial.println("ℹ️ [唤醒] 监听已在进行，跳过重复请求");
    return false;
  }
  voiceWakeBusy = true;

  Serial.println("\n\n****************************************");
  Serial.println("*     进入语音唤醒流程                 *");
  Serial.println("****************************************\n");

  bool wakeDetected = listenForWakeWord(wakeWord, WAKE_TIMEOUT_MS);
  if (!wakeDetected) {
    Serial.println("❌ [唤醒] 未检测到唤醒词，退出语音唤醒流程");
    voiceWakeBusy = false;
    return false;
  }

  if (WAKE_ACK_ENABLED && !wakeResponse.isEmpty()) {
    Serial.printf("🔊 [唤醒] 播放提示语: %s\n", wakeResponse.c_str());
    speakText(wakeResponse);
  } else {
    playBeepTone(880, 180);
    delay(200);
  }

  Serial.println("🎯 [唤醒] 唤醒成功，请说出指令...");
  delay(300);
  bool analysisOk = performVoiceAnalysis();
  if (!analysisOk) {
    Serial.println("❌ [唤醒] 语音唤醒后分析失败");
  }
  voiceWakeBusy = false;
  return analysisOk;
#endif
}

// ==================== 执行语音输入分析流程 ====================
bool performVoiceAnalysis() {
  Serial.println("\n\n****************************************");
  Serial.println("*     开始执行语音输入分析流程         *");
  Serial.println("****************************************\n");
  
  unsigned long startTime = millis();
  
  // 1. 录音
  Serial.printf("🎤 [步骤 1/4] 录音 %d 秒指令...\n", VOICE_COMMAND_SECONDS);
  const size_t voiceBufferSize = VOICE_COMMAND_BUFFER_BYTES;
  uint8_t* audioBuffer = (uint8_t*)malloc(voiceBufferSize);
  if (!audioBuffer) {
    Serial.println("❌ 内存分配失败");
    return false;
  }
  
  size_t recordedBytes = recordAudio(audioBuffer, voiceBufferSize, VOICE_COMMAND_SECONDS);
  if (recordedBytes == 0) {
    Serial.println("❌ 录音失败");
    free(audioBuffer);
    return false;
  }
  
  // 2. 语音识别
  Serial.println("\n🎙️ [步骤 2/4] 语音识别...");
  String speechText = recognizeSpeech(audioBuffer, recordedBytes);
  free(audioBuffer); // 释放录音缓冲区
  
  if (speechText.length() == 0) {
    Serial.println("❌ 语音识别失败");
    return false;
  }
  
  // 3. 生成提示词并拍照分析
  Serial.println("\n💡 [步骤 3/4] 生成提示词并拍照分析...");
  String customPrompt = generatePromptFromSpeech(speechText);
  
  // 临时修改VISION_PROMPT
  const char* originalPrompt = VISION_PROMPT;
  VISION_PROMPT = customPrompt.c_str();
  
  // 执行视觉分析
  performVisionAnalysis();
  
  // 恢复原始提示词
  VISION_PROMPT = originalPrompt;
  
  // 4. 输出语音识别结果
  Serial.println("\n📝 [步骤 4/4] 输出语音识别结果...");
  outputToSerial("语音识别结果: " + speechText);
  
  unsigned long totalTime = millis() - startTime;
  Serial.println("\n****************************************");
  Serial.printf("*  语音分析流程完成！总耗时: %lu ms (%.1f 秒) *\n", totalTime, totalTime / 1000.0);
  Serial.println("****************************************\n");
  return true;
}

static esp_err_t jpg_handler(httpd_req_t* req) {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("获取图片失败");
    const char* error_msg = "Camera capture failed";
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, error_msg, strlen(error_msg));
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  const size_t chunk_size = 4096;
  size_t offset = 0;
  while (offset < fb->len) {
    size_t to_send = fb->len - offset;
    if (to_send > chunk_size) {
      to_send = chunk_size;
    }
    esp_err_t res = httpd_resp_send_chunk(req, reinterpret_cast<const char*>(fb->buf + offset), to_send);
    if (res != ESP_OK) {
      esp_camera_fb_return(fb);
      return res;
    }
    offset += to_send;
    delay(1);
  }

  httpd_resp_send_chunk(req, NULL, 0);
  esp_camera_fb_return(fb);
  return ESP_OK;
}

static esp_err_t stream_handler(httpd_req_t* req) {
  esp_err_t res = httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
  if (res != ESP_OK) {
    return res;
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (true) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("获取视频帧失败");
      return ESP_FAIL;
    }

    res = httpd_resp_send_chunk(req, "--frame\r\n", 9);
    if (res == ESP_OK) {
      char header[128];
      int header_len = snprintf(header, sizeof(header),
                                "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                                fb->len);
      res = httpd_resp_send_chunk(req, header, header_len);
    }

    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, reinterpret_cast<const char*>(fb->buf), fb->len);
    }

    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, "\r\n", 2);
    }

    esp_camera_fb_return(fb);

    if (res != ESP_OK) {
      break;
    }
    delay(1);
  }

  return res;
}

static esp_err_t index_handler(httpd_req_t* req) {
  const char* html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32-S3 AI Vision</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Arial, sans-serif;
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      min-height: 100vh;
      padding: 20px;
    }
    .container {
      max-width: 1200px;
      margin: 0 auto;
    }
    h1 {
      color: white;
      text-align: center;
      margin-bottom: 30px;
      font-size: 2.5em;
      text-shadow: 2px 2px 4px rgba(0,0,0,0.3);
    }
    .grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 20px;
      margin-bottom: 20px;
    }
    @media (max-width: 768px) {
      .grid { grid-template-columns: 1fr; }
    }
    .card {
      background: white;
      border-radius: 12px;
      padding: 20px;
      box-shadow: 0 4px 20px rgba(0,0,0,0.2);
    }
    .card h2 {
      color: #333;
      margin-bottom: 15px;
      font-size: 1.5em;
      display: flex;
      align-items: center;
      gap: 10px;
    }
    #stream-container {
      position: relative;
      background: #f5f5f5;
      border-radius: 8px;
      overflow: hidden;
      min-height: 300px;
      display: flex;
      align-items: center;
      justify-content: center;
    }
    #stream, #ai-image {
      max-width: 100%;
      height: auto;
      display: block;
      border-radius: 4px;
    }
    #ai-image {
      max-height: 400px;
      object-fit: contain;
    }
    .placeholder {
      color: #999;
      font-size: 1.2em;
    }
    .controls {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
      justify-content: center;
      margin-top: 15px;
    }
    button {
      padding: 12px 24px;
      font-size: 16px;
      font-weight: 600;
      border: none;
      border-radius: 8px;
      cursor: pointer;
      transition: all 0.3s;
      box-shadow: 0 2px 8px rgba(0,0,0,0.15);
    }
    button:hover {
      transform: translateY(-2px);
      box-shadow: 0 4px 12px rgba(0,0,0,0.25);
    }
    button:active {
      transform: translateY(0);
    }
    .btn-primary { background: #4CAF50; color: white; }
    .btn-danger { background: #f44336; color: white; }
    .btn-secondary { background: #2196F3; color: white; }
    .btn-warning { background: #FF9800; color: white; }
    .btn-info { background: #00BCD4; color: white; }
    #status {
      text-align: center;
      padding: 15px;
      background: white;
      border-radius: 8px;
      box-shadow: 0 2px 8px rgba(0,0,0,0.1);
      font-size: 16px;
      color: #333;
    }
    #ai-result {
      background: linear-gradient(135deg, #f5f7fa 0%, #c3cfe2 100%);
      padding: 25px;
      border-radius: 12px;
      min-height: 120px;
      text-align: left;
      line-height: 1.8;
      color: #2c3e50;
      white-space: pre-wrap;
      word-wrap: break-word;
      font-size: 15px;
      box-shadow: inset 0 2px 4px rgba(0,0,0,0.1);
      font-family: 'Segoe UI', Arial, sans-serif;
    }
    #ai-result::first-line {
      font-weight: 600;
      font-size: 16px;
      color: #1a252f;
    }
    .loading {
      display: inline-block;
      width: 20px;
      height: 20px;
      border: 3px solid #f3f3f3;
      border-top: 3px solid #667eea;
      border-radius: 50%;
      animation: spin 1s linear infinite;
      margin-right: 10px;
      vertical-align: middle;
    }
    @keyframes spin {
      0% { transform: rotate(0deg); }
      100% { transform: rotate(360deg); }
    }
    .analyzing {
      background: #fff3cd;
      border-left: 4px solid #ffc107;
      padding: 15px;
      border-radius: 4px;
      margin-top: 10px;
    }
    .success {
      background: #d4edda;
      border-left: 4px solid #28a745;
    }
    .error {
      background: #f8d7da;
      border-left: 4px solid #dc3545;
    }
    .hidden { display: none !important; }
  </style>
</head>
<body>
  <div class="container">
    <h1>📷 ESP32-S3 AI 视觉系统</h1>

    <div class="grid">
      <div class="card">
        <h2>📹 实时画面</h2>
        <div id="stream-container">
          <div class="placeholder">点击"开始视频流"查看实时画面</div>
          <img id="stream" src="" class="hidden">
        </div>
        <div class="controls">
          <button class="btn-primary" onclick="startStream()">▶ 开始</button>
          <button class="btn-danger" onclick="stopStream()">⏸ 停止</button>
          <button class="btn-secondary" onclick="capture()">📸 拍照</button>
        </div>
      </div>

      <div class="card">
        <h2>🤖 AI 图像分析</h2>
        <div id="ai-image-container">
          <div class="placeholder">点击"AI分析"开始识别</div>
          <img id="ai-image" src="" class="hidden">
        </div>
        <div class="controls">
          <button class="btn-warning" onclick="aiAnalyze()">🤖 AI分析</button>
          <button class="btn-info" onclick="voiceAnalyze()">🎤 语音分析</button>
          <button class="btn-info" onclick="voiceWake()">🛎️ 语音唤醒</button>
          <button class="btn-info" onclick="testBeep()">🔊 测试扬声器</button>
          <button class="btn-info" onclick="location.reload()">🔄 刷新</button>
        </div>
      </div>
    </div>

    <div class="card">
      <h2>💬 分析结果</h2>
      <div id="ai-result">等待AI分析...</div>
    </div>

    <div id="status">系统就绪 - 点击按钮开始使用</div>
  </div>

  <script>
    const streamImg = document.getElementById('stream');
    const aiImage = document.getElementById('ai-image');
    const status = document.getElementById('status');
    const aiResult = document.getElementById('ai-result');
    let streamActive = false;

    function updateStatus(msg, type = '') {
      status.innerHTML = msg;
      status.className = type;
    }

    function startStream() {
      if (!streamActive) {
        document.querySelector('#stream-container .placeholder').classList.add('hidden');
        streamImg.classList.remove('hidden');
        streamImg.src = '/stream?' + Date.now();
        streamActive = true;
        updateStatus('✅ 视频流运行中...', 'success');
      }
    }

    function stopStream() {
      if (streamActive) {
        streamImg.src = '';
        streamImg.classList.add('hidden');
        document.querySelector('#stream-container .placeholder').classList.remove('hidden');
        streamActive = false;
        updateStatus('⏸ 视频流已停止');
      }
    }

    function capture() {
      updateStatus('📸 正在拍照...', 'analyzing');
      fetch('/capture')
        .then(response => response.blob())
        .then(blob => {
          const url = URL.createObjectURL(blob);
          const a = document.createElement('a');
          a.href = url;
          a.download = 'capture_' + Date.now() + '.jpg';
          a.click();
          updateStatus('✅ 拍照成功！照片已下载', 'success');
          setTimeout(() => updateStatus('系统就绪'), 3000);
        })
        .catch(err => {
          updateStatus('❌ 拍照失败: ' + err.message, 'error');
          console.error('拍照错误:', err);
        });
    }

    function beautifyAIText(text) {
      return text
        .replace(/\*\*(.+?)\*\*/g, '$1')
        .replace(/\*(.+?)\*/g, '$1')
        .replace(/_(.+?)_/g, '$1')
        .replace(/^[\-\*]\s+/gm, '• ')
        .replace(/^(\d+)\.\s+/gm, '$1. ')
        .replace(/```[\s\S]*?```/g, (match) => match.replace(/```/g, ''))
        .replace(/`(.+?)`/g, '$1')
        .replace(/^#+\s+/gm, '')
        .replace(/\n{3,}/g, '\n\n')
        .trim();
    }

    function aiAnalyze() {
      updateStatus('<span class="loading"></span>🤖 AI正在分析图像，请稍候...（预计10-30秒）', 'analyzing');
      aiResult.innerHTML = '⏳ AI分析中...\n\n步骤：\n📷 正在拍摄图片...\n🔄 正在编码为Base64...\n🌐 正在调用AI API...\n💬 等待AI响应...\n✅ 准备显示结果...';

      aiImage.classList.add('hidden');
      document.querySelector('#ai-image-container .placeholder').classList.remove('hidden');

      const startTime = Date.now();

      fetch('/ai_analyze')
        .then(response => response.json())
        .then(data => {
          const elapsed = ((Date.now() - startTime) / 1000).toFixed(1);

          if (data.success) {
            document.querySelector('#ai-image-container .placeholder').classList.add('hidden');
            aiImage.src = 'data:image/jpeg;base64,' + data.image;
            aiImage.classList.remove('hidden');

            const beautifiedResult = beautifyAIText(data.result);
            aiResult.innerHTML = '✅ 分析完成\n\n' + beautifiedResult + '\n\n⏱️ 耗时：' + elapsed + ' 秒';
            updateStatus('✅ AI分析完成！用时 ' + elapsed + ' 秒', 'success');
            setTimeout(() => updateStatus('系统就绪'), 5000);
          } else {
            aiResult.innerHTML = '❌ 分析失败\n\n错误信息：' + data.error;
            updateStatus('❌ AI分析失败: ' + data.error, 'error');
          }
        })
        .catch(err => {
          aiResult.innerHTML = '❌ 网络错误\n\n' + err.message;
          updateStatus('❌ 请求失败: ' + err.message, 'error');
          console.error('AI分析错误:', err);
        });
    }

    function testBeep() {
      updateStatus('🔊 发送测试蜂鸣请求...');
      fetch('/beep')
        .then(res => res.json())
        .then(data => {
          if (data && data.success) {
            updateStatus('🔊 蜂鸣播放成功', 'success');
          } else {
            updateStatus('❌ 蜂鸣播放失败', 'error');
          }
        })
        .catch(err => {
          updateStatus('❌ 蜂鸣请求失败', 'error');
          console.error('beep请求错误:', err);
        });
    }

    function voiceAnalyze() {
      updateStatus('<span class="loading"></span>🎤 语音分析中，请说话...（预计10-20秒）', 'analyzing');
      aiResult.innerHTML = '⏳ 语音分析中...\n\n步骤：\n🎤 正在录音...\n🎙️ 正在语音识别...\n💡 生成提示词...\n🤖 调用视觉AI...\n✅ 准备显示结果...';

      fetch('/voice_analyze')
        .then(response => response.json())
        .then(data => {
          if (data.success) {
            aiResult.innerHTML = '✅ 语音分析完成\n\n' + data.message;
            updateStatus('✅ 语音分析完成！', 'success');
            setTimeout(() => updateStatus('系统就绪'), 5000);
          } else {
            aiResult.innerHTML = '❌ 语音分析失败\n\n错误信息：' + data.error;
            updateStatus('❌ 语音分析失败: ' + data.error, 'error');
          }
        })
        .catch(err => {
          aiResult.innerHTML = '❌ 网络错误\n\n' + err.message;
          updateStatus('❌ 请求失败: ' + err.message, 'error');
          console.error('语音分析错误:', err);
        });
    }

    function voiceWake() {
      updateStatus('<span class="loading"></span>�️ 语音唤醒模式激活中，请说出唤醒词...（最长45秒）', 'analyzing');
      aiResult.innerHTML = '⏳ 语音唤醒中...\n\n步骤：\n👂 监听唤醒词...\n🎧 检测成功后提示音...\n🎤 再次录音识别指令...\n🤖 调用视觉AI...\n🔊 扬声器播报结果...';

      fetch('/voice_wake')
        .then(response => response.json())
        .then(data => {
          if (data.success) {
            aiResult.innerHTML = '✅ 语音唤醒完成\n\n' + data.message;
            updateStatus('✅ 语音唤醒完成！', 'success');
            setTimeout(() => updateStatus('系统就绪'), 5000);
          } else {
            aiResult.innerHTML = '❌ 语音唤醒失败\n\n错误信息：' + data.error;
            updateStatus('❌ 语音唤醒失败: ' + data.error, 'error');
          }
        })
        .catch(err => {
          aiResult.innerHTML = '❌ 网络错误\n\n' + err.message;
          updateStatus('❌ 请求失败: ' + err.message, 'error');
          console.error('语音唤醒错误:', err);
        });
    }

    updateStatus('�💡 提示：先点击"开始视频流"查看画面，然后点击"AI分析"、"语音分析"或"语音唤醒"体验不同模式');
  </script>
</body>
</html>
)rawliteral";

  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

// ==================== Base64 编码函数 ====================
String encodeBase64(const uint8_t* data, size_t length) {
  size_t olen = 0;
  // 计算需要的输出缓冲区大小
  mbedtls_base64_encode(NULL, 0, &olen, data, length);
  
  // 分配缓冲区
  uint8_t* encoded = (uint8_t*)malloc(olen + 1);
  if (!encoded) {
    Serial.println("Base64编码内存分配失败");
    return "";
  }
  
  // 执行编码
  int ret = mbedtls_base64_encode(encoded, olen + 1, &olen, data, length);
  if (ret != 0) {
    Serial.printf("Base64编码失败，错误码: %d\n", ret);
    free(encoded);
    return "";
  }
  
  encoded[olen] = '\0';
  String result = String((char*)encoded);
  free(encoded);
  
  return result;
}

// ==================== 调用视觉大模型API ====================
String callVisionAPI(String base64Image) {
  // 首先检查WiFi连接状态
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ 错误: WiFi未连接！");
    Serial.println("请检查:");
    Serial.println("1. WiFi是否在范围内");
    Serial.println("2. SSID和密码是否正确");
    Serial.printf("   当前SSID: %s\n", ssid);
    Serial.println("3. 尝试重启ESP32重新连接");
    return "错误：WiFi未连接";
  }
  
  Serial.printf("✓ WiFi已连接，IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("  信号强度: %d dBm\n", WiFi.RSSI());
  
  WiFiClientSecure secureClient;
  secureClient.setInsecure();  // 快速调试使用，生产环境建议配置根证书

  HTTPClient http;
  String response = "";
  
  // 根据API类型选择配置
  const char* apiKey = "";
  const char* endpoint = "";
  const char* model = "";
  
  if (strcmp(API_TYPE, "openai") == 0) {
    apiKey = OPENAI_API_KEY;
    endpoint = OPENAI_ENDPOINT;
    model = OPENAI_MODEL;
  } else if (strcmp(API_TYPE, "qwen") == 0) {
    apiKey = QWEN_API_KEY;
    endpoint = QWEN_ENDPOINT;
    model = QWEN_MODEL;
  } else if (strcmp(API_TYPE, "custom") == 0) {
    apiKey = CUSTOM_API_KEY;
    endpoint = CUSTOM_ENDPOINT;
    model = CUSTOM_MODEL;
  }
  
  Serial.println("\n========== 调用视觉AI API ==========");
  Serial.printf("API类型: %s\n", API_TYPE);
  Serial.printf("端点: %s\n", endpoint);
  Serial.printf("模型: %s\n", model);
  
  // 检查API密钥
  if (strlen(apiKey) < 10 || strstr(apiKey, "your-") != NULL) {
    Serial.println("❌ 错误: API密钥未配置！");
    Serial.println("请在代码中设置正确的API_KEY");
    return "错误：API密钥未配置";
  }
  
  if (!http.begin(secureClient, endpoint)) {
    Serial.println("✗ HTTP begin 失败，请检查URL或TLS配置");
    return "错误：HTTP begin失败";
  }
  http.setTimeout(30000);  // 30秒超时
  
  // 设置请求头 (OpenAI兼容格式,适用于OpenAI和通义千问)
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + apiKey);
  
  // 构建JSON请求体 (OpenAI兼容格式)
  DynamicJsonDocument doc(4096);
  doc["model"] = model;
  doc["max_tokens"] = 1024; // increase output capacity
  doc["temperature"] = 0.2;
  
  JsonArray messages = doc.createNestedArray("messages");
  JsonObject message = messages.createNestedObject();
  message["role"] = "user";
  
  JsonArray content = message.createNestedArray("content");
  
  JsonObject textContent = content.createNestedObject();
  textContent["type"] = "text";
  textContent["text"] = VISION_PROMPT;
  
  JsonObject imageContent = content.createNestedObject();
  imageContent["type"] = "image_url";
  JsonObject imageUrl = imageContent.createNestedObject("image_url");
  imageUrl["url"] = "data:image/jpeg;base64," + base64Image;
  
  String requestBody;
  serializeJson(doc, requestBody);
  
  Serial.printf("请求体大小: %d bytes\n", requestBody.length());
  Serial.println("发送请求中...");
  
  // 发送POST请求
  int httpResponseCode = http.POST(requestBody);
  String httpErrorStr = http.errorToString(httpResponseCode);
  
  Serial.printf("HTTP响应码: %d (%s)\n", httpResponseCode, httpErrorStr.c_str());
  
  if (httpResponseCode > 0) {
    String payload = http.getString();
    Serial.printf("响应长度: %d bytes\n", payload.length());
    
    // 解析响应
  // enlarge response buffer to handle longer textual outputs
  DynamicJsonDocument responseDoc(16384);
    DeserializationError error = deserializeJson(responseDoc, payload);
    
    if (error) {
      Serial.printf("JSON解析失败: %s\n", error.c_str());
      Serial.println("原始响应:");
      Serial.println(payload.substring(0, 500));  // 只打印前500字符
      response = "JSON解析失败";
    } else {
      // OpenAI兼容格式响应解析 (适用于OpenAI和通义千问)
      if (responseDoc.containsKey("choices")) {
        // print finish_reason if present for debugging (helps detect truncation)
        if (responseDoc["choices"][0].containsKey("finish_reason")) {
          const char* fr = responseDoc["choices"][0]["finish_reason"].as<const char*>();
          Serial.printf("↪ finish_reason: %s\n", fr);
        }
        response = responseDoc["choices"][0]["message"]["content"].as<String>();
        Serial.println("✓ API调用成功");
        Serial.printf("↪ 返回文本长度: %d 字符\n", response.length());
        if (response.length() > 2000) Serial.println(response.substring(0, 2000));
      } else if (responseDoc.containsKey("error")) {
        response = "API错误: " + responseDoc["error"]["message"].as<String>();
        Serial.println("✗ API返回错误");
      } else {
        Serial.println("✗ 无法从响应中提取内容");
        Serial.println("响应结构:");
        serializeJsonPretty(responseDoc, Serial);
        response = "响应格式错误";
      }
    }
  } else {
    Serial.printf("✗ HTTP请求失败，错误码: %d (%s)\n", httpResponseCode, httpErrorStr.c_str());
    response = "HTTP请求失败: " + String(httpResponseCode) + " (" + httpErrorStr + ")";
  }
  
  http.end();
  Serial.println("====================================\n");
  
  return response;
}

// ==================== 串口输出AI结果 ====================
void outputToSerial(String aiResponse) {
  Serial.println("\n╔════════════════════════════════════╗");
  Serial.println("║      AI 视觉分析结果               ║");
  Serial.println("╚════════════════════════════════════╝");
  Serial.println(aiResponse);
  Serial.println("══════════════════════════════════════\n");
}

// ==================== TTS 语音合成与播放 ====================
// 支持多种在线TTS服务，默认使用有道语音以提升可访问性。

bool requestAndPlayTTS(const String& text);
bool playMP3StreamFromURL(const String& url);

// helper: split long text into chunks (tries to split at punctuation or space)
std::vector<String> splitTextIntoChunks(const String &text, size_t maxLen) {
  std::vector<String> chunks;
  if (text.length() <= (int)maxLen) {
    chunks.push_back(text);
    return chunks;
  }

  int pos = 0;
  int len = text.length();
  while (pos < len) {
    int remain = len - pos;
    int take = remain <= (int)maxLen ? remain : (int)maxLen;
    // try to find a punctuation or space backwards within take
    int splitPos = -1;
    for (int i = take - 1; i >= 0; --i) {
      char c = text.charAt(pos + i);
      if (c == '\n' || c == '。' || c == '！' || c == '？' || c == '.' || c == '!' || c == '?' || c == ';' || c == '；' || c == ',' || c == '，' || c == ' ' || c == '、') {
        splitPos = i + 1; // include punctuation
        break;
      }
    }
    if (splitPos == -1) splitPos = take; // no punctuation found

    String part = text.substring(pos, pos + splitPos);
    part.trim();
    if (part.length() > 0) chunks.push_back(part);
    pos += splitPos;
  }
  return chunks;
}

using TTSChunkFunc = std::function<bool(const String&)>;

// Play text by splitting into chunks and calling the chunk handler sequentially
bool playTextInChunks(const TTSChunkFunc &chunkFunc, const String &text, size_t maxLen) {
  std::vector<String> chunks = splitTextIntoChunks(text, maxLen);
  if (chunks.empty()) {
    Serial.println(F("⚠️ [TTS] 分段结果为空，无法播放"));
    return false;
  }

  Serial.printf("ℹ️ [TTS] 播放单段文本，长度=%d\n", chunks[0].length());
  bool ok = chunkFunc(chunks[0]);
  if (!ok) {
    Serial.println(F("✗ [TTS] 单段播放失败"));
  }
  return ok;
}

void speakText(String text) {
  text.trim();
  if (text.isEmpty()) {
    Serial.println(F("🎙️ [TTS] 文本为空，跳过语音播报"));
    return;
  }

  if (!requestAndPlayTTS(text)) {
    Serial.println(F("❌ [TTS] 语音播放失败"));
  }
}

String urlEncode(const String& value) {
  static const char* hex = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(value.length() * 3);

  for (size_t i = 0; i < value.length(); ++i) {
    uint8_t c = static_cast<uint8_t>(value[i]);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else if (c == ' ') {
      encoded += '+';
    } else {
      encoded += '%';
      encoded += hex[(c >> 4) & 0x0F];
      encoded += hex[c & 0x0F];
    }
  }

  return encoded;
}

// Mask a secret for logging: show first `head` and last `tail` chars, mask the middle
String maskString(const String &s, int head = 6, int tail = 4) {
  if (s.length() <= head + tail) return String("****");
  String out = s.substring(0, head);
  out += "...";
  out += s.substring(s.length() - tail);
  return out;
}

bool playBeepTone(int freqHz, int durationMs) {
  Serial.println(F("🔔 [TTS] 播放测试蜂鸣"));

  AudioOutputI2S* out = new AudioOutputI2S(I2S_NUM, 0);
  if (out == nullptr) {
    Serial.println(F("❌ [TTS] 分配I2S输出失败"));
    return false;
  }

  if (!out->SetPinout(I2S_BCLK_PIN, I2S_LRC_PIN, I2S_DOUT_PIN)) {
    Serial.println(F("❌ [TTS] I2S引脚配置失败"));
    delete out;
    return false;
  }
  out->SetOutputModeMono(true);
  out->SetRate(AUDIO_SAMPLE_RATE);

  if (!out->begin()) {
    Serial.println(F("❌ [TTS] I2S输出启动失败"));
    delete out;
    return false;
  }

  const int totalSamples = (AUDIO_SAMPLE_RATE * durationMs) / 1000;
  const float phaseIncrement = 2.0f * PI * static_cast<float>(freqHz) / static_cast<float>(AUDIO_SAMPLE_RATE);
  float phase = 0.0f;
  int16_t frame[2];

  for (int i = 0; i < totalSamples; ++i) {
    const int16_t sample = static_cast<int16_t>(sinf(phase) * 28000.0f);
    frame[0] = sample;
    frame[1] = sample;

    while (!out->ConsumeSample(frame)) {
      delay(1);
    }

    phase += phaseIncrement;
    if (phase > 2.0f * PI) {
      phase -= 2.0f * PI;
    }
  }

  out->flush();
  out->stop();
  delete out;

  Serial.println(F("✅ [TTS] 蜂鸣播放完成"));
  return true;
}

bool playMP3StreamFromURL(const String& url) {
  Serial.println(F("🎧 [TTS] 开始拉取音频流"));
  Serial.printf("🔎 [TTS] 请求 URL: %s\n", url.c_str());
  Serial.printf("🔋 [TTS] 可用堆内存: %d bytes, 可用PSRAM: %d bytes\n", ESP.getFreeHeap(), ESP.getFreePsram());

  // work on a mutable copy because parameter is const
  String reqUrl = url;

  // Runtime safety: if URL uses http://, force to https:// to avoid plaintext redirect or proxy issues
  if (reqUrl.startsWith("http://")) {
    String old = reqUrl;
    reqUrl = String("https://") + reqUrl.substring(7);
    Serial.println(F("⚠️ [TTS] 将 http:// 强制升级为 https://，以避免被中间代理篡改"));
    Serial.printf("↪ 原始 URL: %s\n", old.c_str());
    Serial.printf("↪ 升级后 URL: %s\n", reqUrl.c_str());
  }

  // --- Diagnostic probe: do a lightweight HTTP GET to inspect headers and first bytes ---
  Serial.println(F("🔍 [TTS] 进行诊断性 HTTP 探测（仅获取前若干字节以判断响应类型）"));
  {
    HTTPClient httpProbe;
    WiFiClient *baseClient = nullptr;
    WiFiClientSecure *secureClient = nullptr;
    bool isHttps = reqUrl.startsWith("https://");
    if (isHttps) {
      secureClient = new WiFiClientSecure();
      secureClient->setInsecure();
      baseClient = secureClient;
      if (!httpProbe.begin(*secureClient, reqUrl)) {
        Serial.println(F("✗ [TTS][probe] HTTPS begin 失败"));
      }
    } else {
      baseClient = new WiFiClient();
      if (!httpProbe.begin(reqUrl)) {
        Serial.println(F("✗ [TTS][probe] HTTP begin 失败"));
      }
    }

  httpProbe.setTimeout(5000);
  // set browser-like User-Agent to avoid anti-bot / anti-leech responses
  httpProbe.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
  int code = httpProbe.GET();
    Serial.printf("↪ [TTS][probe] HTTP 响应码: %d\n", code);
    if (code == 200) {
      String ctype = httpProbe.header("Content-Type");
      long clen = httpProbe.getSize();
      Serial.printf("↪ [TTS][probe] Content-Type: %s\n", ctype.c_str());
      Serial.printf("↪ [TTS][probe] Content-Length: %ld\n", clen);

      // 只读取前 512 字节用于判断（不打印大量二进制）
      WiFiClient *stream = httpProbe.getStreamPtr();
      if (stream) {
        const int maxPeek = 512;
        int availWait = 0;
        while (!stream->available() && availWait < 20) { availWait++; delay(50); }
        int toRead = min(maxPeek, stream->available());
        if (toRead > 0) {
          uint8_t buf[513];
          int r = stream->read(buf, toRead);
          if (r > 0) {
            // 判断是否为文本（可打印）还是二进制
            bool printable = true;
            for (int i = 0; i < r; ++i) {
              if (buf[i] < 9 || (buf[i] > 13 && buf[i] < 32)) { printable = false; break; }
            }
            if (printable) {
              buf[r] = '\0';
              Serial.println(F("↪ [TTS][probe] 响应前缀(文本):"));
              Serial.println((char*)buf);
            } else {
              Serial.println(F("↪ [TTS][probe] 响应前缀(二进制 / 非文本)，以十六进制显示前 64 字节:"));
              int hexShow = min(r, 64);
              for (int i = 0; i < hexShow; ++i) {
                Serial.printf("%02X ", buf[i]);
                if ((i+1) % 16 == 0) Serial.println();
              }
              Serial.println();
            }
          }
        } else {
          Serial.println(F("↪ [TTS][probe] 流中无可读字节"));
        }
      }
    } else {
      String err = httpProbe.getString();
      Serial.println(F("↪ [TTS][probe] 非200响应体片段:"));
      if (err.length() > 512) Serial.println(err.substring(0,512)); else Serial.println(err);
    }

    httpProbe.end();
    if (secureClient) delete secureClient; else if (baseClient) delete baseClient;
  }
  Serial.println(F("🔍 [TTS] HTTP 探测完成，开始正式打开流以播放"));
  // --- end probe ---

  AudioHTTPSStream* httpsStream = nullptr;
  AudioFileSourceHTTPStream* httpStream = nullptr;
  AudioFileSource* file = nullptr;

  if (reqUrl.startsWith("https://")) {
    Serial.println(F("🔐 [TTS] 使用 HTTPS 流"));
    httpsStream = new AudioHTTPSStream();
    if (httpsStream == nullptr) {
      Serial.println(F("❌ [TTS] 分配HTTPS流对象失败"));
      return false;
    }
    httpsStream->setUseInsecure(true);
    httpsStream->setFollowRedirects(true);
    httpsStream->setTimeout(20000);
    httpsStream->setUserAgent(F("Mozilla/5.0 (ESP32-S3)"));

    if (!httpsStream->open(reqUrl.c_str())) {
      Serial.println(F("❌ [TTS] 建立HTTPS音频流失败，尝试诊断..."));
      httpsDiagnostic(reqUrl);
      delete httpsStream;
      return false;
    }
    Serial.println(F("✓ [TTS] HTTPS 流已打开"));
    file = httpsStream;
  } else {
    Serial.println(F("🔐 [TTS] 使用 HTTP 流"));
    httpStream = new AudioFileSourceHTTPStream();
    if (httpStream == nullptr) {
      Serial.println(F("❌ [TTS] 分配HTTP流对象失败"));
      return false;
    }
    if (!httpStream->open(reqUrl.c_str())) {
      Serial.println(F("❌ [TTS] 建立HTTP音频流失败"));
      delete httpStream;
      return false;
    }
    Serial.println(F("✓ [TTS] HTTP 流已打开"));
    file = httpStream;
  }

  AudioOutputI2S* out = new AudioOutputI2S(I2S_NUM, 0);
  if (out == nullptr) {
    Serial.println(F("❌ [TTS] 分配I2S输出失败"));
    if (httpsStream) {
      delete httpsStream;
    }
    if (httpStream) {
      delete httpStream;
    }
    return false;
  }
  out->SetPinout(I2S_BCLK_PIN, I2S_LRC_PIN, I2S_DOUT_PIN);
  out->SetGain(0.65f);

  if (!out->begin()) {
    Serial.println(F("❌ [TTS] I2S 输出 begin() 失败"));
    delete out;
    if (httpsStream) delete httpsStream;
    if (httpStream) delete httpStream;
    return false;
  }

  AudioGeneratorMP3* mp3 = new AudioGeneratorMP3();
  if (mp3 == nullptr) {
    Serial.println(F("❌ [TTS] 分配MP3解码器失败"));
    delete out;
    if (httpsStream) {
      delete httpsStream;
    }
    if (httpStream) {
      delete httpStream;
    }
    return false;
  }

  bool success = false;
  if (mp3->begin(file, out)) {
    Serial.println(F("🔊 [TTS] 正在播放语音..."));
    while (mp3->isRunning()) {
      if (!mp3->loop()) {
        Serial.println(F("✖ [TTS] mp3->loop() 返回 false"));
        mp3->stop();
        break;
      }
      success = true;
      delay(1);
    }
    if (mp3->isRunning()) {
      mp3->stop();
    }
  } else {
    Serial.println(F("❌ [TTS] MP3解码器初始化失败"));
    Serial.printf("🔋 [TTS] 解码器初始化时可用堆内存: %d bytes\n", ESP.getFreeHeap());
  }

  delete mp3;
  // ensure audio output is cleanly stopped
  out->flush();
  out->stop();
  delete out;
  if (httpsStream) {
    delete httpsStream;
  }
  if (httpStream) {
    delete httpStream;
  }

  if (success) {
    Serial.println(F("✅ [TTS] 音频播放完成"));
  } else {
    Serial.println(F("❌ [TTS] 音频播放失败"));
  }

  return success;
}

// 下载MP3到SPIFFS，返回是否成功并把文件路径写入 outPath
bool downloadMP3ToSPIFFS(const String &url, const String &outPath) {
  Serial.printf("⬇️ [TTS] 下载音频到 SPIFFS: %s -> %s\n", url.c_str(), outPath.c_str());
  if (!SPIFFS.begin(true)) {
    Serial.println(F("✗ [TTS] SPIFFS mount 失败，无法下载"));
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(20000);
  if (!http.begin(client, url)) {
    Serial.println(F("✗ [TTS] HTTP begin 失败 (download)"));
    return false;
  }

  int code = http.GET();
  if (code != 200) {
    Serial.printf("✗ [TTS] 下载请求返回 %d\n", code);
    http.end();
    return false;
  }

  File f = SPIFFS.open(outPath, FILE_WRITE);
  if (!f) {
    Serial.println(F("✗ [TTS] 无法在 SPIFFS 创建文件"));
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[1024];
  int len = 0;
  while (http.connected() && (len = stream->available() ? stream->readBytes((char*)buf, sizeof(buf)) : 0) > 0) {
    f.write(buf, len);
  }

  f.close();
  http.end();
  Serial.println(F("✓ [TTS] 下载完成到 SPIFFS"));
  return true;
}

// 从 SPIFFS 播放下载的 MP3 文件
bool playMP3FromSPIFFS(const String &path) {
  Serial.printf("▶️ [TTS] 从 SPIFFS 播放: %s\n", path.c_str());
  if (!SPIFFS.begin(false)) {
    Serial.println(F("✗ [TTS] SPIFFS 未挂载，无法播放"));
    return false;
  }
  AudioFileSourceSPIFFS *file = new AudioFileSourceSPIFFS(path.c_str());
  if (!file) {
    Serial.println(F("✗ [TTS] 无法分配 SPIFFS 文件源"));
    return false;
  }

  AudioOutputI2S* out = new AudioOutputI2S(I2S_NUM, 0);
  out->SetPinout(I2S_BCLK_PIN, I2S_LRC_PIN, I2S_DOUT_PIN);
  out->SetGain(0.65f);
  if (!out->begin()) {
    Serial.println(F("✗ [TTS] SPIFFS 播放：I2S begin() 失败"));
    delete out; delete file; return false;
  }
  AudioGeneratorMP3* mp3 = new AudioGeneratorMP3();
  bool ok = false;
  if (mp3->begin(file, out)) {
    Serial.println(F("🔊 [TTS] 正在播放 SPIFFS 中的语音..."));
    while (mp3->isRunning()) {
      if (!mp3->loop()) { mp3->stop(); break; }
      ok = true; delay(1);
    }
    if (mp3->isRunning()) mp3->stop();
  } else {
    Serial.println(F("✗ [TTS] SPIFFS 上的 MP3 初始化失败"));
  }

  delete mp3;
  out->flush(); out->stop(); delete out;
  delete file;
  if (ok) Serial.println(F("✅ [TTS] SPIFFS 播放完成")); else Serial.println(F("❌ [TTS] SPIFFS 播放失败"));
  return ok;
}



// 百度TTS provider（推荐通过本地代理，避免token管理和HTTPS问题）
bool requestAndPlayBaiduTTS(const String& text) {
  // 如果配置了 BAIDU_API_KEY 与 SECRET，则优先在设备上直接获取 token 并直连百度（无需代理）
  // 直接优先：若配置了临时 access token 则直接使用；否则尝试设备获取 token 并直连
  // chunk-level handler for Baidu (single chunk)
  auto chunkFunc = [](const String &chunk)->bool{
    if (strlen(BAIDU_TTS_ACCESS_TOKEN) > 0) {
      String encoded = urlEncode(chunk);
  String url = String("https://tsn.baidu.com/text2audio?tex=") + encoded + "&tok=" + String(BAIDU_TTS_ACCESS_TOKEN) + "&cuid=ESP32CAM001&ctp=1&lan=zh&spd=5&pit=5&vol=7&per=0";
      Serial.printf("🔐 [Baidu] 使用 config 中的 access_token 请求 (token掩码=%s)\n", maskString(String(BAIDU_TTS_ACCESS_TOKEN)).c_str());
      Serial.printf("🌐 [Baidu] 请求 URL: %s\n", url.c_str());
      return playMP3StreamFromURL(url);
    }
    if (!fetchBaiduTokenIfNeeded()) {
      Serial.println(F("✗ [Baidu] 无法获取 token，直接调用百度失败 (chunk)"));
      return false;
    }
    String encoded = urlEncode(chunk);
  String url = String("https://tsn.baidu.com/text2audio?tex=") + encoded + "&tok=" + baidu_access_token + "&cuid=ESP32CAM001&ctp=1&lan=zh&spd=5&pit=5&vol=7&per=0";
    Serial.printf("🔐 [Baidu] 设备直连请求 (token掩码=%s)\n", maskString(baidu_access_token).c_str());
    Serial.printf("🌐 [Baidu] 请求 URL: %s\n", url.c_str());
    return playMP3StreamFromURL(url);
  };

  // Baidu supports longer texts; use a generous chunk size
  return playTextInChunks(chunkFunc, text, 1024);
}

// 在设备上获取 token（HTTPS），并缓存
bool fetchBaiduTokenIfNeeded() {
  if (baidu_access_token.length() > 0 && millis() < baidu_token_expires_ms) return true;
  if (strlen(BAIDU_API_KEY) == 0 || strlen(BAIDU_SECRET_KEY) == 0) return false;
  Serial.println(F("🔐 [Baidu] 获取 access_token 中..."));
  Serial.printf("🔋 [Baidu] 可用堆内存: %d bytes, 可用PSRAM: %d bytes\n", ESP.getFreeHeap(), ESP.getFreePsram());
  Serial.printf("🔑 [Baidu] 使用 API_KEY=%s, SECRET=%s (已掩码)\n", maskString(String(BAIDU_API_KEY)).c_str(), maskString(String(BAIDU_SECRET_KEY)).c_str());
  String url = String("https://openapi.baidu.com/oauth/2.0/token?grant_type=client_credentials&client_id=") + BAIDU_API_KEY + "&client_secret=" + BAIDU_SECRET_KEY;

  WiFiClientSecure client;
  client.setInsecure(); // 开发时可用，生产建议安装根证书
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, url)) {
    Serial.println(F("✗ [Baidu] HTTP begin(token) 失败"));
    return false;
  }
  int code = http.GET();
  if (code != 200) {
    Serial.printf("✗ [Baidu] token 请求返回 %d\n", code);
    String err = http.getString();
    if (err.length() > 512) err = err.substring(0, 512);
    Serial.println(err);
    http.end();
    return false;
  }
  String payload = http.getString();
  Serial.print("↪ [Baidu] token 返回片段: ");
  if (payload.length() > 512) Serial.println(payload.substring(0, 512)); else Serial.println(payload);
  http.end();

  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.println(F("✗ [Baidu] JSON 解析 token 失败"));
    Serial.println(payload);
    return false;
  }
  if (!doc.containsKey("access_token")) {
    Serial.println(F("✗ [Baidu] token 响应不含 access_token"));
    Serial.println(payload);
    return false;
  }
  baidu_access_token = doc["access_token"].as<String>();
  int expires_in = doc["expires_in"].as<int>();
  baidu_token_expires_ms = millis() + (unsigned long)(expires_in - 60) * 1000UL;
  Serial.printf("✓ [Baidu] 获取到 token，expires_in=%d 秒\n", expires_in);
  Serial.printf("🔑 [Baidu] access_token (已掩码): %s\n", maskString(baidu_access_token).c_str());
  Serial.printf("⏳ [Baidu] 本地 token 过期时间 (ms since boot): %lu\n", baidu_token_expires_ms);
  return true;
}

// 设备上直接调用百度TTS
bool requestAndPlayBaiduTTS_OnDevice(const String& text) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("❌ [TTS] WiFi未连接，无法请求TTS"));
    return false;
  }
  // 优先使用 config 中的临时 access token（用于快速测试）
  if (strlen(BAIDU_TTS_ACCESS_TOKEN) > 0) {
    // 使用 config 中的 access token 时，也采用分段播放，避免截断长文本
    auto chunkFunc = [](const String &chunk)->bool{
      String encoded = urlEncode(chunk);
  String url = String("https://tsn.baidu.com/text2audio?tex=") + encoded + "&tok=" + String(BAIDU_TTS_ACCESS_TOKEN) + "&cuid=ESP32CAM001&ctp=1&lan=zh&spd=5&pit=5&vol=7&per=0";
      Serial.printf("🔐 [Baidu] 使用 config 中的 access_token 请求 (token掩码=%s)\n", maskString(String(BAIDU_TTS_ACCESS_TOKEN)).c_str());
      Serial.printf("🔋 [Baidu] 可用堆内存: %d bytes, 可用PSRAM: %d bytes\n", ESP.getFreeHeap(), ESP.getFreePsram());
      Serial.printf("🌐 [Baidu] 请求 URL: %s\n", url.c_str());
      return playMP3StreamFromURL(url);
    };
    return playTextInChunks(chunkFunc, text, 1024);
  }

  if (!fetchBaiduTokenIfNeeded()) {
    Serial.println(F("✗ [Baidu] 无法获取 token，直接调用百度失败"));
    return false;
  }
  // 设备端获取到 token 后，按段播放完整文本（避免单次 200 字截断）
  auto chunkFunc = [](const String &chunk)->bool{
    String encoded = urlEncode(chunk);
  String url = String("https://tsn.baidu.com/text2audio?tex=") + encoded + "&tok=" + baidu_access_token + "&cuid=ESP32CAM001&ctp=1&lan=zh&spd=5&pit=5&vol=7&per=0";
    Serial.printf("🔐 [Baidu] 设备直连请求 (token掩码=%s)\n", maskString(baidu_access_token).c_str());
    Serial.printf("🔋 [Baidu] 可用堆内存: %d bytes, 可用PSRAM: %d bytes\n", ESP.getFreeHeap(), ESP.getFreePsram());
    Serial.printf("🌐 [Baidu] 请求 URL: %s\n", url.c_str());
    return playMP3StreamFromURL(url);
  };
  return playTextInChunks(chunkFunc, text, 1024);
}

bool requestAndPlayEdgeTTS(const String& text) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("❌ [TTS] WiFi未连接，无法请求TTS"));
    return false;
  }
  return false; // Edge provider removed; keep stub to avoid link errors if referenced elsewhere
}

bool requestAndPlayTTS(const String& text) {
  // 目前简化为仅使用百度 TTS
  Serial.println(F("ℹ️ [TTS] 仅使用百度 TTS 进行语音合成"));
  bool ok = requestAndPlayBaiduTTS(text);
  if (ok) {
    Serial.println(F("✅ [TTS] 由 baidu 成功播放"));
  } else {
    Serial.println(F("❌ [TTS] baidu 播放失败"));
  }
  return ok;
}

// ==================== 执行完整的拍照分析流程 ====================
void performVisionAnalysis() {
  Serial.println("\n\n****************************************");
  Serial.println("*     开始执行视觉分析流程             *");
  Serial.println("****************************************\n");
  
  unsigned long startTime = millis();
  
  // 1. 拍摄图片
  Serial.println("📷 [步骤 1/5] 拍摄图片...");
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("❌ 拍照失败！");
    return;
  }
  Serial.printf("✓ 拍照成功，图片大小: %u bytes (%.1f KB)\n", fb->len, fb->len / 1024.0);
  
  // 2. Base64编码
  Serial.println("\n🔄 [步骤 2/5] 编码图片为Base64...");
  unsigned long encodeStart = millis();
  String base64Image = encodeBase64(fb->buf, fb->len);
  unsigned long encodeTime = millis() - encodeStart;
  
  if (base64Image.length() == 0) {
    Serial.println("❌ Base64编码失败！");
    esp_camera_fb_return(fb);
    return;
  }
  Serial.printf("✓ 编码成功，耗时: %lu ms\n", encodeTime);
  Serial.printf("  Base64长度: %d 字符\n", base64Image.length());
  
  // 释放图片缓冲区
  esp_camera_fb_return(fb);
  
  // 3. 调用AI API
  Serial.println("\n🤖 [步骤 3/5] 调用视觉AI API...");
  Serial.printf("  可用堆内存: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("  可用PSRAM: %d bytes\n", ESP.getFreePsram());
  
  unsigned long apiStart = millis();
  String aiResponse = callVisionAPI(base64Image);
  unsigned long apiTime = millis() - apiStart;
  
  Serial.printf("\n⏱️  API调用耗时: %lu ms (%.1f 秒)\n", apiTime, apiTime / 1000.0);
  
  // 清理base64字符串释放内存
  base64Image = "";
  
  // 4. 串口输出结果
  Serial.println("\n📝 [步骤 4/5] 串口输出AI分析结果...");
  outputToSerial(aiResponse);
  
  // 5. 语音播报（预留）
  Serial.println("🔊 [步骤 5/5] 语音播报（预留功能）...");
  speakText(aiResponse);
  
  unsigned long totalTime = millis() - startTime;
  Serial.println("\n****************************************");
  Serial.printf("*  流程完成！总耗时: %lu ms (%.1f 秒) *\n", totalTime, totalTime / 1000.0);
  Serial.println("****************************************\n");
}

// ==================== 检测按钮触发 ====================
void checkButtonTrigger() {
  // 视觉分析按钮
  int reading = digitalRead(TRIGGER_BUTTON_PIN);
  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }
  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (reading == LOW && lastButtonState == HIGH) {
      Serial.println("\n🔘 按钮触发：开始拍照分析");
      performVisionAnalysis();
    }
  }
  lastButtonState = reading;
  
  // 语音输入按钮
  int voiceReading = digitalRead(VOICE_BUTTON_PIN);
  if (voiceReading != lastVoiceButtonState) {
    lastVoiceDebounceTime = millis();
  }
  if ((millis() - lastVoiceDebounceTime) > debounceDelay) {
    if (voiceReading == LOW && lastVoiceButtonState == HIGH) {
      voiceButtonPressStart = millis();
      Serial.println("\n🎤 语音按钮按下");
    }
    if (voiceReading == HIGH && lastVoiceButtonState == LOW) {
      unsigned long pressDuration = millis() - voiceButtonPressStart;
      if (pressDuration >= 1500) {
        Serial.println("\n🎤 长按触发：进入语音唤醒模式");
        bool wakeOk = performVoiceWakeFlow();
        if (!wakeOk) {
          Serial.println("⚠️ 语音唤醒流程未完成或失败");
        }
      } else {
        Serial.println("\n🎤 短按触发：直接语音分析");
        bool voiceOk = performVoiceAnalysis();
        if (!voiceOk) {
          Serial.println("⚠️ 语音分析失败，请重试");
        }
      }
    }
  }
  lastVoiceButtonState = voiceReading;
}

// ==================== HTTP处理 - AI分析 ====================
// 全局变量存储最后的AI分析结果
String lastAIResult = "";
String lastImageBase64 = "";

static esp_err_t ai_analyze_handler(httpd_req_t *req){
  Serial.println("\n🌐 Web触发：AI分析请求");
  
  // 1. 拍摄图片
  Serial.println("📷 [Web] 拍摄图片...");
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
    const char* error_json = "{\"success\":false,\"error\":\"拍照失败\"}";
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, error_json, strlen(error_json));
  }
  
  // 2. Base64编码
  Serial.println("🔄 [Web] 编码图片...");
  String base64Image = encodeBase64(fb->buf, fb->len);
  
  // 保存图片（用于Web显示）
  lastImageBase64 = base64Image;
  
  // 释放图片缓冲区
  esp_camera_fb_return(fb);
  
  if (base64Image.length() == 0) {
    const char* error_json = "{\"success\":false,\"error\":\"Base64编码失败\"}";
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, error_json, strlen(error_json));
  }
  
  // 3. 调用AI API
  Serial.println("🤖 [Web] 调用AI API...");
  String aiResponse = callVisionAPI(base64Image);
  
  // 保存结果
  lastAIResult = aiResponse;
  
  // 清理base64字符串释放内存
  base64Image = "";
  
  bool shouldPlay = true;
  if (aiResponse.indexOf("错误") >= 0 || aiResponse.indexOf("失败") >= 0) {
    shouldPlay = false;
  }

  // 4. 构建JSON响应
  DynamicJsonDocument doc(8192);
  
  if (aiResponse.indexOf("错误") >= 0 || aiResponse.indexOf("失败") >= 0) {
    doc["success"] = false;
    doc["error"] = aiResponse;
  } else {
    doc["success"] = true;
    doc["result"] = aiResponse;
    doc["image"] = lastImageBase64;  // 返回图片的Base64
  }
  
  String jsonResponse;
  serializeJson(doc, jsonResponse);

  if (shouldPlay) {
    Serial.println("🔊 [Web] 准备语音播报AI结果...");
    speakText(aiResponse);
  } else {
    Serial.println("ℹ️ [Web] AI返回错误消息，跳过语音播报");
  }
  
  // 返回JSON响应
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  
  Serial.println("✅ [Web] 响应已发送");
  return httpd_resp_send(req, jsonResponse.c_str(), jsonResponse.length());
}

// 临时路由：蜂鸣测试
static esp_err_t beep_handler(httpd_req_t *req) {
  Serial.println("/beep 路由调用 - 播放测试蜂鸣");
  bool ok = playBeepTone(600, 600);
  const char* response_json = ok ? "{\"success\":true}" : "{\"success\":false}";
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, response_json, strlen(response_json));
}

// ==================== HTTP处理 - 语音分析 ====================
static esp_err_t voice_analyze_handler(httpd_req_t *req){
  Serial.println("\n🌐 Web触发：语音分析请求");
  
  // 执行语音分析
  bool success = performVoiceAnalysis();

  DynamicJsonDocument doc(256);
  doc["success"] = success;
  if (success) {
    doc["message"] = "语音分析完成";
  } else {
    doc["error"] = "语音分析失败，请重试";
  }

  String jsonResponse;
  serializeJson(doc, jsonResponse);

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, jsonResponse.c_str(), jsonResponse.length());
}

static esp_err_t voice_wake_handler(httpd_req_t *req) {
  Serial.println("\n🌐 Web触发：语音唤醒请求");

  bool success = performVoiceWakeFlow();

  DynamicJsonDocument doc(256);
  doc["success"] = success;
  if (success) {
    doc["message"] = "语音唤醒完成";
  } else {
    doc["error"] = "未检测到唤醒词或分析失败";
  }

  String jsonResponse;
  serializeJson(doc, jsonResponse);

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, jsonResponse.c_str(), jsonResponse.length());
}

// 启动Web服务器
void startCameraServer(){
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.max_uri_handlers = 8;
  config.stack_size = 8192;  // 增加栈大小
  
  httpd_uri_t index_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_handler,
    .user_ctx  = NULL
  };
  
  httpd_uri_t capture_uri = {
    .uri       = "/capture",
    .method    = HTTP_GET,
    .handler   = jpg_handler,
    .user_ctx  = NULL
  };
  
  httpd_uri_t stream_uri = {
    .uri       = "/stream",
    .method    = HTTP_GET,
    .handler   = stream_handler,
    .user_ctx  = NULL
  };
  
  httpd_uri_t ai_analyze_uri = {
    .uri       = "/ai_analyze",
    .method    = HTTP_GET,
    .handler   = ai_analyze_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t beep_uri = {
    .uri       = "/beep",
    .method    = HTTP_GET,
    .handler   = beep_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t voice_analyze_uri = {
    .uri       = "/voice_analyze",
    .method    = HTTP_GET,
    .handler   = voice_analyze_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t voice_wake_uri = {
    .uri       = "/voice_wake",
    .method    = HTTP_GET,
    .handler   = voice_wake_handler,
    .user_ctx  = NULL
  };
  
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &stream_uri);
    httpd_register_uri_handler(camera_httpd, &ai_analyze_uri);
    httpd_register_uri_handler(camera_httpd, &beep_uri);
    httpd_register_uri_handler(camera_httpd, &voice_analyze_uri);
    httpd_register_uri_handler(camera_httpd, &voice_wake_uri);
    Serial.println("HTTP服务器启动成功");
  } else {
    Serial.println("HTTP服务器启动失败");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n╔══════════════════════════════════════╗");
  Serial.println("║  ESP32-S3 CAM AI视觉智能问答机      ║");
  Serial.println("║  Vision AI Question Answering       ║");
  Serial.println("╚══════════════════════════════════════╝");
  
  // 检查PSRAM
  if(psramFound()){
    Serial.println("✓ PSRAM已启用");
    Serial.printf("   PSRAM大小: %d bytes (%.1f MB)\n", ESP.getPsramSize(), ESP.getPsramSize() / 1024.0 / 1024.0);
    Serial.printf("   可用PSRAM: %d bytes (%.1f MB)\n", ESP.getFreePsram(), ESP.getFreePsram() / 1024.0 / 1024.0);
    Serial.printf("   可用堆内存: %d bytes (%.1f KB)\n", ESP.getFreeHeap(), ESP.getFreeHeap() / 1024.0);
  } else {
    Serial.println("✗ 未检测到PSRAM");
    Serial.println("必须在 Tools > PSRAM 中启用！");
    while(1) delay(1000);
  }
  
  // 配置LED
  pinMode(LED_GPIO_NUM, OUTPUT);
  digitalWrite(LED_GPIO_NUM, LOW);
  
  // 配置触发按钮
  pinMode(TRIGGER_BUTTON_PIN, INPUT_PULLUP);
  pinMode(VOICE_BUTTON_PIN, INPUT_PULLUP);
  Serial.printf("✓ 触发按钮配置在 GPIO%d (视觉)\n", TRIGGER_BUTTON_PIN);
  Serial.printf("✓ 语音按钮配置在 GPIO%d (语音)\n", VOICE_BUTTON_PIN);
  
  // 初始化摄像头
  Serial.println("\n[1/4] 初始化摄像头...");
  setupCamera();
  
  // 初始化麦克风
  Serial.println("\n[2/4] 初始化麦克风...");
  setupMicrophone();

  // 初始化 SPIFFS（用于 TTS 临时缓存）
  Serial.println("\n[?] 尝试挂载 SPIFFS 用于 TTS 缓存...");
  if (SPIFFS.begin(true)) {
    Serial.println("✓ SPIFFS 挂载成功");
  } else {
    Serial.println("✗ SPIFFS 挂载失败（可能未选择带 SPIFFS 的分区），TTS 回退到本地文件将不可用");
  }
  
  // 连接WiFi
  Serial.println("\n[3/4] 连接WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  WiFi.setSleep(false);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if(WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ WiFi连接成功");
    Serial.print("   IP地址: ");
    Serial.println(WiFi.localIP());
    Serial.print("   信号强度: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    
    // 尝试通过 NTP 同步时间，HTTPS 连接需要正确的系统时间
    configTime(8 * 3600, 0, "pool.ntp.org", "time.google.com");
    Serial.println("⏳ 尝试 NTP 时间同步（最多等待 10 秒）...");
    time_t now = time(nullptr);
    int ntpWait = 0;
    while (now < 1600000000 && ntpWait < 10) { // 约为 2020-09-13 之后的时间
      delay(1000);
      Serial.print('.');
      now = time(nullptr);
      ntpWait++;
    }
    Serial.println();
    if (now >= 1600000000) {
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);
      Serial.printf("✓ NTP 时间同步成功: %s", asctime(&timeinfo));
    } else {
      Serial.println("✗ NTP 时间同步失败，HTTPS 可能会失败");
    }
    
  // 启动HTTP服务器
  Serial.println("\n[4/4] 启动Web服务器...");
    startCameraServer();
    
    Serial.println("\n╔══════════════════════════════════════╗");
    Serial.println("║        系统启动完成！                ║");
    Serial.println("╚══════════════════════════════════════╝");
    Serial.print("🌐 Web界面: http://");
    Serial.println(WiFi.localIP());
    Serial.println();
  } else {
    Serial.println("\n✗ WiFi连接失败");
    Serial.println("   请检查WiFi配置");
  }
}

void loop() {
  checkButtonTrigger();

#if AUTO_WAKE_ENABLED
  static bool wakeInProgress = false;
  static unsigned long nextWakeStart = 0;

  if (!wakeInProgress && millis() >= nextWakeStart) {
    if (voiceWakeBusy) {
      nextWakeStart = millis() + AUTO_WAKE_RETRY_DELAY_MS;
    } else {
      wakeInProgress = true;
      bool wakeSuccess = performVoiceWakeFlow();
      if (!wakeSuccess) {
        Serial.println("ℹ️ [自动唤醒] 监听结束，1分钟内未检测到唤醒词");
      }
      nextWakeStart = millis() + AUTO_WAKE_RETRY_DELAY_MS;
      wakeInProgress = false;
    }
  }
#endif

  delay(10);
}
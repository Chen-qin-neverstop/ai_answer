#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "esp_http_server.h"
#include "esp_err.h"
#include "esp_timer.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "mbedtls/base64.h"
#include <math.h>
#include <time.h>
#include <AudioFileSourceHTTPStream.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include "AudioHTTPSStream.h"
#include <vector>
#include <functional>

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

// 可选代理: 如果设备无法直接访问外部TTS（网络/防火墙问题），
// 可以在本地或VPS上运行一个简单的HTTP代理，将真实TTS请求由代理发出并返回音频。
// 例: "http://192.168.1.100:3000/tts_proxy" 或 "http://your-vps:3000/tts_proxy"
// 置为空字符串表示不使用代理。
const char* TTS_PROXY_URL = "";

// ==================== I2S 音频输出配置 ====================
#define I2S_BCLK_PIN    21
#define I2S_LRC_PIN     42
#define I2S_DOUT_PIN    41
#define I2S_NUM         I2S_NUM_0

#define AUDIO_SAMPLE_RATE     16000
#define AUDIO_BITS_PER_SAMPLE 16
#define AUDIO_CHANNELS        1

// 触发按钮配置
#define TRIGGER_BUTTON_PIN 0
bool lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;

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

    updateStatus('💡 提示：先点击"开始视频流"查看画面，然后点击"AI分析"识别图像');
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
bool playBeepTone(int freqHz = 600, int durationMs = 500);

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
  bool anyOk = false;
  for (size_t i = 0; i < chunks.size(); ++i) {
    Serial.printf("ℹ️ [TTS] 播放第 %d/%d 段，长度=%d\n", (int)(i+1), (int)chunks.size(), chunks[i].length());
    bool ok = chunkFunc(chunks[i]);
    if (!ok) {
      Serial.printf("✗ [TTS] 第 %d 段播放失败\n", (int)(i+1));
      // continue to next chunk to try to play remaining text
    } else {
      anyOk = true;
    }
    // small delay between chunks to ensure streams close cleanly
    delay(200);
  }
  return anyOk;
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
  out->SetGain(0.8f);

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
  int reading = digitalRead(TRIGGER_BUTTON_PIN);
  
  // 防抖处理
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
  
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &stream_uri);
    httpd_register_uri_handler(camera_httpd, &ai_analyze_uri);
  httpd_register_uri_handler(camera_httpd, &beep_uri);
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
  Serial.printf("✓ 触发按钮配置在 GPIO%d\n", TRIGGER_BUTTON_PIN);
  
  // 初始化摄像头
  Serial.println("\n[1/3] 初始化摄像头...");
  setupCamera();
  
  // 连接WiFi
  Serial.println("\n[2/3] 连接WiFi...");
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
  Serial.println("\n[3/3] 启动Web服务器...");
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

  delay(10);
}
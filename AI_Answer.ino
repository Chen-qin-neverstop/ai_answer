#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "esp_http_server.h"
#include "esp_timer.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "mbedtls/base64.h"
#include "driver/i2s.h"  // I2S音频输出

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
const char* QWEN_ENDPOINT = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";  // ⚠️ 使用OpenAI兼容端点
const char* QWEN_MODEL = "qwen-vl-plus";  // 或 qwen-vl-plus / qwen-vl-max-latest

// 自定义API配置（如果使用其他兼容OpenAI格式的API）
const char* CUSTOM_ENDPOINT = "https://your-custom-endpoint/v1/chat/completions";
const char* CUSTOM_MODEL = "your-model-name";

// 提示词配置
const char* VISION_PROMPT = "请详细描述这张图片中的内容，包括物体、场景、颜色等细节。用中文回答。";

// ==================== I2S 音频输出配置 ====================
// MAX98357A I2S 引脚定义 (修改避免与摄像头冲突)
// ⚠️ 原GPIO 13/14/15与摄像头冲突,改用以下引脚:
#define I2S_BCLK_PIN    21  // 位时钟 → MAX98357A BCLK
#define I2S_LRC_PIN     42  // 帧时钟 → MAX98357A LRC  
#define I2S_DOUT_PIN    41  // 数据输出 → MAX98357A DIN
#define I2S_NUM         I2S_NUM_0  // 使用I2S端口0

// 音频参数
#define AUDIO_SAMPLE_RATE    16000  // TTS采样率16kHz
#define AUDIO_BITS_PER_SAMPLE 16    // 16位音频
#define AUDIO_CHANNELS        1     // 单声道

// 触发按钮配置
#define TRIGGER_BUTTON_PIN 0  // GPIO0 - Boot按钮
bool lastButtonState = HIGH;
unsigned long lastDebounceTime = 0; 
const unsigned long debounceDelay = 50;

// 定时触发配置
bool autoCapture = false;  // 是否启用定时自动拍照
unsigned long autoCaptureInterval = 30000;  // 自动拍照间隔(毫秒) - 30秒
unsigned long lastAutoCaptureTime = 0;

// 果云ESP32-S3 CAM引脚定义
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM     4
#define SIOC_GPIO_NUM     5

#define Y9_GPIO_NUM       16
#define Y8_GPIO_NUM       17
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       12
#define Y5_GPIO_NUM       10
#define Y4_GPIO_NUM       8
#define Y3_GPIO_NUM       9
#define Y2_GPIO_NUM       11
#define VSYNC_GPIO_NUM    6
#define HREF_GPIO_NUM     7
#define PCLK_GPIO_NUM     13

#define LED_GPIO_NUM      2  // 板载LED

httpd_handle_t camera_httpd = NULL;

// 摄像头初始化
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
  config.grab_mode = CAMERA_GRAB_LATEST;  // 使用最新帧模式
  
  // 使用较小的配置避免内存问题
  config.frame_size = FRAMESIZE_VGA;    // 640x480
  config.jpeg_quality = 10;              // 质量10-12较好
  config.fb_count = 2;                   // 使用2个缓冲区
  config.fb_location = CAMERA_FB_IN_PSRAM;
  
  // 初始化摄像头
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("摄像头初始化失败，错误码: 0x%x\n", err);
    Serial.println("请检查：");
    Serial.println("1. Arduino IDE设置 Tools > PSRAM 必须启用");
    Serial.println("2. 摄像头连接是否正确");
    return;
  }
  
  Serial.println("摄像头初始化成功！");
  
  // 摄像头传感器设置
  sensor_t * s = esp_camera_sensor_get();
  if (s != NULL) {
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
    s->set_special_effect(s, 0);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, 0);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 0);
    s->set_gain_ctrl(s, 1);
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

// HTTP处理函数 - 拍照（修复版本）
static esp_err_t jpg_handler(httpd_req_t *req){
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  
  Serial.println("收到拍照请求");
  
  // 获取摄像头帧
  fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("获取图片失败");
    const char* error_msg = "Camera capture failed";
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, error_msg, strlen(error_msg));
    return ESP_FAIL;
  }
  
  Serial.printf("图片大小: %u bytes\n", fb->len);
  
  // 设置响应头
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  
  // 分块发送，避免内存问题
  size_t fb_len = fb->len;
  const size_t chunk_size = 4096;  // 使用4KB块大小
  size_t sent = 0;
  
  while (sent < fb_len) {
    size_t to_send = (fb_len - sent > chunk_size) ? chunk_size : (fb_len - sent);
    res = httpd_resp_send_chunk(req, (const char *)(fb->buf + sent), to_send);
    if (res != ESP_OK) {
      break;
    }
    sent += to_send;
    delay(1);  // 短暂延迟，让系统有时间处理其他任务
  }
  
  // 发送结束
  httpd_resp_send_chunk(req, NULL, 0);
  
  // 释放帧缓冲
  esp_camera_fb_return(fb);
  
  Serial.println("拍照完成");
  return res;
}

// HTTP处理函数 - 视频流（优化版本）
static esp_err_t stream_handler(httpd_req_t *req){
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  char part_buf[128];
  
  Serial.println("开始视频流");
  
  res = httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
  if(res != ESP_OK){
    return res;
  }
  
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "X-Framerate", "60");
  
  while(true){
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("获取帧失败");
      res = ESP_FAIL;
      break;
    }
    
    // 发送边界
    if(res == ESP_OK){
      res = httpd_resp_send_chunk(req, "--frame\r\n", 9);
    }
    
    // 发送头部
    if(res == ESP_OK){
      size_t hlen = snprintf(part_buf, 128, 
        "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", 
        fb->len);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    
    // 分块发送图片数据
    if(res == ESP_OK){
      size_t fb_len = fb->len;
      const size_t chunk_size = 8192;  // 流传输用更大的块
      size_t sent = 0;
      
      while (sent < fb_len && res == ESP_OK) {
        size_t to_send = (fb_len - sent > chunk_size) ? chunk_size : (fb_len - sent);
        res = httpd_resp_send_chunk(req, (const char *)(fb->buf + sent), to_send);
        sent += to_send;
        delay(1);  // 短暂延迟
      }
    }
    
    // 发送结束符
    if(res == ESP_OK){
      res = httpd_resp_send_chunk(req, "\r\n", 2);
    }
    
    // 释放帧缓冲
    esp_camera_fb_return(fb);
    fb = NULL;
    
    if(res != ESP_OK){
      Serial.println("流传输中断");
      break;
    }
  }
  
  Serial.println("视频流结束");
  return res;
}

// 网页界面
static esp_err_t index_handler(httpd_req_t *req){
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
    .btn-secondary { background: #2196F3; color: white; }
    .btn-warning { background: #FF9800; color: white; }
    .btn-danger { background: #f44336; color: white; }
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
      <!-- 左侧：实时视频流 -->
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
      
      <!-- 右侧：AI分析 -->
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
    
    <!-- AI分析结果 -->
    <div class="card">
      <h2>💬 分析结果</h2>
      <div id="ai-result">等待AI分析...</div>
    </div>
    
    <!-- 状态栏 -->
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
    
    // 美化AI输出文本：去除Markdown格式符号
    function beautifyAIText(text) {
      return text
        // 移除加粗标记 **text**
        .replace(/\*\*(.+?)\*\*/g, '$1')
        // 移除斜体标记 *text* 或 _text_
        .replace(/\*(.+?)\*/g, '$1')
        .replace(/_(.+?)_/g, '$1')
        // 移除列表标记 - 或 * 开头
        .replace(/^[\-\*]\s+/gm, '• ')
        // 移除数字列表的点号，保留数字
        .replace(/^(\d+)\.\s+/gm, '$1. ')
        // 移除代码块标记 ```
        .replace(/```[\s\S]*?```/g, (match) => match.replace(/```/g, ''))
        // 移除行内代码标记 `code`
        .replace(/`(.+?)`/g, '$1')
        // 移除标题标记 # 
        .replace(/^#+\s+/gm, '')
        // 清理多余的空行（保留最多2个连续换行）
        .replace(/\n{3,}/g, '\n\n')
        // 去除行首行尾空格
        .trim();
    }
    
    function aiAnalyze() {
      // 更新状态
      updateStatus('<span class="loading"></span>🤖 AI正在分析图像，请稍候...（预计10-30秒）', 'analyzing');
      aiResult.innerHTML = '⏳ AI分析中...\n\n步骤：\n📷 正在拍摄图片...\n🔄 正在编码为Base64...\n🌐 正在调用AI API...\n💬 等待AI响应...\n✅ 准备显示结果...';
      
      // 隐藏旧图片
      aiImage.classList.add('hidden');
      document.querySelector('#ai-image-container .placeholder').classList.remove('hidden');
      
      const startTime = Date.now();
      
      fetch('/ai_analyze')
        .then(response => response.json())
        .then(data => {
          const elapsed = ((Date.now() - startTime) / 1000).toFixed(1);
          
          if (data.success) {
            // 显示拍摄的图片
            document.querySelector('#ai-image-container .placeholder').classList.add('hidden');
            aiImage.src = 'data:image/jpeg;base64,' + data.image;
            aiImage.classList.remove('hidden');
            
            // 美化并显示AI分析结果
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
    
        // 临时：前端触发测试蜂鸣
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
    
    // 初始化提示
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
  doc["max_tokens"] = 500;
  
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
    DynamicJsonDocument responseDoc(8192);
    DeserializationError error = deserializeJson(responseDoc, payload);
    
    if (error) {
      Serial.printf("JSON解析失败: %s\n", error.c_str());
      Serial.println("原始响应:");
      Serial.println(payload.substring(0, 500));  // 只打印前500字符
      response = "JSON解析失败";
    } else {
      // OpenAI兼容格式响应解析 (适用于OpenAI和通义千问)
      if (responseDoc.containsKey("choices")) {
        response = responseDoc["choices"][0]["message"]["content"].as<String>();
        Serial.println("✓ API调用成功");
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

// ==================== I2S 音频初始化 ====================
void initI2S() {
  Serial.println("🔊 初始化I2S音频输出...");
  
  // I2S配置
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = AUDIO_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // MAX98357A单声道
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 1024,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  
  // I2S引脚配置
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK_PIN,
    .ws_io_num = I2S_LRC_PIN,
    .data_out_num = I2S_DOUT_PIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  
  // 安装I2S驱动
  esp_err_t err = i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("❌ I2S驱动安装失败: %d\n", err);
    return;
  }
  
  // 设置引脚
  err = i2s_set_pin(I2S_NUM, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("❌ I2S引脚设置失败: %d\n", err);
    return;
  }
  
  // 清空DMA缓冲区
  i2s_zero_dma_buffer(I2S_NUM);
  
  Serial.println("✓ I2S音频输出已初始化");
  Serial.printf("  引脚: BCLK=%d, LRC=%d, DOUT=%d\n", I2S_BCLK_PIN, I2S_LRC_PIN, I2S_DOUT_PIN);
  Serial.printf("  采样率: %d Hz\n", AUDIO_SAMPLE_RATE);
}

// 测试I2S输出 - 播放500Hz蜂鸣音
void testI2SBeep() {
  Serial.println("🔔 测试I2S输出 - 播放500Hz蜂鸣音(1秒)");
  
  const int freq = 500;  // 500Hz
  const int duration = 1000;  // 1秒
  const int samples = AUDIO_SAMPLE_RATE * duration / 1000;
  
  int16_t* audioData = (int16_t*)malloc(samples * sizeof(int16_t));
  if (!audioData) {
    Serial.println("❌ 内存分配失败");
    return;
  }
  
  // 生成正弦波
  for (int i = 0; i < samples; i++) {
    float t = (float)i / AUDIO_SAMPLE_RATE;
    audioData[i] = (int16_t)(sin(2.0 * PI * freq * t) * 30000);
  }
  
  // 播放
  size_t bytes_written;
  i2s_write(I2S_NUM, audioData, samples * sizeof(int16_t), &bytes_written, portMAX_DELAY);
  
  free(audioData);
  Serial.println("✓ 蜂鸣测试完成");
}

// ==================== TTS 语音合成与播放 ====================
// TTS API 配置（可选：阿里云、百度、腾讯云）
const char* TTS_TYPE = "aliyun";  // "aliyun" 或 "baidu" 或 "disabled"

// 阿里云 TTS 配置（推荐）
// AppKey/Token 请在 config_local.h / config_example.h 中提供（已在顶部包含）
const char* ALIYUN_TTS_ENDPOINT = "https://nls-gateway-cn-shanghai.aliyuncs.com/stream/v1/tts";

// 百度 TTS 配置（可选）- 请在 config_local.h / config_example.h 中提供（已在顶部包含）
const char* BAIDU_TTS_ENDPOINT = "https://tsn.baidu.com/text2audio";

// 直接播放阿里云 TTS PCM 流
void playPCMFromURL(const String& url) {
  Serial.println("🔊 开始播放音频 (PCM)...");

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("❌ TTS http.begin 失败");
    return;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("❌ TTS请求失败，HTTP代码: %d\n", httpCode);
    http.end();
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buffer[1024];
  size_t totalBytes = 0;
  unsigned long lastDataTime = millis();

  while (http.connected()) {
    int available = stream->available();
    if (available > 0) {
      if (available > (int)sizeof(buffer)) available = sizeof(buffer);
      int bytesRead = stream->readBytes((char*)buffer, available);
      if (bytesRead > 0) {
        size_t bytesWritten = 0;
        esp_err_t err = i2s_write(I2S_NUM, buffer, bytesRead, &bytesWritten, portMAX_DELAY);
        if (err != ESP_OK) {
          Serial.printf("❌ I2S写入失败: %d\n", err);
          break;
        }
        totalBytes += bytesWritten;
        lastDataTime = millis();
      }
    } else {
      if (!stream->connected()) break;
      if (millis() - lastDataTime > 3000) {
        Serial.println("⚠️  3秒未收到音频数据，结束播放");
        break;
      }
      delay(1);
    }
  }

  http.end();
  i2s_zero_dma_buffer(I2S_NUM);
  Serial.printf("✓ PCM 播放完成，总字节数: %u (约 %.2f 秒)\n", (unsigned int)totalBytes, totalBytes / 2.0 / AUDIO_SAMPLE_RATE);
}

// 调用在线 TTS API，将文本转换为语音并播放
void speakText(String text) {
  if (strcmp(TTS_TYPE, "disabled") == 0) {
    Serial.println("\n🔊 [语音输出] 功能已禁用");
    Serial.println("输出内容: " + text);
    return;
  }
  
  Serial.println("\n🔊 [步骤 5/5] 语音播报...");
  
  // 检查 WiFi 连接
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi未连接，无法使用在线TTS");
    return;
  }
  
  // 限制文本长度（避免超时）
  if (text.length() > 300) {
    text = text.substring(0, 300) + "...";
    Serial.println("⚠️  文本过长，已截取前300字符");
  }
  
  Serial.printf("📝 待播报文本: %s\n", text.c_str());
  
  // URL 编码文本
  String encodedText = urlEncode(text);
  
  // 构建阿里云TTS请求URL (PCM 16kHz)
  String ttsURL = String(ALIYUN_TTS_ENDPOINT) + 
                  "?appkey=" + ALIYUN_TTS_APPKEY +
                  "&text=" + encodedText +
                  "&format=pcm" +
                  "&sample_rate=16000" +
                  "&voice=xiaoyun";  // 可选: xiaoyun/xiaogang/ruoxi 等
  
  Serial.println("🎤 调用阿里云TTS...");
  
  // 直接播放PCM音频流
  playPCMFromURL(ttsURL);
}



// URL 编码函数
String urlEncode(String str) {
  String encoded = "";
  char c;
  char code0;
  char code1;
  
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (c == ' ') {
      encoded += '+';
    } else if (isalnum(c)) {
      encoded += c;
    } else {
      code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9) {
        code1 = (c & 0xf) - 10 + 'A';
      }
      c = (c >> 4) & 0xf;
      code0 = c + '0';
      if (c > 9) {
        code0 = c - 10 + 'A';
      }
      encoded += '%';
      encoded += code0;
      encoded += code1;
    }
  }
  return encoded;
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
  
  // 返回JSON响应
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  
  Serial.println("✅ [Web] 响应已发送");
  return httpd_resp_send(req, jsonResponse.c_str(), jsonResponse.length());
}

// 临时路由：蜂鸣测试
static esp_err_t beep_handler(httpd_req_t *req) {
  Serial.println("/beep 路由调用 - 播放测试蜂鸣");
  testI2SBeep();
  const char* ok_json = "{\"success\":true}";
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, ok_json, strlen(ok_json));
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
  Serial.println("\n[1/4] 初始化摄像头...");
  setupCamera();
  
  // 初始化I2S音频输出
  Serial.println("\n[2/4] 初始化I2S音频输出...");
  initI2S();
  
  // 可选：测试I2S输出
  // testI2SBeep();  // 取消注释以测试蜂鸣音
  
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
    
    // 启动HTTP服务器
    Serial.println("\n[4/4] 启动Web服务器...");
    startCameraServer();
    
    Serial.println("\n╔══════════════════════════════════════╗");
    Serial.println("║        系统启动完成！                ║");
    Serial.println("╚══════════════════════════════════════╝");
    Serial.print("🌐 Web界面: http://");
    Serial.println(WiFi.localIP());
    Serial.println("📷 功能: 视频流 | 拍照 | AI分析 | 🔊语音播报");
    Serial.println("🔘 按下Boot按钮触发AI分析");
    Serial.printf("🔊 音频输出: MAX98357A (引脚 BCLK=%d, LRC=%d, DIN=%d)\n", 
                  I2S_BCLK_PIN, I2S_LRC_PIN, I2S_DOUT_PIN);
    if (autoCapture) {
      Serial.printf("⏰ 定时拍照: 每%lu秒自动分析\n", autoCaptureInterval / 1000);
    }
    Serial.println("══════════════════════════════════════");
    Serial.println("\n💡 提示:");
    Serial.println("   1. 在代码中配置API密钥 (AI视觉 + TTS)");
    Serial.println("   2. 选择API类型 (qwen推荐)");
    Serial.println("   3. 通过Web界面或按钮触发AI分析");
    Serial.println("   4. AI结果会自动通过扬声器播报\n");
    
    digitalWrite(LED_GPIO_NUM, HIGH); // LED亮表示启动完成
  } else {
    Serial.println("\n✗ WiFi连接失败");
    Serial.println("请检查WiFi名称和密码");
  }
}

void loop() {
  // 检测按钮触发
  checkButtonTrigger();
  
  // 定时自动拍照分析
  if (autoCapture && WiFi.status() == WL_CONNECTED) {
    if (millis() - lastAutoCaptureTime > autoCaptureInterval) {
      Serial.println("\n⏰ 定时触发：自动拍照分析");
      performVisionAnalysis();
      lastAutoCaptureTime = millis();
    }
  }
  
  // 定期打印状态
  static unsigned long lastStatusTime = 0;
  if (millis() - lastStatusTime > 60000) {  // 每60秒
    if(WiFi.status() == WL_CONNECTED) {
      Serial.println("\n────────── 系统状态 ──────────");
      Serial.printf("⏱️  运行时间: %lu 秒 (%.1f 分钟)\n", 
                    millis() / 1000, millis() / 60000.0);
      Serial.printf("💾 堆内存: %d bytes (%.1f KB)\n", 
                    ESP.getFreeHeap(), ESP.getFreeHeap() / 1024.0);
      Serial.printf("💾 PSRAM: %d bytes (%.1f MB)\n", 
                    ESP.getFreePsram(), ESP.getFreePsram() / 1024.0 / 1024.0);
      Serial.printf("📶 WiFi信号: %d dBm\n", WiFi.RSSI());
      Serial.println("─────────────────────────────\n");
    }
    lastStatusTime = millis();
  }
  
  // 临时：串口命令支持，输入 'beep' 将触发 I2S 蜂鸣测试
  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.equalsIgnoreCase("beep")) {
      Serial.println("串口命令: beep -> 播放测试蜂鸣");
      testI2SBeep();
    }
  }

  delay(50);  // 短延迟，避免占用CPU
}
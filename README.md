# ESP32-S3 CAM AI视觉智能问答机

## 项目概述
基于果云ESP32-S3 CAM开发板的AI视觉智能问答系统，能够实时拍摄图像并通过视觉大模型分析图像内容，最终通过语音播报分析结果。

## 硬件平台
- **开发板**: 果云ESP32-S3 CAM
- **主控芯片**: ESP32-S3
- **摄像头**: OV2640/OV5640
- **PSRAM**: 必须启用（用于图像缓冲）
- **扬声器**: 预留接口（未来实现）

## 功能模块

### 1. 摄像头模块 (Camera Module)
**文件位置**: `AI_Answer.ino` - `setupCamera()`
**功能描述**:
- 初始化OV2640摄像头
- 配置图像参数: VGA分辨率(640x480), JPEG格式
- 使用PSRAM作为帧缓冲区
- 支持亮度、对比度、饱和度等参数调整

**引脚定义**:
```cpp
XCLK_GPIO_NUM:  15    // 时钟信号
SIOD_GPIO_NUM:  4     // I2C数据线
SIOC_GPIO_NUM:  5     // I2C时钟线
Y9-Y2_GPIO_NUM: 16,17,18,12,10,8,9,11  // 8位数据线
VSYNC_GPIO_NUM: 6     // 垂直同步
HREF_GPIO_NUM:  7     // 水平参考
PCLK_GPIO_NUM:  13    // 像素时钟
LED_GPIO_NUM:   2     // 板载LED
```

**关键参数**:
- `frame_size`: FRAMESIZE_VGA (640x480)
- `jpeg_quality`: 10-12 (数值越小质量越高)
- `fb_count`: 2 (双缓冲)
- `xclk_freq_hz`: 20MHz

### 2. WiFi网络模块 (WiFi Module)
**文件位置**: `AI_Answer.ino` - `setup()`
**功能描述**:
- 连接到指定WiFi网络
- 获取IP地址用于Web访问
- 监控WiFi连接状态和信号强度
- 禁用WiFi休眠以提高响应速度

**配置参数**:
```cpp
const char* ssid = "你的WiFi名称";
const char* password = "你的WiFi密码";
```

### 3. Web服务器模块 (Web Server Module)
**文件位置**: `AI_Answer.ino` - `startCameraServer()`
**功能描述**:
提供三个HTTP端点:

#### 3.1 主页面 `/`
- 提供可视化Web界面
- 响应式设计，支持移动设备
- 实时视频流预览
- 拍照和下载功能

#### 3.2 视频流端点 `/stream`
- 实现MJPEG视频流
- 使用multipart/x-mixed-replace协议
- 分块传输避免内存溢出
- 帧率约60fps（受网络和处理能力影响）

#### 3.3 拍照端点 `/capture`
- 单次图像捕获
- 返回JPEG格式图片
- 支持浏览器直接下载
- 分块传输大图片（4KB块）

### 4. Base64编码模块 (Base64 Encoder)
**文件位置**: `AI_Answer.ino` - `encodeBase64()`
**功能描述**:
- 将相机捕获的JPEG图像编码为Base64字符串
- 用于API调用时嵌入图片数据
- 优化内存使用，分块编码

**实现细节**:
- 输入: `camera_fb_t*` 帧缓冲指针
- 输出: `String` Base64编码字符串
- 使用mbedtls库的base64编码函数

### 5. 视觉大模型API模块 (Vision AI API)
**文件位置**: `AI_Answer.ino` - `callVisionAPI()`
**功能描述**:
- 调用多模态视觉大模型API
- 支持多种API提供商（OpenAI、通义千问等）
- 发送Base64编码图片
- 接收并解析JSON格式的AI回复

**支持的API**:
1. **OpenAI GPT-4 Vision API**
   - Endpoint: `https://api.openai.com/v1/chat/completions`
   - Model: `gpt-4-vision-preview`
   
2. **阿里云通义千问 Vision API**
   - Endpoint: `https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation`
   - Model: `qwen-vl-plus`

3. **其他兼容OpenAI格式的API**
   - 支持自定义endpoint和API密钥

**配置参数**:
```cpp
const char* API_KEY = "你的API密钥";
const char* API_ENDPOINT = "API地址";
const char* API_MODEL = "模型名称";
```

**请求格式** (OpenAI格式):
```json
{
  "model": "gpt-4-vision-preview",
  "messages": [
    {
      "role": "user",
      "content": [
        {"type": "text", "text": "请描述这张图片中的内容"},
        {"type": "image_url", "image_url": {"url": "data:image/jpeg;base64,..."}}
      ]
    }
  ],
  "max_tokens": 300
}
```

### 6. JSON解析模块 (JSON Parser)
**文件位置**: `AI_Answer.ino` - `parseVisionResponse()`
**功能描述**:
- 解析API返回的JSON响应
- 提取AI生成的图像描述文本
- 错误处理和状态码检查

**依赖库**: ArduinoJson

### 7. 串口输出模块 (Serial Output)
**文件位置**: `AI_Answer.ino` - `outputToSerial()`
**功能描述**:
- 将AI分析结果输出到串口监视器
- 用于调试和测试
- 显示完整的对话流程

**波特率**: 115200

### 8. 语音输出模块 (Audio Output)
**文件位置**: `AI_Answer.ino` - `speakText()` / `requestAndPlayBaiduTTS()`
**当前实现**:
- 优先使用百度 TTS（直接从设备请求或使用本地配置的 access token）。
- 支持 MP3 流在线播放，使用 I2S 输出和 `AudioGeneratorMP3` 解码。
- 已添加详细串口日志用于诊断（见“诊断与日志”部分）。

**如何配置百度 TTS（直接调用）**:
1. 在 `config_local.h` 中设置以下项（用于设备端获取 token）:
```cpp
#define BAIDU_API_KEY "你的百度API Key"
#define BAIDU_SECRET_KEY "你的百度Secret Key"
```
2. 可选：为了快速测试，可以直接在 `config_local.h` 中设置一个临时的 `BAIDU_TTS_ACCESS_TOKEN`：
```cpp
#define BAIDU_TTS_ACCESS_TOKEN "临时token"
```
3. 生产环境推荐留空 `BAIDU_TTS_ACCESS_TOKEN`，设备会在运行时通过 `openapi.baidu.com/oauth/2.0/token` 获取并缓存 token。

**注意**: 设备需能够访问 `openapi.baidu.com` (HTTPS，用于 token)，以及 `tsn.baidu.com` (HTTP，用于拉取 MP3 流)。如果网络受限，请参阅“代理/备用方案”。

### 9. 触发控制模块 (Trigger Control)
**文件位置**: `AI_Answer.ino` - `checkTrigger()`
**功能描述**:
- 检测触发条件（按钮/定时）
- 执行完整的拍照→分析→输出流程
- 防抖处理

**触发方式**:
1. **按钮触发**: 连接物理按钮到GPIO
2. **定时触发**: 按设定间隔自动拍照
3. **Web触发**: 通过Web界面手动触发
4. **串口触发**: 通过串口命令触发

## 系统工作流程

```
[开机启动]
    ↓
[初始化摄像头] → [连接WiFi] → [启动Web服务器]
    ↓
[等待触发]
    ↓
[捕获图像] ← 按钮/定时器/Web触发
    ↓
[Base64编码]
    ↓
[调用视觉AI API]
    ↓
[解析JSON响应]
    ↓
[串口输出结果] → [语音播报(预留)]
    ↓
[返回等待触发]
```

## 内存使用优化

### 内存分配策略
- **PSRAM**: 用于图像帧缓冲（主要）
- **内部RAM**: 用于程序代码和变量
- **堆内存**: 用于动态分配（JSON、HTTP缓冲）

### 优化措施
1. 使用分块传输，避免一次性加载大文件
2. 及时释放camera frame buffer
3. 限制JSON文档大小
4. 使用String时注意内存碎片
5. 定期监控堆内存和PSRAM使用量

### 内存监控
```cpp
Serial.printf("堆内存: %d bytes\n", ESP.getFreeHeap());
Serial.printf("PSRAM: %d bytes\n", ESP.getFreePsram());
```

## 依赖库

### Arduino IDE库管理器安装
1. **ESP32 Board Support**: v2.0.11 或更高
   - 工具 → 开发板 → 开发板管理器 → 搜索"ESP32"
   
2. **ArduinoJson**: v6.21.0 或更高
   - 工具 → 管理库 → 搜索"ArduinoJson"

### ESP32核心库（已包含）
- `esp_camera.h`: 摄像头驱动
- `WiFi.h`: WiFi功能
- `esp_http_server.h`: HTTP服务器
- `HTTPClient.h`: HTTP客户端
- `base64.h`: Base64编码
- `ArduinoJson.h`: JSON解析

## Arduino IDE配置

### 开发板设置
```
工具菜单配置:
- 开发板: "ESP32S3 Dev Module"
- Upload Speed: 921600
- USB CDC On Boot: "Enabled"
- CPU Frequency: 240MHz
- Flash Mode: "QIO 80MHz"
- Flash Size: "8MB (64Mb)"
- Partition Scheme: "8M with spiffs (3MB APP/1.5MB SPIFFS)"
- PSRAM: "OPI PSRAM" ⚠️ 必须启用！
- Arduino Runs On: "Core 1"
- Events Run On: "Core 1"
```

### 编译选项
- **优化级别**: 默认 (-Os)
- **调试级别**: None (生产) / Info (调试)

## API配置说明

### OpenAI GPT-4 Vision
```cpp
// API配置
const char* API_TYPE = "openai";
const char* OPENAI_API_KEY = "sk-xxxxxxxxxxxxxxxx";
const char* OPENAI_ENDPOINT = "https://api.openai.com/v1/chat/completions";
const char* OPENAI_MODEL = "gpt-4-vision-preview";

// 提示词配置
const char* VISION_PROMPT = "请详细描述这张图片中的内容，包括物体、场景、颜色和动作。";
```

### 阿里云通义千问
```cpp
// API配置
const char* API_TYPE = "qwen";
const char* QWEN_API_KEY = "sk-xxxxxxxxxxxxxxxx";
const char* QWEN_ENDPOINT = "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation";
const char* QWEN_MODEL = "qwen-vl-plus";
```

### 自定义API
支持任何兼容OpenAI格式的API端点，包括本地部署的模型。

## 使用方法

### 1. 基本使用
1. 上传代码到ESP32-S3 CAM
2. 打开串口监视器 (115200波特率)
3. 等待WiFi连接成功
4. 在浏览器访问显示的IP地址
5. 点击"拍照"或"开始视频流"

### 2. AI视觉分析
- **Web触发**: 在Web界面点击"AI分析"按钮
- **按钮触发**: 按下连接到GPIO的物理按钮
- **定时触发**: 在代码中设置定时间隔
- **串口触发**: 发送命令 `ANALYZE` 到串口

### 3. 查看结果
- **串口输出**: 打开串口监视器查看AI返回的文字描述
- **语音播报**: (未来) 通过扬声器播放

## 故障排除

### 常见问题

#### 1. 摄像头初始化失败
**症状**: 串口显示 "摄像头初始化失败"
**解决方案**:
- 检查PSRAM是否启用 (工具 → PSRAM → OPI PSRAM)
- 检查摄像头排线连接是否正确
- 尝试降低图像质量: `config.jpeg_quality = 12;`
- 尝试降低分辨率: `config.frame_size = FRAMESIZE_SVGA;`

#### 2. WiFi连接失败
**症状**: 持续显示 "连接WiFi..."
**解决方案**:
- 检查WiFi名称和密码是否正确
- 确认WiFi是2.4GHz频段（ESP32不支持5GHz）
- 检查路由器是否开启MAC过滤
- 尝试使用手机热点测试

#### 3. 内存不足
**症状**: 程序运行一段时间后崩溃重启
**解决方案**:
- 确认PSRAM已启用
- 降低图像质量和分辨率
- 检查代码中是否有内存泄漏
- 使用 `ESP.getFreeHeap()` 监控内存

#### 4. API调用失败
**症状**: 串口显示 "API调用失败" 或超时
**解决方案**:
- 检查API密钥是否正确
- 确认网络连接正常
- 检查API额度是否用完
- 尝试使用其他API端点
- 增加HTTP超时时间

### TTS/音频 诊断与常见问题

- 在串口输出中查找关键日志关键词：
  - "[Baidu] 获取 access_token"：表明设备尝试请求百度 token
  - "access_token (已掩码)"：成功获取并缓存 token（日志会以掩码形式显示）
  - "开始拉取音频流" / "HTTPS 流已打开" / "HTTP 流已打开"：表明音频流已建立
  - "mp3->loop() 返回 false" 或 "MP3解码器初始化失败"：解码/播放问题，通常与内存或损坏的MP3流有关
  - "🔍 [TTS] 进行 HTTPS 连接诊断..."：当 HTTPS 连接失败时，诊断工具会打印 DNS、TCP 测试与 HTTP 响应片段

- 常见原因与解决：
  - 如果看到 TCP 连接失败（诊断输出为 TCP 连接失败），说明网络或防火墙阻止了设备访问目标主机；请检查路由器或企业网络策略。
  - 如果 token 请求返回非200并带有错误信息，请确认 `BAIDU_API_KEY` 与 `BAIDU_SECRET_KEY` 是否正确，并检查百度开放平台是否有调用限制。
  - 若 MP3 解码报错且设备内存很低，尝试降低 JPEG/分辨率设置或启用 PSRAM（必须启用）。

### 代理/备用方案（可选）

如果设备无法直接访问百度或其他 TTS 提供商，可以在局域网或 VPS 上运行一个轻量代理（示例仓库中提供 `baidu_tts_proxy.py`）。代理的作用：
- 由代理负责获取并缓存百度 token（避免在设备上处理 HTTPS/TLS 或频繁刷新 token）。
- 代理从百度拉取音频并以普通 HTTP 返回给设备，降低设备的 TLS 需求。

代理仅作为应急方案。当前实现优先直接在设备上请求百度（更安全）。

#### 5. 图片过大无法编码
**症状**: Base64编码失败或内存不足
**解决方案**:
- 降低JPEG质量: `config.jpeg_quality = 15;`
- 降低分辨率: `FRAMESIZE_SVGA` 或 `FRAMESIZE_VGA`
- 使用压缩后的图片

## 性能指标

### 典型参数
- **图像分辨率**: 640x480 (VGA)
- **图像大小**: 20-60KB (取决于场景复杂度)
- **Base64编码时间**: ~100-300ms
- **API调用时间**: 2-10秒 (取决于网络和API响应)
- **总处理时间**: 3-12秒 (拍照到结果输出)
- **内存占用**: 
  - 空闲堆内存: ~150KB
  - 空闲PSRAM: ~7.5MB

### 优化建议
- 使用较低分辨率可提高速度
- 选择响应快的API端点
- 使用本地WiFi而非移动热点
- 预热API连接（首次调用较慢）

## 扩展功能建议

### 短期扩展
1. ✅ 添加物理按钮触发
2. ✅ 实现定时自动拍照
3. ✅ 支持多种API切换
4. ⏳ 添加本地图像预处理
5. ⏳ 实现简单的语音播报

### 长期扩展
1. 🔲 集成本地AI模型（TensorFlow Lite）
2. 🔲 添加物体识别和跟踪
3. 🔲 实现实时视频分析
4. 🔲 添加人脸识别功能
5. 🔲 支持语音唤醒和语音指令
6. 🔲 添加LCD屏幕显示结果
7. 🔲 实现边缘计算和云端协同

## 版本历史

### v1.0.0 (2025-10-07)
- ✅ 实现基础摄像头功能
- ✅ 实现WiFi连接
- ✅ 实现Web视频流和拍照
- ✅ 添加响应式Web界面

### v2.0.0 (2025-10-07)
- ✅ 添加Base64图像编码
- ✅ 集成视觉大模型API

### v2.1.0 (2025-10-19)
- ✅ 切换为优先直接调用百度TTS（支持设备端获取 token 或使用 config 中的临时 token）
- ✅ 增强 TTS 与 token 相关的串口诊断日志（掩码显示 token、打印响应片段与内存信息）
- ✅ 添加 HTTPS 连接诊断工具 `httpsDiagnostic` 用于快速定位 TLS/TCP 问题
- ✅ 实现串口调试输出
- ✅ 添加触发控制逻辑
- ⏳ 预留语音输出接口

### 未来版本
- 🔲 v2.1: 完整实现语音输出
- 🔲 v2.2: 添加本地图像处理
- 🔲 v3.0: 集成边缘AI模型

## 引脚使用总结

| 功能 | GPIO | 说明 |
|------|------|------|
| 摄像头XCLK | 15 | 时钟信号 |
| 摄像头SIOD | 4 | I2C数据 |
| 摄像头SIOC | 5 | I2C时钟 |
| 摄像头数据线 | 16,17,18,12,10,8,9,11 | 8位并行数据 |
| 摄像头VSYNC | 6 | 垂直同步 |
| 摄像头HREF | 7 | 水平参考 |
| 摄像头PCLK | 13 | 像素时钟 |
| 板载LED | 2 | 状态指示 |
| 按钮(预留) | 0 | 触发拍照 |
| I2S音频(预留) | TBD | 扬声器输出 |

## 技术支持

### 开发环境
- Arduino IDE 2.x
- ESP32 Board Package 2.0.11+
- PlatformIO (可选)

### 参考资源
- [ESP32-S3技术手册](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_cn.pdf)
- [ESP32 Camera驱动文档](https://github.com/espressif/esp32-camera)
- [OpenAI Vision API文档](https://platform.openai.com/docs/guides/vision)
- [阿里云通义千问API文档](https://help.aliyun.com/zh/dashscope/)

### 联系方式
- 项目仓库: (待添加)
- 问题反馈: (待添加)

## 许可证
本项目采用 MIT 许可证

---

**注意事项**:
1. 使用前请确保API密钥已正确配置
2. 注意保护API密钥，不要上传到公共仓库
3. 注意API调用费用
4. PSRAM必须启用，否则无法运行
5. 建议使用稳定的WiFi网络

**最后更新**: 2025-10-07

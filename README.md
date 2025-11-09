# ESP32-S3 CAM AI视觉智能问答机

> ✅ **2025-01-09 更新**: I2S驱动冲突问题已解决！麦克风功能已使用新I2S API重写，现可与TTS播放完美共存。

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

### 2. 麦克风模块 (Microphone Module) ✅ **已修复**
**文件位置**: `AI_Answer.ino` - `setupMicrophone()` / `recordAudio()`
**状态**: ✅ **正常工作** - 已使用ESP32-S3新版I2S标准模式驱动重写

**技术升级**:
- ✅ 使用新I2S API (`driver/i2s_std.h`)替代旧版API
- ✅ 与ESP8266Audio库的TTS播放功能完全兼容，无冲突
- ✅ 采用按需初始化/释放策略，优化资源使用

**功能描述**:
- 初始化I2S麦克风输入
- 录音音频数据到内存缓冲区
- 支持16位单声道16kHz采样率
- 支持语音唤醒和语音识别功能

**引脚定义**:
```cpp
MIC_I2S_BCLK_PIN: 47  // I2S位时钟
MIC_I2S_LRC_PIN:  45  // I2S左右时钟
MIC_I2S_DIN_PIN:  48  // I2S数据输入
```

**关键参数**:
- `sample_rate`: 16000 Hz
- `bits_per_sample`: 16位
- `channels`: 1 (单声道)
- `buffer_size`: 8192字节

### 3. 语音识别模块 (Speech Recognition) ✅
**文件位置**: `AI_Answer.ino` - `recognizeSpeech()`
**状态**: ✅ 完全可用 (默认阿里云)
**功能描述**:
- 调用阿里云短语音识别API (NLS Gateway)
- 自动申请并缓存阿里云临时Token
- 上传PCM音频并解析识别结果
- 识别失败时返回空结果并打印错误

**API配置**:
```cpp
#define BAIDU_ASR_API_KEY "你的百度ASR API Key"
#define BAIDU_ASR_SECRET_KEY "你的百度ASR Secret Key"
#define BAIDU_ASR_APP_ID "你的百度ASR App ID"
```

**支持格式**:
- 音频格式: PCM
- 采样率: 16kHz
- 位深度: 16位
- 声道: 单声道

### 4. 提示词生成模块 (Prompt Generation)
**文件位置**: `AI_Answer.ino` - `generatePromptFromSpeech()`
**功能描述**:
- 将语音识别结果转换为视觉AI提示词
- 直接使用用户的语音命令作为AI分析的指导

**示例**:
输入语音: "描述一下这张图片中的人"
输出提示词: "用户命令：描述一下这张图片中的人\n\n请根据用户的命令分析当前图片,并用中文详细回答。"

### 5. 语音唤醒模块 (Voice Wake-up) ✅
**文件位置**: `AI_Answer.ino` - `listenForWakeWord()`, `performVoiceWakeFlow()`
**状态**: ✅ 完全可用
**功能描述**:
- 持续监听预设的唤醒词
- 检测到唤醒词后自动进入语音分析流程
- 支持自定义唤醒词配置

**唤醒词配置**:
在 `config_local.h` 中设置:
```cpp
static const char* CUSTOM_WAKE_WORD = "你好小智";  // 可修改为任意短语
```

**关键参数**:
```cpp
WAKE_LISTEN_SECONDS: 2        // 每次监听时长(秒)
WAKE_TIMEOUT_MS: 45000        // 唤醒超时时间(毫秒)
VOICE_COMMAND_SECONDS: 5      // 命令录音时长(秒)
```

**工作流程**:
1. **进入唤醒模式**: 长按语音按钮(>1.5秒)或Web触发
2. **循环监听**: 每2秒录音一次,识别是否包含唤醒词
3. **检测成功**: 播放提示音,提示用户说出指令
4. **录音指令**: 录音5秒捕获用户的语音命令
5. **执行分析**: 自动进入语音分析流程

### 6. 语音分析工作流程 (Voice Analysis Workflow)
**文件位置**: `AI_Answer.ino` - `performVoiceAnalysis()`
**功能描述**:
- 完整语音输入到输出的工作流程
- 录音 → 语音识别 → 提示词生成 → 视觉AI分析 → TTS输出

**工作流程**:
1. **录音阶段**: 录音5秒音频数据到缓冲区
2. **识别阶段**: 调用百度ASR API识别语音内容
3. **生成阶段**: 将识别结果转换为视觉AI提示词
4. **分析阶段**: 调用视觉AI模型分析当前摄像头图像
5. **输出阶段**: 通过TTS将分析结果转换为语音输出

### 6. TTS语音合成模块 (Text-to-Speech)
**文件位置**: `AI_Answer.ino` - `performTTS()`
**功能描述**:
- 调用百度TTS API生成语音
- 将文本转换为音频流并通过I2S扬声器播放

**优化特性**:
- **分块处理**: 长文本自动分割为小块，避免爆鸣
- **增益控制**: 动态调整音量防止失真
- **生命周期管理**: 正确初始化和清理I2S资源

**引脚定义**:
```cpp
SPEAKER_I2S_BCLK_PIN: 41  // I2S位时钟
SPEAKER_I2S_LRC_PIN:  42  // I2S左右时钟
SPEAKER_I2S_DOUT_PIN: 40  // I2S数据输出
```

## 控制接口 (Control Interfaces)

### 1. 物理按钮控制 (Physical Button Control)
**文件位置**: `AI_Answer.ino` - `checkButtonTrigger()`
**功能描述**:
- 检测GPIO按钮按下事件
- 支持视觉分析和语音分析两种模式

**按钮配置**:
```cpp
TRIGGER_BUTTON_PIN: 0   // 视觉分析按钮
VOICE_BUTTON_PIN:  1   // 语音分析按钮
```

**触发逻辑**:
- **短按(<1.5秒)语音按钮**: 直接执行语音分析流程
- **长按(>1.5秒)语音按钮**: 进入语音唤醒模式
- **按视觉分析按钮**: 执行视觉分析流程
- 支持防抖处理避免误触发

### 2. Web界面控制 (Web Interface Control)
**文件位置**: `AI_Answer.ino` - Web服务器处理函数
**功能描述**:
- 提供HTTP Web界面进行远程控制
- 支持视觉分析和语音分析两种模式

**API端点**:
- `GET /`: 主页，显示控制界面
- `GET /analyze`: 执行视觉分析
- `POST /ai_analyze`: 执行AI视觉分析并返回JSON结果
- `POST /voice_analyze`: 执行语音分析流程
- `POST /voice_wake`: 执行语音唤醒流程

**界面特性**:
- 响应式设计，适配移动设备
- 实时状态显示
- 按钮点击触发分析流程

## 系统工作流程

### 视觉分析流程
```
[开机启动]
    ↓
[初始化摄像头] → [初始化麦克风] → [连接WiFi] → [启动Web服务器]
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
[串口输出结果] → [语音播报]
    ↓
[返回等待触发]
```

### 语音分析流程
```
[语音触发]
    ↓
[录音5秒音频] ← 按钮短按/Web触发
    ↓
[调用百度ASR API]
    ↓
[解析语音识别结果]
    ↓
[生成视觉AI提示词]
    ↓
[捕获当前图像]
    ↓
[调用视觉AI API]
    ↓
[解析分析结果]
    ↓
[语音播报结果]
    ↓
[返回等待触发]
```

### 语音唤醒流程
```
[语音唤醒触发]
    ↓
[进入唤醒模式] ← 按钮长按/Web触发
    ↓
[循环监听唤醒词] ← 每2秒录音识别
    ↓
[检测到唤醒词?] → 否 → [继续监听直到超时]
    ↓ 是
[播放提示音]
    ↓
[提示用户说出指令]
    ↓
[录音5秒捕获命令]
    ↓
[执行语音分析流程]
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

### 3. AI语音分析
- **Web触发**: 在Web界面点击"语音分析"按钮
- **按钮触发**: 短按连接到GPIO1的语音按钮(<1.5秒)
- **工作流程**: 录音5秒 → 语音识别 → 生成提示词 → 视觉分析 → 语音输出

### 4. AI语音唤醒
- **Web触发**: 在Web界面点击"语音唤醒"按钮
- **按钮触发**: 长按连接到GPIO1的语音按钮(>1.5秒)
- **工作流程**: 
  1. 进入唤醒模式,循环监听"你好小智"(可自定义)
  2. 检测到唤醒词后播放提示音
  3. 录音5秒捕获用户指令
  4. 自动执行语音分析流程

**唤醒词配置**:
在 `config_local.h` 中修改:
```cpp
static const char* CUSTOM_WAKE_WORD = "你好小智";  // 改为你想要的唤醒词
```

### 5. 查看结果
- **串口输出**: 打开串口监视器查看AI返回的文字描述
- **语音播报**: 通过扬声器播放分析结果

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
4. ✅ 实现语音唤醒功能
5. ✅ 支持自定义唤醒词
6. ⏳ 添加本地图像预处理
7. ⏳ 实现更多TTS提供商支持

### 长期扩展
1. 🔲 集成本地AI模型（TensorFlow Lite）
2. 🔲 添加物体识别和跟踪
3. 🔲 实现实时视频分析
4. 🔲 添加人脸识别功能
5. 🔲 支持多唤醒词和自然对话
6. 🔲 添加LCD屏幕显示结果
7. 🔲 实现边缘计算和云端协同
8. 🔲 支持离线语音识别

## 版本历史

### v1.0.0 (2025-10-07)
- ✅ 实现基础摄像头功能
- ✅ 实现WiFi连接
- ✅ 实现Web视频流和拍照
- ✅ 添加响应式Web界面

### v2.0.0 (2025-10-07)
- ✅ 添加Base64图像编码
- ✅ 集成视觉大模型API

### v2.3.0 (2025-11-06)
- ✅ 添加语音唤醒功能
- ✅ 支持自定义唤醒词配置
- ✅ 实现长按/短按按钮区分不同功能
- ✅ 添加语音唤醒Web界面控制
- ✅ 优化提示词生成逻辑(直接使用用户命令)
- ✅ 修复Base64编码函数调用错误
- ✅ 更新文档添加语音唤醒说明

### v2.2.0 (2025-01-XX)
- ✅ 添加麦克风音频输入模块
- ✅ 集成百度语音识别API
- ✅ 实现语音到提示词转换
- ✅ 添加语音分析工作流程
- ✅ 支持物理按钮和Web界面语音触发
- ✅ 更新README文档

### v2.1.0 (2025-10-19)
- ✅ 切换为优先直接调用百度TTS（支持设备端获取 token 或使用 config 中的临时 token）
- ✅ 增强 TTS 与 token 相关的串口诊断日志（掩码显示 token、打印响应片段与内存信息）
- ✅ 添加 HTTPS 连接诊断工具 `httpsDiagnostic` 用于快速定位 TLS/TCP 问题
- ✅ 实现串口调试输出
- ✅ 添加触发控制逻辑
- ⏳ 预留语音输出接口

### 未来版本
- 🔲 v2.4: 优化语音识别准确率
- 🔲 v2.5: 支持阿里云ASR实现
- 🔲 v2.6: 添加多唤醒词支持
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
| 麦克风BCLK | 47 | I2S位时钟 |
| 麦克风LRC | 45 | I2S左右时钟 |
| 麦克风DIN | 48 | I2S数据输入 |
| 扬声器BCLK | 41 | I2S位时钟 |
| 扬声器LRC | 42 | I2S左右时钟 |
| 扬声器DOUT | 40 | I2S数据输出 |
| 板载LED | 2 | 状态指示 |
| 视觉分析按钮 | 0 | 触发视觉分析 |
| 语音分析按钮 | 1 | 触发语音分析 |

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

**最后更新**: 2025-11-06

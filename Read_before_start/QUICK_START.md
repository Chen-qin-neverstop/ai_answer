# 快速开始指南 - ESP32-S3 CAM AI视觉智能问答机

## 📋 准备工作

### 硬件要求
- ✅ 果云 ESP32-S3 CAM 开发板
- ✅ USB数据线（Type-C）
- ✅ 电脑（Windows/Mac/Linux）
- ⏳ 扬声器模块（可选，未来版本）

### 软件要求
- ✅ Arduino IDE 2.x 或更高版本
- ✅ ESP32开发板支持包 v2.0.11+
- ✅ ArduinoJson 库 v6.21.0+

### API准备（选择其一）
- 🔑 OpenAI API密钥（GPT-4 Vision）
- 🔑 阿里云通义千问API密钥
- 🔑 其他兼容OpenAI格式的API

---

## 🚀 5分钟快速部署

### 步骤1：安装Arduino IDE和依赖

1. **下载Arduino IDE**
   - 访问 https://www.arduino.cc/en/software
   - 下载并安装最新版本

2. **安装ESP32开发板支持**
   ```
   Arduino IDE → 文件 → 首选项
   附加开发板管理器网址: 
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   
   工具 → 开发板 → 开发板管理器 → 搜索"ESP32" → 安装
   ```

3. **安装ArduinoJson库**
   ```
   工具 → 管理库 → 搜索"ArduinoJson" → 安装 (by Benoit Blanchon)
   ```

### 步骤2：配置开发板参数

```
工具菜单设置：
├─ 开发板: "ESP32S3 Dev Module"
├─ USB CDC On Boot: "Enabled"
├─ CPU Frequency: "240MHz"
```
├─ Flash Mode: "QIO 80MHz"  
├─ Flash Size: "8MB (64Mb)"
├─ Partition Scheme: "8M with spiffs (3MB APP/1.5MB SPIFFS)"
├─ PSRAM: "OPI PSRAM" ⚠️ 必须启用！
├─ Upload Speed: "921600"
└─ Port: 选择对应的COM口
```

### 步骤3：修改代码配置

打开 `AI_Answer.ino`，修改以下配置：

#### 3.1 WiFi配置（第11-12行）
```cpp
const char* ssid = "你的WiFi名称";
const char* password = "你的WiFi密码";
```

#### 3.2 选择API类型（第16行）
```cpp
// 选择一个：
const char* API_TYPE = "openai";  // OpenAI GPT-4 Vision
// const char* API_TYPE = "qwen";  // 阿里云通义千问
// const char* API_TYPE = "custom"; // 自定义API
```

#### 3.3 配置API密钥

**使用OpenAI（第19-21行）：**
```cpp
const char* OPENAI_API_KEY = "sk-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
const char* OPENAI_ENDPOINT = "https://api.openai.com/v1/chat/completions";
const char* OPENAI_MODEL = "gpt-4-vision-preview";
```

**或使用通义千问（第24-26行）：**
```cpp
const char* QWEN_API_KEY = "sk-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
const char* QWEN_ENDPOINT = "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation";
const char* QWEN_MODEL = "qwen-vl-plus";
```

**或使用自定义API（第29-31行）：**
```cpp
const char* CUSTOM_API_KEY = "your-api-key";
const char* CUSTOM_ENDPOINT = "https://your-endpoint/v1/chat/completions";
const char* CUSTOM_MODEL = "your-model";
```

#### 3.4 可选：修改提示词（第34行）
```cpp
const char* VISION_PROMPT = "请详细描述这张图片中的内容，包括物体、场景、颜色等细节。用中文回答。";
```

#### 3.5 可选：配置语音唤醒词（第150行左右）
在 `config_local.h` 或 `config_example.h` 中设置:
```cpp
static const char* CUSTOM_WAKE_WORD = "你好小智";  // 改为你想要的唤醒词
```

也可以在 `AI_Answer.ino` 中调整唤醒参数:
```cpp
#define WAKE_LISTEN_SECONDS 2       // 每次监听时长(秒)
#define WAKE_TIMEOUT_MS (45000UL)   // 唤醒超时(毫秒)
#define VOICE_COMMAND_SECONDS 5     // 命令录音时长(秒)
```

#### 3.6 可选：启用定时拍照（第42-44行）
```cpp
bool autoCapture = true;  // 改为true启用
unsigned long autoCaptureInterval = 30000;  // 间隔30秒
```

### 步骤4：上传代码

1. 连接ESP32-S3 CAM到电脑USB口
2. 按住板子上的**BOOT按钮**，同时按一下**RESET按钮**，进入下载模式
3. 点击Arduino IDE的"上传"按钮
4. 等待编译和上传完成（约1-2分钟）

### 步骤5：测试运行

1. **打开串口监视器**
   ```
   工具 → 串口监视器
   波特率设为: 115200
   ```

2. **查看启动信息**
   - 看到"系统启动完成！"表示成功
   - 记下显示的IP地址，如：`192.168.1.100`

3. **访问Web界面**
   - 在浏览器输入：`http://192.168.1.100`
   - 你会看到实时视频和控制按钮

---

## 🎯 使用方法

### 方法1：Web界面触发（推荐）

**视觉分析**:
1. 在浏览器打开Web界面
2. 点击"🤖 AI分析"按钮
3. 等待10-30秒（取决于网络速度）
4. 在串口监视器查看AI分析结果

**语音分析**:
1. 点击"🎤 语音分析"按钮
2. 系统录音5秒,请对着麦克风说话
3. 等待语音识别和AI分析
4. 结果通过扬声器播报

**语音唤醒**:
1. 点击"🛎️ 语音唤醒"按钮
2. 对着麦克风说出唤醒词(默认"你好小智")
3. 听到提示音后说出你的指令
4. 系统自动完成分析并播报结果

### 方法2：按钮触发

**视觉分析**:
1. 按下开发板上的**BOOT按钮**（GPIO0）
2. 自动开始拍照和AI分析
3. 在串口监视器查看结果

**语音分析**:
1. **短按**GPIO1按钮（<1.5秒）
2. 系统录音5秒并自动分析
3. 查看串口输出和扬声器播报

**语音唤醒**:
1. **长按**GPIO1按钮（>1.5秒）
2. 进入唤醒模式,说出"你好小智"
3. 听到提示音后说出指令
4. 自动完成分析

### 方法3：定时自动触发

1. 在代码中设置 `autoCapture = true;`
2. 设置间隔时间 `autoCaptureInterval = 30000;` (30秒)
3. 重新上传代码
4. 系统将每30秒自动分析一次

### 方法4：串口命令触发（高级）

在串口监视器发送命令（需要修改代码添加串口命令解析）：
```
ANALYZE    - 执行一次AI分析
STATUS     - 显示系统状态
CONFIG     - 显示配置信息
```

---

## 📊 预期输出示例

### 串口监视器输出：
```
****************************************
*     开始执行视觉分析流程             *
****************************************

📷 [步骤 1/5] 拍摄图片...
✓ 拍照成功，图片大小: 45678 bytes (44.6 KB)

🔄 [步骤 2/5] 编码图片为Base64...
✓ 编码成功，耗时: 234 ms
  Base64长度: 61024 字符

🤖 [步骤 3/5] 调用视觉AI API...
  可用堆内存: 156789 bytes
  可用PSRAM: 7864320 bytes

========== 调用视觉AI API ==========
API类型: openai
端点: https://api.openai.com/v1/chat/completions
模型: gpt-4-vision-preview
请求体大小: 61234 bytes
发送请求中...
HTTP响应码: 200
响应长度: 523 bytes
✓ API调用成功
====================================

⏱️  API调用耗时: 8523 ms (8.5 秒)

📝 [步骤 4/5] 串口输出AI分析结果...

╔════════════════════════════════════╗
║      AI 视觉分析结果               ║
╚════════════════════════════════════╝
这张图片展示了一个现代化的办公桌场景。桌面上
放置着一台银色的笔记本电脑，屏幕正在显示代码
编辑器。电脑旁边有一个白色的咖啡杯，杯中装着
热气腾腾的咖啡。桌面背景是浅木色，光线充足，
营造出专业而舒适的工作氛围。
══════════════════════════════════════

🔊 [步骤 5/5] 语音播报（预留功能）...

[语音输出] (当前为预留接口，未实现)
计划输出内容: 这张图片展示了...

****************************************
*  流程完成！总耗时: 9876 ms (9.9 秒) *
****************************************
```

---

## 🐛 常见问题排查

### ❌ 问题1：编译错误 - "ArduinoJson.h not found"
**解决**：
```
工具 → 管理库 → 搜索"ArduinoJson" → 安装
重启Arduino IDE
```

### ❌ 问题2：上传失败 - "Failed to connect"
**解决**：
1. 检查USB线是否连接良好
2. 按住BOOT按钮，同时按RESET按钮
3. 释放RESET，再释放BOOT
4. 立即点击上传

### ❌ 问题3：串口显示"摄像头初始化失败"
**解决**：
1. **最重要**：检查 `工具 → PSRAM` 是否设为 "OPI PSRAM"
2. 检查摄像头排线是否插好
3. 尝试降低分辨率（改为FRAMESIZE_SVGA）
4. 重新给板子上电

### ❌ 问题4：WiFi连接失败
**解决**：
1. 确认WiFi名称和密码正确
2. 确认是2.4GHz频段（不支持5GHz）
3. 尝试靠近路由器
4. 检查路由器是否限制了设备连接

### ❌ 问题5：API调用失败 - "API密钥未配置"
**解决**：
1. 检查是否修改了API_KEY（不能包含"your-"字样）
2. 确认API_KEY长度至少10个字符
3. 检查API_KEY格式是否正确

### ❌ 问题6：API调用失败 - HTTP错误码401
**解决**：
1. API密钥错误，检查是否复制完整
2. API密钥已过期，重新生成
3. OpenAI账户余额不足

### ❌ 问题7：API调用失败 - HTTP错误码429
**解决**：
1. API调用频率超限，等待后重试
2. 检查API套餐限制
3. 增加请求间隔时间

### ❌ 问题8：内存不足重启
**解决**：
1. 确认PSRAM已启用
2. 降低图像质量：`jpeg_quality = 12` 或更高
3. 降低分辨率：改为FRAMESIZE_VGA或FRAMESIZE_CIF
4. 检查代码是否有内存泄漏

### ❌ 问题9：Base64编码失败
**解决**：
1. 图片太大，降低分辨率和质量
2. 内存不足，启用PSRAM
3. 在拍照后及时释放framebuffer

### ❌ 问题10：Web界面无法访问
**解决**：
1. 确认WiFi已连接（串口显示IP地址）
2. 电脑和ESP32在同一WiFi网络
3. 尝试关闭防火墙
4. 尝试用手机连接测试

---

## 💡 优化建议

### 提升速度
- 使用FRAMESIZE_VGA或更小分辨率
- 提高JPEG质量值（降低图片质量）
- 选择响应更快的API端点
- 使用本地WiFi而非移动热点

### 降低成本
- 使用免费的API（有使用限制）
- 减少调用频率
- 使用更短的提示词
- 考虑使用本地模型

### 提高准确性
- 使用更好的光照条件
- 提高图像分辨率
- 优化提示词描述
- 选择更强大的模型

---

## 🔧 进阶配置

### 切换图像分辨率
在 `setupCamera()` 函数中修改：
```cpp
// 高清模式（较慢，质量好）
config.frame_size = FRAMESIZE_XGA;  // 1024x768

// 标准模式（推荐）
config.frame_size = FRAMESIZE_VGA;  // 640x480

// 快速模式（快速，质量较低）
config.frame_size = FRAMESIZE_CIF;  // 400x296
```

### 调整图像质量
```cpp
// 质量范围: 0-63，数值越小质量越高，文件越大
config.jpeg_quality = 10;  // 高质量
config.jpeg_quality = 12;  // 标准（推荐）
config.jpeg_quality = 15;  // 快速
```

### 修改提示词
根据应用场景自定义：
```cpp
// 通用描述
"请详细描述这张图片中的内容"

// 物体识别
"请识别图片中的所有物体并列出它们的名称"

// 场景分析
"请分析这个场景，说明是什么地方，有什么特点"

// 文字识别
"请读出图片中的所有文字内容"

// 安全检测
"请检查图片中是否有异常情况或安全隐患"
```

### 添加串口命令控制
在 `loop()` 中添加：
```cpp
if (Serial.available() > 0) {
  String command = Serial.readStringUntil('\n');
  command.trim();
  
  if (command == "ANALYZE") {
    performVisionAnalysis();
  } else if (command == "STATUS") {
    // 显示状态
  } else if (command == "HELP") {
    // 显示帮助
  }
}
```

---

## 📚 API获取指南

### OpenAI GPT-4 Vision API

1. **注册账号**
   - 访问 https://platform.openai.com/signup
   - 注册并验证邮箱

2. **获取API密钥**
   - 登录后访问 https://platform.openai.com/api-keys
   - 点击 "Create new secret key"
   - 复制密钥（只显示一次！）

3. **充值余额**
   - 访问 https://platform.openai.com/account/billing
   - 充值至少$5（建议$10起）

4. **费用说明**
   - GPT-4 Vision: 约$0.01-0.03/次（取决于图片大小）
   - 建议设置使用限额避免超支

### 阿里云通义千问 Vision API

1. **注册阿里云账号**
   - 访问 https://www.aliyun.com/
   - 注册并完成实名认证

2. **开通DashScope服务**
   - 访问 https://dashscope.aliyun.com/
   - 点击"立即开通"

3. **获取API Key**
   - 控制台 → API-KEY管理
   - 创建新的API-KEY
   - 复制保存

4. **费用说明**
   - 有免费额度（每月100万tokens）
   - 超出后按量付费，具体见官网

### 其他可选API

- **国内**：百度文心一言、讯飞星火、腾讯混元
- **国外**：Anthropic Claude、Google Gemini
- **开源**：LLaVA、MiniGPT-4（需自己部署）

---

## 📁 项目文件说明

```
AI_Answer/
├── AI_Answer.ino          # 主程序代码
├── README.md              # 详细技术文档
├── QUICK_START.md         # 本文件 - 快速开始
└── data/                  # 数据文件夹（可选）
    └── ...
```

---

## 🎓 学习资源

### 官方文档
- [ESP32-S3技术手册](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_cn.pdf)
- [ESP32 Camera库](https://github.com/espressif/esp32-camera)
- [OpenAI API文档](https://platform.openai.com/docs/guides/vision)
- [通义千问API文档](https://help.aliyun.com/zh/dashscope/)

### 教程视频
- B站搜索：ESP32-S3 CAM 教程
- YouTube: ESP32 Camera Tutorial

### 社区支持
- Arduino论坛：https://forum.arduino.cc/
- ESP32论坛：https://www.esp32.com/
- GitHub Issues: (待添加项目链接)

---

## 🚀 下一步

完成基础配置后，你可以：

1. ✅ **测试基本功能**：确保拍照和Web界面正常
2. ✅ **配置API**：选择并配置你的视觉AI API
3. ✅ **首次AI分析**：触发一次完整的分析流程
4. ⏳ **添加语音输出**：集成TTS实现语音播报
5. ⏳ **优化提示词**：根据应用场景调整
6. ⏳ **开发应用**：集成到你的项目中

---

## ⚠️ 重要提示

1. **API密钥安全**：不要把包含API密钥的代码上传到公共仓库
2. **费用控制**：设置API使用限额，避免意外高额账单
3. **网络稳定**：确保WiFi连接稳定，否则API调用会失败
4. **PSRAM必须启用**：这是最常见的问题，务必检查
5. **电源供应**：使用质量好的USB线和电源适配器

---

## 📞 获取帮助

遇到问题？
1. 查看本文档的"常见问题排查"部分
2. 查看README.md中的详细技术文档
3. 检查串口监视器的错误信息
4. 在GitHub Issues提交问题（待添加链接）

---

**祝你使用愉快！🎉**

如果这个项目对你有帮助，欢迎Star⭐和分享！

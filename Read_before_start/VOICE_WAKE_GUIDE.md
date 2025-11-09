# 语音唤醒功能使用指南

## 📖 功能概述

语音唤醒功能允许设备通过识别特定的唤醒词来激活语音交互流程。就像"嘿Siri"、"小爱同学"一样,您可以通过说出预设的唤醒词来唤醒设备并进行语音交互。

---

## 🎯 完整交互流程

```
用户: "你好小智"(唤醒词)
      ↓
设备: 🔔 播放提示音
      ↓
设备: "我在听,请说出您的指令"
      ↓
用户: "这张图片里有什么?"
      ↓
设备: 📷 自动拍照
      ↓
设备: 🤖 调用AI分析图片
      ↓
设备: 🔊 "图片中有一个红色的杯子,放在木质桌面上..."
```

---

## ⚙️ 配置唤醒词

### 方法1: 在config_local.h中配置（推荐）

打开 `config_local.h` 文件,找到以下行:

```cpp
// 语音唤醒
static const char* CUSTOM_WAKE_WORD = "你好小智";  // 自定义唤醒词，可修改为任意短语
```

修改为你想要的唤醒词:

```cpp
// 示例1: 使用自己的名字
static const char* CUSTOM_WAKE_WORD = "小明小明";

// 示例2: 使用创意唤醒词
static const char* CUSTOM_WAKE_WORD = "魔镜魔镜";

// 示例3: 使用英文
static const char* CUSTOM_WAKE_WORD = "Hey Robot";
```

### 方法2: 在AI_Answer.ino中配置

如果 `config_local.h` 中没有定义,代码会使用 `AI_Answer.ino` 中的默认值:

```cpp
#ifndef CUSTOM_WAKE_WORD
#define CUSTOM_WAKE_WORD "你好小智"
#endif

static String wakeWord = String(CUSTOM_WAKE_WORD);
```

---

## 🎛️ 调整唤醒参数

在 `AI_Answer.ino` 中可以调整以下参数:

```cpp
// 每次监听的时长(秒)
#ifndef WAKE_LISTEN_SECONDS
#define WAKE_LISTEN_SECONDS 2
#endif

// 唤醒超时时间(毫秒) - 如果这段时间内未检测到唤醒词则退出
#ifndef WAKE_TIMEOUT_MS
#define WAKE_TIMEOUT_MS (45000UL)  // 45秒
#endif

// 命令录音时长(秒) - 唤醒后用户说指令的时长
#ifndef VOICE_COMMAND_SECONDS
#define VOICE_COMMAND_SECONDS 5
#endif
```

### 参数调整建议

**提高识别率**:
```cpp
#define WAKE_LISTEN_SECONDS 3     // 增加监听时长
#define WAKE_TIMEOUT_MS (60000UL) // 延长超时时间
```

**快速响应**:
```cpp
#define WAKE_LISTEN_SECONDS 1     // 减少监听时长
#define WAKE_TIMEOUT_MS (30000UL) // 缩短超时时间
```

**更长指令时间**:
```cpp
#define VOICE_COMMAND_SECONDS 8   // 允许更长的语音指令
```

---

## 🚀 使用方法

### 方法1: 物理按钮触发（推荐）

1. **长按** GPIO1 按钮 **超过1.5秒**
2. 松开按钮后,设备进入唤醒监听模式
3. 对着麦克风清晰地说出唤醒词: "你好小智"
4. 听到提示音(哔~)后,立即说出你的指令
5. 设备自动完成: 拍照 → AI分析 → 语音播报

**示例对话**:
```
[长按按钮1.5秒]
设备: "👂 进入语音唤醒模式,唤醒词: 你好小智"

用户: "你好小智"
设备: 🔔 哔~ (提示音)

用户: "描述一下这张图片"
设备: "📷 拍照中..."
设备: "🤖 AI分析中..."
设备: "🔊 图片中显示一个现代化的办公室..."
```

### 方法2: Web界面触发

1. 在浏览器打开设备的Web界面
2. 点击 **"🛎️ 语音唤醒"** 按钮
3. 对着麦克风说出唤醒词
4. 听到提示音后说出指令

---

## 🎤 唤醒词选择建议

### ✅ 好的唤醒词

1. **3-5个字**: "你好小智"、"魔镜魔镜"
2. **音节清晰**: "小爱同学"、"天猫精灵"
3. **不常见**: 避免日常对话中频繁出现的词
4. **易于发音**: 避免绕口令

### ❌ 避免的唤醒词

1. **过短**: "你好"、"嗨"（容易误触发）
2. **过长**: "请问人工智能助手在吗"（难以记忆）
3. **常见词**: "好的"、"是的"（容易误触发）
4. **难发音**: 包含复杂辅音组合的词

### 💡 推荐的唤醒词列表

```
中文:
- "你好小智"
- "魔镜魔镜"
- "小助手你好"
- "智能相机"
- "嘿小精灵"

英文:
- "Hey Robot"
- "Hello Vision"
- "Wake up AI"

创意:
- "芝麻开门"
- "变身开始"
- "启动魔法"
```

---

## 🔧 工作原理

### 技术流程

1. **进入监听模式**
   - 长按按钮或Web触发
   - 初始化音频缓冲区
   - 开始循环监听

2. **循环录音识别**
   - 每2秒录音一次
   - 通过百度ASR识别语音
   - 检查是否包含唤醒词(忽略大小写)

3. **唤醒成功**
   - 播放880Hz提示音180ms
   - 延迟300ms准备录音
   - 提示用户说出指令

4. **录音指令**
   - 录音5秒(可配置)
   - 识别用户的语音指令
   - 将指令作为提示词

5. **执行分析**
   - 拍摄当前画面
   - 调用视觉AI分析
   - 生成AI回答

6. **语音播报**
   - 通过百度TTS合成语音
   - I2S扬声器播放结果

### 内存管理

```cpp
// 唤醒词监听缓冲区
constexpr size_t WAKE_WORD_BUFFER_BYTES = MIC_BYTES_PER_SECOND * WAKE_LISTEN_SECONDS;
// 16000 Hz * 2 bytes * 1 channel * 2 sec = 64KB

// 命令录音缓冲区
constexpr size_t VOICE_COMMAND_BUFFER_BYTES = MIC_BYTES_PER_SECOND * VOICE_COMMAND_SECONDS;
// 16000 Hz * 2 bytes * 1 channel * 5 sec = 160KB
```

---

## 🐛 常见问题

### ❌ 问题1: 唤醒词无法识别

**可能原因**:
1. 环境噪音太大
2. 麦克风距离太远
3. 发音不清晰
4. 唤醒词设置过于复杂

**解决方案**:
1. 在安静环境中测试
2. 距离麦克风20-50cm说话
3. 清晰、响亮地说出唤醒词
4. 更换为更简单的唤醒词

### ❌ 问题2: 频繁误触发

**可能原因**:
1. 唤醒词过于常见
2. 环境中有相似声音
3. 监听时长设置过长

**解决方案**:
1. 更换为不常见的唤醒词
2. 移动到安静环境
3. 减少 `WAKE_LISTEN_SECONDS` 参数

### ❌ 问题3: 提示音后来不及说话

**解决方案**:
增加延迟时间,在 `performVoiceWakeFlow()` 函数中:
```cpp
playBeepTone(880, 180);
delay(500);  // 从250ms增加到500ms
Serial.println("🎯 [唤醒] 唤醒成功，请在提示音后说出指令...");
delay(500);  // 从300ms增加到500ms
```

### ❌ 问题4: 唤醒超时太短

**解决方案**:
增加超时时间:
```cpp
#define WAKE_TIMEOUT_MS (90000UL)  // 从45秒增加到90秒
```

### ❌ 问题5: 语音识别不准确

**解决方案**:
1. 检查网络连接是否稳定
2. 确认百度ASR API配置正确
3. 在安静环境中测试
4. 说话速度适中,吐字清晰
5. 检查麦克风连接是否正常

---

## 📊 性能指标

### 典型参数

| 指标 | 数值 | 说明 |
|------|------|------|
| 唤醒词识别延迟 | 2-4秒 | 取决于网络速度 |
| 命令录音时长 | 5秒 | 可配置 |
| 单次循环监听 | 2秒 | 可配置 |
| 最大等待时间 | 45秒 | 可配置 |
| 提示音频率 | 880Hz | 固定 |
| 提示音时长 | 180ms | 固定 |
| 内存占用 | ~224KB | 缓冲区总计 |

### 优化建议

**提高识别速度**:
- 使用更快的网络连接
- 减少 `WAKE_LISTEN_SECONDS`
- 优化ASR API配置

**降低功耗**:
- 减少 `WAKE_TIMEOUT_MS`
- 使用短按触发代替长时间监听
- 仅在需要时启用唤醒模式

**提高准确率**:
- 增加 `WAKE_LISTEN_SECONDS`
- 使用更清晰的唤醒词
- 改善麦克风质量

---

## 🎨 使用场景示例

### 场景1: 智能家居控制

```
用户: "你好小智"
设备: 🔔 哔~
用户: "看看门口有什么"
设备: "门口有一位快递员,手里拿着包裹"
```

### 场景2: 宠物监控

```
用户: "你好小智"
设备: 🔔 哔~
用户: "我的猫在做什么"
设备: "你的猫正在沙发上睡觉,姿势很舒服"
```

### 场景3: 学习助手

```
用户: "你好小智"
设备: 🔔 哔~
用户: "读一下黑板上的字"
设备: "黑板上写着:明天考试范围第3章到第5章"
```

### 场景4: 安全监控

```
用户: "你好小智"
设备: 🔔 哔~
用户: "检查房间里有没有异常"
设备: "房间内一切正常,没有发现异常情况"
```

---

## 🔒 隐私与安全

### 数据处理说明

1. **本地录音**: 音频数据仅在ESP32内存中临时存储
2. **云端识别**: 音频发送到百度ASR进行识别
3. **即时删除**: 识别完成后立即释放音频缓冲区
4. **无持久化**: 不保存任何录音文件到存储器

### 隐私保护建议

1. 仅在需要时启用唤醒功能
2. 选择不易被他人模仿的唤醒词
3. 定期检查API调用日志
4. 在公共场所使用时注意周围环境

---

## 🚀 进阶定制

### 添加多个唤醒词

修改 `listenForWakeWord()` 函数:

```cpp
bool listenForWakeWord(const String& targetWord, unsigned long timeoutMs) {
  // ...现有代码...
  
  // 定义多个唤醒词
  String wakeWords[] = {"你好小智", "魔镜魔镜", "智能助手"};
  int numWords = 3;
  
  // 检查是否匹配任一唤醒词
  for (int i = 0; i < numWords; i++) {
    String normalizedWake = wakeWords[i];
    normalizedWake.toLowerCase();
    if (normalizedSpeech.indexOf(normalizedWake) != -1) {
      Serial.printf("✅ [唤醒] 检测到唤醒词: %s\n", wakeWords[i].c_str());
      detected = true;
      break;
    }
  }
}
```

### 自定义提示音

修改 `performVoiceWakeFlow()` 函数:

```cpp
// 播放三声提示音
playBeepTone(880, 100);
delay(100);
playBeepTone(988, 100);
delay(100);
playBeepTone(1047, 150);
delay(250);
```

### 添加LED指示

```cpp
void performVoiceWakeFlow() {
  // 开启LED表示进入唤醒模式
  digitalWrite(LED_GPIO_NUM, HIGH);
  
  bool wakeDetected = listenForWakeWord(wakeWord, WAKE_TIMEOUT_MS);
  
  if (wakeDetected) {
    // 闪烁LED表示唤醒成功
    for(int i=0; i<3; i++) {
      digitalWrite(LED_GPIO_NUM, LOW);
      delay(100);
      digitalWrite(LED_GPIO_NUM, HIGH);
      delay(100);
    }
  }
  
  // 关闭LED
  digitalWrite(LED_GPIO_NUM, LOW);
  
  // ...继续原有流程...
}
```

---

## 📚 相关文档

- [README.md](../README.md) - 完整技术文档
- [QUICK_START.md](./QUICK_START.md) - 快速开始指南
- [README_TTS.md](./README_TTS.md) - TTS语音合成指南
- [INDEX.md](./INDEX.md) - 文档导航

---

## 💡 最佳实践

1. **选择合适的唤醒词**
   - 3-5个字
   - 音节清晰
   - 不常见但易记

2. **优化环境**
   - 安静的使用环境
   - 麦克风距离适中(20-50cm)
   - 避免回声和噪音

3. **合理设置参数**
   - 根据使用场景调整监听时长
   - 平衡响应速度和识别准确率
   - 设置合适的超时时间

4. **测试和调试**
   - 在不同环境中测试
   - 记录识别成功率
   - 根据反馈优化参数

---

## 🎉 开始使用

现在您已经了解了语音唤醒功能的所有细节,可以开始配置和使用了!

**基础使用三步骤**:
1. 配置唤醒词: 编辑 `config_local.h`
2. 上传代码: 编译并上传到ESP32
3. 测试唤醒: 长按按钮,说出唤醒词

祝您使用愉快! 🚀

---

**最后更新**: 2025-11-06
**版本**: v2.3.0

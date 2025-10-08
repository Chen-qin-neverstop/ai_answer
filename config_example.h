#pragma once

// 将本文件复制为 config_local.h 并填入真实密钥。
// config_local.h 会被 .gitignore 忽略，不会提交到 Git。

// WiFi
static const char* ssid     = "YourWiFiName";
static const char* password = "YourWiFiPassword";

// Vision/Chat API Keys
static const char* OPENAI_API_KEY = "sk-your-openai-key";   // OpenAI
static const char* QWEN_API_KEY   = "sk-your-qwen-key";     // 通义千问（OpenAI兼容模式）
static const char* CUSTOM_API_KEY = "sk-your-custom-key";   // 其他兼容OpenAI格式

// TTS Keys
static const char* ALIYUN_TTS_APPKEY    = "your-aliyun-tts-appkey";  // 阿里云TTS
static const char* BAIDU_TTS_API_KEY    = "your-baidu-api-key";      // 百度TTS（可选）
static const char* BAIDU_TTS_SECRET_KEY = "your-baidu-secret-key";   // 百度TTS（可选）

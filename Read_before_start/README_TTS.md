````markdown
# Baidu TTS 使用说明（设备直接调用与代理选项）

本文件说明如何在本项目中使用百度语音合成（TTS）。当前代码优先实现“设备直接调用百度 TTS”：

- 设备可直接使用 `BAIDU_API_KEY` 与 `BAIDU_SECRET_KEY` 在运行时获取 access_token 并拉取 MP3 流播放。
- 可选：在内网环境或受限网络下，使用仓库提供的 `baidu_tts_proxy.py` 作为代理，由代理负责与百度通信，设备通过 HTTP 向代理请求音频。

文件说明：
- `baidu_tts_proxy.py`：可选的 Flask 代理示例（将 API Key/Secret 放在代理端以避免设备处理 token）。

设备直接调用（推荐）
1. 在 `config_local.h` 中添加：
```cpp
// 用于设备端获取 token
#define BAIDU_API_KEY "你的百度API Key"
#define BAIDU_SECRET_KEY "你的百度Secret Key"

// 可选：临时 token（仅用于快速测试，生产环境建议留空）
#define BAIDU_TTS_ACCESS_TOKEN "临时token"
```
2. 设备运行时会自动尝试使用 `BAIDU_TTS_ACCESS_TOKEN`（若存在），否则使用 `BAIDU_API_KEY`/`BAIDU_SECRET_KEY` 向 `https://openapi.baidu.com/oauth/2.0/token` 获取 token 并缓存。
3. 设备随后通过 `http://tsn.baidu.com/text2audio` 拉取 MP3 流并播放。

如何在设备上快速测试 TTS
1. 在 Web UI 或触发流程后（AI分析返回文本），设备会自动调用 TTS 并在串口输出诊断日志。
2. 串口查看关键日志：
	- "[Baidu] 获取 access_token"：表示设备正在请求 token
	- "access_token (已掩码)"：表示成功获取 token（日志中会掩码显示）
	- "开始拉取音频流"：开始下载并播放 MP3
	- "mp3->loop() 返回 false" 或 "MP3解码器初始化失败"：播放失败的线索

代理（可选）
如果你的设备无法直接访问百度的域，或者你希望在代理中集中管理 token，请使用 `baidu_tts_proxy.py`：

快速启动（Windows PowerShell）：

```powershell
python -m venv venv
.\venv\Scripts\Activate.ps1
pip install flask requests
# 编辑 baidu_tts_proxy.py，填写 API_KEY 和 SECRET_KEY
python baidu_tts_proxy.py
```

本地测试代理（在运行代理的机器上）：

```powershell
curl -v "http://127.0.0.1:3000/baidu_tts?text=你好百度" -o baidu_proxy.mp3
```

在设备上配置代理（可选）：在 `config_local.h` 中设置：
```cpp
#define BAIDU_TTS_PROXY_URL "http://192.168.1.100:3000/baidu_tts"
```

注意事项：
- 代理默认仅作为网络受限情况下的备用方案。当前代码逻辑已切换为优先直接在设备上调用百度（当可达时）。
- 请将 API Key 和 Secret 保密，不要提交到公共仓库。

````

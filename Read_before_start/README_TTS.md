Baidu TTS Proxy

This repository contains a small Flask-based proxy to call Baidu Text-to-Speech and return audio streams.

Files:
- baidu_tts_proxy.py  - simple Flask proxy, fill API_KEY and SECRET_KEY and run

Quick start (Windows PowerShell):

```powershell
python -m venv venv
.\venv\Scripts\Activate.ps1
pip install flask requests
# edit baidu_tts_proxy.py and set API_KEY and SECRET_KEY
python baidu_tts_proxy.py
```

Test (from same machine):

```powershell
curl -v "http://127.0.0.1:3000/baidu_tts?text=你好百度" -o baidu_proxy.mp3
``` 

Set device `AI_Answer.ino` to use proxy URL (example in `config_local.h`):

```c
#define BAIDU_TTS_PROXY_URL "http://192.168.1.100:3000/baidu_tts"
```

Notes:
- The proxy will automatically fetch and cache Baidu access_token.
- Keep your API Key and Secret secret. Do not commit them to a public repo.

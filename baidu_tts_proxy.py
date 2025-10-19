from flask import Flask, request, Response
import requests, time

API_KEY = 'YOUR_API_KEY'
SECRET_KEY = 'YOUR_SECRET_KEY'
CUID = 'ESP32CAM001'

app = Flask(__name__)
token_cache = {'token': '', 'expires_at': 0}

def get_token():
    now = time.time()
    if token_cache['token'] and now < token_cache['expires_at']:
        return token_cache['token']
    url = f'https://openapi.baidu.com/oauth/2.0/token?grant_type=client_credentials&client_id={API_KEY}&client_secret={SECRET_KEY}'
    r = requests.get(url, timeout=10)
    r.raise_for_status()
    data = r.json()
    token_cache['token'] = data['access_token']
    token_cache['expires_at'] = now + int(data['expires_in']) - 60
    return token_cache['token']

@app.route('/baidu_tts')
def baidu_tts():
    text = request.args.get('text', '')
    if not text:
        return 'missing text', 400
    token = get_token()
    params = {
        'tex': text,
        'tok': token,
        'cuid': CUID,
        'ctp': 1,
        'lan': 'zh',
        'spd': 5,
        'pit': 5,
        'vol': 7,
        'per': 0
    }
    r = requests.get('http://tsn.baidu.com/text2audio', params=params, stream=True, timeout=15)
    if r.headers.get('Content-Type','').startswith('audio/'):
        return Response(r.iter_content(4096), content_type=r.headers['Content-Type'])
    else:
        return r.text, 500

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=3000)

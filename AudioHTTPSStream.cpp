#include "AudioHTTPSStream.h"

AudioHTTPSStream::AudioHTTPSStream()
  : insecure(true), followRedirects(true), opened(false), caCert(nullptr), timeoutMs(15000), contentLength(-1), position(0) {
  userAgent = F("Mozilla/5.0 (ESP32-S3)");
}

AudioHTTPSStream::AudioHTTPSStream(const char *url)
  : AudioHTTPSStream() {
  open(url);
}

AudioHTTPSStream::~AudioHTTPSStream() {
  close();
}

void AudioHTTPSStream::setUseInsecure(bool enable) {
  insecure = enable;
}

void AudioHTTPSStream::setCACert(const char *cert) {
  caCert = cert;
}

void AudioHTTPSStream::setUserAgent(const String &ua) {
  userAgent = ua;
}

void AudioHTTPSStream::setFollowRedirects(bool enable) {
  followRedirects = enable;
}

void AudioHTTPSStream::setTimeout(uint32_t ms) {
  timeoutMs = ms;
}

bool AudioHTTPSStream::open(const char *url) {
  close();
  if (url == nullptr || strlen(url) == 0) {
    return false;
  }

  lastUrl = url;
  if (insecure) {
    client.setInsecure();
  } else if (caCert != nullptr) {
    client.setCACert(caCert);
  }

  http.setTimeout(timeoutMs);
  http.setUserAgent(userAgent);
  if (followRedirects) {
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  }

  bool beginOk = http.begin(client, lastUrl);
  if (!beginOk) {
    http.end();
    client.stop();
    opened = false;
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    client.stop();
    opened = false;
    return false;
  }

  contentLength = http.getSize();
  position = 0;
  opened = true;
  return true;
}

bool AudioHTTPSStream::ensureConnection() {
  if (!opened) {
    return false;
  }
  if (http.connected()) {
    return true;
  }
  // simple reconnect attempt
  http.end();
  client.stop();
  return open(lastUrl.c_str());
}

uint32_t AudioHTTPSStream::read(void *data, uint32_t len) {
  return readInternal(data, len, false);
}

uint32_t AudioHTTPSStream::readNonBlock(void *data, uint32_t len) {
  return readInternal(data, len, true);
}

uint32_t AudioHTTPSStream::readInternal(void *data, uint32_t len, bool nonBlock) {
  if (data == nullptr || len == 0) {
    return 0;
  }
  if (!ensureConnection()) {
    return 0;
  }

  auto *stream = http.getStreamPtr();
  if (stream == nullptr) {
    return 0;
  }

  uint32_t waited = 0;
  while (!nonBlock && stream->available() == 0 && waited < timeoutMs) {
    delay(10);
    waited += 10;
  }

  int avail = stream->available();
  if (avail <= 0) {
    return 0;
  }

  if (len > static_cast<uint32_t>(avail)) {
    len = static_cast<uint32_t>(avail);
  }

  int readBytes = stream->read(reinterpret_cast<uint8_t *>(data), len);
  if (readBytes > 0) {
    position += readBytes;
    return static_cast<uint32_t>(readBytes);
  }

  return 0;
}

bool AudioHTTPSStream::seek(int32_t pos, int dir) {
  (void)pos;
  (void)dir;
  return false;
}

bool AudioHTTPSStream::close() {
  if (opened) {
    http.end();
    client.stop();
  }
  opened = false;
  position = 0;
  contentLength = -1;
  return true;
}

bool AudioHTTPSStream::isOpen() {
  return opened && http.connected();
}

uint32_t AudioHTTPSStream::getSize() {
  return contentLength < 0 ? 0 : static_cast<uint32_t>(contentLength);
}

uint32_t AudioHTTPSStream::getPos() {
  return static_cast<uint32_t>(position);
}

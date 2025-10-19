#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <AudioFileSource.h>

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
#  include <NetworkClient.h>
#else
#  include <WiFiClient.h>
#endif

class AudioHTTPSStream : public AudioFileSource {
public:
  AudioHTTPSStream();
  explicit AudioHTTPSStream(const char *url);
  ~AudioHTTPSStream() override;

  bool open(const char *url) override;
  uint32_t read(void *data, uint32_t len) override;
  uint32_t readNonBlock(void *data, uint32_t len) override;
  bool seek(int32_t pos, int dir) override;
  bool close() override;
  bool isOpen() override;
  uint32_t getSize() override;
  uint32_t getPos() override;

  void setUseInsecure(bool enable);
  void setCACert(const char *cert);
  void setUserAgent(const String &ua);
  void setFollowRedirects(bool enable);
  void setTimeout(uint32_t ms);

private:
  bool ensureConnection();
  uint32_t readInternal(void *data, uint32_t len, bool nonBlock);

  WiFiClientSecure client;
  HTTPClient http;
  bool insecure;
  bool followRedirects;
  bool opened;
  const char *caCert;
  String lastUrl;
  String userAgent;
  uint32_t timeoutMs;
  int32_t contentLength;
  int32_t position;
};
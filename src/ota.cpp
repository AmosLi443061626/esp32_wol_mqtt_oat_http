#include "http_auth.h"
#include "ota.h"
#include "mqtt.h"
#include "wifi_config.h"
#include "firmware_version.h"
#include <WiFi.h>
#include <Update.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

extern const uint8_t otaCaBundle[] asm("_binary_data_ota_ca_bundle_bin_start");
namespace {
WebServer* web = nullptr;
bool uploadAllowed = false, uploadSucceeded = false, restartPending = false;
bool remotePending = false, cloudPending = false, busy = false;
uint32_t restartAt = 0;
String uploadError, pendingUrl, lastResult = "尚未升级";
String escape(String text) {
  text.replace("&", "&amp;"); text.replace("<", "&lt;"); text.replace(">", "&gt;");
  text.replace("\"", "&quot;"); text.replace("'", "&#39;"); return text;
}
bool validUrl(const String& url) {
  if (url.length() > 1024 || url.indexOf('\r') >= 0 || url.indexOf('\n') >= 0 ||
      url.indexOf(' ') >= 0 || url.indexOf('#') >= 0) return false;
  const int prefix = url.startsWith("https://") ? 8 : url.startsWith("http://") ? 7 : 0;
  if (!prefix) return false;
  int end = url.indexOf('/', prefix);
  if (end < 0) end = url.length();
  return end > prefix && url.substring(prefix, end).indexOf('@') < 0;
}
bool authenticate() {
  if (WiFi.status() != WL_CONNECTED) {
    web->send(503, "text/plain; charset=utf-8", "请先连接联网 Wi-Fi。"); return false;
  }
  if (!httpCredentialsValid(*web)) {
    httpRequireAuthentication(*web); return false;
  }
  return true;
}
void scheduleRestart() { restartPending = true; restartAt = millis(); }
void page() {
  if (!authenticate()) return;
  String html = "<!doctype html><html lang='zh-CN'><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<h1>OTA 固件升级</h1><p>当前版本：" + escape(FIRMWARE_BUILD_VERSION) + "</p>"
    "<p>最近结果：" + escape(lastResult) + "</p>"
    "<h2>附件上传</h2><p>选择本项目的 firmware.bin，成功后自动重启。</p>"
    "<form method='post' action='/ota' enctype='multipart/form-data'>"
    "<input type='file' name='firmware' accept='.bin' required><button>上传并升级</button></form>"
    "<h2>远程 HTTP / HTTPS 升级</h2><form method='post' action='/ota/remote'>"
    "<input type='url' name='url' maxlength='1024' placeholder='https://服务器/firmware.bin' required>"
    "<button>下载并升级</button></form>"
    "<h2>巴法云升级</h2><form method='post' action='/ota/cloud'><button>获取云端固件并升级</button></form>"
    "<p>也可向当前 MQTT 主题发送 update。先在巴法云同名 MQTT 主题的 OTA 页面上传固件。</p>"
    "<p>HTTPS 校验证书，使用公共 CA；URL 应直接返回固件。升级时网页暂时无法响应。</p>"
    "<p><a href='/'>返回设备状态</a></p>";
  web->sendHeader("Cache-Control", "no-store");
  web->send(200, "text/html; charset=utf-8", html);
}
void upload() {
  HTTPUpload& data = web->upload();
  if (data.status == UPLOAD_FILE_START) {
    uploadAllowed = !busy && !remotePending && !restartPending &&
      WiFi.status() == WL_CONNECTED && httpCredentialsValid(*web);
    uploadSucceeded = false; uploadError = "";
    if (!uploadAllowed) return;
    busy = true;
    if (!data.filename.endsWith(".bin")) { uploadError = "请选择 .bin 固件文件"; return; }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) uploadError = Update.errorString();
  } else if (data.status == UPLOAD_FILE_WRITE && uploadAllowed && uploadError.isEmpty()) {
    if (Update.write(data.buf, data.currentSize) != data.currentSize) {
      uploadError = Update.errorString(); Update.abort();
    }
  } else if (data.status == UPLOAD_FILE_END && uploadAllowed && uploadError.isEmpty()) {
    uploadSucceeded = Update.end(true);
    if (!uploadSucceeded) uploadError = Update.errorString();
  } else if (data.status == UPLOAD_FILE_ABORTED && uploadAllowed) {
    Update.abort(); uploadError = "上传中断"; busy = false; uploadAllowed = false;
  }
}
void complete() {
  if (!authenticate()) { if (uploadAllowed) { Update.abort(); busy = false; } return; }
  if (!uploadAllowed || !uploadSucceeded) {
    if (uploadAllowed) busy = false;
    uploadAllowed = false;
    web->send(400, "text/plain; charset=utf-8", "升级失败：" +
      (uploadError.isEmpty() ? String("未收到有效固件或已有升级任务") : uploadError)); return;
  }
  web->sendHeader("Connection", "close");
  web->send(200, "text/plain; charset=utf-8", "升级成功，设备将在约 1 秒后重启。");
  lastResult = "附件升级成功"; scheduleRestart();
  uploadAllowed = false; uploadSucceeded = false;
}
bool syncTime() {
  if (time(nullptr) > 1700000000) return true;
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com", "ntp.aliyun.com");
  const uint32_t start = millis();
  while (time(nullptr) <= 1700000000 && millis() - start < 20000) delay(100);
  return time(nullptr) > 1700000000;
}
void configureTls(WiFiClientSecure& client) {
  client.setCACertBundle(otaCaBundle);
  client.setHandshakeTimeout(10);
}
class MetadataStream : public Stream {
 public:
  String value;
  MetadataStream() { value.reserve(4096); }
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* data, size_t length) override {
    size_t space = 4096 - value.length();
    size_t take = length < space ? length : space;
    if (take) value.concat(reinterpret_cast<const char*>(data), take);
    return take;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};
String urlEncode(const String& value) {
  static const char hex[] = "0123456789ABCDEF";
  String encoded;
  for (unsigned i = 0; i < value.length(); ++i) {
    const uint8_t c = value[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') encoded += char(c);
    else { encoded += '%'; encoded += hex[c >> 4]; encoded += hex[c & 15]; }
  }
  return encoded;
}
bool cloudUrl(String& url) {
  if (!syncTime()) { lastResult = "时间同步失败，无法校验 HTTPS 证书"; return false; }
  WiFiClientSecure tls;
  configureTls(tls);
  HTTPClient http;
  http.setConnectTimeout(10000); http.setTimeout(10000);
  // Escape configuration values; never log the URL containing the private key.
  const String request = "https://apis.bemfa.com/vb/api/v1/firmwareVersion?openID=" +
    urlEncode(String(mqttPrivateKey())) + "&topic=" + urlEncode(mqttTopic()) + "&deviceType=1&secure=true";
  if (!http.begin(tls, request)) { lastResult = "云端接口初始化失败"; return false; }
  const int status = http.GET();
  if (status != 200) {
    lastResult = "获取云端固件失败，HTTP/连接状态：" + String(status); http.end(); return false;
  }
  MetadataStream response;
  const int received = http.writeToStream(&response);
  http.end();
  if (received < 0) { lastResult = "云端响应读取失败或超过 4 KB"; return false; }
  StaticJsonDocument<2048> metadata;
  if (deserializeJson(metadata, response.value) || !metadata["code"].is<int>() ||
      metadata["code"].as<int>() != 0 || !metadata["data"]["url"].is<const char*>()) {
    lastResult = "云端未返回有效固件，请检查私钥、主题及 OTA 上传记录"; return false;
  }
  url = metadata["data"]["url"].as<String>();
  if (!validUrl(url)) { lastResult = "云端返回的固件 URL 无效"; return false; }
  return true;
}
void remoteUpdate(String url) {
  if (url.startsWith("https://") && !syncTime()) {
    lastResult = "时间同步失败，无法校验 HTTPS 证书"; return;
  }
  WiFiClient plain;
  WiFiClientSecure tls;
  configureTls(tls);
  HTTPClient http;
  http.setConnectTimeout(10000);
  const bool opened = url.startsWith("https://") ? http.begin(tls, url) : http.begin(plain, url);
  if (!opened) { lastResult = "固件 URL 初始化失败"; return; }
  HTTPUpdate updater(15000);
  updater.rebootOnUpdate(false);
  updater.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  updater.onProgress([](int current, int total) {
    static uint32_t loggedAt = 0;
    if (millis() - loggedAt >= 1000) {
      loggedAt = millis(); WIFI_LOG_PRINTF("OTA download: %d / %d bytes\n", current, total);
    }
  });
  const auto result = updater.update(http, FIRMWARE_VERSION);
  http.end();
  if (result == HTTP_UPDATE_OK) { lastResult = "远程升级成功，即将重启"; scheduleRestart(); }
  else if (result == HTTP_UPDATE_NO_UPDATES) lastResult = "服务器表示无可用更新";
  else { Update.abort(); lastResult = "远程升级失败：" + updater.getLastErrorString(); }
  WIFI_LOG_PRINTLN(lastResult);
}
void remoteRequest() {
  if (!authenticate()) return;
  String url = web->arg("url"); url.trim();
  if (!validUrl(url)) { web->send(400, "text/plain; charset=utf-8", "请输入直接返回固件的 HTTP/HTTPS URL。"); return; }
  if (busy || remotePending || restartPending) { web->send(409, "text/plain; charset=utf-8", "已有升级任务。"); return; }
  pendingUrl = url; cloudPending = false; remotePending = true;
  lastResult = "已接受远程升级任务";
  web->send(202, "text/plain; charset=utf-8", "已接受任务，设备将下载升级；成功后自动重启，失败后返回 /ota 查看结果。");
}
void cloudRequest() {
  if (!authenticate()) return;
  if (!otaRequestCloudUpdate()) { web->send(409, "text/plain; charset=utf-8", "已有升级任务。"); return; }
  web->send(202, "text/plain; charset=utf-8", "已接受巴法云升级任务，成功后自动重启。");
}
}
void otaSetup(WebServer& server) {
  web = &server;
  httpOn(server, "/ota", HTTP_GET, page);
  httpOn(server, "/ota", HTTP_POST, complete, upload);
  httpOn(server, "/ota/remote", HTTP_POST, remoteRequest);
  httpOn(server, "/ota/cloud", HTTP_POST, cloudRequest);
}
bool otaRequestCloudUpdate() {
  if (busy || remotePending || restartPending || WiFi.status() != WL_CONNECTED) return false;
  cloudPending = true; remotePending = true; lastResult = "已接受巴法云升级任务"; return true;
}
void otaLoop() {
  if (restartPending) { if (millis() - restartAt >= 1000) ESP.restart(); return; }
  if (!remotePending || busy) return;
  remotePending = false; busy = true;
  String url = pendingUrl; pendingUrl = "";
  if (WiFi.status() != WL_CONNECTED) lastResult = "升级失败：Wi-Fi 未连接";
  else if (!cloudPending || cloudUrl(url)) remoteUpdate(url);
  cloudPending = false; busy = false;
}
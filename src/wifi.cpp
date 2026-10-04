#include "http_auth.h"
#include "watchdog.h"
#include "wifi_config.h"
#include "firmware_version.h"
#include "wol.h"
#include "mqtt.h"
#include "ota.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

namespace {
WebServer server(34567);
Preferences preferences;
String savedSsid, savedPassword, pendingSsid, pendingPassword;
bool provisioning = false, connecting = false, resetHeld = false, storageReady = false;
uint32_t connectStarted = 0, resetStarted = 0, lastRetry = 0;
bool wifiClearPending = false;
uint32_t wifiClearStarted = 0;
bool portalLedActive = false;
uint32_t portalLedStarted = 0;
constexpr uint32_t CONNECT_TIMEOUT_MS = 30000;
constexpr uint32_t RECONNECT_INTERVAL_MS = 10000;
constexpr uint32_t RESET_HOLD_MS = 5000;
const IPAddress apIp(192, 168, 1, 1);

String escapeHtml(String value) {
  value.replace("&", "&amp;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  value.replace("\"", "&quot;");
  value.replace("'", "&#39;");
  return value;
}

void startStation(const String& ssid, const String& password) {
  WIFI_LOG_PRINTF("Connecting to SSID: %s (status=%d)\n", ssid.c_str(), static_cast<int>(WiFi.status()));
  const wl_status_t result = WiFi.begin(ssid.c_str(), password.c_str());
  WIFI_LOG_PRINTF("WiFi.begin result=%d, password length=%u\n", static_cast<int>(result), static_cast<unsigned>(password.length()));
  lastRetry = millis();
}

void startPortal() {
  provisioning = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIp, apIp, IPAddress(255, 255, 255, 0));
  String name = "ESP32-Setup-" + WiFi.macAddress().substring(12);
  name.replace(":", "");
  if (!WiFi.softAP(name.c_str())) {
    WIFI_LOG_PRINTLN("Failed to start setup access point");
    return;
  }
  portalLedActive = true;
  portalLedStarted = millis();
  neopixelWrite(RGB_BUILTIN, 0, 0, WIFI_LED_BLUE_BRIGHTNESS);
  WIFI_LOG_PRINTF("Setup Wi-Fi: %s, open http://192.168.1.1:34567/\n", name.c_str());
}

void handleRoot() {
  String html = "<!doctype html><html lang='zh-CN'><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>ESP32 Wi-Fi</title><body><h1>ESP32 设备状态</h1>";
  html += "<p>编译版本：" + escapeHtml(String(FIRMWARE_BUILD_VERSION)) + "</p>";
  html += "<p>联网状态：";
  html += WiFi.status() == WL_CONNECTED ? "已连接" : "未连接";
  html += "</p><p>Wi-Fi：" + escapeHtml(connecting ? pendingSsid : savedSsid) + "</p>";
  html += "<p>内网 IP：" + WiFi.localIP().toString() + "</p>";
  html += "<p>运行时间：" + String(millis() / 1000) + " 秒</p>";
  if (watchdogIsActive()) {
    const uint32_t fedAt = watchdogLastFeedMillis();
    html += "<p>看门狗：运行中；上次成功喂狗：启动后 " + String(fedAt / 1000) +
            " 秒（距今 " + String((millis() - fedAt) / 1000) + " 秒）</p>";
  } else {
    html += "<p>看门狗：未启用</p>";
  }
  html += "<p>可用内存：" + String(ESP.getFreeHeap()) + " 字节</p>";
  html += "<p>芯片内部温度（ESP32-S3 N16R8）：" + String(temperatureRead(), 1) + " °C</p>";
  if (provisioning) {
    html += "<h2>配置联网 Wi-Fi</h2><form method='post' action='/configure'>"
            "<p><label>SSID <input name='ssid' maxlength='32' required></label></p>"
            "<p><label>密码 <input type='password' name='password' maxlength='64'></label></p>"
            "<button type='submit'>连接并保存</button></form>"
            "<p>联网成功后保存配置并自动重启，热点会关闭。通过串口查看设备内网 IP。</p>";
    if (connecting) html += "<p>正在连接，请稍候。</p>";
  }
  html += "<h2>重新配网</h2><form method='post' action='/wifi/clear' "
          "onsubmit='return confirm(\"确认清除 Wi-Fi 配置并重启进入配网模式？\")'>"
          "<button type='submit'>清除 Wi-Fi 配置</button></form>";
  html += "<p><a href='/wol'>WOL 设备管理与唤醒</a></p>"
          "<p><a href='/http'>外网 HTTP / HTTPS 请求测试</a></p>";
  html += "<p><a href='/mqtt'>巴法云 MQTT 开关</a> · <a href='/ota'>OTA 固件升级</a></p>";
  html += "</body></html>";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", html);
}

void handleHttpPage() {
  const String html = "<!doctype html><html lang='zh-CN'><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>HTTP 请求测试</title><body><h1>外网 HTTP / HTTPS 请求测试</h1>"
      "<p><a href='/'>设备状态</a></p><form method='post' action='/http'>"
      "<p><label>URL <input type='url' name='url' maxlength='1024' "
      "placeholder='https://example.com/' required style='width:90%'></label></p>"
      "<button type='submit'>请求测试</button></form>"
      "<p>设备发送 GET 请求。HTTPS 仅测试连通性，不校验证书。最多显示 4 KB 正文。</p>"
      "</body></html>";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", html);
}

// Stop HTTPClient's decoded stream at the preview limit, without buffering
// the full response. writeToStream also handles chunked transfer encoding.
class ResponsePreview : public Stream {
 public:
  String body;
  bool truncated = false;
  ResponsePreview() { body.reserve(4096); }
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* data, size_t length) override {
    watchdogFeed();
    const size_t remaining = 4096 - body.length();
    const size_t take = length < remaining ? length : remaining;
    if (take) body.concat(reinterpret_cast<const char*>(data), take);
    if (take < length) truncated = true;
    return take;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

void handleRequestTest() {
  server.sendHeader("Cache-Control", "no-store");
  if (WiFi.status() != WL_CONNECTED || connecting) {
    server.send(503, "text/plain; charset=utf-8", "请等待设备连接联网 Wi-Fi 后再测试。");
    return;
  }
  String url = server.arg("url");
  url.trim();
  if (url.length() > 1024 ||
      !(url.startsWith("http://") || url.startsWith("https://")) ||
      url.indexOf('\r') >= 0 || url.indexOf('\n') >= 0) {
    server.send(400, "text/plain; charset=utf-8", "请输入有效的 http:// 或 https:// URL，最多 1024 字节。");
    return;
  }
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  // Diagnostic requests only. Do not send sensitive data with this client.
  secureClient.setInsecure();
  secureClient.setHandshakeTimeout(5);
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  const uint32_t started = millis();
  const bool opened = url.startsWith("https://")
      ? http.begin(secureClient, url) : http.begin(plainClient, url);
  int code = opened ? http.GET() : HTTPC_ERROR_CONNECTION_REFUSED;
  ResponsePreview preview;
  int readResult = 0;
  if (code > 0) readResult = http.writeToStream(&preview);
  const uint32_t elapsed = millis() - started;
  String html = "<!doctype html><html lang='zh-CN'><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>请求结果</title><body><h1>外网请求结果</h1>";
  html += "<p>URL：" + escapeHtml(url) + "</p>";
  html += "<p>耗时：" + String(elapsed) + " ms</p>";
  if (code > 0) {
    html += "<p>HTTP 状态码：" + String(code) + "</p>";
    if (code >= 300 && code < 400) html += "<p>服务器返回重定向；本测试不自动跳转。</p>";
    if (preview.truncated) html += "<p>正文超过 4 KB，已截断显示。</p>";
    else if (readResult < 0) html += "<p>正文读取失败：" + escapeHtml(HTTPClient::errorToString(readResult)) + "</p>";
    html += "<pre style='white-space:pre-wrap;overflow-wrap:anywhere'>" + escapeHtml(preview.body) + "</pre>";
  } else {
    html += "<p>请求失败：" + escapeHtml(opened ? HTTPClient::errorToString(code) : String("URL 解析或初始化失败")) + "</p>";
  }
  http.end();
  html += "<p><a href='/http'>继续请求测试</a></p><p><a href='/'>返回设备状态</a></p></body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

void handleClearWifi() {
  if (!storageReady || !preferences.clear()) {
    server.send(500, "text/plain; charset=utf-8", "Wi-Fi 配置清除失败，请查看存储状态。");
    return;
  }
  // Prevent a connection completing in this same loop from saving credentials again.
  connecting = false;
  pendingSsid = "";
  pendingPassword = "";
  savedSsid = "";
  savedPassword = "";
  wifiClearPending = true;
  wifiClearStarted = millis();
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Connection", "close");
  server.send(200, "text/html; charset=utf-8",
    "<!doctype html><meta charset='utf-8'><p>Wi-Fi 配置已清除，设备将在约 1 秒后重启进入配网模式。</p>"
    "<p>请连接 ESP32-Setup-xxxx 热点，再访问 http://192.168.1.1:34567/ 配置 Wi-Fi。</p>"
    "<p>登录账号密码仍为当前 HTTP 账号密码。</p>");
}

void handleConfigure() {
  if (!provisioning) {
    server.send(403, "text/plain; charset=utf-8", "请通过双 GPIO 长按复位后重新配网。");
    return;
  }
  if (connecting) {
    server.send(409, "text/plain; charset=utf-8", "正在连接，请稍候再试。");
    return;
  }
  if (!storageReady) {
    server.send(503, "text/plain; charset=utf-8", "配置存储不可用，请查看串口错误信息。" );
    return;
  }
  pendingSsid = server.arg("ssid");
  pendingPassword = server.arg("password");
  if (pendingSsid.length() == 0 || pendingSsid.length() > 32 || pendingPassword.length() > 64) {
    server.send(400, "text/plain; charset=utf-8", "SSID 或密码长度无效。");
    return;
  }
  server.send(202, "text/html; charset=utf-8",
              "<meta charset='utf-8'><p>正在连接 Wi-Fi，成功后保存配置并自动重启，热点关闭。"
              "请通过串口查看内网 IP，使用该 IP 访问设备。连接失败时热点保留，可重新配置。</p>"
              "<a href='/'>返回状态页面</a>");
  startStation(pendingSsid, pendingPassword);
  connectStarted = millis();
  connecting = true;
}
}

void wifiSetup() {
  WIFI_LOG_PRINTLN("Firmware: " FIRMWARE_BUILD_VERSION);
  // GPIO4 alternates LOW/released to distinguish a jumper from grounded pins.
  digitalWrite(WIFI_RESET_PIN_A, HIGH);
  pinMode(WIFI_RESET_PIN_A, OUTPUT_OPEN_DRAIN);
  pinMode(WIFI_RESET_PIN_B, INPUT_PULLUP);
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      const uint8_t reason = info.wifi_sta_disconnected.reason;
      const char* detail = "See ESP-IDF disconnect reason code";
      switch (reason) {
        case WIFI_REASON_NO_AP_FOUND: detail = "SSID not found by scan"; break;
        case WIFI_REASON_AUTH_FAIL: detail = "Authentication failed (check password/router security)"; break;
        case WIFI_REASON_ASSOC_FAIL: detail = "Router association failed"; break;
        case WIFI_REASON_HANDSHAKE_TIMEOUT: detail = "Security handshake timed out"; break;
        case WIFI_REASON_AUTH_EXPIRE: detail = "Authentication timed out"; break;
        case WIFI_REASON_BEACON_TIMEOUT: detail = "Router signal/beacon lost"; break;
        case WIFI_REASON_ASSOC_LEAVE: detail = "Station disconnected (may be requested by application)"; break;
      }
      WIFI_LOG_PRINTF("STA disconnected: reason=%u, %s\n", reason, detail);
    } else if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
      WIFI_LOG_PRINTLN("STA associated with router; waiting for DHCP IP");
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      WIFI_LOG_PRINTLN("STA received DHCP IP");
    }
  });
  WiFi.persistent(false);
  WiFi.setSleep(false); // Disable Wi-Fi modem power saving for continuous connectivity.
  WiFi.setAutoReconnect(false); // wifiLoop controls the ten-second retry interval.
  storageReady = preferences.begin("wifi-config", false);
  if (!storageReady) WIFI_LOG_PRINTLN("ERROR: Wi-Fi configuration storage could not be opened" );
  savedSsid = preferences.getString("ssid", "");
  savedPassword = preferences.getString("password", "");
  WIFI_LOG_PRINTF("Boot: saved SSID=%s, password length=%u\n", savedSsid.c_str(), static_cast<unsigned>(savedPassword.length()));
  if (savedSsid.isEmpty()) {
    startPortal();
  } else {
    WIFI_LOG_PRINTLN("Boot: station mode only, setup hotspot disabled" );
    WiFi.mode(WIFI_STA);
    startStation(savedSsid, savedPassword);
  }
  httpOn(server, "/", HTTP_GET, handleRoot);
  httpOn(server, "/configure", HTTP_POST, handleConfigure);
  httpOn(server, "/wifi/clear", HTTP_POST, handleClearWifi);
  httpOn(server, "/http", HTTP_GET, handleHttpPage);
  httpOn(server, "/http", HTTP_POST, handleRequestTest);
  server.onNotFound([]() { if (httpRequireAuthentication(server)) server.send(404, "text/plain", "Not found"); });
  wolSetup(server);
  mqttSetup(server);
  otaSetup(server);
  server.begin();
}

void wifiLoop() {
  if (wifiClearPending) {
    if (millis() - wifiClearStarted >= 1000) {
      preferences.end();
      WiFi.disconnect(true, true);
      ESP.restart();
    }
    return;
  }
  // Portal: blue for five seconds; connected: green blinking for ten seconds.
  static bool ledConnected = false;
  static uint8_t lastGreen = 0, lastBlue = 0;
  static uint32_t ledStarted = 0;
  const uint32_t ledNow = millis();
  const bool currentlyConnected = WiFi.status() == WL_CONNECTED;
  if (currentlyConnected && !ledConnected) ledStarted = ledNow;
  if (portalLedActive && ledNow - portalLedStarted >= 5000) portalLedActive = false;
  const uint32_t elapsed = ledNow - ledStarted;
  const bool nextLedOn = currentlyConnected && elapsed < 10000 && (elapsed / 500) % 2 == 0;
  const uint8_t green = !portalLedActive && nextLedOn ? WIFI_LED_GREEN_BRIGHTNESS : 0;
  const uint8_t blue = portalLedActive ? WIFI_LED_BLUE_BRIGHTNESS : 0;
  if (green != lastGreen || blue != lastBlue) {
    neopixelWrite(RGB_BUILTIN, 0, green, blue);
    lastGreen = green;
    lastBlue = blue;
  }
  ledConnected = currentlyConnected;
  uint32_t now = millis();
  digitalWrite(WIFI_RESET_PIN_A, LOW);
  delayMicroseconds(50);
  const bool followsLow = digitalRead(WIFI_RESET_PIN_B) == LOW;
  digitalWrite(WIFI_RESET_PIN_A, HIGH); // Release the open-drain output.
  delayMicroseconds(50);
  const bool followsHigh = digitalRead(WIFI_RESET_PIN_B) == HIGH;
  if (followsLow && followsHigh) {
    if (!resetHeld) {
      resetHeld = true;
      resetStarted = now;
    } else if (now - resetStarted >= RESET_HOLD_MS) {
      WIFI_LOG_PRINTLN("Erasing Wi-Fi settings and restarting setup portal");
      preferences.clear();
      WiFi.disconnect(true, true);
      delay(100);
      ESP.restart();
    }
  } else {
    resetHeld = false;
  }
  server.handleClient();
  if (wifiClearPending) return;
  // HTTP handlers may update connectStarted/lastRetry. Sample time afterwards
  // to prevent unsigned underflow and an immediate false timeout.
  now = millis();
  if (connecting) {
    if (WiFi.status() == WL_CONNECTED) {
      // Write SSID last so incomplete first-time configuration is not loaded.
      size_t passwordWritten = preferences.putString("password", pendingPassword);
      size_t ssidWritten = 0;
      if (passwordWritten == pendingPassword.length()) {
        ssidWritten = preferences.putString("ssid", pendingSsid);
      }
      // Close and reopen NVS, then verify the exact values a reboot will load.
      preferences.end();
      storageReady = preferences.begin("wifi-config", false);
      const bool savedCorrectly = storageReady &&
          preferences.getString("ssid", "") == pendingSsid &&
          preferences.isKey("password") &&
          preferences.getString("password", "") == pendingPassword;
      if (passwordWritten != pendingPassword.length() || ssidWritten != pendingSsid.length() || !savedCorrectly) {
        WIFI_LOG_PRINTLN("Failed to save Wi-Fi settings; setup portal remains available");
        preferences.clear();
        connecting = false;
        WiFi.disconnect(false, false);
        return;
      }
      savedSsid = pendingSsid;
      savedPassword = pendingPassword;
      pendingPassword = "";
      connecting = false;
      provisioning = false;
      WiFi.softAPdisconnect(true);
      WIFI_LOG_PRINTLN("Boot: station mode only, setup hotspot disabled" );
    WiFi.mode(WIFI_STA);
      WIFI_LOG_PRINTF("Wi-Fi connected. Device status: http://%s:34567/\n", WiFi.localIP().toString().c_str());
      preferences.end();
      WIFI_LOG_PRINTLN("Wi-Fi settings saved. Restarting into station mode...");
#if WIFI_SERIAL_DEBUG
      Serial.flush();
#endif
      delay(200);
      ESP.restart();
      return;
    } else if (now - connectStarted >= CONNECT_TIMEOUT_MS) {
      connecting = false;
      pendingPassword = "";
      WiFi.disconnect(false, false);
      WIFI_LOG_PRINTF("Connection timed out after %lu ms; configure Wi-Fi again at http://192.168.1.1:34567/\n", static_cast<unsigned long>(now - connectStarted));
    }
  } else if (!provisioning && WiFi.status() != WL_CONNECTED && now - lastRetry >= RECONNECT_INTERVAL_MS) {
    startStation(savedSsid, savedPassword);
  }
  static bool wasConnected = false;
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) {
    WIFI_LOG_PRINTF("Device status: http://%s:34567/\n", WiFi.localIP().toString().c_str());
  }
  if (!connected && wasConnected && !provisioning) {
    lastRetry = now;
    WIFI_LOG_PRINTLN("Wi-Fi disconnected; retrying every 10 seconds" );
  }
  wasConnected = connected;
}

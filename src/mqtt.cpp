#include "http_auth.h"
#include "mqtt.h"
#include "device_restart.h"
#include "ota.h"
#include "mqtt_config.h"
#include "wifi_config.h"
#include "wol.h"
#include <WiFi.h>
#include <Preferences.h>
#include <mqtt_client.h>
#include <atomic>
#include <cstring>

namespace {
WebServer* web = nullptr;
Preferences storage;
bool storageReady = false;
esp_mqtt_client_handle_t client = nullptr;
String topic = BEMFA_DEFAULT_TOPIC;
String host = BEMFA_HOST;
String privateKey = BEMFA_PRIVATE_KEY;
uint16_t port = BEMFA_PORT;
struct StoredConfiguration {
  uint32_t version;
  char host[254];
  char privateKey[129];
  char topic[65];
  uint16_t port;
};
bool validConfiguration(const String& server, unsigned serverPort,
                        const String& key, const String& switchTopic) {
  if (server.isEmpty() || server.length() > 253 || serverPort < 1 || serverPort > 65535 ||
      key.isEmpty() || key.length() > 128 || switchTopic.length() < 4 ||
      switchTopic.length() > 64 || !switchTopic.endsWith("006")) return false;
  if (server[0] == '.' || server[server.length() - 1] == '.') return false;
  for (unsigned i = 0; i < server.length(); ++i) {
    const char c = server[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
  }
  for (unsigned i = 0; i < key.length(); ++i) if (key[i] <= 32 || key[i] >= 127) return false;
  for (unsigned i = 0; i < switchTopic.length(); ++i) {
    const char c = switchTopic[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
  }
  return true;
}
bool saveConfiguration(const String& server, uint16_t serverPort,
                       const String& key, const String& switchTopic) {
  if (!storageReady || !validConfiguration(server, serverPort, key, switchTopic)) return false;
  StoredConfiguration value = {};
  value.version = 1;
  server.toCharArray(value.host, sizeof(value.host));
  key.toCharArray(value.privateKey, sizeof(value.privateKey));
  switchTopic.toCharArray(value.topic, sizeof(value.topic));
  value.port = serverPort;
  if (storage.putBytes("config", &value, sizeof(value)) != sizeof(value)) return false;
  StoredConfiguration verified = {};
  return storage.getBytes("config", &verified, sizeof(verified)) == sizeof(verified) &&
         memcmp(&value, &verified, sizeof(value)) == 0;
}
std::atomic<bool> connected{false};
std::atomic<bool> subscribed{false};
std::atomic<int> command{-1};
bool switchOn = false;
bool reportPending = false;
String lastResult = "尚未收到开关指令";
uint32_t lastStart = 0;
bool attemptedStart = false;

String escape(String text) {
  text.replace("&", "&amp;"); text.replace("<", "&lt;");
  text.replace(">", "&gt;"); text.replace("\"", "&quot;"); text.replace("'", "&#39;");
  return text;
}
void event(void*, esp_event_base_t, int32_t id, void* data) {
  auto* info = static_cast<esp_mqtt_event_handle_t>(data);
  if (id == MQTT_EVENT_CONNECTED) {
    connected = true;
    subscribed = false;
    if (!topic.isEmpty()) esp_mqtt_client_subscribe(client, topic.c_str(), 1);
    WIFI_LOG_PRINTLN("Bemfa MQTT connected");
  } else if (id == MQTT_EVENT_SUBSCRIBED) {
    subscribed = true;
    WIFI_LOG_PRINTLN("Bemfa switch topic subscribed");
  } else if (id == MQTT_EVENT_DISCONNECTED) {
    connected = false;
    subscribed = false;
    WIFI_LOG_PRINTLN("Bemfa MQTT disconnected; reconnect interval 10 seconds");
  } else if (id == MQTT_EVENT_DATA && info->current_data_offset == 0 &&
             info->data_len == info->total_data_len &&
             info->topic_len == static_cast<int>(topic.length()) &&
             memcmp(info->topic, topic.c_str(), topic.length()) == 0) {
    // Commands are short and must match exactly; ignore unrelated payloads.
    if (info->data_len == 2 && memcmp(info->data, "on", 2) == 0) command = 1;
    else if (info->data_len == 3 && memcmp(info->data, "off", 3) == 0) command = 0;
    else if (!info->retain && info->data_len == 6 && memcmp(info->data, "update", 6) == 0) command = 2;
  } else if (id == MQTT_EVENT_ERROR) {
    WIFI_LOG_PRINTLN("Bemfa MQTT transport/protocol error (credentials are not logged)");
  }
}
void stopClient() {
  if (client) {
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    client = nullptr;
  }
  connected = false;
  subscribed = false;
  command = -1;
}
void startClient() {
  esp_mqtt_client_config_t options = {};
  options.host = host.c_str();
  options.port = port;
  options.client_id = privateKey.c_str();
  options.transport = MQTT_TRANSPORT_OVER_TCP;
  options.keepalive = 60;
  options.reconnect_timeout_ms = 10000;
  options.network_timeout_ms = 5000;
  options.buffer_size = 512;
  client = esp_mqtt_client_init(&options);
  if (!client) { WIFI_LOG_PRINTLN("MQTT initialization failed"); return; }
  if (esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, event, nullptr) != ESP_OK ||
      esp_mqtt_client_start(client) != ESP_OK) {
    esp_mqtt_client_destroy(client);
    client = nullptr;
    WIFI_LOG_PRINTLN("MQTT start failed");
  }
}
void page() {
  String html = "<!doctype html><html lang='zh-CN'><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>巴法云 MQTT</title><body><h1>巴法云开关</h1><p><a href='/'>设备状态</a></p>"
    "<p>服务器：" + escape(host) + ":" + String(port) + "</p><p>MQTT：";
  html += connected ? "已连接" : "未连接（掉线每 10 秒重试）";
  html += "</p><p>主题订阅：" + String(subscribed ? "已确认" : "未订阅或等待确认") + "</p>";
  html += "<p>开关状态：" + String(switchOn ? "on" : "off") + "（指令状态，不代表电脑实际电源状态）</p>";
  html += "<p>最近执行结果：" + escape(lastResult) + "</p>";
  html += "<form method='post' action='/mqtt'>"
    "<p>服务器域名 <input name='host' maxlength='253' value='" + escape(host) + "' required></p>"
    "<p>端口 <input type='number' name='port' min='1' max='65535' value='" + String(port) + "' required></p>"
    "<p>私钥 <input type='password' name='privateKey' maxlength='128' autocomplete='new-password' "
    "placeholder='留空保留已保存的私钥'></p>"
    "<p>开关主题 "
    "<input name='topic' maxlength='64' placeholder='例如 computer006' value='" + escape(topic) + "' required></p>"
    "<p>先在巴法云控制台创建同名 MQTT 开关主题，仅字母数字，以 006 结尾。</p>"
    "<button>保存并重启设备</button></form>"
    "<p>on 唤醒所有已保存的 WOL 设备；off 仅更新状态，WOL 不支持关机。</p>"
    "<p><a href='/wol'>管理 WOL 设备</a> · <a href='/ota'>OTA 固件升级</a></p></body></html>";
  web->sendHeader("Cache-Control", "no-store");
  web->send(200, "text/html; charset=utf-8", html);
}
void configure() {
  String newTopic = web->arg("topic"), newHost = web->arg("host"), newKey = web->arg("privateKey");
  newTopic.trim(); newHost.trim();
  if (newKey.isEmpty()) newKey = privateKey;
  const String portText = web->arg("port");
  unsigned newPort = 0;
  bool portValid = !portText.isEmpty() && portText.length() <= 5;
  for (unsigned i = 0; i < portText.length() && portValid; ++i) {
    if (portText[i] < '0' || portText[i] > '9') portValid = false;
    else newPort = newPort * 10 + portText[i] - '0';
  }
  if (!portValid || !validConfiguration(newHost, newPort, newKey, newTopic)) {
    web->send(400, "text/plain; charset=utf-8", "请填写有效域名（不含协议或端口）、1–65535 端口及私钥；主题为字母数字，以 006 结尾，长度 4–64 字节。");
    return;
  }
  // A single NVS blob commits all connection fields together.
  if (!saveConfiguration(newHost, newPort, newKey, newTopic)) {
    web->send(500, "text/plain; charset=utf-8", "MQTT 配置保存或读取校验失败。"); return;
  }
  // Keep the running client's strings intact until reboot. Do not stop/destroy
  // the MQTT task inside an HTTP handler, where it can block the main loop.
  web->sendHeader("Cache-Control", "no-store");
  web->sendHeader("Connection", "close");
  web->send(200, "text/html; charset=utf-8",
    "<!doctype html><meta charset='utf-8'><p>巴法云配置已保存并校验，设备将在约 1 秒后重启。</p>"
    "<p>重新联网后使用新配置连接巴法云。</p><a href='/mqtt'>返回 MQTT 页面</a>");
  deviceScheduleRestart();
}
}

void mqttSetup(WebServer& server) {
  web = &server;
  storageReady = storage.begin("bemfa-mqtt", false);
  bool loaded = false;
  if (storageReady && storage.getBytesLength("config") == sizeof(StoredConfiguration)) {
    StoredConfiguration value = {};
    if (storage.getBytes("config", &value, sizeof(value)) == sizeof(value) && value.version == 1 &&
        memchr(value.host, 0, sizeof(value.host)) && memchr(value.privateKey, 0, sizeof(value.privateKey)) &&
        memchr(value.topic, 0, sizeof(value.topic)) &&
        validConfiguration(value.host, value.port, value.privateKey, value.topic)) {
      host = value.host; port = value.port; privateKey = value.privateKey; topic = value.topic;
      loaded = true;
    }
  }
  if (!loaded) {
    // Migrate the previous topic-only setting and persist the initial defaults.
    const String legacyTopic = storage.getString("topic", BEMFA_DEFAULT_TOPIC);
    if (validConfiguration(host, port, privateKey, legacyTopic)) topic = legacyTopic;
    if (!saveConfiguration(host, port, privateKey, topic))
      WIFI_LOG_PRINTLN("MQTT defaults could not be persisted; using RAM defaults");
  }
  httpOn(server, "/mqtt", HTTP_GET, page);
  httpOn(server, "/mqtt", HTTP_POST, configure);
}

void mqttLoop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (client) stopClient();
    attemptedStart = false;
    return;
  }
  const uint32_t now = millis();
  if (!client && (!attemptedStart || now - lastStart >= 10000)) {
    attemptedStart = true;
    lastStart = now;
    startClient(); // Network operations run in the ESP-IDF MQTT task.
  }
  const int received = command.exchange(-1);
  if (received == 2) {
    lastResult = otaRequestCloudUpdate() ? "已接受巴法云 OTA 升级任务" : "OTA 正忙或 Wi-Fi 未连接";
  } else if (received >= 0) {
    switchOn = received == 1;
    if (switchOn) {
      unsigned count = 0, sent = 0;
      for (unsigned i = 0; i < 16; ++i) {
        if (wolDeviceName(i).isEmpty()) continue;
        ++count;
        if (wolWakeDevice(i)) ++sent;
      }
      lastResult = count == 0 ? String("没有已保存的 WOL 设备") :
          "已发送 " + String(sent) + " / " + String(count) + " 台设备的唤醒包";
    } else lastResult = "开关设为 off（不发送关机指令）";
    reportPending = true;
  }
  if (reportPending && client && connected) {
    // /up updates cloud state without echoing the command back to subscribers.
    if (esp_mqtt_client_publish(client, (topic + "/up").c_str(), switchOn ? "on" : "off", 0, 1, 0) >= 0)
      reportPending = false;
  }
}
const char* mqttPrivateKey() { return privateKey.c_str(); }

String mqttTopic() { return topic; }

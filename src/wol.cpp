#include "http_auth.h"
#include "wol.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <cstring>

namespace {
constexpr unsigned MAX_DEVICES = 16;
struct Device {
  uint8_t used;
  uint8_t mac[6];
  uint8_t broadcast[4];
  char name[49];
  uint16_t port;
};
struct Configuration {
  uint32_t version;
  Device devices[MAX_DEVICES];
};
Configuration config = {};
Preferences storage;
bool storageReady = false;
WebServer* web = nullptr;

String escape(String value) {
  value.replace("&", "&amp;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  value.replace("\"", "&quot;");
  value.replace("'", "&#39;");
  return value;
}
int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
bool parseMac(const String& text, uint8_t mac[6]) {
  if (text.length() != 17 || (text[2] != ':' && text[2] != '-')) return false;
  bool nonzero = false;
  for (unsigned i = 0; i < 6; ++i) {
    const unsigned offset = i * 3;
    const int high = hexDigit(text[offset]), low = hexDigit(text[offset + 1]);
    if (high < 0 || low < 0 || (i < 5 && text[offset + 2] != text[2])) return false;
    mac[i] = (high << 4) | low;
    nonzero |= mac[i] != 0;
  }
  return nonzero && !(mac[0] & 1); // Reject multicast/broadcast target MACs.
}
bool number(const String& value, unsigned minimum, unsigned maximum, unsigned& result) {
  if (value.isEmpty() || value.length() > 5) return false;
  unsigned parsed = 0;
  for (unsigned i = 0; i < value.length(); ++i) {
    if (value[i] < '0' || value[i] > '9') return false;
    parsed = parsed * 10 + value[i] - '0';
  }
  if (parsed < minimum || parsed > maximum) return false;
  result = parsed;
  return true;
}
bool parseIp(const String& text, IPAddress& ip) {
  uint8_t parts[4];
  int start = 0;
  for (unsigned i = 0; i < 4; ++i) {
    const int end = i == 3 ? text.length() : text.indexOf('.', start);
    unsigned value;
    if (end < start || !number(text.substring(start, end), 0, 255, value)) return false;
    parts[i] = value;
    start = end + 1;
  }
  ip = IPAddress(parts);
  return parts[0] != 0 && (parts[0] < 224 || uint32_t(ip) == 0xFFFFFFFF);
}
String macText(const uint8_t mac[6]) {
  char value[18];
  snprintf(value, sizeof(value), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(value);
}
bool save(const Configuration& next) {
  if (!storageReady || storage.putBytes("devices", &next, sizeof(next)) != sizeof(next)) return false;
  config = next;
  return true;
}
void result(int status, const String& message) {
  web->sendHeader("Cache-Control", "no-store");
  web->send(status, "text/html; charset=utf-8",
    "<!doctype html><meta charset='utf-8'><p>" + escape(message) +
    "</p><a href='/wol'>返回 WOL 管理</a>");
}
void redirect() {
  web->sendHeader("Location", "/wol");
  web->send(303, "text/plain", "");
}
bool selected(unsigned& index) {
  if (!number(web->arg("id"), 0, MAX_DEVICES - 1, index) || !config.devices[index].used) {
    result(404, "设备不存在。");
    return false;
  }
  return true;
}
void page() {
  IPAddress broadcast(255, 255, 255, 255);
  if (WiFi.status() == WL_CONNECTED) {
    const IPAddress ip = WiFi.localIP(), mask = WiFi.subnetMask();
    broadcast = IPAddress(uint32_t(ip) | ~uint32_t(mask));
  }
  String html = "<!doctype html><html lang='zh-CN'><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>WOL 管理</title><body><h1>WOL 设备管理</h1><p><a href='/'>设备状态</a></p>"
    "<p>最多保存 16 台设备，断电后保留。广播地址填写目标网段的广播 IP，通常不是路由器网关 IP。</p>";
  if (!storageReady) html += "<p>配置存储不可用，无法保存或删除。</p>";
  html += "<table border='1' cellpadding='6'><tr><th>名称</th><th>MAC</th><th>广播地址</th><th>UDP 端口</th><th>操作</th></tr>";
  unsigned count = 0;
  for (unsigned i = 0; i < MAX_DEVICES; ++i) {
    const Device& device = config.devices[i];
    if (!device.used) continue;
    ++count;
    html += "<tr><td>" + escape(String(device.name)) + "</td><td>" + macText(device.mac) +
      "</td><td>" + IPAddress(device.broadcast).toString() + "</td><td>" + String(device.port) + "</td><td>";
    html += "<form method='post' action='/wol/send'><input type='hidden' name='id' value='" + String(i) +
      "'><button>唤醒</button></form><form method='post' action='/wol/delete' "
      "onsubmit='return confirm(\"确认删除设备？\")'><input type='hidden' name='id' value='" + String(i) +
      "'><button>删除</button></form></td></tr>";
  }
  html += "</table><p>已保存 " + String(count) + " / 16 台</p>"
    "<h2>添加设备</h2><form method='post' action='/wol/add'>"
    "<p>名称 <input name='name' maxlength='48' required></p>"
    "<p>网卡 MAC <input name='mac' placeholder='AA:BB:CC:DD:EE:FF' maxlength='17' required></p>"
    "<p>广播地址 <input name='broadcast' value='" + broadcast.toString() + "' maxlength='15' required></p>"
    "<p>UDP 端口 <input type='number' name='port' value='9' min='1' max='65535' required></p>"
    "<button>添加并保存</button></form>"
    "<p>发送成功仅代表唤醒包已发出，不代表目标已开机。</p></body></html>";
  web->sendHeader("Cache-Control", "no-store");
  web->send(200, "text/html; charset=utf-8", html);
}
void add() {
  Device device = {};
  String name = web->arg("name"), mac = web->arg("mac"), destination = web->arg("broadcast");
  name.trim(); mac.trim(); destination.trim();
  unsigned port;
  IPAddress ip;
  if (name.isEmpty() || name.length() > 48 || !parseMac(mac, device.mac) ||
      !parseIp(destination, ip) || !number(web->arg("port"), 1, 65535, port)) {
    result(400, "名称最多 48 字节；请填写有效 MAC、IPv4 广播地址和 1–65535 的端口。");
    return;
  }
  int slot = -1;
  for (unsigned i = 0; i < MAX_DEVICES; ++i) {
    if (!config.devices[i].used && slot < 0) slot = i;
    if (config.devices[i].used && memcmp(config.devices[i].mac, device.mac, 6) == 0 &&
        memcmp(config.devices[i].broadcast, &ip[0], 4) == 0 && config.devices[i].port == port) {
      result(409, "相同 MAC、广播地址和端口的设备已存在。");
      return;
    }
  }
  if (slot < 0) { result(409, "设备数量已达上限，请先删除设备。"); return; }
  device.used = 1;
  memcpy(device.name, name.c_str(), name.length());
  for (unsigned i = 0; i < 4; ++i) device.broadcast[i] = ip[i];
  device.port = port;
  Configuration next = config;
  next.devices[slot] = device;
  if (!save(next)) { result(500, "保存失败，设备列表未更新。"); return; }
  redirect();
}
void removeDevice() {
  unsigned index;
  if (!selected(index)) return;
  Configuration next = config;
  next.devices[index] = Device{};
  if (!save(next)) { result(500, "删除保存失败，设备列表未更新。"); return; }
  redirect();
}
void sendDevice() {
  unsigned index;
  if (!selected(index)) return;
  if (WiFi.status() != WL_CONNECTED) { result(503, "请先连接联网 Wi-Fi。"); return; }
  const Device& device = config.devices[index];
  if (!wolSend(device.mac, IPAddress(device.broadcast), device.port)) {
    result(502, "唤醒包发送失败。"); return;
  }
  result(200, "已发送唤醒包至 " + IPAddress(device.broadcast).toString() + ":" +
    String(device.port) + "，目标 " + macText(device.mac) + "。此结果不确认目标开机。");
}
}

bool wolSend(const uint8_t mac[6], const IPAddress& destination, uint16_t port) {
  if (!mac || !port || WiFi.status() != WL_CONNECTED) return false;
  uint8_t packet[102];
  memset(packet, 0xFF, 6);
  for (unsigned i = 0; i < 16; ++i) memcpy(packet + 6 + i * 6, mac, 6);
  WiFiUDP udp;
  if (!udp.begin(0)) return false;
  bool success = udp.beginPacket(destination, port) == 1;
  if (success) {
    success = udp.write(packet, sizeof(packet)) == sizeof(packet);
    if (success) success = udp.endPacket() == 1;
  }
  udp.stop();
  return success;
}

void wolSetup(WebServer& server) {
  web = &server;
  storageReady = storage.begin("wol-config", false);
  config.version = 1;
  if (storageReady && storage.getBytesLength("devices") == sizeof(config)) {
    Configuration loaded = {};
    if (storage.getBytes("devices", &loaded, sizeof(loaded)) == sizeof(loaded) && loaded.version == 1) {
      config = loaded;
      for (Device& device : config.devices) device.name[sizeof(device.name) - 1] = '\0';
    }
  }
  httpOn(server, "/wol", HTTP_GET, page);
  httpOn(server, "/wol/add", HTTP_POST, add);
  httpOn(server, "/wol/delete", HTTP_POST, removeDevice);
  httpOn(server, "/wol/send", HTTP_POST, sendDevice);
}
String wolDeviceName(unsigned index) {
  return index < MAX_DEVICES && config.devices[index].used ? String(config.devices[index].name) : String();
}
bool wolWakeDevice(unsigned index) {
  if (index >= MAX_DEVICES || !config.devices[index].used) return false;
  const Device& device = config.devices[index];
  return wolSend(device.mac, IPAddress(device.broadcast), device.port);
}
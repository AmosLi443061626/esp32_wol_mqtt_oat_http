# ESP32 WOL / 巴法云 MQTT / OTA

## 路由

- `http://设备IP:34567/`：Wi-Fi 和设备状态。
- `/wol`：保存、删除和唤醒 WOL 设备。
- `/http`：外网 HTTP/HTTPS GET 测试。
- `/mqtt`：MQTT 连接状态、域名/端口/私钥/主题配置和最近唤醒结果。
- `/ota`：浏览器附件上传升级。

## 巴法云

1. 在巴法云控制台创建 MQTT 开关主题 `esp32wol006`（006 为开关类型）。
2. 固件使用 `mqtt.bemfa.com:9501`，本地 `src/mqtt_config.h` 中的私钥作为 Client ID。
3. 收到主题消息 `on` 时，对 `/wol` 中所有已保存设备各发送一次 Magic Packet。
4. 收到 `off` 时只更新逻辑开关状态；WOL 不能远程关机。
5. 指令状态通过 `主题/up` 回传，避免向本设备回送指令；状态不表示电脑实际开机状态。
6. MQTT 掉线后 10 秒重试。Wi-Fi 未连接时暂停 MQTT，联网后重新连接。
7. 可在 `/mqtt` 修改服务器域名、端口、私钥和主题，一起保存到 NVS，启动时读取，断电保留。私钥输入框留空保留当前值，不在网页或日志中输出。首次启动从 mqtt_config.h 载入默认值并持久保存，兼容之前的主题配置。

## OTA 附件升级

第一次安装需 USB 烧录。之后访问 `/ota`，所有 HTTP 页面和操作使用统一 HTTP Basic 登录，用户名 `admin`，密码 `admin`。账号密码在 src/http_auth.h 中修改，与巴法云私钥独立。
上传本项目构建生成的 `.pio/build/esp32-s3-devkitc-1/firmware.bin`，不要上传 bootloader、分区表或合并镜像。
当前 `default_16MB.csv` 包含 app0/app1 OTA 双分区。升级成功后自动重启；Wi-Fi、WOL、MQTT 配置保留。
支持内网附件上传、HTTP/HTTPS URL 下载和巴法云远程升级。上传页面使用 HTTP Basic 登录，应在可信内网使用。

## 凭据和模块

- `src/mqtt.cpp` / `mqtt.h`：ESP-IDF 异步 MQTT，重连、开关指令和批量 WOL。
- `src/ota.cpp` / `ota.h`：认证、固件写入和延迟重启。
- `src/mqtt_config.h`：本地凭据，已加入 `.gitignore`。移植项目时可从 `mqtt_config.example.h` 创建。

## 验证

PlatformIO 编译通过。实机需检查 MQTT 连接/订阅状态，发送 on/off、路由器掉线恢复、附件升级后的重启及配置保留。

参考：https://cloud.bemfa.com/docs/src/mqtt.html 、https://cloud.bemfa.com/docs/src/api_device.html
主页显示编译版本号和编译时间。版本号在 src/firmware_version.h 中定义，编译时间每次重新编译 wifi.cpp 时更新。

## 远程 OTA（1.2.0）

- `/ota` 输入直接返回 firmware.bin 的 http:// 或 https:// URL，点击下载并升级。
- 巴法云：在当前 MQTT 主题的 OTA 页面上传 firmware.bin，再发送非保留消息 `update`。固件使用已保存的私钥和主题调用 firmwareVersion API（deviceType=1、secure=true），读取固件 URL 并升级。
- 也可以在 `/ota` 点击“获取云端固件并升级”。
- MQTT update 在主循环处理，避免在 MQTT 事件任务中执行 Flash 升级；保留消息 update 被忽略，防止重启后反复升级。
- HTTPS 验证公共 CA 证书，升级前通过 NTP 同步时间；证书或时间同步失败会停止升级。不支持自签名证书。
- URL 必须直接返回固件（HTTP 200 和 Content-Length）；不跟随重定向。
- 写入备用 OTA 分区，成功才安排重启；失败保留当前固件并在 /ota 显示结果。Wi-Fi/WOL/MQTT 的 NVS 配置保留。
- 升级和时间同步期间主循环暂时阻塞，网页及 GPIO 检测暂时暂停。MQTT 网络任务独立运行。
- 首次需要用已有的附件 OTA 或 USB 安装支持远程 OTA 的版本，再开始使用云端 update 指令。

TLS 根证书包：data/ota_ca_bundle.bin，从本机 certifi 的 Mozilla 公共根证书集生成；生成器 tools/gen_crt_bundle.py 来自 ESP-IDF v4.4.7。根证书更新后重新生成并编译固件。
Wi-Fi 每次连接成功后，板载 RGB 灯以 500 ms 间隔闪烁暗绿色，10 秒后熄灭；掉线立即熄灭。

## 回到配网模式

主页点击清除 Wi-Fi 配置，或将 GPIO4 与 GPIO5 直接短接持续 5 秒，即清除 Wi-Fi 并重启进入配网。
GPIO4 为开漏检测输出，GPIO5 为上拉输入；仅检测两脚直接短接，接地不会触发。
移除短接后连接 ESP32-Setup-xxxx 热点，访问 http://192.168.1.1:34567/，登录 admin/admin 重新配网。
WOL 和 MQTT 配置保留。

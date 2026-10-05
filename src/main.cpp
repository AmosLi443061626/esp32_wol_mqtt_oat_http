#include <Arduino.h>
#include "wifi_config.h"
#include "mqtt.h"
#include "ota.h"
#include "watchdog.h"
#include "device_restart.h"

void setup() {
#if WIFI_SERIAL_DEBUG
  Serial.begin(115200);
#endif
  neopixelWrite(RGB_BUILTIN, 0, 0, 0);
  watchdogSetup();
  wifiSetup();
}

void loop() {
  if (!deviceRestartPending()) wifiLoop();
  if (!deviceRestartPending()) {
    mqttLoop();
    otaLoop();
  }
  deviceRestartLoop();
  watchdogFeed();
  delay(50);
}

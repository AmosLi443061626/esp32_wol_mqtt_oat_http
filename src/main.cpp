#include <Arduino.h>
#include "wifi_config.h"
#include "mqtt.h"
#include "ota.h"
#include "watchdog.h"

void setup() {
#if WIFI_SERIAL_DEBUG
  Serial.begin(115200);
#endif
  neopixelWrite(RGB_BUILTIN, 0, 0, 0);
  watchdogSetup();
  wifiSetup();
}

void loop() {
  wifiLoop();
  mqttLoop();
  otaLoop();
  watchdogFeed();
  delay(50);
}

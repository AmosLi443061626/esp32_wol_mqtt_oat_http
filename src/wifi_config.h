#pragma once
#include <Arduino.h>

// Short GPIO4 to GPIO5 for five seconds to erase Wi-Fi settings.
// GPIO4: open-drain probe output. GPIO5: input with internal pull-up.
constexpr uint8_t WIFI_RESET_PIN_A = 4;
constexpr uint8_t WIFI_RESET_PIN_B = 5;
// RGB green brightness: 0 (off) to 255 (full brightness).
constexpr uint8_t WIFI_LED_GREEN_BRIGHTNESS = 8;
constexpr uint8_t WIFI_LED_BLUE_BRIGHTNESS = 8;
void wifiSetup();
void wifiLoop();
// Set to 1 temporarily when serial diagnostics are needed.
#ifndef WIFI_SERIAL_DEBUG
#define WIFI_SERIAL_DEBUG 1
#endif
#if WIFI_SERIAL_DEBUG
#define WIFI_LOG_PRINTF(...) Serial.printf(__VA_ARGS__)
#define WIFI_LOG_PRINTLN(...) Serial.println(__VA_ARGS__)
#else
#define WIFI_LOG_PRINTF(...) ((void)0)
#define WIFI_LOG_PRINTLN(...) ((void)0)
#endif

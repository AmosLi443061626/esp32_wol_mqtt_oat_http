#include "watchdog.h"
#include "watchdog_config.h"
#include "wifi_config.h"
#include <Arduino.h>
#include <esp_task_wdt.h>

namespace {
bool active = false;
uint32_t lastFeed = 0;
TaskHandle_t monitoredTask = nullptr;
[[noreturn]] void restartAfterFailure(const char* operation, esp_err_t error) {
  WIFI_LOG_PRINTF("Watchdog %s failed: %d; restarting\n", operation, error);
  active = false;
  ESP.restart();
  for (;;) delay(1000);
}
}
bool watchdogSetup() {
  esp_err_t error = esp_task_wdt_init(WATCHDOG_TIMEOUT_SECONDS, true);
  if (error != ESP_OK) {
    restartAfterFailure("initialization", error);
  }
  monitoredTask = xTaskGetCurrentTaskHandle();
  if (esp_task_wdt_status(monitoredTask) != ESP_OK) {
    error = esp_task_wdt_add(monitoredTask);
    if (error != ESP_OK) {
      restartAfterFailure("task registration", error);
    }
  }
  error = esp_task_wdt_reset();
  if (error != ESP_OK) restartAfterFailure("initial feed", error);
  active = true;
  lastFeed = millis();
  WIFI_LOG_PRINTF("Watchdog: timeout=%lu s, feed interval=%lu ms\n",
                  static_cast<unsigned long>(WATCHDOG_TIMEOUT_SECONDS),
                  static_cast<unsigned long>(WATCHDOG_FEED_INTERVAL_MS));
  return active;
}
void watchdogFeed() {
  if (!active || xTaskGetCurrentTaskHandle() != monitoredTask) return;
  const uint32_t now = millis();
  if (now - lastFeed < WATCHDOG_FEED_INTERVAL_MS) return;
  const esp_err_t error = esp_task_wdt_reset();
  if (error != ESP_OK) restartAfterFailure("feed", error);
  lastFeed = millis();
}
bool watchdogIsActive() { return active; }
uint32_t watchdogLastFeedMillis() { return lastFeed; }

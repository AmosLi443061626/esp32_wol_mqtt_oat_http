#include "device_restart.h"
#include <Arduino.h>

namespace {
bool pending = false;
uint32_t requestedAt = 0;
}
void deviceScheduleRestart() {
  if (pending) return;
  requestedAt = millis();
  pending = true;
}
bool deviceRestartPending() { return pending; }
void deviceRestartLoop() {
  // Give the HTTP response time to reach the browser before resetting.
  if (pending && millis() - requestedAt >= 1000) ESP.restart();
}

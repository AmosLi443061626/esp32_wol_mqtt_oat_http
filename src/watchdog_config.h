#pragma once
#include <stdint.h>

// Timeout before a stalled main task triggers a panic/restart.
constexpr uint32_t WATCHDOG_TIMEOUT_SECONDS = 60;
// Feed interval during normal operation and progressing OTA transfers.
constexpr uint32_t WATCHDOG_FEED_INTERVAL_MS = 2000;
static_assert(WATCHDOG_TIMEOUT_SECONDS > 0, "Watchdog timeout must be positive");
static_assert(WATCHDOG_FEED_INTERVAL_MS > 0 &&
              WATCHDOG_FEED_INTERVAL_MS < uint64_t(WATCHDOG_TIMEOUT_SECONDS) * 1000,
              "Feed interval must be shorter than watchdog timeout");

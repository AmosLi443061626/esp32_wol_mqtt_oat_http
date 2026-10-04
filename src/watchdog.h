#pragma once
#include <stdint.h>
bool watchdogSetup();
// Call only from the monitored main task, including long operations making progress.
void watchdogFeed();
bool watchdogIsActive();
uint32_t watchdogLastFeedMillis();

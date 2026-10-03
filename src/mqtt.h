#pragma once
#include <WebServer.h>
void mqttSetup(WebServer& server);
void mqttLoop();
// Current configured key for OTA authentication; never render in HTML.
const char* mqttPrivateKey();

String mqttTopic();

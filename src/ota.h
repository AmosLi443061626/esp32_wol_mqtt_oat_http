#pragma once
#include <WebServer.h>
void otaSetup(WebServer& server);
void otaLoop();
// Called from the main loop, never the MQTT event task.
bool otaRequestCloudUpdate();

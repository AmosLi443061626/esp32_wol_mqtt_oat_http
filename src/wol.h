#pragma once
#include <Arduino.h>
#include <IPAddress.h>
#include <WebServer.h>

// Registers /wol and its POST actions on the existing HTTP server.
void wolSetup(WebServer& server);
// Success means the UDP packet was sent, not that the PC has booted.
bool wolSend(const uint8_t mac[6], const IPAddress& destination, uint16_t port = 9);
String wolDeviceName(unsigned index);
bool wolWakeDevice(unsigned index);

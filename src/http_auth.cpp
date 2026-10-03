#include "http_auth.h"
bool httpCredentialsValid(WebServer& server) {
  return server.authenticate(HTTP_USERNAME, HTTP_PASSWORD);
}
bool httpRequireAuthentication(WebServer& server) {
  if (httpCredentialsValid(server)) return true;
  server.sendHeader("Cache-Control", "no-store");
  server.requestAuthentication(BASIC_AUTH, "ESP32", "Authentication required");
  return false;
}
void httpOn(WebServer& server, const char* uri, HTTPMethod method,
            WebServer::THandlerFunction handler, WebServer::THandlerFunction upload) {
  WebServer* web = &server;
  auto guarded = [web, handler]() {
    if (httpRequireAuthentication(*web)) handler();
  };
  if (upload) {
    server.on(uri, method, guarded, [web, upload]() {
      // Authentication is checked before any upload bytes reach Update.write.
      // The request completion handler sends the single HTTP challenge.
      if (httpCredentialsValid(*web)) upload();
    });
  } else server.on(uri, method, guarded);
}
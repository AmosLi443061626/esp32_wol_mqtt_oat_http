#pragma once
#include <WebServer.h>
constexpr const char* HTTP_USERNAME = "admin";
constexpr const char* HTTP_PASSWORD = "admin";
bool httpCredentialsValid(WebServer& server);
bool httpRequireAuthentication(WebServer& server);
void httpOn(WebServer& server, const char* uri, HTTPMethod method,
            WebServer::THandlerFunction handler, WebServer::THandlerFunction upload = nullptr);
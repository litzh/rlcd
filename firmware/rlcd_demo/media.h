#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <cJSON.h>
void mediaSetup();
void mediaRoutes(WebServer &server);
void mediaAfterHttp();
void buttonsRoutes(WebServer &server);
cJSON *mediaAudioStatus();
cJSON *mediaSDStatus();
cJSON *mediaButtonsStatus();
void mediaKeyClick();
void mediaKeyDouble();
void mediaStartButtons(void (*bootClick)(), void (*bootLong)());
void mediaScreen(String &state, String &file, String &detail);

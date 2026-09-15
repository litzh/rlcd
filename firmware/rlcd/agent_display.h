#pragma once
#include <U8g2lib.h>
#include <WebServer.h>
#include <cJSON.h>
void agentRoutes(WebServer &server);
void agentDraw(U8G2 &display, const String &ip, bool provisioning);
bool agentTakeFocus();
cJSON *agentStatus();
bool agentStandby();

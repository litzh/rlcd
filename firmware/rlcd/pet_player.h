#pragma once
#include <U8g2lib.h>
#include <WebServer.h>
void petSetup();
void petRoutes(WebServer &server);
void petDraw(U8G2 &display, const String &state);
void petTransition();
int petSound(const String &state);
bool petTakeFocus();
int petSelectTemporary(const String &id);
String petCurrentId();

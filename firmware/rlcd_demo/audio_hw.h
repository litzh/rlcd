#pragma once
#include <stddef.h>
#include <stdint.h>
bool audioHardwareInit();
bool audioHardwareStart(bool recording, int volume);
bool audioHardwareRead(int16_t *stereo, size_t bytes);
bool audioHardwareWrite(int16_t *stereo, size_t bytes);
void audioHardwareStop(bool recording);
bool audioHardwareVolume(int volume);

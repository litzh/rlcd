#pragma once
#include <stdint.h>
// Pure debounce/click state machine; update at least every 10–20 ms.
struct ButtonLogic {
  bool raw = false, pressed = false, longSent = false, pending = false;
  uint32_t changed = 0, down = 0, released = 0;
  uint32_t longMs = 1000;
  // 0 none, 1 single click, 2 double click, 3 long press.
  int update(bool low, uint32_t now) {
    if (low != raw) {
      raw = low;
      changed = now;
    }
    if (raw != pressed && uint32_t(now - changed) >= 30) {
      pressed = raw;
      if (pressed) {
        down = now;
        longSent = false;
      } else if (!longSent) {
        if (pending && uint32_t(now - released) <= 350) {
          pending = false;
          return 2;
        }
        pending = true;
        released = now;
      }
    }
    if (pressed && !longSent && uint32_t(now - down) >= longMs) {
      longSent = true;
      pending = false;
      return 3;
    }
    if (!pressed && pending && uint32_t(now - released) > 350) {
      pending = false;
      return 1;
    }
    return 0;
  }
};

#include "../firmware/rlcd_demo/button_logic.h"
#include <cassert>
#include <cstdio>
int main() {
  ButtonLogic b;
  assert(!b.update(true, 10));
  assert(!b.update(false, 20));
  assert(!b.update(true, 25));
  assert(!b.update(true, 60));
  assert(b.pressed);
  assert(!b.update(false, 100));
  assert(!b.update(false, 140));
  assert(b.update(false, 491) == 1);
  assert(!b.update(false, 510));
  b = {};
  b.update(true, 1);
  b.update(true, 35);
  b.update(false, 70);
  b.update(false, 105);
  b.update(true, 160);
  b.update(true, 195);
  b.update(false, 220);
  assert(b.update(false, 255) == 2);
  assert(!b.update(false, 700));
  b = {};
  b.longMs = 3000;
  b.update(true, 1);
  b.update(true, 35);
  assert(!b.update(true, 3034));
  assert(b.update(true, 3035) == 3);
  assert(!b.update(true, 4000));
  b.update(false, 4010);
  assert(!b.update(false, 4050));
  assert(!b.update(false, 4500));
  b = {};
  b.update(true, 0xfffffff0);
  b.update(true, 20);
  b.update(false, 50);
  b.update(false, 90);
  assert(b.update(false, 441) == 1);
  puts("PASS debounce, single/double click, long press suppression, clock wrap");
}

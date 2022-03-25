// MIT license, see LICENSE_MIT in Silice repo root
// @sylefeb 2021
// https://github.com/sylefeb/Silice

#include "config.h"

volatile int go  = 0;
volatile int red = 0;

static inline int core_id()
{
   unsigned int cycles;
   asm volatile ("rdcycle %0" : "=r"(cycles));
   return cycles&1;
}

__attribute__((section(".data"))) void main_core0()
{
  while (1) {
    red = 16;
    for (int i=0;i<65536*2;i++) { }
    red = 0;
    for (int i=0;i<65536*2;i++) { }
  }
}

void main_core1()
{
  int l = 1;
  while (1) {
    l <<= 1;
    if (l > 8) {
      l = 1;
    }
    *LEDS = l | red;
    for (int i=0;i<65536;i++) { }
  }
}

void main()
{
  if (core_id()) {
    go = 1;            // sync core 0
    main_core1();
  } else {
    while (go == 0) {} // wait for core 1
    main_core0();
  }
}

#pragma once
#include <Arduino.h>

namespace battery {
void begin();
void loop();
float volts();
uint8_t percent();   // 0..100
bool usbPowered();   // VBUS present
}  // namespace battery

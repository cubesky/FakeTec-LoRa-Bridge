#pragma once
#include <Arduino.h>

namespace display {
void powerCycleRail();  // call first thing in setup()
bool begin();             // false if no OLED found on I2C
void loop();
void wake();
bool isOn();
void nextPage();
void toast(const char* msg, uint32_t ms = 2000);
const char* info();                       // driver / address / detection result
size_t scan(uint8_t* found, size_t max);  // I2C bus scan
}  // namespace display

#pragma once
#include <Arduino.h>

namespace ble {

void begin();          // always enables the SoftDevice; advertises only if cfg.bleEnable
void loop();
bool connected();
bool uartReady();      // central subscribed to NUS TX notifications

int uartAvailable();
size_t uartRead(uint8_t* buf, size_t max);
size_t uartWrite(const uint8_t* buf, size_t len);
size_t uartChunk();    // max bytes per notification

bool cfgPop(char* line, size_t max);  // next command line from the config characteristic
void cfgReply(const char* s, size_t n);
Print& cfgPrint();

// pairing (LE Secure Connections numeric comparison, confirmed with the button)
bool pairPending();        // waiting for the button
bool pairShowing();        // a passkey should be on the display
const char* pairCode();    // 6 digits
void pairConfirm(bool ok);
uint8_t takePairEvent();   // 0 = none, 1 = paired, 2 = failed
void clearBonds();

void setBattery(uint8_t pct);
void disconnect();

}  // namespace ble

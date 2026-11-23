#pragma once
#include <Arduino.h>

namespace at {

// Per-port state for a transparent data port with "+++" escape into command mode.
struct Port {
  const char* name;
  Print* out;
  bool cmdMode = false;
  uint8_t esc = 0;          // number of '+' held back
  uint32_t lastIn = 0;      // time of last received byte
  uint32_t escT = 0;        // time the 3rd '+' arrived
  uint32_t lastCmd = 0;     // last activity in command mode
  char line[160];
  uint8_t len = 0;
  // in-band control channel (works in data mode, no guard time):
  //   host -> device : ESC ] L B ; <AT command> BEL
  //   device -> host : ESC ] L B ; <response text> BEL
  uint8_t osc = 0;          // matched prefix bytes held back
  bool oscBody = false;
  uint32_t oscT = 0;
  char oline[160];
  uint8_t olen = 0;
};

static const uint32_t GUARD_MS = 1000;
static const uint32_t CMD_IDLE_EXIT_MS = 300000;

// Data mode: filter incoming bytes, returns bytes to forward over LoRa (out >= n + 8).
size_t filter(Port& p, const uint8_t* in, size_t n, uint8_t* out);
// Timers: completes "+++" escape, releases held bytes. Returns bytes to forward (out >= 8).
size_t poll(Port& p, uint8_t* out);
// Command mode input (line editing + execute).
void feed(Port& p, const uint8_t* in, size_t n);
// Execute one command line; port may be nullptr (BLE config characteristic).
void execute(const char* line, Print& out, Port* port);

}  // namespace at

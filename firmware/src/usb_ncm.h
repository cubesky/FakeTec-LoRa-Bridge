#pragma once
#include <Arduino.h>

// USB CDC-NCM network function ("ETH" mode, see eth.h).
//
// A small class driver of its own (TinyUSB application driver): one NTB16 buffer in each
// direction, the Ethernet frames are handed over to / taken from the main loop, and the
// media state can be dropped and raised again so that the host repeats DHCP.
// The function is part of the configuration descriptor only while it is exposed; exposing or
// hiding it re-enumerates the whole device (the serial port comes back with the same number).
namespace ncm {

void begin(const uint8_t mac[6], bool exposed);  // once in setup(), before USB is re-attached
void expose(bool on);       // show / hide the network function (re-enumerates ~0.5 s later)
bool exposed();
bool reenumerating();
bool ready();               // the host has configured and activated the data interface
void bounceLink();          // media disconnect, then connect: the host asks for its address again
void setRxHandler(void (*cb)(const uint8_t* frame, size_t len));  // Ethernet frame from the host
bool send(const uint8_t* frame, size_t len);                      // Ethernet frame to the host
void loop();
bool busy();                // frames waiting in either direction: the main loop should not sleep

struct Stats {
  uint32_t rxFrames, txFrames, txDropped, badNtb;
};
const Stats& stats();

}  // namespace ncm

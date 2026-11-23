#pragma once
#include <Arduino.h>

// "FAST" mode: the data stream moves from LoRa to a 2.4 GHz ESB-style link on the
// nRF52840's own radio (see rf24.h). It is negotiated over LoRa (AT+FAST=1), only while
// neither end has a Bluetooth connection, and falls back to LoRa on its own when the
// 2.4 GHz link gets bad. LoRa keeps running underneath as the control channel.
namespace fast {

enum State : uint8_t { OFF, SCAN, REQ, PROBE, UP, ENDING };

const char* request();     // start the negotiation; nullptr = started, else why not
void release();            // back to LoRa (AT+FAST=0)
void loop();

State state();
bool isUp();               // data is flowing over 2.4 GHz
bool negotiating();        // scan / request / probing
const char* stateName();
const char* lastReason();  // why the last session ended / was refused ("" = none yet)
int rssi();                // dBm, average of the last window
uint8_t lossPct();         // packet loss of the last window
float freqMHz();           // channel of the last exchange
uint8_t channels();        // size of the hop set
uint8_t channelsInUse();
bool busy();               // bytes in flight: the main loop should not sleep
bool holdData();           // LoRa must not send user data now
void stat(Print& o);       // AT+STAT lines

// ---- hooks for lora_link.cpp
size_t ctlPoll(uint8_t* buf, size_t max);    // a control frame to send over LoRa now (0 = none)
void onCtl(const uint8_t* pl, size_t n);     // control frame received over LoRa

}  // namespace fast

#pragma once
#include <Arduino.h>

// 2.4 GHz link on the nRF52840's own radio ("FAST" mode).
//
// ESB-style: 2 Mbit/s proprietary PHY, 5 byte address, 24 bit CRC, hopping over the 8
// quietest channels found by an energy scan, one side (PTX / master)
// polls, the other (PRX / slave) answers every poll with a packet that may carry data as
// well, so the two ends never collide with each other. Both directions are acknowledged
// with a sequence / next-expected-sequence bit pair in the packet header, which gives
// loss-free, in-order, flow-controlled byte streams.
//
// The SoftDevice stays enabled, so the radio is used through the Timeslot API (Bluetooth
// advertising is stopped by fast.cpp while the link runs).
namespace rf24 {

#define RF24_MAX_CH 8      // channels in the hop set
#define RF24_SCAN_N 37     // candidate channels of the energy scan

struct Params {
  bool master;
  uint8_t nch;
  uint8_t ch[RF24_MAX_CH];  // 2400 + ch MHz
  uint32_t addrBase;
  uint8_t addrPrefix;
  uint32_t crcInit;
  int8_t txPower;           // dBm
};

struct Stats {
  uint32_t good;     // valid packets received from the peer
  uint32_t miss;     // master: polls without an answer; slave: polls that never arrived
  uint32_t crcErr;
  uint32_t polls;    // master: polls sent
  uint32_t slots;    // radio timeslots granted
  uint32_t rssiSum;  // sum of -RSSI over the good packets
};

// energy scan of the candidate channels (runs in the background, ~80 ms)
bool scanStart();
bool scanDone();
uint8_t scanChannel(uint8_t i);  // candidate i -> channel number
int8_t scanLevel(uint8_t i);     // strongest signal seen on candidate i, dBm

bool start(const Params& p);
void stop();                     // returns with the radio released
bool running();
void pause(bool on);             // give the radio up for a moment (flash writes need the gap)
void loop();

size_t txSpace();
size_t txWrite(const uint8_t* d, size_t n);
size_t txPending();              // written but not yet acknowledged by the peer
uint32_t txAcked();              // bytes acknowledged since start()
size_t txLeftover(uint8_t* d, size_t max);  // after stop(): the unacknowledged bytes

size_t rxAvailable();
size_t rxRead(uint8_t* d, size_t max);
uint32_t rxTotal();              // bytes accepted since start()

void stats(Stats& s);
uint8_t channel();               // channel of the last exchange
uint8_t channelMask();           // channels of the hop set in use (bit i = ch[i])
int8_t lastRssi();

}  // namespace rf24

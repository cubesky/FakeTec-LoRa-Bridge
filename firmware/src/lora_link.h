#pragma once
#include <Arduino.h>

enum LinkState : uint8_t {
  LINK_UNPAIRED = 0,  // no peer: broadcast mode on the base frequency
  LINK_SEARCHING,     // paired, peer not heard / hop clock not synchronised
  LINK_UP,            // paired and synchronised
};

struct LinkStats {
  uint32_t txFrames = 0, rxFrames = 0;
  uint32_t txBytes = 0, rxBytes = 0;    // user payload bytes delivered
  uint32_t txWire = 0;                  // bytes actually sent for them (after compression)
  uint32_t retries = 0, lost = 0, crcErr = 0, dup = 0, lbtBusy = 0;
  uint32_t hops = 0, syncs = 0;
  int16_t rssi = 0;                     // last packet from peer, as seen locally
  float snr = 0;
  int16_t remoteRssi = 0;               // our signal as seen by the peer
  float remoteSnr = 0;
  uint8_t remoteBatt = 255;             // 255 = unknown
  uint32_t txRate = 0, rxRate = 0;      // payload bytes / second
  uint32_t txLong = 0;                  // frames sent with the long wake-up preamble
  uint32_t txAirMs = 0;                 // total time on air of everything sent
  uint32_t rxOnMs = 0;                  // standby: time the receiver was continuously on
  uint16_t sbHbMiss = 0, sbSlotMiss = 0, sbRetry = 0, sbFallback = 0, sbReset = 0;
  int16_t sbSlotErr = 0;                // slave: last heartbeat start minus expected, ms
  uint8_t sbSync = 0;                   // standby schedule: bit0 heartbeat answered, bit1 slot known
};

namespace link {

bool begin();              // initialise radio with cfg (call after configLoad / BLE init)
bool applyConfig();        // re-initialise radio after a config change
void loop();

bool timeCritical();
void setActive(bool active);   // a local serial port (USB / BLE UART) is in use
const char* modeName();        // UNPR / PAIR / STBY / LINK / SRCH / NOOP
bool isConn();                 // false = power-save standby (duty-cycled RX, slow heartbeat)
bool powerSave();
void setPowered(bool p);        // external (USB) power: keep the receiver on in standby
void waitIrq(uint32_t ms);      // sleep up to ms, return early on a radio interrupt
size_t queueSpace();
size_t queued();
size_t queue(const uint8_t* data, size_t len);  // serial -> LoRa
void setRxHandler(void (*cb)(const uint8_t* data, size_t len));  // LoRa -> serial

void startPairing(uint32_t durationMs);
void stopPairing();
bool isPairing();
uint32_t pairRemainingS();
void unpair();

LinkState state();
bool radioOk();
const char* radioStatus();
const char* chipInfo();     // version string read from the chip + BUSY level
bool hopping();
uint8_t channel();
uint16_t channelMask();     // AFH blacklist (bit = channel excluded)
float freqMHz();
uint32_t dwellMs();
bool isMaster();
uint32_t lastHeardAgoMs();
uint32_t txActivityMs();    // millis() of last TX start
uint32_t rxActivityMs();    // millis() of last valid RX
const LinkStats& stats();
void resetStats();
void setLocalBattery(uint8_t pct);
float channelFreqMHz(uint8_t ch);
uint32_t random32();

// ---- FAST mode (2.4 GHz bypass, see fast.h): the user data stream leaves / re-enters here
size_t fastTake(uint8_t* buf, size_t max);       // next bytes to send; 0 while a LoRa data frame is still in flight
void fastRequeue(const uint8_t* d, size_t n);    // bytes that did not make it: back to the front of the queue
void fastDeliver(const uint8_t* d, size_t n);    // bytes received over 2.4 GHz
void setFastRxMux(bool mux);                     // ... are a framed stream (told by the peer for the session)

// ---- ETH mode (see eth.h): IP packets share the reliable byte stream with the serial data
void setMux(bool want);        // frame the stream (takes effect once the queue has drained)
bool muxActive();              // the stream we send is framed now
bool queuePacket(const uint8_t* ip, size_t len);  // IP packet for the peer; false = dropped
void setNetHandler(void (*cb)(const uint8_t* ip, size_t len));  // IP packet from the peer

}  // namespace link

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

}  // namespace link

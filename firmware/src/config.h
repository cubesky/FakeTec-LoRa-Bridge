#pragma once
#include <Arduino.h>

#define CFG_MAGIC   0x4C4F5241u  // "LORA"
#define CFG_VERSION 5

struct Config {
  uint32_t magic;
  uint16_t version;
  uint16_t size;

  // ---- radio ----
  uint32_t freqHz;        // base / sync channel frequency
  uint16_t bwX10;         // bandwidth in kHz * 10 (1250 = 125 kHz)
  uint8_t sf;             // 5..12
  uint8_t cr;             // 5..8  (4/5 .. 4/8)
  int8_t txPower;         // dBm, -9..22
  uint8_t syncWord;       // LoRa sync word
  uint16_t preamble;      // symbols
  uint16_t tcxoMv;        // TCXO voltage in mV, 0 = crystal

  // ---- frequency hopping / interference ----
  uint8_t hopEnable;      // FHSS when paired
  uint8_t hopChannels;    // 2..16
  uint16_t hopSpacingKHz; // channel spacing
  uint16_t hopDwellMs;    // 0 = auto
  uint8_t afhEnable;      // adaptive channel blacklisting
  uint8_t lbtEnable;      // listen before talk (preamble/header detect)
  int8_t lbtRssi;         // extra RSSI CCA threshold in dBm, 0 = off

  // ---- link ----
  uint32_t peerId;        // 0 = unpaired (broadcast mode)
  uint8_t key[16];        // AES-128 key
  uint8_t encrypt;
  uint8_t ack;            // ARQ on/off
  uint8_t retries;
  uint8_t maxPayload;     // bytes of user data per LoRa frame
  uint16_t packTimeoutMs; // serial idle time before sending a partial frame
  uint16_t heartbeatS;    // keepalive interval

  // ---- BLE ----
  uint8_t bleEnable;
  char bleName[24];
  uint32_t blePin;        // 0 = no passkey, else 6 digit static passkey

  // ---- UI ----
  uint16_t oledTimeoutS;  // 0 = always on
  uint8_t oledType;       // 0 = SSD1306, 1 = SH1106
  uint8_t oledFlip;
  uint16_t battCalX1000;  // battery voltage calibration factor * 1000
  uint8_t outputs;        // MOSFET T1..T3 state bits

  // ---- added in v3 ----
  uint8_t powerSave;      // 1 = standby until a serial port is in use
  uint8_t usbActive;      // what counts as "USB serial in use": 1 = plugged into a host, 0 = port opened (DTR)
  uint8_t blePair;        // 1 = BLE needs pairing confirmed with the button
  uint16_t standbyHbS;    // heartbeat interval in standby
  uint8_t compress;       // LZ-compress data frames when it saves air time

  // ---- added in v4: FAST mode (2.4 GHz link on the nRF52840 radio)
  uint8_t fastEnable;     // 1 = AT+FAST may be used / accepted from the peer
  uint8_t fastLoss;       // fall back to LoRa above this packet loss, %
  int8_t fastRssi;        // ... or below this RSSI, dBm
  int8_t fastPower;       // 2.4 GHz TX power, dBm (-40..8)

  // ---- added in v5: ETH mode (USB CDC-NCM network adapter, see eth.h). These are shared with
  // the peer: a change on one end is sent to the other.
  uint8_t ethEnable;
  uint8_t ethPrefix;      // subnet prefix length, 8..30
  uint32_t ethMasterIp;   // address of the master's host (a.b.c.d = a << 24 | b << 16 | c << 8 | d)
  uint32_t ethSlaveIp;    // address of the slave's host
  uint32_t ethRev;        // revision of the four above: the higher one wins when the ends differ

  uint32_t crc;
};

extern Config cfg;
extern uint32_t g_nodeId;

void configDefaults(Config& c);
void configLoad();
bool configSave();
float cfgBandwidthKHz();
uint32_t crc32(const void* data, size_t len, uint32_t crc = 0);

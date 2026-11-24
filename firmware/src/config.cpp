#include "config.h"

#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>

#include "rf24.h"

using namespace Adafruit_LittleFS_Namespace;

static const char* CFG_PATH = "/lorabridge.cfg";

Config cfg;
uint32_t g_nodeId = 0;

uint32_t crc32(const void* data, size_t len, uint32_t crc) {
  const uint8_t* p = (const uint8_t*)data;
  crc = ~crc;
  while (len--) {
    crc ^= *p++;
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

float cfgBandwidthKHz() {
  // RadioLib needs the exact SX126x bandwidth values
  switch (cfg.bwX10) {
    case 78: return 7.8f;
    case 104: return 10.4f;
    case 156: return 15.6f;
    case 208: return 20.8f;
    case 312: return 31.25f;
    case 417: return 41.7f;
    case 625: return 62.5f;
    default: return cfg.bwX10 / 10.0f;
  }
}

void configDefaults(Config& c) {
  memset(&c, 0, sizeof(c));
  c.magic = CFG_MAGIC;
  c.version = CFG_VERSION;
  c.size = sizeof(Config);

  c.freqHz = 433000000;
  c.bwX10 = 1250;
  c.sf = 7;
  c.cr = 5;
  c.txPower = 17;
  c.syncWord = 0x12;
  c.preamble = 8;
  c.tcxoMv = 0;  // Ra-01S on FakeTec: clock runs without DIO3 TCXO control (verified, all TCXO voltages fail)

  c.hopEnable = 1;
  c.hopChannels = 8;
  c.hopSpacingKHz = 200;
  c.hopDwellMs = 0;
  c.afhEnable = 1;
  c.lbtEnable = 1;
  c.lbtRssi = 0;

  c.peerId = 0;
  c.encrypt = 1;
  c.ack = 1;
  c.retries = 4;
  c.maxPayload = 200;
  c.packTimeoutMs = 20;
  c.heartbeatS = 3;

  c.bleEnable = 1;
  snprintf(c.bleName, sizeof(c.bleName), "LoRaBridge-%04X", (unsigned)(g_nodeId & 0xFFFF));
  c.blePin = 0;

  c.oledTimeoutS = 120;
  c.oledType = 0;
  c.oledFlip = 0;
  c.battCalX1000 = 1000;
  c.outputs = 0;

  c.powerSave = 1;
  c.usbActive = 1;
  c.blePair = 1;
  c.standbyHbS = 60;
  c.compress = 1;

  c.fastEnable = 1;
  c.fastLoss = 30;
  c.fastRssi = -85;
  c.fastPower = 8;

  c.ethEnable = 0;
  c.ethPrefix = 30;
  c.ethMasterIp = (10u << 24) | (211u << 16) | 1;  // 10.211.0.1
  c.ethSlaveIp = (10u << 24) | (211u << 16) | 2;   // 10.211.0.2
  c.ethRev = 0;
}

// Older files have the identical layout up to their last field, followed by the CRC:
// v2 ends with "outputs", v3 with "compress", v4 with "fastPower".
#define CFG_V2_BODY (offsetof(Config, outputs) + 1)
#define CFG_V3_BODY (offsetof(Config, compress) + 1)
#define CFG_V4_BODY (offsetof(Config, fastPower) + 1)

bool configSave();
// keeps pairing + settings, the fields the old version did not have stay at their defaults
static bool loadOld(const Config& tmp, size_t n, uint16_t version, size_t body) {
  size_t crcOfs = (body + 3) & ~(size_t)3, size = crcOfs + 4;
  if (n != size || tmp.version != version || tmp.size != size) return false;
  uint32_t crc;
  memcpy(&crc, (const uint8_t*)&tmp + crcOfs, 4);
  if (crc != crc32(&tmp, crcOfs)) return false;
  memcpy(&cfg, &tmp, body);
  cfg.version = CFG_VERSION;
  cfg.size = sizeof(Config);
  cfg.bleName[sizeof(cfg.bleName) - 1] = 0;
  configSave();
  return true;
}
void configLoad() {
  g_nodeId = NRF_FICR->DEVICEID[0] ^ (NRF_FICR->DEVICEID[1] * 2654435761u);
  if (g_nodeId == 0 || g_nodeId == 0xFFFFFFFFu) g_nodeId = 0x13572468u;

  configDefaults(cfg);
  InternalFS.begin();

  File f(InternalFS);
  if (!f.open(CFG_PATH, FILE_O_READ)) return;
  Config tmp;
  size_t n = f.read(&tmp, sizeof(tmp));
  f.close();
  if (n == sizeof(tmp) && tmp.magic == CFG_MAGIC && tmp.version == CFG_VERSION &&
      tmp.size == sizeof(Config) && tmp.crc == crc32(&tmp, offsetof(Config, crc))) {
    cfg = tmp;
    cfg.bleName[sizeof(cfg.bleName) - 1] = 0;
  } else if (tmp.magic == CFG_MAGIC) {
    if (!loadOld(tmp, n, 4, CFG_V4_BODY) && !loadOld(tmp, n, 3, CFG_V3_BODY)) loadOld(tmp, n, 2, CFG_V2_BODY);
  }
}

bool configSave() {
  cfg.magic = CFG_MAGIC;
  cfg.version = CFG_VERSION;
  cfg.size = sizeof(Config);
  cfg.crc = crc32(&cfg, offsetof(Config, crc));
  // flash operations need the radio to be idle: FAST mode lets go of its timeslots meanwhile
  rf24::pause(true);
  InternalFS.remove(CFG_PATH);
  File f(InternalFS);
  size_t n = 0;
  if (f.open(CFG_PATH, FILE_O_WRITE)) {
    n = f.write((const uint8_t*)&cfg, sizeof(cfg));
    f.close();
  }
  rf24::pause(false);
  return n == sizeof(cfg);
}

#include "at_cmd.h"

#include "battery.h"
#include "ble_svc.h"
#include "config.h"
#include "display.h"
#include "eth.h"
#include "fast.h"

namespace display { const char* probeInfo(); }
#include "lora_link.h"
#include "pins.h"

extern void applyOutputs();
extern uint32_t g_resetReason;
extern uint8_t g_hangStage;
extern const char* stageName(uint8_t s);
extern uint8_t g_bootMark;
static void printReset(Print& o) {
  o.printf("Boot mark: 0x%02X\r\n", g_bootMark);
  // RESETREAS: bit0 pin, bit1 watchdog, bit2 soft reset, bit3 lockup
  if (g_resetReason & 0x2) o.printf("Last reset: WATCHDOG - main loop was stuck in stage %u (%s)\r\n", g_hangStage, stageName(g_hangStage));
  else o.printf("Last reset: 0x%lX (%s)\r\n", (unsigned long)g_resetReason,
                (g_resetReason & 0x8) ? "CPU lockup" : (g_resetReason & 0x4) ? "software" : (g_resetReason & 0x1) ? "reset pin" : "power on");
}
extern void scheduleReboot(uint8_t mode);  // 0 = reboot, 1 = UF2/serial DFU, 2 = OTA DFU

namespace at {

// ------------------------------------------------------------------ escape handling
static const char OSC_PFX[] = "\x1b]LB;";
#define OSC_PFX_LEN 5

size_t filter(Port& p, const uint8_t* in, size_t n, uint8_t* out) {
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    uint8_t c = in[i];
    uint32_t now = millis();
    // ---- in-band control channel
    if (p.oscBody) {
      p.oscT = now;
      if (c == 0x07) {
        p.oscBody = false;
        p.oline[p.olen] = 0;
        p.out->print(OSC_PFX);
        execute(p.oline, *p.out, &p);
        p.out->write((uint8_t)0x07);
      } else if (p.olen < sizeof(p.oline) - 1) {
        p.oline[p.olen++] = (char)c;
      } else {
        p.oscBody = false;  // too long: drop
      }
      continue;
    }
    if (c == (uint8_t)OSC_PFX[p.osc]) {
      p.oscT = now;
      if (++p.osc == OSC_PFX_LEN) {
        p.osc = 0;
        p.oscBody = true;
        p.olen = 0;
      }
      continue;
    }
    if (p.osc) {  // not our sequence: release what was held back
      for (uint8_t k = 0; k < p.osc; k++) out[o++] = (uint8_t)OSC_PFX[k];
      p.osc = 0;
      if (c == 0x1b) {
        p.osc = 1;
        p.oscT = now;
        continue;
      }
    }
    if (p.esc > 0 && p.esc < 3 && c == '+' && now - p.lastIn < GUARD_MS) {
      if (++p.esc == 3) p.escT = now;
      p.lastIn = now;
      continue;
    }
    if (p.esc > 0) {  // not an escape after all: release held '+'
      for (uint8_t k = 0; k < p.esc; k++) out[o++] = '+';
      p.esc = 0;
    } else if (c == '+' && now - p.lastIn >= GUARD_MS) {
      p.esc = 1;
      p.lastIn = now;
      continue;
    }
    out[o++] = c;
    p.lastIn = now;
  }
  return o;
}

size_t poll(Port& p, uint8_t* out) {
  uint32_t now = millis();
  if (p.oscBody && now - p.oscT > 500) p.oscBody = false;
  if (p.osc && now - p.oscT >= 30) {  // e.g. a lone ESC key: pass it on
    size_t o = p.osc;
    for (uint8_t k = 0; k < p.osc; k++) out[k] = (uint8_t)OSC_PFX[k];
    p.osc = 0;
    return o;
  }
  if (p.cmdMode) {
    if (now - p.lastCmd > CMD_IDLE_EXIT_MS) {
      p.cmdMode = false;
      p.out->print("\r\n+DATA (timeout)\r\n");
    }
    return 0;
  }
  if (p.esc == 3 && now - p.escT >= GUARD_MS) {
    p.esc = 0;
    p.cmdMode = true;
    p.len = 0;
    p.lastCmd = now;
    p.out->print("\r\nOK\r\n");
    return 0;
  }
  if (p.esc > 0 && p.esc < 3 && now - p.lastIn >= GUARD_MS) {
    size_t o = p.esc;
    for (uint8_t k = 0; k < p.esc; k++) out[k] = '+';
    p.esc = 0;
    return o;
  }
  return 0;
}

void feed(Port& p, const uint8_t* in, size_t n) {
  p.lastCmd = millis();
  for (size_t i = 0; i < n; i++) {
    char c = (char)in[i];
    if (c == '\r' || c == '\n') {
      if (p.len) {
        p.line[p.len] = 0;
        p.out->print("\r\n");
        execute(p.line, *p.out, &p);
        p.len = 0;
      }
    } else if (c == 8 || c == 127) {
      if (p.len) {
        p.len--;
        p.out->print("\b \b");
      }
    } else if (c >= 32 && p.len < sizeof(p.line) - 1) {
      p.line[p.len++] = c;
      p.out->write((uint8_t)c);  // echo
    }
  }
}

// ------------------------------------------------------------------ helpers
#define APPLY_NONE 0
#define APPLY_RADIO 1
#define APPLY_REBOOT 2
#define APPLY_OUT 3

struct IntParam {
  const char* name;
  void* ptr;
  char type;  // 'b' int8, 'B' uint8, 'H' uint16, 'I' uint32
  int32_t lo, hi;
  uint8_t apply;
};

static const IntParam kInts[] = {
    {"SF", &cfg.sf, 'B', 5, 12, APPLY_RADIO},
    {"CR", &cfg.cr, 'B', 5, 8, APPLY_RADIO},
    {"PWR", &cfg.txPower, 'b', -9, 22, APPLY_RADIO},
    {"PRE", &cfg.preamble, 'H', 6, 1000, APPLY_RADIO},
    {"HOP", &cfg.hopEnable, 'B', 0, 1, APPLY_RADIO},
    {"HOPCH", &cfg.hopChannels, 'B', 2, 16, APPLY_RADIO},
    {"HOPSP", &cfg.hopSpacingKHz, 'H', 25, 2000, APPLY_RADIO},
    {"DWELL", &cfg.hopDwellMs, 'H', 0, 60000, APPLY_RADIO},
    {"AFH", &cfg.afhEnable, 'B', 0, 1, APPLY_NONE},
    {"LBT", &cfg.lbtEnable, 'B', 0, 1, APPLY_NONE},
    {"LBTRSSI", &cfg.lbtRssi, 'b', -127, 0, APPLY_NONE},
    {"ENC", &cfg.encrypt, 'B', 0, 1, APPLY_NONE},
    {"ACK", &cfg.ack, 'B', 0, 1, APPLY_NONE},
    {"RETRY", &cfg.retries, 'B', 0, 15, APPLY_NONE},
    {"MAXPL", &cfg.maxPayload, 'B', 1, 239, APPLY_RADIO},
    {"PKTTO", &cfg.packTimeoutMs, 'H', 1, 5000, APPLY_NONE},
    {"HB", &cfg.heartbeatS, 'H', 1, 600, APPLY_NONE},
    {"COMP", &cfg.compress, 'B', 0, 1, APPLY_NONE},
    {"PSM", &cfg.powerSave, 'B', 0, 1, APPLY_RADIO},
    {"SBHB", &cfg.standbyHbS, 'H', 5, 3600, APPLY_NONE},
    {"USBACT", &cfg.usbActive, 'B', 0, 1, APPLY_NONE},
    {"BLEPAIR", &cfg.blePair, 'B', 0, 3, APPLY_REBOOT},
    {"BLE", &cfg.bleEnable, 'B', 0, 1, APPLY_REBOOT},
    {"BLEPIN", &cfg.blePin, 'I', 0, 999999, APPLY_REBOOT},
    {"OLED", &cfg.oledTimeoutS, 'H', 0, 65535, APPLY_NONE},
    {"OLEDTYPE", &cfg.oledType, 'B', 0, 1, APPLY_REBOOT},
    {"FLIP", &cfg.oledFlip, 'B', 0, 1, APPLY_REBOOT},
    {"BATCAL", &cfg.battCalX1000, 'H', 500, 1500, APPLY_NONE},
    {"FASTEN", &cfg.fastEnable, 'B', 0, 1, APPLY_NONE},
    {"FASTLOSS", &cfg.fastLoss, 'B', 5, 90, APPLY_NONE},
    {"FASTRSSI", &cfg.fastRssi, 'b', -100, -40, APPLY_NONE},
    {"FASTPWR", &cfg.fastPower, 'b', -40, 8, APPLY_NONE},
};

static int32_t getInt(const IntParam& p) {
  switch (p.type) {
    case 'b': return *(int8_t*)p.ptr;
    case 'B': return *(uint8_t*)p.ptr;
    case 'H': return *(uint16_t*)p.ptr;
    default: return (int32_t) * (uint32_t*)p.ptr;
  }
}
static void setInt(const IntParam& p, int32_t v) {
  switch (p.type) {
    case 'b': *(int8_t*)p.ptr = (int8_t)v; break;
    case 'B': *(uint8_t*)p.ptr = (uint8_t)v; break;
    case 'H': *(uint16_t*)p.ptr = (uint16_t)v; break;
    default: *(uint32_t*)p.ptr = (uint32_t)v; break;
  }
}

static const uint16_t kBw[] = {78, 104, 156, 208, 312, 417, 625, 1250, 2500, 5000};

#ifdef LORA_CHIP_SX1262
static const float F_MIN = 150.0f, F_MAX = 960.0f;
#else
static const float F_MIN = 410.0f, F_MAX = 810.0f;  // SX1268 (Ra-01S module: 410-525 MHz)
#endif

static bool freqPlanOk(uint32_t baseHz, uint8_t ch, uint16_t spKHz) {
  float lo = baseHz / 1e6f;
  float hi = (baseHz + (uint32_t)(ch - 1) * spKHz * 1000u) / 1e6f;
  return lo >= F_MIN && hi <= F_MAX;
}

static bool parseHex(const char* s, uint8_t* out, size_t n) {
  if (strlen(s) != n * 2) return false;
  for (size_t i = 0; i < n; i++) {
    char b[3] = {s[2 * i], s[2 * i + 1], 0};
    char* e;
    out[i] = (uint8_t)strtoul(b, &e, 16);
    if (*e) return false;
  }
  return true;
}

static void ok(Print& o) { o.print("OK\r\n"); }
static void err(Print& o, const char* why) {
  o.print("ERROR: ");
  o.print(why);
  o.print("\r\n");
}

static void printCfg(Print& o) {
  o.printf("+FREQ=%.3f\r\n+BW=%g\r\n+SF=%u\r\n+CR=%u\r\n+PWR=%d\r\n+SYNC=%02X\r\n+PRE=%u\r\n+TCXO=%.1f\r\n",
           cfg.freqHz / 1e6, cfg.bwX10 / 10.0, cfg.sf, cfg.cr, cfg.txPower, cfg.syncWord, cfg.preamble,
           cfg.tcxoMv / 1000.0);
  o.printf("+HOP=%u\r\n+HOPCH=%u\r\n+HOPSP=%u\r\n+DWELL=%u (eff %lu ms)\r\n+AFH=%u\r\n+LBT=%u\r\n+LBTRSSI=%d\r\n",
           cfg.hopEnable, cfg.hopChannels, cfg.hopSpacingKHz, cfg.hopDwellMs, (unsigned long)link::dwellMs(),
           cfg.afhEnable, cfg.lbtEnable, cfg.lbtRssi);
  o.printf("+PEER=%08lX\r\n+ENC=%u\r\n+ACK=%u\r\n+RETRY=%u\r\n+MAXPL=%u\r\n+PKTTO=%u\r\n+HB=%u\r\n",
           (unsigned long)cfg.peerId, cfg.encrypt, cfg.ack, cfg.retries, cfg.maxPayload, cfg.packTimeoutMs,
           cfg.heartbeatS);
  o.printf("+BLE=%u\r\n+NAME=%s\r\n+BLEPIN=%lu\r\n+OLED=%u\r\n+OLEDTYPE=%u\r\n+FLIP=%u\r\n+BATCAL=%u\r\n+OUT=%u\r\n",
           cfg.bleEnable, cfg.bleName, (unsigned long)cfg.blePin, cfg.oledTimeoutS, cfg.oledType, cfg.oledFlip,
           cfg.battCalX1000, cfg.outputs);
  o.printf("+COMP=%u\r\n", cfg.compress);
  o.printf("+PSM=%u\r\n+SBHB=%u\r\n+USBACT=%u\r\n+BLEPAIR=%u\r\n", cfg.powerSave, cfg.standbyHbS, cfg.usbActive,
           cfg.blePair);
  o.printf("+FASTEN=%u\r\n+FASTLOSS=%u\r\n+FASTRSSI=%d\r\n+FASTPWR=%d\r\n", cfg.fastEnable, cfg.fastLoss, cfg.fastRssi,
           cfg.fastPower);
  char net[16], mip[16], sip[16];
  eth::fmtIp(net, sizeof(net), eth::network());
  eth::fmtIp(mip, sizeof(mip), cfg.ethMasterIp);
  eth::fmtIp(sip, sizeof(sip), cfg.ethSlaveIp);
  o.printf("+ETH=%u\r\n+ETHNET=%s/%u\r\n+ETHMIP=%s\r\n+ETHSIP=%s\r\n", cfg.ethEnable ? 1 : 0, net, cfg.ethPrefix, mip, sip);
}

static void printInfo(Print& o) {
  o.printf("FakeTec LoRa Serial Bridge fw %s\r\n", FW_VERSION);
#ifdef LORA_CHIP_SX1262
  o.print("Radio: SX1262 ");
#else
  o.print("Radio: SX1268 ");
#endif
  o.printf("%s\r\nChip: %s\r\nNode ID: %08lX\r\n", link::radioStatus(), link::chipInfo(), (unsigned long)g_nodeId);
  const char* st = link::state() == LINK_UP ? "UP" : link::state() == LINK_SEARCHING ? "SEARCHING" : "UNPAIRED";
  o.printf("Peer: %08lX  link %s%s\r\n", (unsigned long)cfg.peerId, st,
           cfg.peerId ? (link::isMaster() ? " (master)" : " (slave)") : "");
  o.printf("Mode: %s\r\n", link::modeName());
  o.printf("Channel: %u  %.3f MHz  hopping %s  blacklist %04X\r\n", link::channel(), link::freqMHz(),
           link::hopping() ? "yes" : "no", link::channelMask());
  o.printf("Battery: %.2f V %u%% %s\r\n", battery::volts(), battery::percent(),
           battery::usbPowered() ? "(USB)" : "");
  o.printf("BLE: %s\r\n", ble::connected() ? "connected" : "idle");
  if (fast::isUp()) o.printf("Data path: 2.4G FAST  %.0f MHz  RSSI %d  loss %u%%\r\n", fast::freqMHz(), fast::rssi(), fast::lossPct());
  else o.printf("Data path: LoRa%s\r\n", fast::negotiating() ? " (FAST negotiating)" : "");
  if (eth::enabled()) {
    char ip[16];
    eth::fmtIp(ip, sizeof(ip), eth::hostIp());
    o.printf("ETH: on, host %s/%u%s%s\r\n", ip, cfg.ethPrefix, eth::leased() ? " (leased)" : "",
             eth::synced() ? "" : ", waiting for the peer");
  } else {
    o.print("ETH: off\r\n");
  }
  o.printf("OLED: %s\r\n", display::info());
  if (g_resetReason & 0xA) printReset(o);
}

static void printStats(Print& o) {
  const LinkStats& s = link::stats();
  o.printf("TX frames %lu  bytes %lu  rate %lu B/s\r\n", (unsigned long)s.txFrames, (unsigned long)s.txBytes,
           (unsigned long)s.txRate);
  o.printf("RX frames %lu  bytes %lu  rate %lu B/s\r\n", (unsigned long)s.rxFrames, (unsigned long)s.rxBytes,
           (unsigned long)s.rxRate);
  o.printf("Retries %lu  lost %lu  CRC %lu  dup %lu  LBT busy %lu\r\n", (unsigned long)s.retries,
           (unsigned long)s.lost, (unsigned long)s.crcErr, (unsigned long)s.dup, (unsigned long)s.lbtBusy);
  o.printf("RSSI %d SNR %.1f | remote RSSI %d SNR %.1f batt %u\r\n", s.rssi, s.snr, s.remoteRssi, s.remoteSnr,
           s.remoteBatt);
  o.printf("Compression: %lu bytes sent for %lu (%lu%%)\r\n", (unsigned long)s.txWire, (unsigned long)s.txBytes,
           (unsigned long)(s.txBytes ? s.txWire * 100 / s.txBytes : 100));
  o.printf("Hops %lu  syncs %lu  queue %u\r\n", (unsigned long)s.hops, (unsigned long)s.syncs,
           (unsigned)link::queued());
  fast::stat(o);
}

static const char* HELP =
    "AT                 test\r\n"
    "ATI / AT+INFO      device info\r\n"
    "AT+CFG             show all settings\r\n"
    "AT+STAT / AT+RSTAT link statistics / reset\r\n"
    "ATO                back to data mode\r\n"
    "AT+PAIR[=s]        pairing mode (default 60 s)\r\n"
    "AT+UNPAIR          forget peer (broadcast mode)\r\n"
    "AT+PEER=hex8       set peer ID manually\r\n"
    "AT+KEY=hex32       set AES key manually\r\n"
    "AT+FREQ=433.000    base/sync frequency MHz\r\n"
    "AT+BW=125          7.8 10.4 15.6 20.8 31.25 41.7 62.5 125 250 500\r\n"
    "AT+SF=7 AT+CR=5 AT+PWR=17 AT+PRE=8 AT+SYNC=12 AT+TCXO=1.8\r\n"
    "AT+HOP=1 AT+HOPCH=8 AT+HOPSP=200 AT+DWELL=0  frequency hopping\r\n"
    "AT+AFH=1 AT+LBT=1 AT+LBTRSSI=0               anti-interference\r\n"
    "AT+ENC=1 AT+ACK=1 AT+RETRY=4 AT+MAXPL=200 AT+PKTTO=20 AT+HB=3\r\n"
    "AT+COMP=1          compress data frames\r\n"
    "AT+FAST=1 / 0      switch the data to the 2.4 GHz high speed link / back to LoRa\r\n"
    "                   (negotiated over LoRa, no Bluetooth connection on either end,\r\n"
    "                   falls back to LoRa by itself when the 2.4 GHz link is bad)\r\n"
    "AT+FAST?           +FAST=0|1|2 (off, negotiating, on),state,RSSI,loss%,MHz,last reason\r\n"
    "AT+FASTEN=1 AT+FASTLOSS=30 AT+FASTRSSI=-85 AT+FASTPWR=8  allow, fallback limits, 2.4G power\r\n"
    "AT+ETH=1 / 0       network adapter mode on / off, on both ends (USB CDC-NCM + DHCP,\r\n"
    "                   IP between the two hosts over LoRa / 2.4G; the USB device re-enumerates)\r\n"
    "AT+ETHNET=10.211.0.0/30 AT+ETHMIP=10.211.0.1 AT+ETHSIP=10.211.0.2\r\n"
    "                   subnet, address of the master's host, of the slave's host (sent to the peer)\r\n"
    "AT+ETHCFG=net/prefix,masterIP,slaveIP   all three at once      AT+ETHST  status\r\n"
    "AT+PSM=1 AT+SBHB=30 AT+USBACT=1              power save, standby heartbeat s, 1=USB plugged 0=port open\r\n"
    "AT+BLE=1 AT+NAME=xx AT+BLEPIN=0 AT+BLEPAIR=1 (reboot to apply)\r\n"
    "AT+BLEUNBOND       forget all paired phones\r\n"
    "AT+OLED=120 AT+OLEDTYPE=0 AT+FLIP=0 AT+BATCAL=1000 AT+I2C\r\n"
    "AT+OUT=n,v         MOSFET output n=1..3 v=0/1\r\n"
    "AT+DEFAULT AT+REBOOT AT+DFU AT+OTADFU\r\n"
    "Query with AT+XXX?   Escape from data mode: [1s] +++ [1s]\r\n";

// ------------------------------------------------------------------ execute
void execute(const char* lineIn, Print& o, Port* port) {
  char line[160];
  size_t n = 0;
  while (*lineIn == ' ') lineIn++;
  for (; lineIn[n] && n < sizeof(line) - 1; n++) line[n] = lineIn[n];
  while (n && (line[n - 1] == ' ' || line[n - 1] == '\r' || line[n - 1] == '\n')) n--;
  line[n] = 0;
  if (!n) return;

  // command name uppercase up to '=' or '?'
  for (size_t i = 0; i < n && line[i] != '='; i++) line[i] = toupper(line[i]);

  if (strncmp(line, "AT", 2) != 0) return err(o, "unknown");
  const char* c = line + 2;

  if (!*c) return ok(o);
  if (!strcmp(c, "I") || !strcmp(c, "+INFO")) {
    printInfo(o);
    return ok(o);
  }
  if (!strcmp(c, "O") || !strcmp(c, "+EXIT")) {
    if (port) {
      port->cmdMode = false;
      o.print("OK\r\n");
    } else {
      ok(o);
    }
    return;
  }
  if (*c != '+') return err(o, "unknown");
  c++;

  char name[16];
  size_t k = 0;
  while (c[k] && c[k] != '=' && c[k] != '?' && k < sizeof(name) - 1) {
    name[k] = c[k];
    k++;
  }
  name[k] = 0;
  bool query = c[k] == '?';
  const char* val = c[k] == '=' ? c + k + 1 : nullptr;

  if (!strcmp(name, "HELP")) {
    o.print(HELP);
    return ok(o);
  }
  if (!strcmp(name, "CFG")) {
    printCfg(o);
    return ok(o);
  }
  if (!strcmp(name, "STAT")) {
    printStats(o);
    return ok(o);
  }
  if (!strcmp(name, "RSTAT")) {
    link::resetStats();
    return ok(o);
  }
  if (!strcmp(name, "FAST")) {
    if (query || !val) {
      o.printf("+FAST=%u,%s,%d,%u,%.0f,%s\r\nOK\r\n", fast::isUp() ? 2 : (fast::state() != fast::OFF ? 1 : 0),
               fast::stateName(), fast::rssi(), fast::lossPct(), fast::freqMHz(), fast::lastReason());
      return;
    }
    if (atoi(val)) {
      const char* why = fast::request();
      if (why) return err(o, why);
      o.print("OK (negotiating, check with AT+FAST?)\r\n");
      return;
    }
    fast::release();
    return ok(o);
  }
  if (!strcmp(name, "ETH")) {
    if (query || !val) return (void)o.printf("+ETH=%u\r\nOK\r\n", cfg.ethEnable ? 1 : 0);
    bool on = atoi(val) != 0;
    bool was = eth::enabled();
    const char* why = eth::setEnabled(on);
    if (why) return err(o, why);
    if (was != on) display::toast(on ? "ETH MODE ON" : "ETH MODE OFF");
    o.print(was != on ? "OK (USB re-enumerates)\r\n" : "OK\r\n");
    return;
  }
  if (!strcmp(name, "ETHST")) {
    eth::status(o);
    return ok(o);
  }
  if (!strcmp(name, "ETHNET") || !strcmp(name, "ETHMIP") || !strcmp(name, "ETHSIP") || !strcmp(name, "ETHCFG")) {
    char net[16], mip[16], sip[16];
    if (query || !val) {
      eth::fmtIp(net, sizeof(net), eth::network());
      eth::fmtIp(mip, sizeof(mip), cfg.ethMasterIp);
      eth::fmtIp(sip, sizeof(sip), cfg.ethSlaveIp);
      if (name[3] == 'N') o.printf("+ETHNET=%s/%u\r\nOK\r\n", net, cfg.ethPrefix);
      else if (name[3] == 'M') o.printf("+ETHMIP=%s\r\nOK\r\n", mip);
      else if (name[3] == 'S') o.printf("+ETHSIP=%s\r\nOK\r\n", sip);
      else o.printf("+ETHCFG=%s/%u,%s,%s\r\nOK\r\n", net, cfg.ethPrefix, mip, sip);
      return;
    }
    uint32_t m = cfg.ethMasterIp, s = cfg.ethSlaveIp, nw = 0;
    unsigned prefix = cfg.ethPrefix;
    const char* why = nullptr;
    const char* e = val;
    if (name[3] == 'M') {
      if (!eth::parseIp(val, m)) return err(o, "AT+ETHMIP=a.b.c.d");
    } else if (name[3] == 'S') {
      if (!eth::parseIp(val, s)) return err(o, "AT+ETHSIP=a.b.c.d");
    } else {
      // network/prefix
      char* pe;
      if (!eth::parseIp(val, nw, &e) || *e != '/') return err(o, "network as a.b.c.d/prefix");
      prefix = (unsigned)strtoul(e + 1, &pe, 10);
      if (prefix < 8 || prefix > 30) return err(o, "prefix 8..30");
      uint32_t mask = 0xFFFFFFFFu << (32 - prefix);
      nw &= mask;
      if (name[3] == 'N') {
        if (*pe) return err(o, "AT+ETHNET=a.b.c.d/prefix");
        // keep the host part of the two addresses where the new network allows it
        m = nw | (m & ~mask);
        s = nw | (s & ~mask);
        why = eth::setAddresses(m, s, (uint8_t)prefix);
        if (why) why = eth::setAddresses(nw + 1, nw + 2, (uint8_t)prefix);
        if (why) return err(o, why);
        return ok(o);
      }
      if (*pe != ',' || !eth::parseIp(pe + 1, m, &e) || *e != ',' || !eth::parseIp(e + 1, s))
        return err(o, "AT+ETHCFG=a.b.c.d/prefix,masterIP,slaveIP");
      if ((m & mask) != nw || (s & mask) != nw) return err(o, "addresses are not in that network");
    }
    why = eth::setAddresses(m, s, (uint8_t)prefix);
    if (why) return err(o, why);
    return ok(o);
  }
  if (!strcmp(name, "PAIR")) {
    uint32_t s = val ? strtoul(val, nullptr, 10) : 60;
    if (s < 5 || s > 600) return err(o, "range 5..600");
    link::startPairing(s * 1000);
    display::toast("PAIRING");
    return ok(o);
  }
  if (!strcmp(name, "UNPAIR")) {
    link::unpair();
    return ok(o);
  }
  if (!strcmp(name, "PEER")) {
    if (query || !val) return (void)o.printf("+PEER=%08lX\r\nOK\r\n", (unsigned long)cfg.peerId);
    uint32_t id = strtoul(val, nullptr, 16);
    if (id == g_nodeId || id == 0xFFFFFFFFu) return err(o, "invalid id");
    cfg.peerId = id;
    configSave();
    link::applyConfig();
    return ok(o);
  }
  if (!strcmp(name, "KEY")) {
    if (query || !val) {
      o.print("+KEY=");
      for (int i = 0; i < 16; i++) o.printf("%02X", cfg.key[i]);
      o.print("\r\n");
      return ok(o);
    }
    uint8_t key[16];
    if (!parseHex(val, key, 16)) return err(o, "need 32 hex chars");
    memcpy(cfg.key, key, 16);
    configSave();
    link::applyConfig();
    return ok(o);
  }
  if (!strcmp(name, "FREQ")) {
    if (query || !val) return (void)o.printf("+FREQ=%.3f\r\nOK\r\n", cfg.freqHz / 1e6);
    float f = atof(val);
    uint32_t hz = (uint32_t)(f * 1e6f + 0.5f);
    if (!freqPlanOk(hz, cfg.hopEnable ? cfg.hopChannels : 1, cfg.hopSpacingKHz))
      return err(o, "out of band (incl. hop channels)");
    cfg.freqHz = hz;
    configSave();
    link::applyConfig();
    return ok(o);
  }
  if (!strcmp(name, "BW")) {
    if (query || !val) return (void)o.printf("+BW=%g\r\nOK\r\n", cfg.bwX10 / 10.0);
    uint16_t bw = (uint16_t)(atof(val) * 10 + 0.5f);
    bool okbw = false;
    for (uint16_t b : kBw) okbw |= (b == bw || b + 1 == bw);
    if (!okbw) return err(o, "invalid bandwidth");
    for (uint16_t b : kBw)
      if (b + 1 == bw) bw = b;
    cfg.bwX10 = bw;
    configSave();
    link::applyConfig();
    return ok(o);
  }
  if (!strcmp(name, "SYNC")) {
    if (query || !val) return (void)o.printf("+SYNC=%02X\r\nOK\r\n", cfg.syncWord);
    cfg.syncWord = (uint8_t)strtoul(val, nullptr, 16);
    configSave();
    link::applyConfig();
    return ok(o);
  }
  if (!strcmp(name, "TCXO")) {
    if (query || !val) return (void)o.printf("+TCXO=%.1f\r\nOK\r\n", cfg.tcxoMv / 1000.0);
    float v = atof(val);
    if (v < 0 || v > 3.3f) return err(o, "0 (xtal) or 1.6..3.3");
    cfg.tcxoMv = (uint16_t)(v * 1000 + 0.5f);
    configSave();
    link::applyConfig();
    return ok(o);
  }
  if (!strcmp(name, "NAME")) {
    if (query || !val) return (void)o.printf("+NAME=%s\r\nOK\r\n", cfg.bleName);
    if (!*val || strlen(val) >= sizeof(cfg.bleName)) return err(o, "1..23 chars");
    strcpy(cfg.bleName, val);
    configSave();
    o.print("OK (reboot to apply)\r\n");
    return;
  }
  if (!strcmp(name, "OUT")) {
    if (query || !val) return (void)o.printf("+OUT=%u\r\nOK\r\n", cfg.outputs);
    int ch = 0, v = 0;
    if (sscanf(val, "%d,%d", &ch, &v) != 2 || ch < 1 || ch > 3) return err(o, "AT+OUT=1..3,0/1");
    if (v) cfg.outputs |= (1 << (ch - 1));
    else cfg.outputs &= ~(1 << (ch - 1));
    configSave();
    applyOutputs();
    return ok(o);
  }
  if (!strcmp(name, "HW")) {
    static const char* vout[8] = {"1.8V", "2.1V", "2.4V", "2.7V", "3.0V", "3.3V", "?", "1.8V (erased default)"};
    uint32_t reg = NRF_UICR->REGOUT0 & 7;
    o.printf("REGOUT0 (GPIO/VDD voltage): %s  raw=0x%08lX\r\n", vout[reg], (unsigned long)NRF_UICR->REGOUT0);
    o.printf("Regulator mode: %s\r\n", (NRF_POWER->MAINREGSTATUS & 1) ? "high voltage (VDDH)" : "normal (VDD)");
    o.printf("DC/DC: REG1 %s, REG0 %s\r\n", (NRF_POWER->DCDCEN & 1) ? "on" : "off", (NRF_POWER->DCDCEN0 & 1) ? "on" : "off");
    analogReference(AR_INTERNAL);  // 0.6 V ref, gain 1/6 -> 3.6 V full scale
    analogReadResolution(12);
    uint32_t vraw = analogReadVDD();
    analogReference(AR_DEFAULT);
    o.printf("VDD measured: %lu mV\r\n", (unsigned long)(vraw * 3600UL / 4096UL));
    o.printf("NFCPINS: %s  APPROTECT=0x%08lX\r\n", (NRF_UICR->NFCPINS & 1) ? "NFC" : "GPIO",
             (unsigned long)NRF_UICR->APPROTECT);
    o.printf("Bootloader addr: 0x%08lX\r\n", (unsigned long)NRF_UICR->NRFFW[0]);
    if (*(volatile uint32_t*)0x3004 == 0x51B1E5DB)
      o.printf("SoftDevice FWID: 0x%04X\r\n", *(volatile uint16_t*)0x300C);
    // look for the Adafruit/nice!nano UF2 bootloader banner in the bootloader region
    uint32_t bl = NRF_UICR->NRFFW[0];
    const char* hit = nullptr;
    if (bl >= 0xE0000 && bl < 0x100000) {
      for (uint32_t a = bl; a < 0xFE000 - 16 && !hit; a++)
        if (!memcmp((const void*)a, "UF2 Bootloader", 14)) hit = (const char*)a;
    }
    if (hit) {
      char b[64];
      size_t i = 0;
      for (; i < sizeof(b) - 1 && hit[i] >= 32 && hit[i] < 127; i++) b[i] = hit[i];
      b[i] = 0;
      o.printf("Bootloader: %s\r\n", b);
    } else {
      o.print("Bootloader: banner not found\r\n");
    }
    o.printf("FICR: part %lX variant %.4s ram %luK flash %luK\r\n", (unsigned long)NRF_FICR->INFO.PART,
             (const char*)&NRF_FICR->INFO.VARIANT, (unsigned long)NRF_FICR->INFO.RAM,
             (unsigned long)NRF_FICR->INFO.FLASH);
    o.printf("OLED: %s | %s\r\n", display::info(), display::probeInfo());
    printReset(o);
    return ok(o);
  }
  if (!strcmp(name, "I2C")) {
    uint8_t f[16];
    size_t n = display::scan(f, sizeof(f));
    o.print("+I2C=");
    for (size_t i = 0; i < n; i++) o.printf("%s0x%02X", i ? "," : "", f[i]);
    o.print(n ? "\r\n" : "none\r\n");
    return ok(o);
  }
  if (!strcmp(name, "BAT")) {
    o.printf("+BAT=%.2f,%u\r\n", battery::volts(), battery::percent());
    return ok(o);
  }
  if (!strcmp(name, "DEFAULT")) {
    configDefaults(cfg);
    configSave();
    o.print("OK (rebooting)\r\n");
    scheduleReboot(0);
    return;
  }
  if (!strcmp(name, "ST")) {
    // one machine-readable line for clients:
    // mode,role,rssi,snr,batt%,usb,txRate,rxRate,peerBatt,queue,channel,freqMHz,peerRssi,peerSnr,
    // fast (0 = LoRa, 1 = negotiating 2.4G, 2 = data on 2.4G),fastRssi,
    // eth (0 = off, 1 = on, the peer has not followed yet, 2 = on at both ends)
    const LinkStats& s = link::stats();
    o.printf("+ST=%s,%s,%d,%.1f,%u,%u,%lu,%lu,%u,%u,%u,%.3f,%d,%.1f,%u,%d,%u\r\nOK\r\n", link::modeName(),
             cfg.peerId ? (link::isMaster() ? "M" : "S") : "-", s.rssi, s.snr, battery::percent(),
             battery::usbPowered() ? 1 : 0, (unsigned long)s.txRate, (unsigned long)s.rxRate, s.remoteBatt,
             (unsigned)link::queued(), link::channel(), link::freqMHz(), s.remoteRssi, s.remoteSnr,
             fast::isUp() ? 2 : (fast::state() != fast::OFF ? 1 : 0), fast::isUp() ? fast::rssi() : 0,
             eth::enabled() ? (eth::synced() ? 2 : 1) : 0);
    return;
  }
  if (!strcmp(name, "HANG")) {
    // test of the watchdog: block the main loop on purpose
    o.print("OK (blocking the main loop, the watchdog resets in ~12 s)\r\n");
    delay(50);
    for (;;) {
    }
  }
  if (!strcmp(name, "BLECONFIRM")) {
    // same as the short button press on a pairing request (for units without a reachable button)
    if (!ble::pairPending()) return err(o, "no pairing request");
    ble::pairConfirm(!val || atoi(val) != 0);
    o.print("OK\r\n");
    return;
  }
  if (!strcmp(name, "BLEST")) {
    ble::diag(o);
    o.print("OK\r\n");
    return;
  }
  if (!strcmp(name, "BLEUNBOND")) {
    ble::disconnect();
    ble::clearBonds();
    o.print("OK\r\n");
    return;
  }
  if (!strcmp(name, "REBOOT")) {
    o.print("OK\r\n");
    scheduleReboot(0);
    return;
  }
  if (!strcmp(name, "DFU")) {
    o.print("OK (entering UF2 / serial DFU bootloader)\r\n");
    scheduleReboot(1);
    return;
  }
  if (!strcmp(name, "OTADFU")) {
    o.print("OK (entering BLE OTA DFU bootloader)\r\n");
    scheduleReboot(2);
    return;
  }

  for (const IntParam& p : kInts) {
    if (strcmp(name, p.name)) continue;
    if (query || !val) {
      o.printf("+%s=%ld\r\nOK\r\n", p.name, (long)getInt(p));
      return;
    }
    char* e;
    long v = strtol(val, &e, 10);
    if (*e || v < p.lo || v > p.hi) {
      o.printf("ERROR: range %ld..%ld\r\n", (long)p.lo, (long)p.hi);
      return;
    }
    int32_t old = getInt(p);
    setInt(p, v);
    if (!freqPlanOk(cfg.freqHz, cfg.hopEnable ? cfg.hopChannels : 1, cfg.hopSpacingKHz)) {
      setInt(p, old);
      return err(o, "hop channels out of band");
    }
    configSave();
    if (p.apply == APPLY_RADIO) link::applyConfig();
    if (p.apply == APPLY_REBOOT) {
      o.print("OK (reboot to apply)\r\n");
      return;
    }
    return ok(o);
  }
  err(o, "unknown command (AT+HELP)");
}

}  // namespace at

#include "display.h"

#include <U8g2lib.h>
#include <Wire.h>

#include "battery.h"
#include "ble_svc.h"
#include "config.h"
#include "fast.h"
#include "lora_link.h"
#include "pins.h"

extern bool usbConnected();
extern bool anyCmdMode();

namespace display {

static U8G2* oled = nullptr;
static bool on = true;
static uint8_t page = 0;
static const uint8_t PAGES = 4;  // 0 = animated status, 1 = link details, 2 = statistics, 3 = settings
static uint16_t animFrame = 0;
static uint32_t lastWake = 0, nextDraw = 0;
static char toastMsg[22];
static uint32_t toastUntil = 0;

static void fmtRate(char* out, size_t n, uint32_t bps) {
  if (bps < 1000) snprintf(out, n, "%luB/s", (unsigned long)bps);
  else snprintf(out, n, "%lu.%luk/s", (unsigned long)(bps / 1000), (unsigned long)(bps % 1000 / 100));
}

static char infoStr[40] = "not initialised";

// nRF52 TWIM does not reliably handle zero-length writes: probe with a 1-byte read too
static int lastZeroLen = -1, lastRead = -1;  // raw results of the last probe (for AT+HW)
static bool i2cPresent(uint8_t a) {
  Wire.beginTransmission(a);
  lastZeroLen = Wire.endTransmission();
  if (lastZeroLen == 0) return true;
  lastRead = Wire.requestFrom(a, (uint8_t)1);
  return lastRead == 1;
}

static char probeStr[160] = "";
static int railSda = -1, railScl = -1;  // I2C line levels while the 3.3V rail was off
static int statusByte = -1;             // controller status register after init

// Hard power-cycle of the switched 3.3V rail (OLED + LoRa module). An OLED controller that
// has locked up still ACKs its address but ignores everything, and with a battery attached
// it never loses power on its own - so the stale image survives resets and re-flashing.
// All signal pins are driven low meanwhile so the rail is not back-fed through ESD diodes.
void powerCycleRail() {
  static const uint8_t lows[] = {PIN_WIRE_SDA, PIN_WIRE_SCL, PIN_LORA_CS, PIN_LORA_SCK, PIN_LORA_MOSI,
                                 PIN_LORA_RST, PIN_LORA_RXEN};
  static const uint8_t ins[] = {PIN_LORA_MISO, PIN_LORA_DIO1, PIN_LORA_BUSY};
  digitalWrite(PIN_EXT_VCC_EN, LOW);
  pinMode(PIN_EXT_VCC_EN, OUTPUT);
  for (uint8_t p : lows) { digitalWrite(p, LOW); pinMode(p, OUTPUT); }
  for (uint8_t p : ins) pinMode(p, INPUT_PULLDOWN);
  delay(600);
  pinMode(PIN_WIRE_SDA, INPUT);
  pinMode(PIN_WIRE_SCL, INPUT);
  delay(5);
  railSda = digitalRead(PIN_WIRE_SDA);
  railScl = digitalRead(PIN_WIRE_SCL);
  for (uint8_t p : lows) pinMode(p, INPUT);
  for (uint8_t p : ins) pinMode(p, INPUT);
  digitalWrite(PIN_EXT_VCC_EN, HIGH);
  delay(150);
}
static bool sdaWasStuck = false;

// I2C bus recovery: the OLED keeps its power across MCU resets, so a reset in the middle of
// a transfer (e.g. by a flasher) can leave it holding SDA low. 9 SCL pulses + STOP release it.
static void i2cBusRecover() {
  pinMode(PIN_WIRE_SDA, INPUT_PULLUP);
  pinMode(PIN_WIRE_SCL, INPUT_PULLUP);
  delayMicroseconds(20);
  sdaWasStuck = digitalRead(PIN_WIRE_SDA) == LOW;
  pinMode(PIN_WIRE_SCL, OUTPUT);
  for (int i = 0; i < 9; i++) {
    digitalWrite(PIN_WIRE_SCL, LOW);
    delayMicroseconds(5);
    digitalWrite(PIN_WIRE_SCL, HIGH);
    delayMicroseconds(5);
  }
  pinMode(PIN_WIRE_SDA, OUTPUT);  // STOP: SDA low -> high while SCL high
  digitalWrite(PIN_WIRE_SDA, LOW);
  delayMicroseconds(5);
  digitalWrite(PIN_WIRE_SCL, HIGH);
  delayMicroseconds(5);
  digitalWrite(PIN_WIRE_SDA, HIGH);
  delayMicroseconds(5);
  pinMode(PIN_WIRE_SDA, INPUT);
  pinMode(PIN_WIRE_SCL, INPUT);
}
const char* probeInfo() { return probeStr; }

const char* info() { return infoStr; }

size_t scan(uint8_t* found, size_t max) {
  size_t n = 0;
  for (uint8_t a = 0x08; a < 0x78 && n < max; a++)
    if (i2cPresent(a)) found[n++] = a;
  return n;
}

bool begin() {
  i2cBusRecover();
  Wire.begin();
  Wire.setClock(400000);
  uint8_t addr = 0;
  static const uint8_t addrs[] = {0x3C, 0x3D};
  for (uint8_t a : addrs) {
    if (i2cPresent(a)) {
      addr = a;
      break;
    }
  }
  bool detected = addr != 0;
  int z0 = lastZeroLen, r0 = lastRead;
  if (!detected) addr = 0x3C;  // blind init: harmless if absent, fixes flaky probes
  const u8g2_cb_t* rot = cfg.oledFlip ? U8G2_R2 : U8G2_R0;
  if (cfg.oledType == 1) oled = new U8G2_SH1106_128X64_NONAME_F_HW_I2C(rot);
  else oled = new U8G2_SSD1306_128X64_NONAME_F_HW_I2C(rot);
  oled->setI2CAddress(addr << 1);
  oled->setBusClock(400000);
  oled->begin();
  oled->setFont(u8g2_font_6x10_tf);
  oled->clearBuffer();
  oled->drawStr(22, 24, "LoRa Bridge");
  oled->drawStr(22, 40, "fw " FW_VERSION);
  oled->sendBuffer();
  lastWake = millis();
  if (Wire.requestFrom(addr, (uint8_t)1) == 1) statusByte = Wire.read();
  snprintf(probeStr, sizeof(probeStr),
           "boot: rail-off SDA=%d SCL=%d (0/0 = OLED really lost power), SDA %s, zero-len write=%d, 1-byte read=%d, status=0x%02X",
           railSda, railScl, sdaWasStuck ? "was STUCK LOW (recovered)" : "idle", z0, r0, statusByte & 0xFF);
  snprintf(infoStr, sizeof(infoStr), "%s @0x%02X %s", cfg.oledType == 1 ? "SH1106" : "SSD1306", addr,
           detected ? "detected" : "NOT detected (blind init)");
  return detected;
}

void wake() {
  lastWake = millis();
  if (oled && !on) {
    oled->setPowerSave(0);
    on = true;
    nextDraw = 0;
  }
}
bool isOn() { return oled && on; }
void nextPage() {
  page = (page + 1) % PAGES;
  nextDraw = 0;
}
void toast(const char* msg, uint32_t ms) {
  strncpy(toastMsg, msg, sizeof(toastMsg) - 1);
  toastMsg[sizeof(toastMsg) - 1] = 0;
  toastUntil = millis() + ms;
  nextDraw = 0;
  wake();
}

static void drawBattery(int x, int y, uint8_t pct, bool usb) {
  // 14x7 icon at (x, y) top-left (inverse colour inside the title bar)
  oled->drawFrame(x, y, 12, 7);
  oled->drawBox(x + 12, y + 2, 2, 3);
  int w = (pct * 10 + 50) / 100;
  if (w > 0) oled->drawBox(x + 1, y + 1, w, 5);
  if (usb) {  // lightning-ish marker
    oled->setDrawColor(2);
    oled->drawLine(x + 4, y + 1, x + 7, y + 5);
    oled->setDrawColor(0);
  }
}

static void titleBar() {
  const LinkStats& s = link::stats();
  (void)s;
  oled->setDrawColor(1);
  oled->drawBox(0, 0, 128, 10);
  oled->setDrawColor(0);
  char buf[24];
  // STBY = power-save standby, CONN = link active, SRCH = waking / looking for the peer
  const char* st = link::modeName();
  const char* role = cfg.peerId ? (link::isMaster() ? " M" : " S") : "";
  bool txAct = millis() - link::txActivityMs() < 150;
  bool rxAct = millis() - link::rxActivityMs() < 150;
  snprintf(buf, sizeof(buf), "%s%s%s%s", st, role, txAct ? ">" : " ", rxAct ? "<" : " ");
  oled->drawStr(1, 8, buf);
  int x = 52;
  // "F" = FAST mode, data on 2.4 GHz (blinks while it is being negotiated). Bluetooth is off
  // the air meanwhile, so it takes the place of "BT".
  if (fast::isUp() || (fast::negotiating() && (animFrame & 1))) oled->drawStr(x, 8, "F");
  else if (ble::connected()) oled->drawStr(x, 8, "BT");
  x += 14;
  if (usbConnected()) oled->drawStr(x, 8, "U");
  x += 7;
  if (anyCmdMode()) oled->drawStr(x, 8, "C");
  uint8_t pct = battery::percent();
  snprintf(buf, sizeof(buf), "%3u%%", pct);
  oled->drawStr(84, 8, buf);
  drawBattery(112, 1, pct, battery::usbPowered());
  oled->setDrawColor(1);
}

// Page 0: ASCII-art picture of the link, animated according to the device state.
//
//    .---.         .---.        left box  = this device (M master / S slave)
//    | M |=>=>=>=>=| S |        right box = the peer
//    '---'         '---'        between   = what the link is doing
//
//   LINK  wire of "-": arrows run in the direction data flows ( ->-->-- ), a dot bounces when idle
//   FAST  (LINK with the data on 2.4 GHz) "2.4G" above a wire of "=": double arrows that run
//         twice as fast ( >>===>>== ), a star racing along the wire when idle
//   STBY  z Z z            (both ends asleep, slow heartbeat)
//   SRCH  ) ) ) )          (waking the peer)
//   NOOP  blinking x, dotted peer box (peer not found)
//   UNPR  dotted peer box  (not paired)
//   PAIR  <- ->  closing in
static void pageArt() {
  const LinkStats& s = link::stats();
  const char* mode = link::modeName();
  uint16_t f = animFrame;
  char me = cfg.peerId ? (link::isMaster() ? 'M' : 'S') : '-';
  char pe = cfg.peerId ? (link::isMaster() ? 'S' : 'M') : '?';
  char seg[10] = "         ";
  bool ghost = false;  // peer unknown / missing: dotted box
  char l4[32], l5[32], a[12], b[12];
  snprintf(l5, sizeof(l5), "%.3fM SF%u BW%g", link::freqMHz(), cfg.sf, cfg.bwX10 / 10.0);
  bool hs = fast::isUp() && !strcmp(mode, "LINK");

  if (hs) {
    bool tx = s.txRate > 0, rx = s.rxRate > 0;
    if (!tx && !rx) {  // idle: a star races along the double wire
      memset(seg, '=', 9);
      uint8_t p = (uint8_t)((f * 2) % 16);
      seg[p > 8 ? 16 - p : p] = '*';
    } else {
      uint8_t ph = (uint8_t)((f * 2) % 5);  // two columns per frame
      for (int i = 0; i < 9; i++) {
        bool right = rx && tx ? i < 4 : tx;
        if (rx && tx && i == 4) seg[i] = '|';
        else if (right) seg[i] = ((i + 5 - ph) % 5 < 2) ? '>' : '=';
        else seg[i] = ((i + ph) % 5 < 2) ? '<' : '=';
      }
    }
    snprintf(l4, sizeof(l4), "2.4G %ddBm loss %u%%", fast::rssi(), fast::lossPct());
    fmtRate(a, sizeof(a), s.txRate);
    fmtRate(b, sizeof(b), s.rxRate);
    snprintf(l5, sizeof(l5), "TX %-7s RX %s", a, b);
  } else if (!strcmp(mode, "LINK")) {
    bool tx = s.txRate > 0 || millis() - link::txActivityMs() < 300;
    bool rx = s.rxRate > 0;
    if (s.txRate == 0 && s.rxRate == 0) {  // idle: a dot bounces along the wire
      memset(seg, '-', 9);
      uint8_t p = f % 16;
      seg[p > 8 ? 16 - p : p] = 'o';
    } else {
      for (int i = 0; i < 9; i++) {
        bool right = rx && tx ? i < 4 : tx;  // both directions: left half out, right half in
        if (rx && tx && i == 4) seg[i] = '|';
        else if (right) seg[i] = ((i + 3 - f % 3) % 3 == 0) ? '>' : '-';
        else seg[i] = ((i + f) % 3 == 0) ? '<' : '-';
      }
    }
    if (s.rxFrames) snprintf(l4, sizeof(l4), "RSSI %d SNR %.1f", s.rssi, s.snr);
    else snprintf(l4, sizeof(l4), "LINK up");
    fmtRate(a, sizeof(a), s.txRate);
    fmtRate(b, sizeof(b), s.rxRate);
    snprintf(l5, sizeof(l5), "TX %-7s RX %s", a, b);
  } else if (!strcmp(mode, "STBY")) {
    uint8_t k = f % 4;
    if (k >= 1) seg[2] = 'z';
    if (k >= 2) seg[4] = 'Z';
    if (k >= 3) seg[6] = 'z';
    if (s.remoteBatt <= 100) snprintf(l4, sizeof(l4), "Standby  peer %u%%", s.remoteBatt);
    else snprintf(l4, sizeof(l4), "Standby");
  } else if (!strcmp(mode, "SRCH")) {
    uint8_t n = f % 6;
    for (uint8_t i = 0; i < n && i < 5; i++) seg[i * 2] = ')';
    pe = '?';
    snprintf(l4, sizeof(l4), "Waking peer...");
  } else if (!strcmp(mode, "NOOP")) {
    seg[4] = (f & 1) ? 'x' : ' ';
    ghost = true;
    pe = '?';
    snprintf(l4, sizeof(l4), "Peer not found");
  } else if (!strcmp(mode, "PAIR")) {
    uint8_t k = f % 4;
    seg[k] = '<';
    seg[k + 1] = '-';
    seg[8 - k] = '>';
    seg[7 - k] = '-';
    ghost = true;
    pe = '?';
    snprintf(l4, sizeof(l4), "Pairing... %lus", (unsigned long)link::pairRemainingS());
  } else {  // UNPR
    ghost = true;
    seg[4] = (f & 1) ? '?' : ' ';
    snprintf(l4, sizeof(l4), "Hold button 3s: pair");
  }
  if (!link::radioOk()) snprintf(l4, sizeof(l4), "RADIO %s", link::radioStatus());

  char l[32];
  snprintf(l, sizeof(l), " .---.%s%s", hs ? ((f & 1) ? "  2.4G   " : " >2.4G<  ") : "         ", ghost ? ". - ." : ".---.");
  oled->drawStr(0, 21, l);
  snprintf(l, sizeof(l), " | %c |%s%c %c %c", me, seg, ghost ? ':' : '|', pe, ghost ? ':' : '|');
  oled->drawStr(0, 31, l);
  snprintf(l, sizeof(l), " '---'         %s", ghost ? "' - '" : "'---'");
  oled->drawStr(0, 41, l);
  oled->drawStr(0, 53, l4);
  oled->drawStr(0, 63, l5);
}

static void pageMain() {
  const LinkStats& s = link::stats();
  char l[32], a[12], b[12];
  if (!link::radioOk()) {
    snprintf(l, sizeof(l), "RADIO %s", link::radioStatus());
    oled->drawStr(0, 21, l);
  } else {
    snprintf(l, sizeof(l), "%.3fM SF%u BW%g", link::freqMHz(), cfg.sf, cfg.bwX10 / 10.0);
    oled->drawStr(0, 21, l);
  }
  if (link::isPairing()) {
    snprintf(l, sizeof(l), "PAIRING... %lus", (unsigned long)link::pairRemainingS());
  } else if (cfg.peerId) {
    const char* role = link::isMaster() ? "MST" : "SLV";
    if (link::hopping())
      snprintf(l, sizeof(l), "%s %08lX FH %u/%u", role, (unsigned long)cfg.peerId, link::channel(), cfg.hopChannels);
    else
      snprintf(l, sizeof(l), "%s Peer %08lX", role, (unsigned long)cfg.peerId);
  } else {
    snprintf(l, sizeof(l), "UNPR (broadcast)");
  }
  oled->drawStr(0, 32, l);
  if (s.rxFrames) snprintf(l, sizeof(l), "RSSI %d SNR %.1f", s.rssi, s.snr);
  else snprintf(l, sizeof(l), "RSSI -- SNR --");
  oled->drawStr(0, 43, l);
  fmtRate(a, sizeof(a), s.txRate);
  fmtRate(b, sizeof(b), s.rxRate);
  snprintf(l, sizeof(l), "TX %-7s RX %s", a, b);
  oled->drawStr(0, 54, l);
  snprintf(l, sizeof(l), "Q %u Rt %lu LOST %lu", (unsigned)link::queued(), (unsigned long)s.retries,
           (unsigned long)s.lost);
  oled->drawStr(0, 64, l);
}

static void pageLink() {
  const LinkStats& s = link::stats();
  char l[32];
  snprintf(l, sizeof(l), "ID %08lX %s", (unsigned long)g_nodeId,
           cfg.peerId ? (link::isMaster() ? "MASTER" : "SLAVE") : "");
  oled->drawStr(0, 21, l);
  if (s.remoteBatt <= 100)
    snprintf(l, sizeof(l), "Peer %d/%.1f B %u%%", s.remoteRssi, s.remoteSnr, s.remoteBatt);
  else
    snprintf(l, sizeof(l), "Peer %d/%.1f", s.remoteRssi, s.remoteSnr);
  oled->drawStr(0, 32, l);
  snprintf(l, sizeof(l), "Pkt TX %lu RX %lu", (unsigned long)s.txFrames, (unsigned long)s.rxFrames);
  oled->drawStr(0, 43, l);
  snprintf(l, sizeof(l), "CRC %lu Dup %lu LBT %lu", (unsigned long)s.crcErr, (unsigned long)s.dup,
           (unsigned long)s.lbtBusy);
  oled->drawStr(0, 54, l);
  snprintf(l, sizeof(l), "Hop %lu Blk %04X", (unsigned long)(s.hops % 1000000ul), link::channelMask());
  oled->drawStr(0, 64, l);
}

static void pageCfg() {
  char l[32];
  snprintf(l, sizeof(l), "PWR %ddBm CR 4/%u PRE %u", cfg.txPower, cfg.cr, cfg.preamble);
  oled->drawStr(0, 21, l);
  if (cfg.hopEnable) snprintf(l, sizeof(l), "FH %uch %ukHz %lums", cfg.hopChannels, cfg.hopSpacingKHz, (unsigned long)link::dwellMs());
  else snprintf(l, sizeof(l), "FH off  PSM %s", cfg.powerSave ? "on" : "off");
  oled->drawStr(0, 32, l);
  snprintf(l, sizeof(l), "ACK %u x%u ENC %u AFH %u", cfg.ack, cfg.retries, cfg.encrypt, cfg.afhEnable);
  oled->drawStr(0, 43, l);
  snprintf(l, sizeof(l), "%.20s", cfg.bleName);
  oled->drawStr(0, 54, l);
  snprintf(l, sizeof(l), "Bat %.2fV %s", battery::volts(), battery::usbPowered() ? "USB" : "");
  oled->drawStr(0, 64, l);
}

void loop() {
  if (!oled) return;
  uint32_t now = millis();
  if (ble::pairShowing()) {  // BLE pairing request: show the passkey, the button accepts
    if (!on) {
      oled->setPowerSave(0);
      on = true;
    }
    lastWake = now;
    if ((int32_t)(now - nextDraw) < 0) return;
    nextDraw = now + 250;
    oled->clearBuffer();
    oled->setDrawColor(1);
    oled->drawStr(0, 9, "BLE PAIRING REQUEST");
    oled->setFont(u8g2_font_10x20_tf);
    if (ble::pairNeedsCode()) oled->drawStr((128 - 60) / 2, 36, ble::pairCode());
    else oled->drawStr((128 - 60) / 2, 36, "ALLOW?");
    oled->setFont(u8g2_font_6x10_tf);
    if (!ble::pairNeedsCode()) {
      oled->drawStr(0, 52, "New Bluetooth device");
      oled->drawStr(0, 63, "Press button = YES");
    } else if (ble::pairPending()) {
      oled->drawStr(0, 52, "Same code on phone?");
      oled->drawStr(0, 63, "Press button = YES");
    } else {
      oled->drawStr(0, 52, "Enter this code");
      oled->drawStr(0, 63, "on the phone");
    }
    oled->sendBuffer();
    return;
  }
  // Power: the lit OLED is the biggest consumer after the radio. While the link is not active
  // (STBY / NOOP / UNPR) the screen goes dark after 30 s at most and animates slowly.
  bool active = link::isConn() || link::isPairing();
  uint32_t offMs = cfg.oledTimeoutS * 1000u;
  if (offMs && !active && offMs > 30000) offMs = 30000;
  if (on && offMs && now - lastWake > offMs) {
    oled->setPowerSave(1);
    on = false;
  }
  if (!on || (int32_t)(now - nextDraw) < 0) return;
  // a refresh blocks ~30 ms: never start one while the radio is sending / waiting for an ACK
  if (link::timeCritical() && now - nextDraw < 3000) return;
  // 4 frames/s while linked or pairing, one frame every 2 s otherwise (each refresh keeps the
  // CPU and the I2C bus busy for ~30 ms)
  nextDraw = now + (active || (int32_t)(toastUntil - now) > 0 ? 250 : 2000);
  animFrame++;

  oled->clearBuffer();
  titleBar();
  if ((int32_t)(toastUntil - now) > 0) {
    oled->drawFrame(0, 24, 128, 24);
    oled->drawStr((128 - 6 * strlen(toastMsg)) / 2, 40, toastMsg);
  } else {
    switch (page) {
      case 0: pageArt(); break;
      case 1: pageMain(); break;
      case 2: pageLink(); break;
      default: pageCfg(); break;
    }
  }
  oled->sendBuffer();
}

}  // namespace display

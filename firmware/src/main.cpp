// FakeTec LoRa Serial Bridge
//
// USB CDC  <──┐                         ┌──> USB CDC
//             ├─> LoRa (FHSS/ARQ/AES) ──┤
// BLE NUS  <──┘                         └──> BLE NUS
//
// ETH mode adds a USB CDC-NCM network adapter on both ends; IP packets between the two hosts
// share the same link (eth.cpp).
//
// Configuration: "+++" escape on USB / BLE UART, or the BLE config characteristic.
// Firmware update: 1200 bps touch / double-tap reset -> UF2 + serial DFU bootloader,
//                  AT+DFU, or BLE OTA DFU.
#include <Adafruit_TinyUSB.h>
#include <Arduino.h>
#include <bluefruit.h>

#include "at_cmd.h"
#include "battery.h"
#include "ble_svc.h"
#include "config.h"
#include "display.h"
#include "eth.h"
#include "fast.h"
#include "lora_link.h"
#include "pins.h"
#include "ringbuf.h"

static RingBuf<4096> usbOut;
static RingBuf<4096> bleOut;
static RingBuf<4096> webOut;
static Adafruit_USBD_WebUSB usbWeb;  // vendor interface for the browser client (WebUSB)
static uint32_t outDropped = 0;

class BleUartPrint : public Print {
 public:
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* b, size_t n) override {
    size_t chunk = ble::uartChunk(), done = 0;
    while (done < n) {
      size_t k = n - done < chunk ? n - done : chunk;
      size_t w = ble::uartWrite(b + done, k);
      if (!w) break;
      done += w;
    }
    return n;  // never block the command processor
  }
};
static BleUartPrint bleUartPrint;

static at::Port usbPort;
static at::Port blePort;
static at::Port webPort;

// VBUS gone = unplugged: the stack may still report "mounted" / DTR, so check the cable first
static bool usbPlugged() { return battery::usbPowered(); }
bool usbConnected() { return usbPlugged() && (bool)Serial; }
static bool webConnected() { return usbPlugged() && usbWeb.connected(); }
// "USB serial in use" for the power-save logic: plugged into a host (default) or port opened
static bool usbActive() {
  if (!usbPlugged()) return false;
  if (webConnected()) return true;
  if (cfg.usbActive) return TinyUSBDevice.mounted() && !TinyUSBDevice.suspended();
  return (bool)Serial;
}
bool anyCmdMode() { return usbPort.cmdMode || blePort.cmdMode || webPort.cmdMode; }
// Room in the output buffers of the ports that are taking data. The 2.4 GHz link only accepts
// what fits (the peer repeats the rest), so nothing is dropped at its much higher speed.
size_t serialOutSpace() {
  size_t s = 4095;
  if (usbConnected() && !usbPort.cmdMode && usbOut.space() < s) s = usbOut.space();
  if (ble::uartReady() && !blePort.cmdMode && bleOut.space() < s) s = bleOut.space();
  if (webConnected() && !webPort.cmdMode && webOut.space() < s) s = webOut.space();
  return s;
}

void applyOutputs() {
  digitalWrite(PIN_T1G, (cfg.outputs & 1) ? HIGH : LOW);
  digitalWrite(PIN_T2G, (cfg.outputs & 2) ? HIGH : LOW);
  digitalWrite(PIN_T3G, (cfg.outputs & 4) ? HIGH : LOW);
}

// ---- watchdog + "where was the main loop" breadcrumb
// If the main loop ever stops, the watchdog resets the device after 12 s instead of leaving
// it dead, and the stage it was stuck in is reported after the restart (ATI / AT+HW).
uint32_t g_resetReason = 0;
uint8_t g_hangStage = 0;
uint8_t g_bootMark = 0;
// Breadcrumb that survives a watchdog reset: RAM keeps its contents, but the start-up code
// zeroes .bss and the bootloader uses the low RAM and the top (its stack) on the way through.
// This address is far above anything the heap reaches (heap starts near 0x20010000 and holds
// a few kB) and below both stacks.
#define g_crumb ((volatile uint32_t*)0x2003C000)
#define CRUMB_MAGIC 0xC0FFEE00u
static uint8_t curStage = 0xFF;
static void stage(uint8_t s) {
  if (s == curStage) return;
  curStage = s;
  g_crumb[0] = CRUMB_MAGIC | s;
  g_crumb[1] = ~(CRUMB_MAGIC | s);
}
const char* stageName(uint8_t s) {
  static const char* const names[] = {"-", "usb", "webusb", "ble", "battery", "lora", "button", "led", "display", "idle", "setup", "eth"};
  return s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}
static void watchdogStart() {
  NRF_WDT->CONFIG = (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos) | (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos);
  NRF_WDT->CRV = 32768u * 12;
  NRF_WDT->RREN = WDT_RREN_RR0_Msk;
  NRF_WDT->TASKS_START = 1;
  // GPREGRET2 survives a software / pin reset but is cleared by a watchdog reset (and the
  // bootloader wipes RESETREAS), so "marker gone + breadcrumb in RAM intact" = watchdog.
  sd_power_gpregret_clr(1, 0xFF);
  sd_power_gpregret_set(1, 0x5A);
}
static inline void watchdogFeed() { NRF_WDT->RR[0] = WDT_RR_RR_Reload; }

static int rebootMode = -1;
static uint32_t rebootAt = 0;
void scheduleReboot(uint8_t mode) {
  rebootMode = mode;
  rebootAt = millis() + 300;
}

static void onLoraRx(const uint8_t* d, size_t n) {
  if (usbConnected() && usbOut.write(d, n) != n) outDropped++;
  if (ble::uartReady() && bleOut.write(d, n) != n) outDropped++;
  if (webConnected() && webOut.write(d, n) != n) outDropped++;
}

// ------------------------------------------------------------------ USB CDC port
static void usbTask() {
  static bool wasConnected = false;
  bool conn = usbConnected();
  if (conn != wasConnected) {
    wasConnected = conn;
    usbOut.clear();
    usbPort.cmdMode = false;
    usbPort.esc = 0;
    display::wake();
  }
  static uint8_t in[256], out[256 + 8];  // (static: the loop task's stack is small)

  if (usbPort.cmdMode) {
    int a = Serial.available();
    if (a > 0) {
      size_t n = Serial.readBytes(in, a > (int)sizeof(in) ? sizeof(in) : a);
      at::feed(usbPort, in, n);
    }
  } else {
    // only read what the LoRa queue can take -> USB flow control via NAK
    size_t space = link::queueSpace();
    int a = Serial.available();
    if (a > 0 && space > 8) {
      size_t n = (size_t)a;
      if (n > sizeof(in)) n = sizeof(in);
      if (n > space - 8) n = space - 8;
      n = Serial.readBytes(in, n);
      size_t m = at::filter(usbPort, in, n, out);
      if (m) link::queue(out, m);
    }
  }
  size_t m = at::poll(usbPort, out);
  if (m) link::queue(out, m);

  // LoRa -> USB
  if (!usbPort.cmdMode && conn && !usbOut.empty()) {
    int w = Serial.availableForWrite();
    if (w > 0) {
      uint8_t buf[256];
      size_t n = usbOut.peek(buf, (size_t)w < sizeof(buf) ? (size_t)w : sizeof(buf));
      size_t done = Serial.write(buf, n);
      usbOut.drop(done);
      Serial.flush();
    }
  }
}

// ------------------------------------------------------------------ WebUSB port
static void webTask() {
  static bool wasConnected = false;
  bool conn = webConnected();
  if (conn != wasConnected) {
    wasConnected = conn;
    webOut.clear();
    webPort.cmdMode = false;
    webPort.esc = 0;
    webPort.osc = 0;
    webPort.oscBody = false;
    display::wake();
  }
  static uint8_t in[256], out[256 + 8];
  int a = usbWeb.available();
  if (webPort.cmdMode) {
    if (a > 0) {
      size_t n = usbWeb.read(in, a > (int)sizeof(in) ? sizeof(in) : a);
      at::feed(webPort, in, n);
    }
  } else {
    size_t space = link::queueSpace();
    if (a > 0 && space > 8) {
      size_t n = (size_t)a;
      if (n > sizeof(in)) n = sizeof(in);
      if (n > space - 8) n = space - 8;
      n = usbWeb.read(in, n);
      size_t m = at::filter(webPort, in, n, out);
      if (m) link::queue(out, m);
    }
  }
  size_t m = at::poll(webPort, out);
  if (m) link::queue(out, m);

  if (!webPort.cmdMode && conn && !webOut.empty()) {
    uint32_t w = tud_vendor_write_available();
    if (w > 0) {
      uint8_t buf[256];
      size_t n = webOut.peek(buf, w < sizeof(buf) ? w : sizeof(buf));
      size_t done = usbWeb.write(buf, n);
      webOut.drop(done);
    }
  }
  // Also pushes out command responses: a partly filled packet (anything shorter than 64 bytes,
  // e.g. "OK") otherwise stays in the endpoint FIFO until later output fills it up.
  if (conn) usbWeb.flush();
}

// ------------------------------------------------------------------ BLE UART port
static void bleTask() {
  static bool wasReady = false;
  bool ready = ble::uartReady();
  if (ready != wasReady) {
    wasReady = ready;
    bleOut.clear();
    blePort.cmdMode = false;
    blePort.esc = 0;
    display::wake();
  }
  uint8_t in[64], out[64 + 8];
  int a = ble::uartAvailable();
  if (blePort.cmdMode) {
    if (a > 0) {
      size_t n = ble::uartRead(in, a > (int)sizeof(in) ? sizeof(in) : a);
      at::feed(blePort, in, n);
    }
  } else {
    size_t space = link::queueSpace();
    if (a > 0 && space > 8) {
      size_t n = (size_t)a;
      if (n > sizeof(in)) n = sizeof(in);
      if (n > space - 8) n = space - 8;
      n = ble::uartRead(in, n);
      size_t m = at::filter(blePort, in, n, out);
      if (m) link::queue(out, m);
    }
  }
  size_t m = at::poll(blePort, out);
  if (m) link::queue(out, m);

  if (!blePort.cmdMode && ready && !bleOut.empty()) {
    uint8_t buf[244];
    size_t chunk = ble::uartChunk();
    if (chunk > sizeof(buf)) chunk = sizeof(buf);
    size_t n = bleOut.peek(buf, chunk);
    size_t w = ble::uartWrite(buf, n);
    bleOut.drop(w);
  }

  // BLE config characteristic
  char line[160];
  while (ble::cfgPop(line, sizeof(line))) at::execute(line, ble::cfgPrint(), nullptr);
  uint8_t ev = ble::takePairEvent();
  if (ev == 1) display::toast("BLE PAIRED");
  else if (ev == 2) display::toast("BLE PAIR FAILED");
  ble::loop();
}

// ------------------------------------------------------------------ button
// short press            : wake display / next page (status art, link, statistics, settings) /
//                          cancel pairing
// hold 3 s               : paired and the peer is there (STBY / LINK): ETH mode on / off, on both ends
//                          not paired yet: pairing mode (60 s)
// double click + hold 3 s: pairing mode (60 s) in any state - this is how a paired device is
//                          paired again (click, then press again within half a second and hold)
// hold 10 s              : unpair
// What a 3 s hold does is decided when the "RELEASE: ..." hint appears.
enum : uint8_t { HOLD_NONE, HOLD_PAIR, HOLD_ETH };
#define DOUBLE_CLICK_MS 500
static uint8_t holdAction(bool afterClick) {
  if (afterClick || !cfg.peerId) return HOLD_PAIR;
  if (link::isPairing()) return HOLD_NONE;
  const char* m = link::modeName();
  return (!strcmp(m, "STBY") || !strcmp(m, "LINK")) ? HOLD_ETH : HOLD_NONE;
}
static void buttonTask() {
  static bool down = false, afterClick = false;
  static uint32_t t0 = 0, lastChange = 0, lastClick = 0;
  static uint8_t hint = 0, action = HOLD_NONE;
  bool pressed = digitalRead(PIN_BUTTON) == LOW;
  uint32_t now = millis();
  if (pressed != down && now - lastChange > 30) {
    lastChange = now;
    down = pressed;
    if (down) {
      t0 = now;
      hint = 0;
      afterClick = lastClick && now - lastClick < DOUBLE_CLICK_MS;
    } else {
      uint32_t d = now - t0;
      lastClick = d < DOUBLE_CLICK_MS ? (now ? now : 1) : 0;
      if (ble::pairPending()) {
        ble::pairConfirm(d < 3000);  // short press accepts, long press rejects
      } else if (d >= 10000) {
        link::unpair();
        display::toast("UNPAIRED");
      } else if (d >= 3000) {
        if (!hint) action = holdAction(afterClick);
        if (action == HOLD_ETH) {
          bool on = !eth::enabled();
          const char* why = eth::setEnabled(on);
          display::toast(why ? "ETH: FAILED" : on ? "ETH MODE ON" : "ETH MODE OFF");
        } else if (action == HOLD_PAIR) {
          link::startPairing(60000);
          display::toast("PAIRING 60s");
        }
      } else if (!display::isOn()) {
        display::wake();
      } else if (link::isPairing()) {
        link::stopPairing();
        display::toast("PAIR CANCEL");
      } else {
        display::nextPage();
        display::wake();
      }
    }
  }
  if (down) {
    uint32_t d = now - t0;
    if (ble::pairPending()) {
      // no pair / unpair hints while a BLE pairing request is on screen
    } else if (d >= 10000 && hint < 2) {
      hint = 2;
      display::toast("RELEASE: UNPAIR");
    } else if (d >= 3000 && hint < 1) {
      hint = 1;
      action = holdAction(afterClick);
      display::toast(action == HOLD_PAIR ? "RELEASE: PAIR" : action == HOLD_NONE ? "2xCLICK+HOLD: PAIR"
                     : eth::enabled() ? "RELEASE: ETH OFF" : "RELEASE: ETH ON", 2500);
    }
  }
}

static void ledTask() {
  uint32_t now = millis();
  bool on;
  if (link::isPairing()) on = (now / 250) & 1;
  else on = (now - link::txActivityMs() < 40) || (now - link::rxActivityMs() < 40);
  digitalWrite(PIN_LED, on ? HIGH : LOW);
}

// ------------------------------------------------------------------ setup / loop
void setup() {
  // SoftDevice is not running yet: POWER registers may be accessed directly
  g_resetReason = NRF_POWER->RESETREAS;
  NRF_POWER->RESETREAS = g_resetReason;  // write 1 to clear
  uint8_t mark = (uint8_t)NRF_POWER->GPREGRET2;
  NRF_POWER->GPREGRET2 = 0;
  g_bootMark = mark;
  uint32_t c0 = g_crumb[0], c1 = g_crumb[1];
  g_crumb[0] = g_crumb[1] = 0;
  if (mark != 0x5A && (c0 & 0xFFFFFF00u) == CRUMB_MAGIC && c1 == ~c0) {
    g_resetReason |= 0x2;  // watchdog
    g_hangStage = (uint8_t)c0;
  }

  // USB: the core has started the device with the serial port only. The WebUSB interface
  // (browser client) and, in ETH mode, the network adapter are added here - and whether ETH
  // mode is on is in the settings, which take longer to read than a host waits before it asks
  // for the descriptors. So stay off the bus until the descriptors are complete: the host
  // then sees the device once, in its final shape.
  bool vbus = NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk;
  if (vbus) {
    // (the pull-up is switched on by the USB power-ready event, shortly after start-up)
    for (uint32_t t = millis(); !NRF_USBD->USBPULLUP && millis() - t < 200;) delay(1);
  }
  TinyUSBDevice.detach();
  configLoad();
  TinyUSBDevice.setDeviceVersion(0x0110);  // new bcdDevice: Windows re-reads the WinUSB descriptors
  usbWeb.begin();
  eth::begin();
  if (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) {
    if (NRF_USBD->USBPULLUP) {  // the power-ready event came late and connected us meanwhile
      TinyUSBDevice.detach();
      delay(20);
    }
    TinyUSBDevice.attach();
  }

  display::powerCycleRail();  // off 0.6 s then on: hard-resets OLED + LoRa module
  pinMode(PIN_T1G, OUTPUT);
  pinMode(PIN_T2G, OUTPUT);
  pinMode(PIN_T3G, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);

  Serial.begin(115200);

  applyOutputs();
  ble::begin();  // also enables the SoftDevice (needed for AES / RNG)

  usbPort.name = "USB";
  usbPort.out = &Serial;
  blePort.name = "BLE";
  blePort.out = &bleUartPrint;
  webPort.name = "WEB";
  webPort.out = &usbWeb;

  delay(100);  // let the switched VCC rail settle
  display::begin();
  battery::begin();
  link::setRxHandler(onLoraRx);
  link::begin();
  watchdogStart();
}

void loop() {
  uint32_t t = millis();
  watchdogFeed();
  stage(1);
  usbTask();
  stage(2);
  webTask();
  stage(3);
  bleTask();

  stage(4);
  battery::loop();
  link::setLocalBattery(battery::percent());
  ble::setBattery(battery::percent());
  // "in use": USB plugged in / WebUSB client / a Bluetooth central connected
  link::setActive(usbActive() || ble::connected());
  stage(5);
  link::loop();
  fast::loop();
  stage(11);
  eth::loop();

  stage(6);
  buttonTask();
  stage(7);
  ledTask();
  stage(8);
  display::loop();
  stage(9);

  if (rebootMode >= 0 && (int32_t)(millis() - rebootAt) >= 0) {
    if (rebootMode == 1) enterUf2Dfu();
    else if (rebootMode == 2) enterOTADfu();
    else NVIC_SystemReset();
  }

  if (millis() == t && usbOut.empty() && bleOut.empty() && webOut.empty() && link::queued() == 0 && !fast::busy() && !eth::busy()) {
    // standby: nothing is time critical, let the CPU sleep longer
    delay(link::isConn() ? 1 : 20);
  }
}

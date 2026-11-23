// FakeTec LoRa Serial Bridge
//
// USB CDC  <──┐                         ┌──> USB CDC
//             ├─> LoRa (FHSS/ARQ/AES) ──┤
// BLE NUS  <──┘                         └──> BLE NUS
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

void applyOutputs() {
  digitalWrite(PIN_T1G, (cfg.outputs & 1) ? HIGH : LOW);
  digitalWrite(PIN_T2G, (cfg.outputs & 2) ? HIGH : LOW);
  digitalWrite(PIN_T3G, (cfg.outputs & 4) ? HIGH : LOW);
}

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
  uint8_t in[64], out[64 + 8];

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
  uint8_t in[64], out[64 + 8];
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
      usbWeb.flush();
    }
  }
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
// short press: wake display / next page (status art, link, statistics, settings) / cancel pairing
// hold 3 s   : pairing mode (60 s)
// hold 10 s  : unpair
static void buttonTask() {
  static bool down = false;
  static uint32_t t0 = 0, lastChange = 0;
  static uint8_t hint = 0;
  bool pressed = digitalRead(PIN_BUTTON) == LOW;
  uint32_t now = millis();
  if (pressed != down && now - lastChange > 30) {
    lastChange = now;
    down = pressed;
    if (down) {
      t0 = now;
      hint = 0;
    } else {
      uint32_t d = now - t0;
      if (ble::pairPending()) {
        ble::pairConfirm(d < 3000);  // short press accepts, long press rejects
      } else if (d >= 10000) {
        link::unpair();
        display::toast("UNPAIRED");
      } else if (d >= 3000) {
        link::startPairing(60000);
        display::toast("PAIRING 60s");
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
      display::toast("RELEASE: PAIR");
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
  // WebUSB interface (browser client). It is added after the core started USB, so re-enumerate.
  TinyUSBDevice.setDeviceVersion(0x0110);  // new bcdDevice: Windows re-reads the WinUSB descriptors
  usbWeb.begin();
  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }

  display::powerCycleRail();  // off 0.6 s then on: hard-resets OLED + LoRa module
  pinMode(PIN_T1G, OUTPUT);
  pinMode(PIN_T2G, OUTPUT);
  pinMode(PIN_T3G, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);

  Serial.begin(115200);

  configLoad();
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
}

void loop() {
  uint32_t t = millis();
  usbTask();
  webTask();
  bleTask();

  battery::loop();
  link::setLocalBattery(battery::percent());
  ble::setBattery(battery::percent());
  link::setActive(usbActive() || ble::uartReady());
  link::loop();

  buttonTask();
  ledTask();
  display::loop();

  if (rebootMode >= 0 && (int32_t)(millis() - rebootAt) >= 0) {
    if (rebootMode == 1) enterUf2Dfu();
    else if (rebootMode == 2) enterOTADfu();
    else NVIC_SystemReset();
  }

  if (millis() == t && usbOut.empty() && bleOut.empty() && webOut.empty() && link::queued() == 0) {
    // standby: nothing is time critical, let the CPU sleep longer
    delay(link::isConn() ? 1 : 20);
  }
}

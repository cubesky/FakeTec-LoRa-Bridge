#include "ble_svc.h"

#include <bluefruit.h>

#include "config.h"
#include "ringbuf.h"

namespace ble {

// Config service: 8A9B0001-3C4D-4E5F-8061-7283A4B5C6D7
//   0002 : command (write / write without response), UTF-8 AT command
//   0003 : response (notify / read)
static const uint8_t UUID_CFG_SVC[16] = {0xD7, 0xC6, 0xB5, 0xA4, 0x83, 0x72, 0x61, 0x80,
                                         0x5F, 0x4E, 0x4D, 0x3C, 0x01, 0x00, 0x9B, 0x8A};
static const uint8_t UUID_CFG_CMD[16] = {0xD7, 0xC6, 0xB5, 0xA4, 0x83, 0x72, 0x61, 0x80,
                                         0x5F, 0x4E, 0x4D, 0x3C, 0x02, 0x00, 0x9B, 0x8A};
static const uint8_t UUID_CFG_RSP[16] = {0xD7, 0xC6, 0xB5, 0xA4, 0x83, 0x72, 0x61, 0x80,
                                         0x5F, 0x4E, 0x4D, 0x3C, 0x03, 0x00, 0x9B, 0x8A};

static BLEDfu bledfu;
static BLEDis bledis;
static BLEBas blebas;
static BLEUart bleuart(2048);
static BLEService cfgSvc(UUID_CFG_SVC);
static BLECharacteristic cfgCmd(UUID_CFG_CMD);
static BLECharacteristic cfgRsp(UUID_CFG_RSP);

static volatile uint16_t connHandle = BLE_CONN_HANDLE_INVALID;

// command queue (written from the BLE task, read from loop)
#define CMDQ_N 4
#define CMD_MAX 160
static char cmdq[CMDQ_N][CMD_MAX];
static volatile uint8_t cmdHead = 0, cmdTail = 0;
static RingBuf<2048> rsp;

class RspPrint : public Print {
 public:
  size_t write(uint8_t c) override { return rsp.put(c) ? 1 : 0; }
  size_t write(const uint8_t* b, size_t n) override { return rsp.write(b, n); }
};
static RspPrint rspPrint;

static void onCmdWrite(uint16_t, BLECharacteristic*, uint8_t* data, uint16_t len) {
  uint8_t next = (cmdHead + 1) % CMDQ_N;
  if (next == cmdTail) return;  // queue full
  size_t n = len < CMD_MAX - 1 ? len : CMD_MAX - 1;
  memcpy(cmdq[cmdHead], data, n);
  cmdq[cmdHead][n] = 0;
  cmdHead = next;
}

static void onConnect(uint16_t h) {
  connHandle = h;
  BLEConnection* c = Bluefruit.Connection(h);
  if (c) {
    c->requestPHY();
    c->requestDataLengthUpdate();
    c->requestMtuExchange(247);
  }
}
// ---- pairing: the passkey is shown on the OLED, a short button press accepts it
static volatile uint8_t pairState = 0;  // 0 none, 1 waiting for the button, 2 passkey shown (typed on the phone)
static volatile int8_t pairDecision = -1;
static volatile uint8_t pairEvent = 0;
static char pairDigits[7] = "";

static void onDisconnect(uint16_t, uint8_t) {
  connHandle = BLE_CONN_HANDLE_INVALID;
  pairState = 0;
}

// runs in the Bluefruit callback task, so it may block
static bool onPasskey(uint16_t, uint8_t const passkey[6], bool matchRequest) {
  memcpy(pairDigits, passkey, 6);
  pairDigits[6] = 0;
  if (!matchRequest) {  // legacy pairing: the user types the code on the phone
    pairState = 2;
    return true;
  }
  pairDecision = -1;
  pairState = 1;
  uint32_t t0 = millis();
  while (pairDecision < 0 && millis() - t0 < 30000 && connHandle != BLE_CONN_HANDLE_INVALID) delay(20);
  bool ok = pairDecision == 1;
  pairState = 0;
  return ok;
}
static void onPairComplete(uint16_t, uint8_t status) {
  pairState = 0;
  pairEvent = status == BLE_GAP_SEC_STATUS_SUCCESS ? 1 : 2;
}

bool pairPending() { return pairState == 1; }
bool pairShowing() { return pairState != 0; }
const char* pairCode() { return pairDigits; }
void pairConfirm(bool ok) { pairDecision = ok ? 1 : 0; }
uint8_t takePairEvent() {
  uint8_t e = pairEvent;
  pairEvent = 0;
  return e;
}
void clearBonds() { Bluefruit.Periph.clearBonds(); }

void begin() {
  Bluefruit.autoConnLed(false);
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  Bluefruit.begin(1, 0);
  Bluefruit.setTxPower(4);
  Bluefruit.setName(cfg.bleName);
  Bluefruit.Periph.setConnectCallback(onConnect);
  Bluefruit.Periph.setDisconnectCallback(onDisconnect);

  SecureMode_t perm = SECMODE_OPEN;
  if (cfg.blePin) {
    char pin[8];
    snprintf(pin, sizeof(pin), "%06lu", (unsigned long)(cfg.blePin % 1000000));
    Bluefruit.Security.setPIN(pin);
    perm = SECMODE_ENC_WITH_MITM;
  } else if (cfg.blePair) {
    // display + yes/no: numeric comparison, accepted with the button
    Bluefruit.Security.setIOCaps(true, true, false);
    Bluefruit.Security.setMITM(true);
    Bluefruit.Security.setPairPasskeyCallback(onPasskey);
    perm = SECMODE_ENC_WITH_MITM;
  }
  Bluefruit.Security.setPairCompleteCallback(onPairComplete);

  bledfu.begin();  // OTA DFU (nRF Connect / Adafruit Bluefruit app)

  bledis.setManufacturer("FakeTec");
  bledis.setModel("LoRa Serial Bridge");
  bledis.setFirmwareRev(FW_VERSION);
  bledis.begin();

  blebas.begin();
  blebas.write(100);

  bleuart.setPermission(perm, perm);
  bleuart.begin();

  cfgSvc.setPermission(perm, perm);
  cfgSvc.begin();
  cfgCmd.setProperties(CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP);
  cfgCmd.setPermission(perm, perm);
  cfgCmd.setMaxLen(CMD_MAX);
  cfgCmd.setWriteCallback(onCmdWrite);
  cfgCmd.begin();
  cfgRsp.setProperties(CHR_PROPS_NOTIFY | CHR_PROPS_READ);
  cfgRsp.setPermission(perm, SECMODE_NO_ACCESS);
  cfgRsp.setMaxLen(244);
  cfgRsp.begin();

  if (!cfg.bleEnable) return;

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(bleuart);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0);
}

bool connected() { return connHandle != BLE_CONN_HANDLE_INVALID; }
bool uartReady() { return connected() && bleuart.notifyEnabled(); }
int uartAvailable() { return bleuart.available(); }
size_t uartRead(uint8_t* buf, size_t max) { return bleuart.read(buf, max); }

size_t uartChunk() {
  if (!connected()) return 20;
  BLEConnection* c = Bluefruit.Connection(connHandle);
  uint16_t mtu = c ? c->getMtu() : 23;
  return mtu > 3 ? mtu - 3 : 20;
}

size_t uartWrite(const uint8_t* buf, size_t len) {
  if (!uartReady()) return 0;
  return bleuart.write(buf, len);
}

bool cfgPop(char* line, size_t max) {
  if (cmdTail == cmdHead) return false;
  strncpy(line, cmdq[cmdTail], max - 1);
  line[max - 1] = 0;
  cmdTail = (cmdTail + 1) % CMDQ_N;
  return true;
}

void cfgReply(const char* s, size_t n) { rsp.write((const uint8_t*)s, n); }
Print& cfgPrint() { return rspPrint; }

void loop() {
  if (rsp.empty()) return;
  if (!connected() || !cfgRsp.notifyEnabled()) {
    rsp.clear();
    return;
  }
  uint8_t buf[244];
  size_t chunk = uartChunk();
  if (chunk > sizeof(buf)) chunk = sizeof(buf);
  size_t n = rsp.peek(buf, chunk);
  if (n && cfgRsp.notify(buf, n)) rsp.drop(n);
}

void setBattery(uint8_t pct) {
  static uint8_t last = 255;
  if (pct != last && pct <= 100) {
    last = pct;
    blebas.write(pct);
  }
}

void disconnect() {
  if (connected()) Bluefruit.disconnect(connHandle);
}

}  // namespace ble

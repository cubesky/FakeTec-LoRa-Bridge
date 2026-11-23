#include "ble_svc.h"

#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
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

// diagnostics (AT+BLEST): trace of the last GAP / security events
struct EvLog {
  uint32_t ms;
  uint16_t id;
  uint32_t info;
};
#define EVLOG_N 24
static EvLog evlog[EVLOG_N];
static volatile uint8_t evHead = 0, evCount = 0;
static void onBleEvent(ble_evt_t* evt) {
  uint16_t id = evt->header.evt_id;
  if (id < BLE_GAP_EVT_BASE || id > BLE_GAP_EVT_LAST) return;
  if (id == BLE_GAP_EVT_ADV_REPORT || id == BLE_GAP_EVT_RSSI_CHANGED || id == BLE_GAP_EVT_ADV_SET_TERMINATED) return;
  const ble_gap_evt_t& g = evt->evt.gap_evt;
  uint32_t info = 0;
  switch (id) {
    case BLE_GAP_EVT_DISCONNECTED: info = g.params.disconnected.reason; break;
    case BLE_GAP_EVT_SEC_PARAMS_REQUEST: {
      const ble_gap_sec_params_t& p = g.params.sec_params_request.peer_params;
      info = (p.bond << 0) | (p.mitm << 1) | (p.lesc << 2) | (p.keypress << 3) | (p.io_caps << 4) | (p.oob << 8) |
             ((uint32_t)p.max_key_size << 16);
      break;
    }
    case BLE_GAP_EVT_AUTH_STATUS:
      info = g.params.auth_status.auth_status | (g.params.auth_status.error_src << 8) |
             (g.params.auth_status.bonded << 16) | (g.params.auth_status.lesc << 17);
      break;
    case BLE_GAP_EVT_CONN_SEC_UPDATE:
      info = g.params.conn_sec_update.conn_sec.sec_mode.sm | (g.params.conn_sec_update.conn_sec.sec_mode.lv << 4) |
             (g.params.conn_sec_update.conn_sec.encr_key_size << 8);
      break;
    case BLE_GAP_EVT_PASSKEY_DISPLAY: info = g.params.passkey_display.match_request; break;
    case BLE_GAP_EVT_SEC_INFO_REQUEST: info = g.params.sec_info_request.master_id.ediv; break;
    case BLE_GAP_EVT_TIMEOUT: info = g.params.timeout.src; break;
    case BLE_GAP_EVT_SEC_REQUEST: info = 1; break;
    default: break;
  }
  evlog[evHead] = {millis(), id, info};
  evHead = (evHead + 1) % EVLOG_N;
  if (evCount < EVLOG_N) evCount++;
}
static const char* evName(uint16_t id) {
  switch (id) {
    case BLE_GAP_EVT_CONNECTED: return "CONNECTED";
    case BLE_GAP_EVT_DISCONNECTED: return "DISCONNECTED";
    case BLE_GAP_EVT_CONN_PARAM_UPDATE: return "CONN_PARAM_UPDATE";
    case BLE_GAP_EVT_SEC_PARAMS_REQUEST: return "SEC_PARAMS_REQUEST";
    case BLE_GAP_EVT_SEC_INFO_REQUEST: return "SEC_INFO_REQUEST";
    case BLE_GAP_EVT_PASSKEY_DISPLAY: return "PASSKEY_DISPLAY";
    case BLE_GAP_EVT_KEY_PRESSED: return "KEY_PRESSED";
    case BLE_GAP_EVT_AUTH_KEY_REQUEST: return "AUTH_KEY_REQUEST";
    case BLE_GAP_EVT_LESC_DHKEY_REQUEST: return "LESC_DHKEY_REQUEST";
    case BLE_GAP_EVT_AUTH_STATUS: return "AUTH_STATUS";
    case BLE_GAP_EVT_CONN_SEC_UPDATE: return "CONN_SEC_UPDATE";
    case BLE_GAP_EVT_TIMEOUT: return "TIMEOUT";
    case BLE_GAP_EVT_SEC_REQUEST: return "SEC_REQUEST";
    case BLE_GAP_EVT_PHY_UPDATE_REQUEST: return "PHY_UPDATE_REQUEST";
    case BLE_GAP_EVT_PHY_UPDATE: return "PHY_UPDATE";
    case BLE_GAP_EVT_DATA_LENGTH_UPDATE_REQUEST: return "DLE_REQUEST";
    case BLE_GAP_EVT_DATA_LENGTH_UPDATE: return "DLE_UPDATE";
    default: return "GAP";
  }
}
static volatile uint16_t connCount = 0, securedCount = 0;
static volatile uint8_t lastDiscReason = 0, lastPairStatus = 0xFF;
static volatile uint32_t lastConnMs = 0, lastDiscMs = 0;

// ---- hosts approved with the button (blePair = 1): their addresses, kept in PEER_FILE
static volatile uint8_t approveState = 0;  // 1 = new host / new bond waiting for the button
static volatile uint32_t approveSince = 0;
#define APPROVE_TIMEOUT_MS 60000
#define PEERS_N 8
static const char* PEER_FILE = "/lb_peers";
static char peers[PEERS_N][13];
static uint8_t nPeers = 0;
static char curPeer[13] = "";
static bool peerAllowed(const char* k) {
  for (uint8_t i = 0; i < nPeers; i++)
    if (!strcmp(peers[i], k)) return true;
  return false;
}
static void loadPeers() {
  using namespace Adafruit_LittleFS_Namespace;
  char buf[PEERS_N * 13 + 1];
  size_t n = 0;
  File f(InternalFS);
  if (f.open(PEER_FILE, FILE_O_READ)) {
    n = f.read(buf, sizeof(buf) - 1);
    f.close();
  }
  nPeers = 0;
  for (size_t i = 0; i + 12 <= n && nPeers < PEERS_N; i += 13) {
    memcpy(peers[nPeers], buf + i, 12);
    peers[nPeers++][12] = 0;
  }
}
static void savePeers() {  // main task only
  using namespace Adafruit_LittleFS_Namespace;
  InternalFS.remove(PEER_FILE);
  File f(InternalFS);
  if (!f.open(PEER_FILE, FILE_O_WRITE)) return;
  for (uint8_t i = 0; i < nPeers; i++) {
    f.write((const uint8_t*)peers[i], 12);
    f.write((const uint8_t*)"\n", 1);
  }
  f.close();
}
static void addPeer(const char* k) {
  if (!k[0] || peerAllowed(k)) return;
  if (nPeers == PEERS_N) {  // forget the oldest
    memmove(peers[0], peers[1], sizeof(peers[0]) * (PEERS_N - 1));
    nPeers--;
  }
  strcpy(peers[nPeers++], k);
}

static void onConnect(uint16_t h) {
  connCount++;
  lastConnMs = millis();
  connHandle = h;
  if (cfg.blePair == 1 && !cfg.blePin) {
    BLEConnection* c = Bluefruit.Connection(h);
    curPeer[0] = 0;
    if (c) {
      ble_gap_addr_t a = c->getPeerAddr();
      snprintf(curPeer, sizeof(curPeer), "%02X%02X%02X%02X%02X%02X", a.addr[5], a.addr[4], a.addr[3], a.addr[2], a.addr[1],
               a.addr[0]);
    }
    if (!peerAllowed(curPeer)) {  // a host we have not seen: dead until the button is pressed
      approveSince = millis();
      approveState = 1;
    }
  }
  // No PHY / data length / MTU requests from our side: the central (Windows, Android, ...)
  // starts the same procedures right after connecting, and when both ends do, the link layer
  // drops the connection with reason 0x23 (procedure collision) before services are read.
  // The SoftDevice still accepts whatever the central negotiates (MTU up to 247).
}
// ---- pairing
// blePair = 1 (default): no operating system pairing at all. A host whose address is not known
//   yet can connect, but the serial port and the config service stay dead until a short press
//   of the button on the device; the address is remembered after that. The link is not
//   encrypted. (Pairing from Windows + a browser proved unreliable: the pairing request often
//   never reaches the device, and nothing on the device side can fix that.)
// blePair = 2: LE Secure Connections numeric comparison: the code is shown on the OLED and has
//   to be confirmed on both sides. Encrypted + authenticated.
// blePair = 3: "Just Works" bonding (encrypted), and a NEW bond only becomes usable after a
//   short press of the button; without the press the bond is deleted again.

// Bonds are stored by Bluefruit as files in BOND_DIR. The names of the bonds that were approved
// with the button are kept in ALLOW_FILE; every other bond file is deleted. All file work is
// done from loop() (main task), the BLE callbacks only request it.
static const char* BOND_DIR = "/adafruit/bond_prph";
static const char* ALLOW_FILE = "/lb_bonds";
static volatile uint8_t bondJob = 0;  // 1 = approve what is new, 2 = purge what is not approved, 3 = save hosts
static volatile uint32_t bondJobAt = 0;
static void requestBondJob(uint8_t job, uint32_t delayMs) {
  bondJobAt = millis() + delayMs;
  bondJob = job;
}

static void syncBonds(bool approveNew) {
  using namespace Adafruit_LittleFS_Namespace;
  char allow[13 * 12 + 1];
  size_t alen = 0;
  {
    File f(InternalFS);
    if (f.open(ALLOW_FILE, FILE_O_READ)) {
      alen = f.read(allow, sizeof(allow) - 1);
      f.close();
    }
  }
  allow[alen] = 0;
  char next[sizeof(allow)] = "";
  char victims[8][13];
  uint8_t nv = 0;
  {
    File dir(BOND_DIR, FILE_O_READ, InternalFS);
    File e(InternalFS);
    while ((e = dir.openNextFile(FILE_O_READ))) {
      char name[16];
      strncpy(name, e.name(), sizeof(name) - 1);
      name[sizeof(name) - 1] = 0;
      bool isDir = e.isDirectory();
      e.close();
      if (isDir || strlen(name) != 12) continue;
      if (strstr(allow, name) || approveNew) {
        if (strlen(next) + 14 < sizeof(next)) {
          strcat(next, name);
          strcat(next, "\n");
        }
      } else if (nv < 8) {
        strcpy(victims[nv++], name);
      }
    }
    dir.close();
  }
  for (uint8_t i = 0; i < nv; i++) {
    char path[48];
    snprintf(path, sizeof(path), "%s/%s", BOND_DIR, victims[i]);
    InternalFS.remove(path);
  }
  if (strcmp(next, allow)) {
    InternalFS.remove(ALLOW_FILE);
    File f(InternalFS);
    if (f.open(ALLOW_FILE, FILE_O_WRITE)) {
      f.write((const uint8_t*)next, strlen(next));
      f.close();
    }
  }
}
static volatile uint8_t pairState = 0;  // 0 none, 1 waiting for the button, 2 passkey shown (typed on the phone)
static volatile int8_t pairDecision = -1;
static volatile uint8_t pairEvent = 0;
static char pairDigits[7] = "";

static void onDisconnect(uint16_t, uint8_t reason) {
  lastDiscReason = reason;
  lastDiscMs = millis();
  pairState = 0;
  if (approveState) {  // left before the button was pressed: forget the bond
    approveState = 0;
    pairEvent = 2;
    if (cfg.blePair == 3) requestBondJob(2, 1500);
  }
  connHandle = BLE_CONN_HANDLE_INVALID;
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
static void onSecured(uint16_t) { securedCount++; }
static void onPairComplete(uint16_t, uint8_t status) {
  lastPairStatus = status;
  pairState = 0;
  if (status == BLE_GAP_SEC_STATUS_SUCCESS && cfg.blePair == 3 && !cfg.blePin) {
    approveSince = millis();  // a new bond: usable only after the button
    approveState = 1;
    return;
  }
  pairEvent = status == BLE_GAP_SEC_STATUS_SUCCESS ? 1 : 2;
}

static void approveReject() {
  uint16_t h = connHandle;
  approveState = 0;
  pairEvent = 2;
  if (h != BLE_CONN_HANDLE_INVALID) Bluefruit.disconnect(h);
  if (cfg.blePair == 3) requestBondJob(2, 1500);
}

bool pairPending() { return pairState == 1 || approveState == 1; }
bool pairShowing() { return pairState != 0 || approveState == 1; }
bool pairNeedsCode() { return pairState != 0; }
const char* pairCode() { return pairDigits; }
void pairConfirm(bool ok) {
  if (approveState == 1) {
    if (ok) {
      if (cfg.blePair == 3) {
        requestBondJob(1, 300);
      } else {
        addPeer(curPeer);
        requestBondJob(3, 100);
      }
      approveState = 0;
      pairEvent = 1;
    } else {
      approveReject();
    }
    return;
  }
  pairDecision = ok ? 1 : 0;
}
uint8_t takePairEvent() {
  uint8_t e = pairEvent;
  pairEvent = 0;
  return e;
}
void clearBonds() {
  Bluefruit.Periph.clearBonds();
  InternalFS.remove(ALLOW_FILE);
  InternalFS.remove(PEER_FILE);
  nPeers = 0;
}
void diag(Print& o) {
  o.printf("+BLEST=pair_mode:%u,hosts:%u,peer:%s,waiting_button:%u,connected:%u,uart:%u,connections:%u,secured:%u,last_conn_ms:%lu,last_disc_ms:%lu,"
           "last_disc_reason:0x%02X,last_pair_status:0x%02X,now_ms:%lu\r\n",
           cfg.blePair, nPeers, connected() ? curPeer : "-", approveState, connected() ? 1 : 0, uartReady() ? 1 : 0, connCount, securedCount, (unsigned long)lastConnMs,
           (unsigned long)lastDiscMs, lastDiscReason, lastPairStatus, (unsigned long)millis());
  uint8_t n = evCount, start = (uint8_t)((evHead + EVLOG_N - n) % EVLOG_N);
  for (uint8_t i = 0; i < n; i++) {
    const EvLog& e = evlog[(start + i) % EVLOG_N];
    o.printf("  %8lu %s(0x%02X) 0x%lX\r\n", (unsigned long)e.ms, evName(e.id), e.id, (unsigned long)e.info);
  }
}

void begin() {
  Bluefruit.autoConnLed(false);
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  Bluefruit.begin(1, 0);
  Bluefruit.setTxPower(4);
  Bluefruit.setName(cfg.bleName);
  Bluefruit.setEventCallback(onBleEvent);
  Bluefruit.Periph.setConnectCallback(onConnect);
  Bluefruit.Periph.setDisconnectCallback(onDisconnect);

  SecureMode_t perm = SECMODE_OPEN;
  if (cfg.blePin) {
    char pin[8];
    snprintf(pin, sizeof(pin), "%06lu", (unsigned long)(cfg.blePin % 1000000));
    Bluefruit.Security.setPIN(pin);
    perm = SECMODE_ENC_WITH_MITM;
  } else if (cfg.blePair == 1) {
    loadPeers();  // open characteristics; unknown hosts are held back until the button
  } else if (cfg.blePair == 3) {
    // Just Works bonding (encrypted link), approved with the button afterwards
    Bluefruit.Security.setIOCaps(false, false, false);
    Bluefruit.Security.setMITM(false);
    perm = SECMODE_ENC_NO_MITM;
    requestBondJob(2, 2000);  // drop bonds that were never approved (e.g. power lost while waiting)
  } else if (cfg.blePair) {
    // display + yes/no: numeric comparison, accepted with the button
    Bluefruit.Security.setIOCaps(true, true, false);
    Bluefruit.Security.setMITM(true);
    Bluefruit.Security.setPairPasskeyCallback(onPasskey);
    perm = SECMODE_ENC_WITH_MITM;
  }
  Bluefruit.Security.setPairCompleteCallback(onPairComplete);
  Bluefruit.Security.setSecuredCallback(onSecured);

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
bool uartReady() { return connected() && bleuart.notifyEnabled() && !approveState; }
int uartAvailable() { return approveState ? 0 : bleuart.available(); }
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
  if (approveState) {
    // nothing from an unapproved peer is processed
    while (bleuart.available()) bleuart.read();
    cmdTail = cmdHead;
    if (millis() - approveSince > APPROVE_TIMEOUT_MS) approveReject();
  }
  if (bondJob && (int32_t)(millis() - bondJobAt) >= 0) {
    uint8_t job = bondJob;
    bondJob = 0;
    if (job == 3) savePeers();
    else syncBonds(job == 1);
  }
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

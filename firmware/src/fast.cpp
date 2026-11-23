// FAST mode: negotiation over LoRa, data over the 2.4 GHz link, supervision and fallback.
//
// Control frames (LoRa frame type FT_FAST, payload):
//   REQ    : 1, nonceA(4), flags(bit0 = encrypt), nch, channels...   initiator -> responder
//   ACCEPT : 2, nonceA(4), nonceB(4)                                 responder starts its radio first
//   REJECT : 3, nonceA(4), reason
//   END    : 4, nonceA(4), flags(bit0 = "I have your count"), reason, rxCount(4)
//
// Switching without losing or reordering bytes:
//   * to 2.4 GHz: a side only moves bytes to the 2.4 GHz queue once its last LoRa data frame
//     is finished (acknowledged or given up), so the receiver sees them in order.
//   * back to LoRa: both ends stop their 2.4 GHz radio and tell each other over LoRa how many
//     bytes they received (END). The bytes the peer did not get are put back at the front of
//     the LoRa queue; LoRa data is held until that is settled (or END went unanswered).
//
// The 2.4 GHz byte streams are encrypted like LoRa data frames (AES-128-CTR with the pairing
// key, keystream position = byte position in the stream, nonce = both session nonces + the
// sender's ID). The radio handler never touches the cipher.
#include "fast.h"

#include <nrf_soc.h>

#include "ble_svc.h"
#include "config.h"
#include "display.h"
#include "lora_link.h"
#include "rf24.h"

extern size_t serialOutSpace();  // main.cpp: room in the serial output buffers


namespace fast {

enum : uint8_t { C_REQ = 1, C_ACCEPT, C_REJECT, C_END };
enum : uint8_t {
  R_NONE, R_USER, R_LOSS, R_RSSI, R_LOST, R_BLE, R_LORA, R_NOLINK,
  R_REJECTED, R_NOANSWER, R_PEER, R_DISABLED, R_RADIO, R_BUSY, R_N_
};
static const char* const kReason[R_N_] = {"", "user", "packet loss", "weak signal", "2.4G link lost",
                                          "Bluetooth in use", "LoRa link down", "no 2.4G link", "rejected",
                                          "no answer", "peer", "disabled", "radio busy", "busy"};
static const char* const kShort[R_N_] = {"", "USER", "LOSS", "RSSI", "LOST", "BT", "LORA", "NO LINK",
                                         "REJECT", "NO ANS", "PEER", "DISABLED", "RADIO", "BUSY"};

#define REQ_TRIES 4
#define END_TRIES 5
#define PROBE_GOOD 20          // valid packets that prove the 2.4 GHz link
#define PROBE_INIT_MS 4000     // initiator: the peer is already listening
#define PROBE_RESP_MS 15000    // responder: the initiator may need several tries to get ACCEPT
#define WINDOW_MS 500
#define LOST_MS 800
#define ENDING_MAX_MS 20000
#define RX_UNKNOWN 0xFFFFFFFFu

static State st = OFF;
static uint32_t nonceA = 0, nonceB = 0, peer = 0;
static bool sessionEnc = false;
static uint8_t chs[RF24_MAX_CH], nch = 0;
static uint32_t t0 = 0, nextCtl = 0, probeMs = 0;
static uint8_t tries = 0;
static uint8_t reply[20], replyLen = 0;
static char reasonStr[40] = "";

// stream positions
static uint32_t txPos = 0, rxPos = 0;
// ending
static uint8_t left[2048];
static size_t leftLen = 0;
static uint32_t acked = 0, myRx = 0;
static uint8_t endReason = 0;
// the last finished session (to answer a repeated END)
static uint32_t lastA = 0, lastRx = 0;
static bool haveLast = false;
// supervision
static rf24::Stats w0;
static uint32_t winAt = 0, lastGood = 0, lastGoodMs = 0, good0 = 0;
static uint8_t badWin = 0, winLoss = 0;
static int winRssi = -127;

static inline void put32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
static inline uint32_t get32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

static void setReason(uint8_t r, uint8_t sub = 0) {
  if (r >= R_N_) r = R_NONE;
  if (sub && sub < R_N_) snprintf(reasonStr, sizeof(reasonStr), "%s: %s", kReason[r], kReason[sub]);
  else snprintf(reasonStr, sizeof(reasonStr), "%s", kReason[r]);
}
static void toastEnd(const char* what, uint8_t r) {
  char m[22];
  snprintf(m, sizeof(m), "%s:%s", what, kShort[r < R_N_ ? r : 0]);
  display::toast(m, 2500);
}

// ---------------------------------------------------------------- stream cipher
struct Ks {
  uint32_t blk;
  bool valid;
  uint8_t ks[16];
};
static Ks txKs, rxKs;
static void xorStream(uint8_t* d, size_t n, uint32_t pos, uint32_t sender, Ks& c) {
  for (size_t i = 0; i < n; i++, pos++) {
    uint32_t b = pos >> 4;
    if (!c.valid || c.blk != b) {
      if (sessionEnc) {
        nrf_ecb_hal_data_t e;
        memcpy(e.key, cfg.key, 16);
        memcpy(e.cleartext + 0, &nonceA, 4);
        memcpy(e.cleartext + 4, &nonceB, 4);
        memcpy(e.cleartext + 8, &sender, 4);
        memcpy(e.cleartext + 12, &b, 4);
        sd_ecb_block_encrypt(&e);
        memcpy(c.ks, e.ciphertext, 16);
      } else {
        // encryption off: scramble only, so that long runs of equal bytes do not go on the
        // air as they are (the radio does no whitening)
        uint32_t x = ((b + 1) * 2654435761u) ^ sender ^ nonceA;
        for (int k = 0; k < 16; k++) {
          x ^= x << 13;
          x ^= x >> 17;
          x ^= x << 5;
          c.ks[k] = (uint8_t)x;
        }
      }
      c.blk = b;
      c.valid = true;
    }
    d[i] ^= c.ks[pos & 15];
  }
}
static bool keyIsZero() {
  for (int i = 0; i < 16; i++)
    if (cfg.key[i]) return false;
  return true;
}

// ---------------------------------------------------------------- data pumps
static void pumpRx(bool force) {
  uint8_t buf[256];
  for (int k = 0; k < (force ? 64 : 4) && rf24::rxAvailable(); k++) {
    if (!force && serialOutSpace() < sizeof(buf)) break;  // back-pressure: the peer repeats
    size_t n = rf24::rxRead(buf, sizeof(buf));
    xorStream(buf, n, rxPos, peer, rxKs);
    rxPos += n;
    link::fastDeliver(buf, n);
  }
}
static void pumpTx() {
  uint8_t buf[256];
  for (int k = 0; k < 4; k++) {
    size_t sp = rf24::txSpace();
    if (sp < 64) break;
    size_t n = link::fastTake(buf, sp < sizeof(buf) ? sp : sizeof(buf));
    if (!n) break;
    xorStream(buf, n, txPos, g_nodeId, txKs);
    txPos += n;
    rf24::txWrite(buf, n);
  }
}

// ---------------------------------------------------------------- session start / end
static bool linkReady() {
  return cfg.peerId && link::radioOk() && link::isConn() && link::state() == LINK_UP && !link::isPairing();
}
static bool linkStillOk() { return cfg.peerId && cfg.peerId == peer && link::isConn() && !link::isPairing(); }

static void pickChannels() {
  // the quietest channels of the scan, spread out so that one Wi-Fi network cannot cover them all
  uint8_t order[RF24_SCAN_N];
  for (uint8_t i = 0; i < RF24_SCAN_N; i++) order[i] = i;
  for (uint8_t i = 1; i < RF24_SCAN_N; i++)
    for (uint8_t j = i; j > 0 && rf24::scanLevel(order[j - 1]) > rf24::scanLevel(order[j]); j--) {
      uint8_t t = order[j];
      order[j] = order[j - 1];
      order[j - 1] = t;
    }
  nch = 0;
  for (int sep = 6; sep >= 2 && nch < RF24_MAX_CH; sep -= 2)
    for (uint8_t i = 0; i < RF24_SCAN_N && nch < RF24_MAX_CH; i++) {
      uint8_t c = rf24::scanChannel(order[i]);
      bool ok = true;
      for (uint8_t k = 0; k < nch; k++)
        if (abs((int)c - (int)chs[k]) < sep) ok = false;
      if (ok) chs[nch++] = c;
    }
}

static bool startRadio() {
  rf24::Params p;
  p.master = link::isMaster();
  p.nch = nch;
  memcpy(p.ch, chs, sizeof(p.ch));
  peer = cfg.peerId;
  uint32_t a = g_nodeId < peer ? g_nodeId : peer, b = g_nodeId < peer ? peer : g_nodeId;
  uint32_t h = crc32(cfg.key, 16, nonceA);
  h = crc32(&nonceB, 4, h);
  h = crc32(&a, 4, h);
  h = crc32(&b, 4, h);
  uint32_t h2 = crc32(&h, 4, 0x24242424u);
  p.addrBase = h;
  p.addrPrefix = (uint8_t)h2;
  if (p.addrPrefix == 0x00 || p.addrPrefix == 0xFF || p.addrPrefix == 0x55 || p.addrPrefix == 0xAA) p.addrPrefix = 0xC6;
  p.crcInit = h2 >> 8;
  p.txPower = cfg.fastPower;
  txPos = rxPos = 0;
  txKs.valid = rxKs.valid = false;
  leftLen = 0;
  if (!rf24::start(p)) return false;
  ble::pauseAdv(true);  // no Bluetooth advertising while the 2.4 GHz link runs
  rf24::stats(w0);
  good0 = w0.good;
  uint32_t now = millis();
  winAt = lastGoodMs = t0 = now;
  lastGood = good0;
  badWin = 0;
  winLoss = 0;
  winRssi = -127;
  return true;
}

// radio off; what the peer did not acknowledge stays in left[]
static void shutRadio() {
  rf24::stop();
  ble::pauseAdv(false);
  pumpRx(true);
  myRx = rxPos;
  acked = rf24::txAcked();
  leftLen = rf24::txLeftover(left, sizeof(left));
}
// the peer has received peerRx bytes: everything after that goes back to the LoRa queue
static void finalize(uint32_t peerRx) {
  int32_t d = peerRx == RX_UNKNOWN ? 0 : (int32_t)(peerRx - acked);
  size_t skip = d <= 0 ? 0 : ((size_t)d > leftLen ? leftLen : (size_t)d);
  if (leftLen > skip) {
    xorStream(left + skip, leftLen - skip, acked + skip, g_nodeId, txKs);
    link::fastRequeue(left + skip, leftLen - skip);
  }
  leftLen = 0;
  lastA = nonceA;
  lastRx = myRx;
  haveLast = true;
  st = OFF;
}
static void end(uint8_t reason) {
  shutRadio();
  endReason = reason;
  setReason(reason);
  toastEnd("FAST OFF", reason);
  st = ENDING;
  tries = 0;
  t0 = nextCtl = millis();
}
static void fail(uint8_t reason, uint8_t sub = 0) {
  setReason(reason, sub);
  toastEnd("FAST FAIL", sub ? sub : reason);
  lastA = nonceA;
  lastRx = 0;
  haveLast = true;
  st = OFF;
}

static void queueEnd(uint32_t nonce, uint8_t flags, uint8_t reason, uint32_t rx) {
  reply[0] = C_END;
  put32(reply + 1, nonce);
  reply[5] = flags;
  reply[6] = reason;
  put32(reply + 7, rx);
  replyLen = 11;
}

// ---------------------------------------------------------------- public
const char* request() {
  if (st == UP || st == PROBE) return nullptr;
  if (st != OFF) return "busy";
  if (!cfg.fastEnable) return "disabled (AT+FASTEN=1)";
  if (!cfg.peerId) return "not paired";
  if (!linkReady()) return "LoRa link not up";
  if (ble::connected()) return "Bluetooth in use";
  if (!rf24::scanStart()) return "radio busy";
  reasonStr[0] = 0;
  st = SCAN;
  t0 = millis();
  return nullptr;
}

void release() {
  if (st == UP || st == PROBE) {
    end(R_USER);
  } else if (st == SCAN) {
    rf24::stop();
    st = OFF;
  } else if (st == REQ) {
    fail(R_USER);
  }
}

State state() { return st; }
bool isUp() { return st == UP; }
bool negotiating() { return st == SCAN || st == REQ || st == PROBE; }
const char* stateName() {
  static const char* const n[] = {"OFF", "SCAN", "REQ", "PROBE", "UP", "END"};
  return n[st];
}
const char* lastReason() { return reasonStr; }
int rssi() { return (winRssi == -127 && (st == UP || st == PROBE)) ? rf24::lastRssi() : winRssi; }  // (before the first window)
uint8_t lossPct() { return winLoss; }
float freqMHz() { return 2400.0f + rf24::channel(); }
uint8_t channels() { return nch; }
uint8_t channelsInUse() { return (uint8_t)__builtin_popcount(rf24::channelMask() & ((1u << nch) - 1)); }
bool busy() { return (st == UP || st == PROBE) && (rf24::txPending() || rf24::rxAvailable()); }
bool holdData() { return st == UP || st == ENDING; }

void stat(Print& o) {
  rf24::Stats s;
  rf24::stats(s);

  o.printf("FAST: %s", stateName());
  if (st == UP || st == PROBE) {
    o.printf("  RSSI %d  loss %u%%  hop %u/%u ch:", winRssi, winLoss, channelsInUse(), nch);
    for (uint8_t i = 0; i < nch; i++) o.printf(" %u", 2400 + chs[i]);
    o.printf("\r\nFAST: packets %lu  missed %lu  CRC %lu  polls %lu  slots %lu  TX %lu B  RX %lu B  enc %u\r\n",
             (unsigned long)s.good, (unsigned long)s.miss, (unsigned long)s.crcErr, (unsigned long)s.polls,
             (unsigned long)s.slots, (unsigned long)rf24::txAcked(), (unsigned long)rxPos, sessionEnc ? 1 : 0);
  } else {
    o.printf("  last: %s\r\n", reasonStr[0] ? reasonStr : "-");
  }
}

void loop() {
  rf24::loop();
  if (st == OFF) return;
  uint32_t now = millis();
  bool due = (int32_t)(now - nextCtl) >= 0;

  switch (st) {
    case SCAN:
      if (rf24::scanDone()) {
        pickChannels();
        nonceA = link::random32();
        sessionEnc = cfg.encrypt && !keyIsZero();
        st = REQ;
        tries = 0;
        nextCtl = now;
      } else if (now - t0 > 1500) {
        rf24::stop();
        fail(R_RADIO);
      }
      break;

    case REQ:
      if (ble::connected()) fail(R_BLE);
      else if (!cfg.peerId || !link::isConn() || link::isPairing()) fail(R_LORA);
      else if (tries >= REQ_TRIES && due) fail(R_NOANSWER);
      break;

    case PROBE:
    case UP: {
      if (cfg.peerId != peer) {  // unpaired / re-paired underneath us: nobody to settle with
        shutRadio();
        setReason(R_LORA);
        finalize(RX_UNKNOWN);
        break;
      }
      if (ble::connected()) return end(R_BLE);
      if (!linkStillOk()) return end(R_LORA);
      pumpRx(false);
      rf24::Stats s;
      rf24::stats(s);
      if (s.good != lastGood) {
        lastGood = s.good;
        lastGoodMs = now;
      }
      if (now - winAt >= WINDOW_MS) {
        uint32_t g = s.good - w0.good, m = s.miss - w0.miss, rs = s.rssiSum - w0.rssiSum;
        w0 = s;
        winAt = now;
        uint32_t tot = g + m;
        winLoss = (g < 5 || !tot) ? 100 : (uint8_t)(m * 100 / tot);
        winRssi = g ? -(int)(rs / g) : -127;
        if (st == UP) {
          bool lossy = winLoss > cfg.fastLoss, weak = g && winRssi < cfg.fastRssi;
          if (lossy || weak) {
            if (++badWin >= 2) return end(lossy ? R_LOSS : R_RSSI);
          } else {
            badWin = 0;
          }
        }
      }
      if (st == PROBE) {
        if (s.good - good0 >= PROBE_GOOD) {
          st = UP;
          badWin = 0;
          lastGoodMs = now;
          reasonStr[0] = 0;
          display::toast("FAST 2.4G ON", 1500);
        } else if (now - t0 > probeMs) {
          return end(R_NOLINK);
        }
      } else {
        if (now - lastGoodMs > LOST_MS) return end(R_LOST);
        pumpTx();
      }
      break;
    }

    case ENDING:
      // no END from the peer: assume it has exactly what it acknowledged (may repeat one packet)
      if ((tries >= END_TRIES && due) || now - t0 > ENDING_MAX_MS) finalize(RX_UNKNOWN);
      break;

    default:
      break;
  }
}

// ---------------------------------------------------------------- LoRa control channel
size_t ctlPoll(uint8_t* buf, size_t max) {
  if (max < sizeof(reply)) return 0;
  if (replyLen) {
    size_t n = replyLen;
    memcpy(buf, reply, n);
    replyLen = 0;
    return n;
  }
  uint32_t now = millis();
  if ((int32_t)(now - nextCtl) < 0) return 0;
  uint32_t retry = 3 * link::dwellMs();
  if (retry < 2500) retry = 2500;
  retry += link::random32() % 500;
  if (st == REQ && tries < REQ_TRIES) {
    tries++;
    nextCtl = now + retry;
    buf[0] = C_REQ;
    put32(buf + 1, nonceA);
    buf[5] = sessionEnc ? 1 : 0;
    buf[6] = nch;
    memcpy(buf + 7, chs, nch);
    return 7 + nch;
  }
  if (st == ENDING && tries < END_TRIES) {
    tries++;
    nextCtl = now + retry;
    buf[0] = C_END;
    put32(buf + 1, nonceA);
    buf[5] = 0;
    buf[6] = endReason;
    put32(buf + 7, myRx);
    return 11;
  }
  return 0;
}

static void queueReject(uint32_t nonce, uint8_t reason) {
  reply[0] = C_REJECT;
  put32(reply + 1, nonce);
  reply[5] = reason;
  replyLen = 6;
}
static void queueAccept() {
  reply[0] = C_ACCEPT;
  put32(reply + 1, nonceA);
  put32(reply + 5, nonceB);
  replyLen = 9;
}

void onCtl(const uint8_t* pl, size_t n) {
  if (n < 5) return;
  uint32_t nonce = get32(pl + 1);
  switch (pl[0]) {
    case C_REQ: {
      if (n < 8 || pl[6] < 1 || pl[6] > RF24_MAX_CH || n < 7u + pl[6]) return;
      if ((st == PROBE || st == UP) && nonce == nonceA) {  // our ACCEPT got lost: say it again
        queueAccept();
        return;
      }
      if (st == SCAN || st == REQ) {
        // both ends asked at the same time: the master's request wins
        if (link::isMaster()) return;
        if (st == SCAN) rf24::stop();
        st = OFF;
      }
      if (st == PROBE || st == UP) {  // the peer started over (e.g. it was reset)
        shutRadio();
        finalize(RX_UNKNOWN);
      }
      if (st != OFF) return queueReject(nonce, R_BUSY);
      if (!cfg.fastEnable) return queueReject(nonce, R_DISABLED);
      if (ble::connected()) return queueReject(nonce, R_BLE);
      if (!linkReady()) return queueReject(nonce, R_LORA);
      for (uint8_t i = 0; i < pl[6]; i++)
        if (pl[7 + i] > 80) return queueReject(nonce, R_RADIO);
      nonceA = nonce;
      nonceB = link::random32();
      sessionEnc = (pl[5] & 1) && !keyIsZero();
      nch = pl[6];
      memcpy(chs, pl + 7, nch);
      if (!startRadio()) return queueReject(nonce, R_RADIO);
      probeMs = PROBE_RESP_MS;
      reasonStr[0] = 0;
      st = PROBE;
      queueAccept();
      break;
    }
    case C_ACCEPT:
      if (st != REQ || nonce != nonceA || n < 9) return;
      nonceB = get32(pl + 5);
      if (!startRadio()) return fail(R_RADIO);
      probeMs = PROBE_INIT_MS;
      st = PROBE;
      break;
    case C_REJECT:
      if (st != REQ || nonce != nonceA || n < 6) return;
      fail(R_REJECTED, pl[5]);
      break;
    case C_END: {
      if (n < 11) return;
      bool hasMine = pl[5] & 1;
      uint32_t rx = get32(pl + 7);
      if ((st == PROBE || st == UP) && nonce == nonceA) {
        shutRadio();
        setReason(R_PEER, pl[6]);
        toastEnd("FAST OFF", pl[6] < R_N_ ? pl[6] : R_PEER);
        uint32_t mine = myRx;
        finalize(rx);
        queueEnd(nonce, 1, R_PEER, mine);
      } else if (st == ENDING && nonce == nonceA) {
        uint32_t mine = myRx;
        finalize(rx);
        if (!hasMine) queueEnd(nonce, 1, endReason, mine);
      } else if (st == REQ && nonce == nonceA) {  // accepted and given up before we heard of it
        fail(R_PEER, pl[6]);
        if (!hasMine) queueEnd(nonce, 1, R_PEER, 0);
      } else if (!hasMine) {
        // a session that is over here (or that we never knew, e.g. after a reset)
        bool known = haveLast && nonce == lastA && st == OFF;
        queueEnd(nonce, 1, R_PEER, known ? lastRx : RX_UNKNOWN);
      }
      break;
    }
  }
}

}  // namespace fast

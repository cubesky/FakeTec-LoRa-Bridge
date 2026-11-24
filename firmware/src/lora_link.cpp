// LoRa point-to-point serial link
//
//  * Framing  : 16 byte header + payload (max 255 bytes per LoRa packet)
//  * ARQ      : stop-and-wait with ACK, retries, random backoff, duplicate suppression
//  * Pairing  : button / AT+PAIR, key + peer ID exchanged on the base channel
//  * FHSS     : time-slotted hopping, the node with the lower ID is the clock master,
//               every master frame carries its clock; lost link -> both park on the
//               base (sync) channel, master beacons SYNC until the slave answers
//  * AFH      : per-channel loss / CRC / noise statistics, bad channels are blacklisted
//               by the master and distributed to the slave
//  * LBT      : preamble / header detection (+ optional RSSI threshold) before TX
//  * Security : AES-128-CTR (nRF52 ECB via SoftDevice) for DATA payloads when paired
#include "lora_link.h"

#include <RadioLib.h>
#include <nrf_soc.h>

#include "config.h"
#include "eth.h"
#include "fast.h"
#include "lz.h"
#include "pins.h"
#include "ringbuf.h"

#ifdef LORA_CHIP_SX1262
#define RADIO_BASE SX1262
#else
#define RADIO_BASE SX1268
#endif

// RadioLib rejects a chip whose version string does not start with the expected text
// ("SX1268" for the SX1268 class), but SX1268 dies on Ra-01S and many other modules
// report "SX1261 ...". Like Meshtastic (which probes several driver classes), accept
// any SX126x-family version string that we read from the chip ourselves.
class RadioAuto : public RADIO_BASE {
 public:
  using RADIO_BASE::RADIO_BASE;
  void expectChip(const char* t) { chipType = t; }
  Module* module() { return getMod(); }
};
static RadioAuto radio = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY);

namespace link {

// ---------------------------------------------------------------- frame format
#define FRAME_MAGIC 0xB0
enum : uint8_t {
  FT_DATA = 1,
  FT_ACK = 2,
  FT_PING = 3,
  FT_SYNC = 4,
  FT_SYNC_ACK = 5,
  FT_PAIR_REQ = 6,
  FT_PAIR_ACK = 7,
  FT_FAST = 8,  // FAST mode (2.4 GHz) negotiation, see fast.cpp
  FT_ETH = 9,   // ETH mode: the shared network setting, see eth.cpp
};
#define FL_ENC 0x01
#define FL_ACKREQ 0x02
#define FL_ACTIVE 0x04   // sender has an open serial port (USB / BLE UART): wants the link up
#define FL_AWAKE 0x08    // sender's receiver is continuously on (CONN), no wake-up preamble needed
#define FL_LONGPRE 0x10  // frame was sent with the long wake-up preamble
#define FL_COMP 0x20     // DATA payload is LZ compressed (before encryption)
#define FL_MUX 0x40      // DATA payload is a piece of the framed stream (serial data + IP packets)
#define BCAST 0xFFFFFFFFu

struct __attribute__((packed)) Hdr {
  uint8_t magicType;
  uint8_t flags;
  uint32_t src;
  uint32_t dst;
  uint16_t seq;
  uint32_t time;  // sender's hop clock (ms) at TX start
};
static_assert(sizeof(Hdr) == 16, "header size");
#define HDR_LEN 16
#define MAX_FRAME 255
#define MAX_PAYLOAD (MAX_FRAME - HDR_LEN)
#define MAX_CH 16

// ---------------------------------------------------------------- state
static RingBuf<4096> txRing;
static uint32_t lastQueueIn = 0;
static void (*rxHandler)(const uint8_t*, size_t) = nullptr;

static volatile bool irqFlag = false;
static volatile TickType_t irqTicks = 0;
static void onDio1() {
  irqTicks = xTaskGetTickCountFromISR();
  irqFlag = true;
}
// millis() at the moment the last DIO1 interrupt fired (main loop may lag behind)
static uint32_t irqMillis() {
  uint32_t lag = (uint32_t)(xTaskGetTickCount() - irqTicks) * 1000u / configTICK_RATE_HZ;
  return millis() - lag;
}

// timing margins (main loop latency incl. OLED refresh can reach ~30 ms)
#define ACK_MARGIN_MS 100
#define SLOT_GUARD_MS 40

enum RadioMode : uint8_t { RM_OFF, RM_RX, RM_TX };
static RadioMode mode = RM_OFF;
static bool radioReady = false;
static char radioMsg[48] = "init";
static uint32_t txStartMs = 0, txTimeoutMs = 0, lastTxMs = 0, lastRxMs = 0;
static uint32_t fastTxMs = 0, fastRxMs = 0;  // activity on the 2.4 GHz bypass
static uint8_t txType = 0;
static bool txNeedsAck = false;

static LinkState lstate = LINK_UNPAIRED;
static uint32_t lastHeard = 0;
static LinkStats st;
static uint8_t localBatt = 255;

// data frame in flight
static uint8_t pendBuf[MAX_PAYLOAD];
static size_t pendLen = 0;   // bytes on air
static size_t pendRaw = 0;   // user bytes they carry
static bool pendComp = false;
static bool pendValid = false;
static uint16_t txSeq = 0;
static uint8_t pendTries = 0;
static bool waitingAck = false;
static uint32_t ackDeadline = 0;
static uint32_t nextTxAt = 0;

// ACK / control frames to send
static bool ackPending = false;
static uint16_t ackSeq = 0;
static bool syncAckPending = false;
static bool forcePing = false;

// RX duplicate detection
static bool haveRxSeq = false;
static uint16_t lastRxSeq = 0;
static uint32_t lastRxSrc = 0;

// pairing
static bool pairing = false;
static uint32_t pairUntil = 0, nextPairBeacon = 0;
static uint8_t pairKey[16];
static bool pairAckPending = false;
static uint32_t pairAckDst = 0;
static uint8_t pairAckKey[16];
static bool pairDoneAfterTx = false;

// hopping
static int32_t clockOffset = 0;
static uint8_t perm[MAX_CH];
static uint8_t curCh = 0;
static uint32_t curSlot = 0;
static uint32_t dwell = 400;
static uint32_t toaAckMs = 30, toaMaxMs = 300;
// Time the other side needs to start a frame and for our receiver to detect its preamble.
// Used for turn-taking so that two busy nodes alternate instead of colliding.
static uint32_t turnMs = 20;
// Max time from preamble detection to a valid header; longer = false detection.
static uint32_t hdrWaitMs = 40;
// The ACK is held back this long after RX_DONE so the sender is surely back in RX
// (its main loop needs a few ms to notice TX_DONE; the ACK preamble is only ~8 symbols).
#define ACK_TURN_MS 10
static uint32_t ackAt = 0;
static uint32_t nextSyncBeacon = 0;

// power save: STBY = duty-cycled receiver on the base channel + slow heartbeat,
//             CONN = receiver always on, hopping, data flows.
// CONN (shown as LINK) needs BOTH sides to have a serial port in use (USB / WebUSB / BLE UART);
// each frame carries the sender's state, so unplugging either end drops both to standby.
static bool localActive = false, peerActive = false, peerAwake = false;
static bool conn = true, dutyRx = false, txLong = false;
static uint32_t idleSince = 0, connSince = 0;
static uint16_t wakePre = 64;      // wake-up preamble length in symbols
static uint32_t wakeExtraMs = 0;   // extra time on air of the wake-up preamble
static uint8_t byePings = 0;
#define WAKE_PREAMBLE_MS 300
#define CONN_LINGER_MS 2000

// AFH
struct ChStat {
  uint16_t ok, fail, crc;
  float noise;
  uint32_t badUntil;
};
static ChStat chs[MAX_CH];
static uint16_t ownBadMask = 0;    // locally detected bad channels
static uint16_t peerBadMask = 0;   // reported by slave (master only)
static uint16_t activeMask = 0;    // mask used for hopping
static uint32_t nextAfhEval = 0, nextNoiseSample = 0;

// LBT
static uint32_t busySince = 0;

// rate meter
static uint32_t rateT = 0, rateTx0 = 0, rateRx0 = 0;

// ---------------------------------------------------------------- stream framing (ETH mode)
// With ETH mode on at both ends, the byte stream to the peer carries two things: the serial
// data and IP packets. It is cut into frames
//     C0 <type> <payload> C0        C0 / C1 inside the payload are sent as C1 E0 / C1 E1
// (neither value occurs in UTF-8 text, so terminal traffic costs 3 bytes per frame and no
// more). A frame goes into the queue as a whole or not at all, and the framing is switched
// only while the queue is empty: each LoRa data frame says which kind it carries (FL_MUX), a
// 2.4 GHz session is told once (fast.cpp). ARQ, compression and the FAST hand-over underneath
// see plain bytes, as before; after a lost LoRa frame the receiver is back in step at the
// next C0.
#define MUX_FLAG 0xC0
#define MUX_ESC 0xC1
#define MUX_SERIAL 1
#define MUX_IP 2
#define MUX_IP_MAX 1500
#define NET_BACKLOG 1600  // IP packets are not worth queueing for long: beyond this they are dropped
static bool muxTx = false, muxWant = false, pendMux = false, fastRxMux = false;
static void (*netHandler)(const uint8_t*, size_t) = nullptr;
static uint8_t dType = 0;  // frame being received: 0 = none (skip to the next C0), 0xFF = its type byte is next
static bool dEsc = false, dBad = false;
static uint8_t dIp[MUX_IP_MAX];
static size_t dIpLen = 0;

static inline bool muxSpecial(uint8_t c) { return c == MUX_FLAG || c == MUX_ESC; }
static size_t muxSize(const uint8_t* d, size_t n) {
  size_t k = n + 3;
  for (size_t i = 0; i < n; i++)
    if (muxSpecial(d[i])) k++;
  return k;
}
// (the caller has checked that muxSize() bytes fit)
static void muxPut(uint8_t type, const uint8_t* d, size_t n) {
  uint8_t b[68];
  size_t k = 0;
  b[k++] = MUX_FLAG;
  b[k++] = type;
  for (size_t i = 0; i < n; i++) {
    uint8_t c = d[i];
    if (muxSpecial(c)) {
      b[k++] = MUX_ESC;
      b[k++] = c ^ 0x20;
    } else {
      b[k++] = c;
    }
    if (k >= 64) {
      txRing.write(b, k);
      k = 0;
    }
  }
  b[k++] = MUX_FLAG;
  txRing.write(b, k);
}
// received user bytes -> serial ports / network
static void deliver(const uint8_t* d, size_t n, bool mux) {
  if (!mux) {
    dType = 0;
    if (rxHandler && n) rxHandler(d, n);
    return;
  }
  static uint8_t ser[256];
  size_t sn = 0;
  for (size_t i = 0; i < n; i++) {
    uint8_t c = d[i];
    if (c == MUX_FLAG) {
      if (dType == MUX_IP && !dBad && !dEsc && dIpLen && netHandler) netHandler(dIp, dIpLen);
      dType = 0xFF;
      dEsc = dBad = false;
      dIpLen = 0;
      continue;
    }
    if (dType == 0xFF) {
      dType = (c == MUX_SERIAL || c == MUX_IP) ? c : 0;
      continue;
    }
    if (!dType) continue;
    if (c == MUX_ESC) {
      dEsc = true;
      continue;
    }
    if (dEsc) {
      c ^= 0x20;
      dEsc = false;
    }
    if (dType == MUX_SERIAL) {
      ser[sn++] = c;
      if (sn == sizeof(ser)) {
        if (rxHandler) rxHandler(ser, sn);
        sn = 0;
      }
    } else if (dIpLen < sizeof(dIp)) {
      dIp[dIpLen++] = c;
    } else {
      dBad = true;
    }
  }
  if (sn && rxHandler) rxHandler(ser, sn);
}

// ---------------------------------------------------------------- helpers
static uint32_t rnd32() {
  uint32_t v = 0;
  uint8_t avail = 0;
  if (sd_rand_application_bytes_available_get(&avail) == NRF_SUCCESS && avail >= 4 &&
      sd_rand_application_vector_get((uint8_t*)&v, 4) == NRF_SUCCESS)
    return v ^ (uint32_t)random(0x7FFFFFFF);
  return ((uint32_t)random(0x7FFFFFFF) << 1) ^ micros();
}
static uint32_t rndRange(uint32_t n) { return n ? rnd32() % n : 0; }

static bool paired() { return cfg.peerId != 0 && cfg.peerId != BCAST; }
bool isMaster() { return paired() && g_nodeId < cfg.peerId; }
static bool psm() { return paired() && cfg.powerSave; }
static bool hopActive() {
  return paired() && cfg.hopEnable && cfg.hopChannels >= 2 && !pairing && conn && lstate == LINK_UP;
}
static uint32_t hopClock() { return millis() + (isMaster() ? 0 : clockOffset); }

static uint32_t toaMs(size_t len) {
  uint32_t us = radio.getTimeOnAir(len);
  return (us + 999) / 1000;
}

float channelFreqMHz(uint8_t ch) {
  return (cfg.freqHz + (uint32_t)ch * cfg.hopSpacingKHz * 1000u) / 1e6f;
}

static uint32_t linkTimeoutMs() {
  if (psm() && !conn) return (cfg.standbyHbS ? cfg.standbyHbS : 30) * 3000u + 5000;
  uint32_t hb = (cfg.heartbeatS ? cfg.heartbeatS : 3) * 1000u;
  uint32_t t = hb * 3 + 2 * (hopActive() ? dwell : 0) + 2000;
  return t;
}

// AES-128 CTR using the SoftDevice ECB peripheral
static void aesCtr(uint8_t* data, size_t len, const Hdr& h) {
  nrf_ecb_hal_data_t ecb;
  memcpy(ecb.key, cfg.key, 16);
  uint8_t ctr = 0;
  for (size_t off = 0; off < len; off += 16, ctr++) {
    memcpy(ecb.cleartext + 0, &h.src, 4);
    memcpy(ecb.cleartext + 4, &h.dst, 4);
    memcpy(ecb.cleartext + 8, &h.seq, 2);
    memcpy(ecb.cleartext + 10, &h.time, 4);
    ecb.cleartext[14] = h.magicType;
    ecb.cleartext[15] = ctr;
    if (sd_ecb_block_encrypt(&ecb) != NRF_SUCCESS) {
      // SoftDevice not enabled: fall back to the bare ECB peripheral
      NRF_ECB->ECBDATAPTR = (uint32_t)&ecb;
      NRF_ECB->EVENTS_ENDECB = 0;
      NRF_ECB->TASKS_STARTECB = 1;
      while (!NRF_ECB->EVENTS_ENDECB && !NRF_ECB->EVENTS_ERRORECB) {
      }
    }
    size_t n = len - off < 16 ? len - off : 16;
    for (size_t i = 0; i < n; i++) data[off + i] ^= ecb.ciphertext[i];
  }
}

static bool keyIsZero() {
  for (int i = 0; i < 16; i++)
    if (cfg.key[i]) return false;
  return true;
}

// ---------------------------------------------------------------- hopping
static void buildPermutation() {
  uint8_t n = cfg.hopChannels > MAX_CH ? MAX_CH : cfg.hopChannels;
  uint32_t a = g_nodeId < cfg.peerId ? g_nodeId : cfg.peerId;
  uint32_t b = g_nodeId < cfg.peerId ? cfg.peerId : g_nodeId;
  uint32_t s = crc32(cfg.key, 16, a ^ (b * 2654435761u)) | 1u;
  for (uint8_t i = 0; i < n; i++) perm[i] = i;
  for (int i = n - 1; i > 0; i--) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    int j = s % (uint32_t)(i + 1);
    uint8_t t = perm[i];
    perm[i] = perm[j];
    perm[j] = t;
  }
}

static uint8_t channelForSlot(uint32_t slot) {
  uint8_t n = cfg.hopChannels > MAX_CH ? MAX_CH : cfg.hopChannels;
  uint16_t all = (uint16_t)((1u << n) - 1);
  uint16_t good = (uint16_t)(~activeMask & all) | 1u;  // channel 0 is never excluded
  uint32_t k = slot;
  uint8_t idx = perm[k % n];
  for (uint8_t i = 0; i < n && !(good & (1u << idx)); i++) idx = perm[(++k) % n];
  return idx;
}

static void computeTiming() {
  toaAckMs = toaMs(HDR_LEN + 2);
  float symMs = (float)(1u << cfg.sf) / cfgBandwidthKHz();
  turnMs = 12 + ACK_TURN_MS + (uint32_t)(8.0f * symMs + 0.5f);
  hdrWaitMs = 15 + (uint32_t)((cfg.preamble + 14) * symMs + 0.5f);
  uint32_t wp = (uint32_t)(WAKE_PREAMBLE_MS / symMs + 0.5f);
  if (wp < cfg.preamble + 16u) wp = cfg.preamble + 16u;
  if (wp > 4000) wp = 4000;
  wakePre = (uint16_t)wp;
  wakeExtraMs = (uint32_t)((wakePre - cfg.preamble) * symMs + 0.5f);
  uint8_t mp = cfg.maxPayload > MAX_PAYLOAD ? MAX_PAYLOAD : cfg.maxPayload;
  toaMaxMs = toaMs(HDR_LEN + mp);
  if (cfg.hopDwellMs) {
    dwell = cfg.hopDwellMs;
  } else {
    dwell = 2 * (toaMaxMs + toaAckMs + ACK_MARGIN_MS + turnMs) + 2 * SLOT_GUARD_MS;
    if (dwell < 200) dwell = 200;
    dwell = (dwell + 49) / 50 * 50;
  }
  uint32_t minDwell = toaMaxMs + toaAckMs + ACK_MARGIN_MS + turnMs + 2 * SLOT_GUARD_MS;
  if (dwell < minDwell) dwell = minDwell;
}

// ---------------------------------------------------------------- radio control
static void startRx() {
  // header-valid / preamble-detected flags are latched for LBT; only RX_DONE raises DIO1
  if (psm() && !conn && !pairing) {
    // standby: sleep most of the time, wake just often enough to catch a wake-up preamble
    radio.startReceiveDutyCycleAuto(wakePre, 8,
                                    RADIOLIB_IRQ_RX_DEFAULT_FLAGS | (1UL << RADIOLIB_IRQ_PREAMBLE_DETECTED),
                                    RADIOLIB_IRQ_RX_DEFAULT_MASK);
    dutyRx = true;
  } else {
    radio.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF,
                       RADIOLIB_IRQ_RX_DEFAULT_FLAGS | (1UL << RADIOLIB_IRQ_PREAMBLE_DETECTED),
                       RADIOLIB_IRQ_RX_DEFAULT_MASK, 0);
    dutyRx = false;
  }
  mode = RM_RX;
  busySince = 0;
}

static void tuneChannel(uint8_t ch) {
  if (ch == curCh) return;
  radio.standby();
  radio.setFrequency(channelFreqMHz(ch));
  curCh = ch;
  st.hops++;
  startRx();
}

static bool channelBusy() {
  if (!cfg.lbtEnable || mode != RM_RX || dutyRx) return false;  // chip may be asleep: no SPI polling
  uint32_t f = radio.getIrqFlags();
  bool busy = f & (RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED | RADIOLIB_SX126X_IRQ_HEADER_VALID);
  bool hdr = f & RADIOLIB_SX126X_IRQ_HEADER_VALID;
  if (!busy && cfg.lbtRssi != 0) busy = radio.getRSSI(false) > cfg.lbtRssi;
  uint32_t now = millis();
  if (busy) {
    if (!busySince) busySince = now;
    // false preamble detection (no header follows) or a stale header flag
    if (now - busySince > (hdr ? toaMaxMs + 100 : hdrWaitMs)) {
      radio.standby();
      startRx();
      return false;
    }
  } else {
    busySince = 0;
  }
  return busy;
}

static bool sendFrame(uint8_t type, uint8_t flags, uint32_t dst, uint16_t seq, const uint8_t* pl,
                      size_t plen) {
  if (!radioReady) return false;
  static uint8_t buf[MAX_FRAME];
  Hdr h;
  h.magicType = FRAME_MAGIC | type;
  // a sleeping (duty-cycled) peer only hears frames with the long wake-up preamble
  bool lng = psm() && !peerAwake && dst == cfg.peerId &&
             (type == FT_PING || type == FT_SYNC || type == FT_DATA || type == FT_FAST || type == FT_ETH);
  if (localActive) flags |= FL_ACTIVE;
  if (conn) flags |= FL_AWAKE;
  if (lng) flags |= FL_LONGPRE;
  h.flags = flags;
  h.src = g_nodeId;
  h.dst = dst;
  h.seq = seq;
  h.time = hopClock();
  memcpy(buf, &h, HDR_LEN);
  if (plen) memcpy(buf + HDR_LEN, pl, plen);
  if (flags & FL_ENC) aesCtr(buf + HDR_LEN, plen, h);

  radio.standby();
  if (lng) radio.setPreambleLength(wakePre);
  int16_t r = radio.startTransmit(buf, HDR_LEN + plen);
  txLong = lng;
  if (r != RADIOLIB_ERR_NONE) {
    if (txLong) radio.setPreambleLength(cfg.preamble);
    txLong = false;
    startRx();
    return false;
  }
  mode = RM_TX;
  txType = type;
  txNeedsAck = flags & FL_ACKREQ;
  txStartMs = lastTxMs = millis();
  txTimeoutMs = toaMs(HDR_LEN + plen) * 2 + 200;  // (toaMs already includes the long preamble)
  st.txFrames++;
  return true;
}

// ---------------------------------------------------------------- RX handling
static uint32_t rxDoneMs = 0;
static void setSyncedFrom(const Hdr& h, size_t len) {
  if (isMaster()) return;
  uint32_t toa = toaMs(len) + ((h.flags & FL_LONGPRE) ? wakeExtraMs : 0);
  clockOffset = (int32_t)(h.time + toa + 1 - rxDoneMs);
}

static void completePairing(uint32_t peer, const uint8_t* key) {
  cfg.peerId = peer;
  memcpy(cfg.key, key, 16);
  configSave();
  pairing = false;
  buildPermutation();
  lstate = LINK_SEARCHING;
  lastHeard = millis();
  haveRxSeq = false;
  activeMask = ownBadMask = peerBadMask = 0;
}

static void powerUpdate(uint32_t now) {
  if (!psm()) {
    if (!conn) {
      conn = true;
      if (mode == RM_RX) startRx();
    }
    return;
  }
  // the link is only worth its power when BOTH ends have a serial port in use
  bool want = localActive && peerActive;
  if (want) idleSince = now;
  if (!conn && want) {
    conn = true;
    connSince = now;
    if (cfg.hopEnable) lstate = LINK_SEARCHING;  // hop clock must be re-synchronised
    nextSyncBeacon = now;
    forcePing = true;
    waitingAck = false;
    if (mode == RM_RX) {
      radio.standby();
      startRx();  // receiver continuously on from now
    }
  } else if (conn && !want && now - idleSince > CONN_LINGER_MS && mode == RM_RX && !waitingAck && !ackPending &&
             !syncAckPending) {
    conn = false;
    peerAwake = false;
    byePings = 0;
    radio.standby();
    if (curCh != 0) {
      radio.setFrequency(channelFreqMHz(0));
      curCh = 0;
    }
    startRx();  // duty-cycled
  }
}

static void handleRx() {
  static uint8_t buf[MAX_FRAME];
  rxDoneMs = irqMillis();
  size_t len = radio.getPacketLength();
  if (len > MAX_FRAME) len = MAX_FRAME;
  int16_t r = radio.readData(buf, len);
  float rssi = radio.getRSSI();
  float snr = radio.getSNR();
  uint8_t rxCh = curCh;
  startRx();

  if (r == RADIOLIB_ERR_CRC_MISMATCH) {
    st.crcErr++;
    if (rxCh < MAX_CH) chs[rxCh].crc++;
    return;
  }
  if (r != RADIOLIB_ERR_NONE || len < HDR_LEN) return;

  Hdr h;
  memcpy(&h, buf, HDR_LEN);
  if ((h.magicType & 0xF0) != FRAME_MAGIC) return;
  uint8_t type = h.magicType & 0x0F;
  if (h.src == g_nodeId) return;
  if (h.dst != g_nodeId && h.dst != BCAST) return;
  uint8_t* pl = buf + HDR_LEN;
  size_t plen = len - HDR_LEN;
  uint32_t now = millis();

  // ---- pairing frames
  if (type == FT_PAIR_REQ) {
    if (pairing && plen >= 16) {
      const uint8_t* key = (h.src < g_nodeId) ? pl : pairKey;  // lower ID's key wins
      memcpy(pairAckKey, key, 16);
      pairAckDst = h.src;
      pairAckPending = true;
    }
    return;
  }
  if (type == FT_PAIR_ACK) {
    if (pairing && plen >= 16 && h.dst == g_nodeId) completePairing(h.src, pl);
    return;
  }

  // ---- unpaired: accept broadcast data from anyone
  if (!paired()) {
    if (type == FT_DATA && h.dst == BCAST && !(h.flags & FL_ENC)) {
      st.rxFrames++;
      st.rssi = (int16_t)rssi;
      st.snr = snr;
      lastRxMs = lastHeard = now;
      if (haveRxSeq && lastRxSrc == h.src && lastRxSeq == h.seq) {
        st.dup++;
        return;
      }
      haveRxSeq = true;
      lastRxSrc = h.src;
      lastRxSeq = h.seq;
      st.rxBytes += plen;
      if (rxHandler && plen) rxHandler(pl, plen);
    }
    return;
  }

  // ---- paired: only the peer
  if (h.src != cfg.peerId || h.dst != g_nodeId) return;
  st.rxFrames++;
  st.rssi = (int16_t)rssi;
  st.snr = snr;
  lastRxMs = lastHeard = now;
  if (rxCh < MAX_CH) chs[rxCh].ok++;

  peerActive = h.flags & FL_ACTIVE;
  peerAwake = h.flags & FL_AWAKE;
  powerUpdate(now);  // may switch to CONN before the frame is processed
  if (psm() && !conn) lstate = LINK_UP;  // standby: a heartbeat means the peer is there

  bool fromMaster = h.src < g_nodeId;
  if (fromMaster && cfg.hopEnable) {
    setSyncedFrom(h, len);
    // a master that is still in standby is not hopping: only SYNC may bring the link up then
    if (lstate != LINK_UP && type != FT_SYNC && (!psm() || (conn && peerAwake))) {
      lstate = LINK_UP;  // any master frame synchronises the slave
      st.syncs++;
    }
  }
  if (!cfg.hopEnable) lstate = LINK_UP;

  switch (type) {
    case FT_DATA: {
      if (h.flags & FL_ENC) aesCtr(pl, plen, h);
      if (h.flags & FL_ACKREQ) {
        ackPending = true;
        ackAt = rxDoneMs + ACK_TURN_MS;
        ackSeq = h.seq;
        nextTxAt = millis();  // our turn right after the ACK (the peer yields, see FT_ACK)
      }
      if (haveRxSeq && lastRxSeq == h.seq) {
        st.dup++;
        break;
      }
      haveRxSeq = true;
      lastRxSeq = h.seq;
      if (h.flags & FL_COMP) {
        static uint8_t raw[LZ_RAW_MAX];
        size_t n = lzDecompress(pl, plen, raw, sizeof(raw));
        st.rxBytes += n;
        deliver(raw, n, h.flags & FL_MUX);
        break;
      }
      st.rxBytes += plen;
      deliver(pl, plen, h.flags & FL_MUX);
      break;
    }
    case FT_ACK:
      if (waitingAck && pendValid && h.seq == txSeq) {
        waitingAck = false;
        pendValid = false;
        st.txBytes += pendRaw;
        st.txWire += pendLen;
        pendLen = 0;
        nextTxAt = millis() + turnMs + rndRange(6);  // yield: let the peer send if it has data
        if (plen >= 2) {
          st.remoteRssi = (int8_t)pl[0];
          st.remoteSnr = (int8_t)pl[1] / 4.0f;
        }
      }
      break;
    case FT_PING:
      if (plen >= 5) {
        st.remoteBatt = pl[0];
        st.remoteRssi = (int8_t)pl[1];
        st.remoteSnr = (int8_t)pl[2] / 4.0f;
        uint16_t m = pl[3] | (pl[4] << 8);
        if (isMaster()) {
          peerBadMask = m;
        } else {
          activeMask = m;
        }
        // newer firmware appends the checksum of its ETH setting
        uint32_t dg = 0;
        if (plen >= 9) memcpy(&dg, pl + 5, 4);
        eth::onPeerDigest(plen >= 9, dg);
      }
      break;
    case FT_SYNC:
      if (!isMaster()) {
        if (plen >= 2) activeMask = pl[0] | (pl[1] << 8);
        syncAckPending = true;
      }
      break;
    case FT_SYNC_ACK:
      if (isMaster() && lstate != LINK_UP) {
        lstate = LINK_UP;
        st.syncs++;
        forcePing = true;
      }
      break;
    case FT_FAST:
      if (h.flags & FL_ENC) aesCtr(pl, plen, h);
      fast::onCtl(pl, plen);
      break;
    case FT_ETH:
      if (h.flags & FL_ENC) aesCtr(pl, plen, h);
      eth::onCtl(pl, plen);
      break;
  }
}

// ---------------------------------------------------------------- AFH
static void afhEvaluate() {
  uint32_t now = millis();
  uint8_t n = cfg.hopChannels > MAX_CH ? MAX_CH : cfg.hopChannels;
  // median noise
  float nz[MAX_CH];
  uint8_t cnt = 0;
  for (uint8_t i = 0; i < n; i++)
    if (chs[i].noise < 0) nz[cnt++] = chs[i].noise;
  float median = -200;
  if (cnt) {
    for (uint8_t i = 1; i < cnt; i++)
      for (uint8_t j = i; j > 0 && nz[j - 1] > nz[j]; j--) {
        float t = nz[j];
        nz[j] = nz[j - 1];
        nz[j - 1] = t;
      }
    median = nz[cnt / 2];
  }
  uint16_t mask = 0;
  for (uint8_t i = 1; i < n; i++) {
    ChStat& c = chs[i];
    uint32_t bad = c.fail + c.crc;
    bool isBad = (bad >= 3 && bad * 2 > c.ok + bad) || (cnt >= 3 && c.noise < 0 && c.noise > median + 12);
    if (isBad) c.badUntil = now + 60000;
    if (c.badUntil && (int32_t)(now - c.badUntil) < 0) mask |= (1u << i);
    else c.badUntil = 0;
    c.ok = c.fail = c.crc = 0;  // new observation window
  }
  ownBadMask = mask;
  if (isMaster()) {
    uint16_t m = (ownBadMask | peerBadMask) & ~1u;
    // keep at least half of the channels
    uint8_t bad = __builtin_popcount(m);
    if (bad > n / 2) m = ownBadMask & ~1u;
    if (__builtin_popcount(m) > n / 2) m = 0;
    if (m != activeMask) {
      activeMask = m;
      forcePing = true;
    }
  }
}

// ---------------------------------------------------------------- public API
// true while a blocking job in the main loop (OLED refresh, flash write) would break timing
bool timeCritical() { return mode == RM_TX || waitingAck || ackPending; }
void setActive(bool a) {
  if (a == localActive) return;
  localActive = a;
  if (!a && conn) byePings = 3;  // tell the peer we are done so both can drop to standby
  else forcePing = true;
}
// UNPR unpaired | PAIR pairing | STBY standby, peer present | LINK active | SRCH waking the peer
// NOOP peer not found (no heartbeat / no answer)
const char* modeName() {
  if (pairing) return "PAIR";
  if (!paired()) return "UNPR";
  if (lstate == LINK_UP) return conn ? "LINK" : "STBY";
  if (conn && psm() && millis() - connSince < 8000) return "SRCH";
  return "NOOP";
}
bool isConn() { return conn; }
bool powerSave() { return psm(); }
size_t queueSpace() {
  // the framing is being switched: hold the serial input until the queue has drained
  if (muxTx != muxWant) return 0;
  // FAST mode: keep room for the bytes that may come back from the 2.4 GHz queue
  size_t s = txRing.space(), r = fast::state() != fast::OFF ? 2048 : 0;
  s = s > r ? s - r : 0;
  if (muxTx) s = s > 8 ? (s - 4) / 2 : 0;  // framed: at worst every byte is escaped
  return s;
}
size_t queued() { return txRing.used() + (pendValid ? pendRaw : 0); }
size_t queue(const uint8_t* d, size_t n) {
  size_t w = 0;
  if (muxTx) {
    // one frame with as much of the data as fits
    size_t sp = txRing.space(), k = 3;
    while (w < n) {
      size_t c = muxSpecial(d[w]) ? 2 : 1;
      if (k + c > sp) break;
      k += c;
      w++;
    }
    if (w) muxPut(MUX_SERIAL, d, w);
  } else {
    w = txRing.write(d, n);
  }
  if (w) lastQueueIn = millis();
  return w;
}
void setRxHandler(void (*cb)(const uint8_t*, size_t)) { rxHandler = cb; }

void setMux(bool want) { muxWant = want; }
bool muxActive() { return muxTx; }
void setNetHandler(void (*cb)(const uint8_t*, size_t)) { netHandler = cb; }
void setFastRxMux(bool mux) { fastRxMux = mux; }
bool queuePacket(const uint8_t* d, size_t n) {
  if (!muxTx || !muxWant || !n || n > MUX_IP_MAX) return false;
  if (!conn || lstate != LINK_UP) return false;  // only over an active link
  size_t need = muxSize(d, n), used = txRing.used();
  size_t r = fast::state() != fast::OFF ? 2048 : 0;
  if (need + r > txRing.space()) return false;
  if (!fast::isUp() && used && used + need > NET_BACKLOG) return false;
  muxPut(MUX_IP, d, n);
  lastQueueIn = millis();
  return true;
}

void startPairing(uint32_t ms) {
  for (int i = 0; i < 16; i += 4) {
    uint32_t r = rnd32();
    memcpy(pairKey + i, &r, 4);
  }
  pairing = true;
  pairAckPending = false;
  pairUntil = millis() + ms;
  nextPairBeacon = millis() + 100;
  waitingAck = false;
  if (radioReady) tuneChannel(0);
}
void stopPairing() { pairing = false; }
bool isPairing() { return pairing; }
uint32_t pairRemainingS() {
  if (!pairing) return 0;
  int32_t r = (int32_t)(pairUntil - millis());
  return r > 0 ? (r + 999) / 1000 : 0;
}
void unpair() {
  cfg.peerId = 0;
  memset(cfg.key, 0, 16);
  configSave();
  lstate = LINK_UNPAIRED;
  pendValid = waitingAck = false;
  if (radioReady) tuneChannel(0);
}

LinkState state() { return lstate; }
bool radioOk() { return radioReady; }
const char* radioStatus() { return radioMsg; }
bool hopping() { return hopActive(); }
uint8_t channel() { return curCh; }
uint16_t channelMask() { return activeMask; }
float freqMHz() { return channelFreqMHz(curCh); }
uint32_t dwellMs() { return dwell; }
uint32_t lastHeardAgoMs() { return lastHeard ? millis() - lastHeard : 0xFFFFFFFFu; }
uint32_t txActivityMs() { return (int32_t)(fastTxMs - lastTxMs) > 0 ? fastTxMs : lastTxMs; }
uint32_t rxActivityMs() { return (int32_t)(fastRxMs - lastRxMs) > 0 ? fastRxMs : lastRxMs; }
uint32_t random32() { return rnd32(); }

size_t fastTake(uint8_t* buf, size_t max) {
  if (pendValid || waitingAck || txRing.empty()) return 0;  // LoRa first finishes what it started
  size_t n = txRing.read(buf, max);
  st.txBytes += n;
  st.txWire += n;
  fastTxMs = millis();
  return n;
}
void fastRequeue(const uint8_t* d, size_t n) {
  size_t k = txRing.unread(d, n);
  st.txBytes -= k;
  st.txWire -= k;
  lastQueueIn = millis();
}
void fastDeliver(const uint8_t* d, size_t n) {
  st.rxBytes += n;
  fastRxMs = millis();
  deliver(d, n, fastRxMux);
}
const LinkStats& stats() { return st; }
void resetStats() {
  LinkStats z;
  st = z;
  rateTx0 = rateRx0 = 0;
}
void setLocalBattery(uint8_t pct) { localBatt = pct; }

static int16_t radioInit(uint16_t tcxoMv) {
  float tcxo = tcxoMv / 1000.0f;
  int16_t r = radio.begin(cfg.freqHz / 1e6f, cfgBandwidthKHz(), cfg.sf, cfg.cr, cfg.syncWord,
                          cfg.txPower, cfg.preamble, tcxo, false);
  if (r != RADIOLIB_ERR_NONE) return r;
  radio.setCurrentLimit(140);
  radio.setDio2AsRfSwitch(true);
  radio.setRfSwitchPins(PIN_LORA_RXEN, RADIOLIB_NC);
  radio.setCRC(2);
  radio.setRxBoostedGainMode(true);
  return RADIOLIB_ERR_NONE;
}

// ---------------------------------------------------------------- chip probe
static char chipInfoStr[48] = "not probed";
static char chipType6[7] = "";

static bool probeChip() {
  Module* m = radio.module();
  m->init();
  pinMode(PIN_LORA_BUSY, INPUT);
  radio.reset(false);
  delay(10);
  uint8_t v[16];
  bool found = false;
  for (int tries = 0; tries < 3 && !found; tries++) {
    memset(v, 0, sizeof(v));
    m->SPIreadRegisterBurst(RADIOLIB_SX126X_REG_VERSION_STRING, 16, v);
    found = memcmp(v, "SX126", 5) == 0;
    if (!found) {
      radio.reset(false);
      delay(20);
    }
  }
  int busy = digitalRead(PIN_LORA_BUSY);
  if (found) {
    char txt[17];
    for (int i = 0; i < 16; i++) txt[i] = (v[i] >= 32 && v[i] < 127) ? (char)v[i] : 0;
    txt[16] = 0;
    memcpy(chipType6, v, 6);
    chipType6[6] = 0;
    radio.expectChip(chipType6);
    snprintf(chipInfoStr, sizeof(chipInfoStr), "%s BUSY=%d", txt, busy);
  } else {
    snprintf(chipInfoStr, sizeof(chipInfoStr), "no SX126x: %02X %02X %02X %02X %02X %02X BUSY=%d", v[0], v[1], v[2],
             v[3], v[4], v[5], busy);
  }
  return found;
}

const char* chipInfo() { return chipInfoStr; }

bool applyConfig() {
  mode = RM_OFF;
  radioReady = false;
  int16_t r = radioInit(cfg.tcxoMv);
  if (r == RADIOLIB_ERR_CHIP_NOT_FOUND && !chipType6[0]) {
    // begin() has configured SX126x SPI framing by now: read the real version string
    if (probeChip()) r = radioInit(cfg.tcxoMv);
  } else if (r == RADIOLIB_ERR_NONE && !chipType6[0]) {
    snprintf(chipInfoStr, sizeof(chipInfoStr), "matched driver default");
  }
  if (r != RADIOLIB_ERR_NONE && cfg.tcxoMv) {
    // TCXO voltage wrong / crystal module: retry without TCXO
    int16_t r2 = radioInit(0);
    if (r2 == RADIOLIB_ERR_NONE) {
      snprintf(radioMsg, sizeof(radioMsg), "OK (XTAL; TCXO failed %d)", r);
      r = r2;
    }
  } else if (r == RADIOLIB_ERR_NONE) {
    snprintf(radioMsg, sizeof(radioMsg), "OK");
  }
  if (r != RADIOLIB_ERR_NONE) {
    snprintf(radioMsg, sizeof(radioMsg), "ERR %d", r);
    return false;
  }
  radio.setDio1Action(onDio1);
  radioReady = true;
  curCh = 0;
  computeTiming();
  buildPermutation();
  memset(chs, 0, sizeof(chs));
  activeMask = ownBadMask = peerBadMask = 0;
  lstate = paired() ? LINK_SEARCHING : LINK_UNPAIRED;
  waitingAck = false;
  conn = !psm();
  peerActive = peerAwake = false;
  idleSince = connSince = millis();
  forcePing = paired();  // announce ourselves
  startRx();
  return true;
}

bool begin() {
  randomSeed(rnd32());
  txSeq = (uint16_t)rnd32();
  SPI.begin();
  return applyConfig();
}

// ---------------------------------------------------------------- main loop
static void maybeBuildDataFrame(uint32_t now) {
  if (pendValid || txRing.empty()) return;
  if (fast::holdData()) return;  // the data goes over 2.4 GHz / the switch back is being settled
  size_t maxp = cfg.maxPayload ? cfg.maxPayload : 1;
  if (maxp > MAX_PAYLOAD) maxp = MAX_PAYLOAD;
  if (txRing.used() < maxp && now - lastQueueIn < cfg.packTimeoutMs) return;
  pendComp = false;
  pendMux = muxTx;  // (the queue holds one kind only, see the switch in loop())
  pendLen = 0;
  size_t avail = txRing.used();
  if (cfg.compress && avail > 16) {
    // pack as many user bytes as will fit into one frame after compression
    static uint8_t raw[LZ_RAW_MAX];
    size_t n = txRing.peek(raw, avail < sizeof(raw) ? avail : sizeof(raw));
    size_t c = 0;
    for (;;) {
      c = lzCompress(raw, n, pendBuf, maxp);
      if (c || n <= maxp) break;
      n = n * 3 / 4;
      if (n < maxp) n = maxp;
    }
    if (c && (n > maxp || c < n)) {
      txRing.drop(n);
      pendLen = c;
      pendRaw = n;
      pendComp = true;
    }
  }
  if (!pendComp) pendLen = pendRaw = txRing.read(pendBuf, maxp);
  pendValid = pendLen > 0;
  pendTries = 0;
  txSeq++;
}

#define PING_LEN 9
static bool sendPing() {
  uint16_t m = isMaster() ? activeMask : ownBadMask;
  uint8_t pl[PING_LEN] = {localBatt, (uint8_t)(int8_t)st.rssi, (uint8_t)(int8_t)(st.snr * 4), (uint8_t)m, (uint8_t)(m >> 8)};
  uint32_t dg = eth::digest();  // checksum of the ETH setting: the peer notices when it runs another one
  memcpy(pl + 5, &dg, 4);
  return sendFrame(FT_PING, 0, cfg.peerId, 0, pl, PING_LEN);
}

static void scheduler(uint32_t now) {
  powerUpdate(now);
  // 1. immediate responses (always on the current channel)
  if (ackPending) {
    if ((int32_t)(now - ackAt) < 0) return;
    ackPending = false;
    uint8_t pl[2] = {(uint8_t)(int8_t)st.rssi, (uint8_t)(int8_t)(st.snr * 4)};
    sendFrame(FT_ACK, 0, cfg.peerId, ackSeq, pl, 2);
    return;
  }
  if (syncAckPending) {
    syncAckPending = false;
    if (sendFrame(FT_SYNC_ACK, 0, cfg.peerId, 0, nullptr, 0)) {
      if (lstate != LINK_UP) st.syncs++;
      lstate = LINK_UP;
      lastHeard = now;
    }
    return;
  }

  // 2. pairing
  if (pairing) {
    if ((int32_t)(now - pairUntil) >= 0) {
      pairing = false;
      return;
    }
    if (pairAckPending) {
      pairAckPending = false;
      if (sendFrame(FT_PAIR_ACK, 0, pairAckDst, 0, pairAckKey, 16)) pairDoneAfterTx = true;
      return;
    }
    if ((int32_t)(now - nextPairBeacon) >= 0 && !channelBusy()) {
      sendFrame(FT_PAIR_REQ, 0, BCAST, 0, pairKey, 16);
      nextPairBeacon = now + 800 + rndRange(700);
    }
    return;
  }

  // 3. link supervision
  if (paired() && lastHeardAgoMs() > linkTimeoutMs()) peerActive = peerAwake = false;
  if (paired() && lstate == LINK_UP && lastHeardAgoMs() > linkTimeoutMs()) {
    lstate = LINK_SEARCHING;
    waitingAck = false;
    nextSyncBeacon = now + rndRange(300);
  }

  // 4. channel selection
  bool hop = hopActive();
  if (hop) {
    uint32_t hc = hopClock();
    uint32_t slot = hc / dwell;
    if (!waitingAck && (slot != curSlot || channelForSlot(slot) != curCh)) {
      if (!channelBusy()) {
        curSlot = slot;
        tuneChannel(channelForSlot(slot));
      }
    }
  } else if (curCh != 0) {
    if (!channelBusy()) tuneChannel(0);
  }

  // 5. ACK timeout / retransmission
  if (waitingAck && (int32_t)(now - ackDeadline) >= 0) {
    waitingAck = false;
    if (curCh < MAX_CH) chs[curCh].fail++;
    if (pendTries > cfg.retries) {
      st.lost++;
      pendValid = false;
      pendLen = 0;
    } else {
      st.retries++;
      nextTxAt = now + rndRange((6 + 2 * pendTries) * turnMs);  // window >> preamble detect time
    }
  }
  if (waitingAck) return;

  // 6. SYNC beacons (master, searching, hopping enabled)
  if (paired() && cfg.hopEnable && lstate == LINK_SEARCHING && conn) {
    if ((int32_t)(now - nextSyncBeacon) >= 0 && !channelBusy()) {
      bool sent = false;
      if (isMaster()) {
        uint8_t pl[2] = {(uint8_t)activeMask, (uint8_t)(activeMask >> 8)};
        sent = sendFrame(FT_SYNC, 0, cfg.peerId, 0, pl, 2);
      } else if (psm() && localActive && !peerAwake) {
        sent = sendPing();  // slave wakes the sleeping master, which then starts SYNC
      }
      // wake-up frames are long: slow down when the peer does not answer
      uint32_t gap = (psm() && now - connSince > 10000) ? 4000 : 900;
      nextSyncBeacon = now + gap + rndRange(300);
      (void)sent;
    }
    return;  // hold data until the hop clock is synchronised
  }

  // 6b. FAST / ETH mode control frames
  if (paired()) {
    static uint8_t ctl[24];
    static size_t ctlLen = 0;
    static uint16_t ctlSeq = 0;
    static uint8_t ctlType = FT_FAST;
    if (!ctlLen) {
      ctlType = FT_FAST;
      ctlLen = fast::ctlPoll(ctl, sizeof(ctl));
      if (!ctlLen) {
        ctlType = FT_ETH;
        ctlLen = eth::ctlPoll(ctl, sizeof(ctl));
      }
    }
    if (ctlLen) {
      if (hop) {
        uint32_t pos = hopClock() % dwell;
        if (pos < SLOT_GUARD_MS || dwell - pos < toaMs(HDR_LEN + ctlLen) + SLOT_GUARD_MS) return;
      }
      if (channelBusy()) return;
      size_t n = ctlLen;
      ctlLen = 0;
      sendFrame(ctlType, (cfg.encrypt && !keyIsZero()) ? FL_ENC : 0, cfg.peerId, ++ctlSeq, ctl, n);
      return;
    }
  }

  // 7. data
  maybeBuildDataFrame(now);
  if (pendValid && (int32_t)(now - nextTxAt) >= 0) {
    bool useAck = paired() && cfg.ack;
    uint32_t need = toaMs(HDR_LEN + pendLen) + (useAck ? toaAckMs + ACK_MARGIN_MS : 0) + SLOT_GUARD_MS;
    if (hop) {
      uint32_t pos = hopClock() % dwell;
      // the slave starts one turn later in a new slot, so it hears the master's preamble (LBT)
      uint32_t lead = SLOT_GUARD_MS + (isMaster() ? 0 : turnMs);
      if (pos < lead || dwell - pos < need) return;  // wait: peer may not have hopped yet / no room left
    }
    if (channelBusy()) {
      st.lbtBusy++;
      nextTxAt = now + 5 + rndRange(toaAckMs + 20);
      return;
    }
    uint8_t flags = 0;
    uint32_t dst = BCAST;
    if (paired()) {
      dst = cfg.peerId;
      if (cfg.encrypt && !keyIsZero()) flags |= FL_ENC;
      if (useAck) flags |= FL_ACKREQ;
    }
    if (pendComp) flags |= FL_COMP;
    if (pendMux) flags |= FL_MUX;
    static uint8_t tmp[MAX_PAYLOAD];
    memcpy(tmp, pendBuf, pendLen);  // sendFrame encrypts in its own buffer
    pendTries++;
    if (sendFrame(FT_DATA, flags, dst, txSeq, tmp, pendLen)) {
      if (!useAck) {  // fire and forget
        st.txBytes += pendRaw;
        st.txWire += pendLen;
        pendValid = false;
        pendLen = 0;
      }
    }
    return;
  }

  // 8. keepalive ping
  if (paired()) {
    uint32_t hb = (cfg.heartbeatS ? cfg.heartbeatS : 3) * 1000u;
    // standby: slow heartbeat. An end whose port is in use (so it has power) announces itself
    // more often, so the link comes up quickly once the other end is plugged in too.
    if (psm() && !conn) hb = localActive ? 4000u : (cfg.standbyHbS ? cfg.standbyHbS : 30) * 1000u;
    if (forcePing || byePings || now - lastTxMs >= hb + (g_nodeId & 0xFF)) {
      if (hop) {
        uint32_t pos = hopClock() % dwell;
        if (pos < SLOT_GUARD_MS || dwell - pos < toaMs(HDR_LEN + PING_LEN) + SLOT_GUARD_MS) return;
      }
      if (channelBusy()) return;
      forcePing = false;
      if (byePings) byePings--;
      sendPing();
    }
  }
}

void loop() {
  if (!radioReady) return;
  uint32_t now = millis();

  if (irqFlag) {
    irqFlag = false;
    if (mode == RM_TX) {
      radio.finishTransmit();
      if (txLong) {
        radio.setPreambleLength(cfg.preamble);
        txLong = false;
      }
      uint8_t t = txType;
      startRx();
      if (t == FT_DATA && txNeedsAck) {
        waitingAck = true;
        ackDeadline = irqMillis() + toaAckMs + ACK_MARGIN_MS + toaAckMs / 4;
      }
      if (t == FT_PAIR_ACK && pairDoneAfterTx) {
        pairDoneAfterTx = false;
        completePairing(pairAckDst, pairAckKey);
      }
    } else if (mode == RM_RX) {
      handleRx();
    }
  } else if (mode == RM_TX && now - txStartMs > txTimeoutMs) {
    // lost TX_DONE interrupt: recover
    radio.standby();
    if (txLong) {
      radio.setPreambleLength(cfg.preamble);
      txLong = false;
    }
    startRx();
  }

  // ETH mode on / off: the stream changes between plain and framed once nothing of the old
  // kind is queued any more. A 2.4 GHz session keeps the kind it started with, so it ends first.
  if (muxTx != muxWant) {
    if (fast::state() == fast::OFF) {
      if (txRing.empty()) muxTx = muxWant;
    } else if (fast::state() != fast::ENDING) {
      fast::release();
    }
  }

  if (mode == RM_RX) scheduler(now);

  // channel noise sampling for AFH
  if (cfg.afhEnable && hopActive() && mode == RM_RX && (int32_t)(now - nextNoiseSample) >= 0) {
    nextNoiseSample = now + 250;
    uint32_t f = radio.getIrqFlags();
    if (!(f & (RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED | RADIOLIB_SX126X_IRQ_HEADER_VALID))) {
      float n = radio.getRSSI(false);
      ChStat& c = chs[curCh];
      c.noise = (c.noise < 0) ? c.noise * 0.8f + n * 0.2f : n;
    }
  }
  if (cfg.afhEnable && hopActive() && (int32_t)(now - nextAfhEval) >= 0) {
    nextAfhEval = now + 10000;
    afhEvaluate();
  } else if (!cfg.afhEnable) {
    activeMask = 0;
  }

  if (now - rateT >= 1000) {
    uint32_t dt = now - rateT;
    rateT = now;
    int32_t dTx = (int32_t)(st.txBytes - rateTx0);  // can go backwards when FAST mode hands bytes back
    st.txRate = dTx > 0 ? (uint32_t)dTx * 1000 / dt : 0;
    st.rxRate = (st.rxBytes - rateRx0) * 1000 / dt;
    rateTx0 = st.txBytes;
    rateRx0 = st.rxBytes;
  }
}

}  // namespace link

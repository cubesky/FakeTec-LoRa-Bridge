// 2.4 GHz ESB-style link on the nRF52840 radio, run inside SoftDevice radio timeslots.
//
// Packet (2 Mbit/s, 5 byte address, 24 bit CRC, whitening):
//   S0     : bit0 SN, bit1 NESN, bits 2..7 hop index of the sender
//   LENGTH : 2 + data bytes
//   aux0   : master -> slave: poll counter (the slave counts the gaps = lost polls)
//   aux1   : master -> slave: hop channel mask in use
//   data   : 0..240 bytes
//
// Exchange: the master sends a poll, the slave answers on the same channel ~200 us later,
// then both move to the next channel of the hop set. SN / NESN work as in the BLE link
// layer: a packet is repeated until the other side's NESN shows it arrived, and a receiver
// whose buffer is full simply does not advance NESN (flow control).
//
// Hop synchronisation needs no clock. A channel is left only after a successful exchange:
// the slave moves on once it has answered a poll, the master once it has the answer, and
// every master packet carries its hop index, which the slave adopts. After a miss the slave
// can only be on the same channel (it did not hear the poll) or on the next one (its answer
// got lost), so the master tries those two; if that fails it sweeps the whole hop set, and a
// slave that hears nothing for 25 ms steps on, so they also get away from a jammed channel.
// The master rests channels that lose more than half of their polls for 10 s (the mask
// travels in every poll).
//
// Everything time critical runs in the timeslot signal handler (highest interrupt
// priority): no RTOS / SoftDevice calls in there.
#include "rf24.h"

#include <nrf_soc.h>

namespace rf24 {

// ---------------------------------------------------------------- byte FIFOs (ISR <-> main)
template <uint16_t N>  // N = power of two; indices run free
struct Fifo {
  uint8_t buf[N];
  volatile uint16_t head = 0, tail = 0;
  uint16_t used() const { return (uint16_t)(head - tail); }
  uint16_t space() const { return (uint16_t)(N - used()); }
  uint16_t write(const uint8_t* d, uint16_t n) {
    uint16_t s = space();
    if (n > s) n = s;
    uint16_t h = head & (N - 1), first = N - h;
    if (first > n) first = n;
    memcpy(buf + h, d, first);
    memcpy(buf, d + first, n - first);
    head = (uint16_t)(head + n);
    return n;
  }
  uint16_t peek(uint8_t* d, uint16_t n) const {
    uint16_t u = used();
    if (n > u) n = u;
    uint16_t t = tail & (N - 1), first = N - t;
    if (first > n) first = n;
    memcpy(d, buf + t, first);
    memcpy(d + first, buf, n - first);
    return n;
  }
  void drop(uint16_t n) { tail = (uint16_t)(tail + n); }
  void clear() { tail = head; }
};
static Fifo<2048> txq;  // main writes, ISR reads
static Fifo<2048> rxq;  // ISR writes, main reads

// ---------------------------------------------------------------- timing (microseconds)
#define SLOT_LEN_US 10000     // length of a requested timeslot
#define EXT_LEN_US 10000      // ... and of every extension
#define SLOT_MAX_US 1000000   // then give the SoftDevice a gap (flash, housekeeping)
#define EXT_MARGIN_US 3000    // ask for the extension this long before the end
#define END_MARGIN_US 400     // radio off this long before the end
#define RX_WAIT_US 500        // master: answer must start within this time
#define RX_BODY_US 1300       // longest packet once the address has been seen
#define POLL_GAP_US 300       // master: pause before the next poll (slave turns around)
#define POLL_IDLE_US 4000     // master: poll interval while neither side has data
#define SLAVE_HOP_US 25000    // slave: nothing heard -> next channel
#define DATA_MAX 240

enum : uint8_t { J_NONE, J_SCAN, J_LINK };
enum : uint8_t { S_IDLE, S_TX, S_RX, S_SCAN };

static nrf_radio_request_t req;
static nrf_radio_signal_callback_return_param_t ret;
static bool sessionOpen = false;
static volatile bool wantRun = false, paused = false, slotActive = false;
static volatile uint32_t slotCount = 0;
static volatile uint8_t job = J_NONE;

// owned by the signal handler while a slot is active
static uint32_t slotEnd = 0;
static bool closing = false, endNow = false, extendNow = false;
static uint8_t st = S_IDLE;
static Params prm;
static uint8_t txPkt[4 + DATA_MAX], rxPkt[2 + 255];
static uint8_t hopIdx = 0, txSn = 0, rxNesn = 0, txLen = 0, pollCtr = 0, lastCtr = 0;
static bool needNew = true, haveCtr = false, rxExtended = false;
static uint8_t missRun = 0, missBase = 0;  // master: consecutive polls without an answer

static volatile uint8_t hopMask = 0xFF, curCh = 0;
static volatile int8_t rssiLast = -127;
static volatile uint32_t nGood = 0, nMiss = 0, nCrc = 0, nPolls = 0, rssiSum = 0;
static volatile uint32_t ackedBytes = 0, rxBytes = 0;
static volatile uint16_t chPolls[RF24_MAX_CH], chOk[RF24_MAX_CH];

// scan
#define SCAN_PASSES 6
static volatile int8_t scanLv[RF24_SCAN_N];
static uint8_t scanPos = 0, scanPass = 0, scanPhase = 0;
static volatile bool scanFinished = false;

uint8_t scanChannel(uint8_t i) {
  // 2404 .. 2478 MHz in 2 MHz steps, without the BLE advertising channel at 2426
  uint8_t c = 4 + 2 * i;
  return c >= 26 ? c + 2 : c;
}

// ---------------------------------------------------------------- helpers (handler context)
static inline uint32_t tnow() {
  NRF_TIMER0->TASKS_CAPTURE[2] = 1;
  return NRF_TIMER0->CC[2];
}
static inline void protoTimer(uint32_t us) {
  NRF_TIMER0->EVENTS_COMPARE[1] = 0;
  NRF_TIMER0->CC[1] = tnow() + us;
  NRF_TIMER0->INTENSET = TIMER_INTENSET_COMPARE1_Msk;
}
static inline void protoTimerOff() { NRF_TIMER0->INTENCLR = TIMER_INTENCLR_COMPARE1_Msk; }

static void radioOff() {
  NRF_RADIO->INTENCLR = 0xFFFFFFFFu;
  NRF_RADIO->SHORTS = 0;
  NRF_RADIO->TASKS_RSSISTOP = 1;
  NRF_RADIO->TASKS_DISABLE = 1;
  for (uint32_t i = 0; i < 20000 && NRF_RADIO->STATE != RADIO_STATE_STATE_Disabled; i++) {
  }
  NRF_RADIO->EVENTS_DISABLED = 0;
  NRF_RADIO->EVENTS_END = 0;
  NRF_RADIO->EVENTS_ADDRESS = 0;
  (void)NRF_RADIO->EVENTS_DISABLED;
  NVIC_ClearPendingIRQ(RADIO_IRQn);
}

static void radioConfig() {
  // the SoftDevice keeps the radio powered down between its own uses, and everything has to
  // be set up again in every slot
  NRF_RADIO->POWER = 1;
  NRF_RADIO->INTENCLR = 0xFFFFFFFFu;
  NRF_RADIO->TXPOWER = (uint32_t)(uint8_t)prm.txPower;
  NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit << RADIO_MODE_MODE_Pos;
  // The master turns around fast, the slave with the default (slower) ramp-up: the slave's
  // answer can then never start before the master's receiver is ready.
  bool fastRu = job == J_SCAN || prm.master;
  NRF_RADIO->MODECNF0 = (RADIO_MODECNF0_DTX_Center << RADIO_MODECNF0_DTX_Pos) |
                        ((fastRu ? RADIO_MODECNF0_RU_Fast : RADIO_MODECNF0_RU_Default) << RADIO_MODECNF0_RU_Pos);
  NRF_RADIO->PCNF0 = (8u << RADIO_PCNF0_LFLEN_Pos) | (1u << RADIO_PCNF0_S0LEN_Pos) | (0u << RADIO_PCNF0_S1LEN_Pos) |
                     (RADIO_PCNF0_PLEN_8bit << RADIO_PCNF0_PLEN_Pos);
  NRF_RADIO->PCNF1 = (250u << RADIO_PCNF1_MAXLEN_Pos) | (0u << RADIO_PCNF1_STATLEN_Pos) | (4u << RADIO_PCNF1_BALEN_Pos) |
                     (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
                     (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos);
  NRF_RADIO->BASE0 = prm.addrBase;
  NRF_RADIO->PREFIX0 = prm.addrPrefix;
  NRF_RADIO->TXADDRESS = 0;
  NRF_RADIO->RXADDRESSES = 1;
  NRF_RADIO->CRCCNF = RADIO_CRCCNF_LEN_Three << RADIO_CRCCNF_LEN_Pos;
  NRF_RADIO->CRCPOLY = 0x00065Bu;
  NRF_RADIO->CRCINIT = prm.crcInit & 0xFFFFFFu;
  NRF_RADIO->EVENTS_DISABLED = 0;
  NRF_RADIO->EVENTS_END = 0;
  NRF_RADIO->EVENTS_ADDRESS = 0;
  if (job == J_SCAN) {
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk;
  } else {
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk |
                        RADIO_SHORTS_ADDRESS_RSSISTART_Msk | RADIO_SHORTS_DISABLED_RSSISTOP_Msk;
    NRF_RADIO->INTENSET = RADIO_INTENSET_DISABLED_Msk;
  }
}

static inline uint8_t allMask() { return (uint8_t)((1u << prm.nch) - 1); }
static uint8_t nextIdx(uint8_t i) {
  uint8_t m = hopMask & allMask();
  if (!m) m = allMask();
  do {
    i = (uint8_t)((i + 1) % prm.nch);
  } while (!(m & (1u << i)));
  return i;
}
static inline void tune(uint8_t idx) {
  uint8_t c = prm.ch[idx];
  NRF_RADIO->FREQUENCY = c;
  // Channel dependent CRC start value: at short range a receiver also picks up packets sent
  // several MHz away, and those must not pass as packets of this channel.
  // (No radio whitening, as in ESB: with it only the first packet after a radio set-up came
  // through on this hardware. The payload is encrypted / scrambled by fast.cpp instead.)
  NRF_RADIO->CRCINIT = (prm.crcInit ^ (c * 0x010101u)) & 0xFFFFFFu;
  curCh = c;
}

static void buildTx() {
  if (needNew) {  // the previous packet was acknowledged: a new one may be sent
    txLen = (uint8_t)txq.peek(txPkt + 4, DATA_MAX);
    needNew = false;
  }
  txPkt[0] = (uint8_t)(txSn | (rxNesn << 1) | (hopIdx << 2));
  txPkt[1] = (uint8_t)(2 + txLen);
  txPkt[2] = prm.master ? ++pollCtr : 0;
  txPkt[3] = prm.master ? (uint8_t)(hopMask & allMask()) : 0;
}

static void processRx() {
  uint8_t s0 = rxPkt[0];
  uint8_t dlen = (uint8_t)(rxPkt[1] - 2);
  if (((s0 >> 1) & 1) != txSn) {  // our packet arrived
    txSn ^= 1;
    if (txLen) {
      txq.drop(txLen);
      ackedBytes += txLen;
    }
    txLen = 0;
    needNew = true;
  }
  if ((s0 & 1) == rxNesn) {  // a new packet (not a repetition)
    if (dlen == 0) {
      rxNesn ^= 1;
    } else if (rxq.space() >= dlen) {
      rxq.write(rxPkt + 4, dlen);
      rxBytes += dlen;
      rxNesn ^= 1;
    }  // else: buffer full, NESN stays -> the peer repeats it later
  }
}

static void toRx() {
  NRF_RADIO->PACKETPTR = (uint32_t)rxPkt;
  NRF_RADIO->EVENTS_END = 0;
  NRF_RADIO->EVENTS_ADDRESS = 0;
  rxExtended = false;
  st = S_RX;
  NRF_RADIO->TASKS_RXEN = 1;
}
static void toTx() {
  NRF_RADIO->PACKETPTR = (uint32_t)txPkt;
  NRF_RADIO->EVENTS_END = 0;
  st = S_TX;
  NRF_RADIO->TASKS_TXEN = 1;
}
static uint32_t hopDeadline = 0;  // slave: TIMER0 time to give up on the current channel
static void listen(bool fresh) {  // slave
  if (fresh) {
    tune(hopIdx);
    hopDeadline = tnow() + SLAVE_HOP_US;
  }
  toRx();
  uint32_t now = tnow();
  int32_t left = (int32_t)(hopDeadline - now);
  protoTimer(left > 50 ? (uint32_t)left : 50);
}
static void startPoll() {  // master
  if (closing) {  // no room for a whole exchange in what is left of this slot
    st = S_IDLE;
    return;
  }
  tune(hopIdx);
  buildTx();
  nPolls++;
  toTx();
}

static void onDisabled() {
  if (!wantRun || paused) {
    endNow = true;
    return;
  }
  if (st == S_TX) {
    if (prm.master) {
      toRx();
      protoTimer(RX_WAIT_US);
    } else {
      hopIdx = nextIdx(hopIdx);
      listen(true);
    }
    return;
  }
  if (st != S_RX) return;
  protoTimerOff();
  bool ended = NRF_RADIO->EVENTS_END;
  bool got = ended && NRF_RADIO->CRCSTATUS == 1 && rxPkt[1] >= 2 && rxPkt[1] <= 2 + DATA_MAX;
  if (ended && !got) nCrc++;
  if (got) {
    uint8_t r = (uint8_t)(NRF_RADIO->RSSISAMPLE & 0x7F);
    rssiLast = (int8_t)(-(int)r);
    rssiSum += r;
    nGood++;
  }
  if (prm.master) {
    bool more = true;
    if (got) {
      chPolls[hopIdx]++;
      chOk[hopIdx]++;
      processRx();
      more = rxPkt[1] > 2 || txLen || txq.used();
      missRun = 0;
      hopIdx = nextIdx(hopIdx);
    } else {
      // The slave only moves on after it has answered a poll. So after a miss it is either
      // still here (it did not hear us) or one channel ahead (we did not hear its answer):
      // try both, twice. Still nothing: sweep the hop set until it answers somewhere.
      // Only the first tries count as lost packets (for the loss figure and the channel
      // statistics): a longer silence means the slave is off the air for a moment - between
      // two radio timeslots, or while it writes to flash - and a dead link is caught by the
      // "nothing received" timeout instead.
      if (missRun < 255) missRun++;
      if (missRun <= 4) nMiss++;
      if (missRun <= 2) chPolls[hopIdx]++;
      if (missRun == 1) missBase = hopIdx;
      else if (missRun == 2 || missRun == 4) hopIdx = nextIdx(missBase);
      else if (missRun == 3) hopIdx = missBase;
      else hopIdx = nextIdx(hopIdx);
    }
    st = S_IDLE;
    protoTimer(more ? POLL_GAP_US : POLL_IDLE_US);
  } else if (got) {
    uint8_t idx = rxPkt[0] >> 2;
    if (idx < prm.nch) hopIdx = idx;
    if (rxPkt[3]) hopMask = rxPkt[3];
    if (haveCtr) {
      uint8_t gap = (uint8_t)(rxPkt[2] - lastCtr - 1);
      nMiss += gap > 4 ? 4 : gap;  // (a long gap = one of us was off the air, see the master)
    }
    haveCtr = true;
    lastCtr = rxPkt[2];
    processRx();
    buildTx();
    toTx();  // same channel
  } else if (ended) {
    listen(false);  // damaged packet: the master tries this channel again
  } else {
    hopIdx = nextIdx(hopIdx);  // nothing at all for a while: the master is sweeping, meet it elsewhere
    listen(true);
  }
}

static void scanStep() {
  switch (scanPhase) {
    case 0:
      NRF_RADIO->FREQUENCY = scanChannel(scanPos);
      NRF_RADIO->PACKETPTR = (uint32_t)rxPkt;
      NRF_RADIO->TASKS_RXEN = 1;
      scanPhase = 1;
      protoTimer(220);
      break;
    case 1:
      NRF_RADIO->TASKS_RSSISTART = 1;
      scanPhase = 2;
      protoTimer(60);
      break;
    default: {
      int8_t v = (int8_t)(-(int)(NRF_RADIO->RSSISAMPLE & 0x7F));
      if (v > scanLv[scanPos]) scanLv[scanPos] = v;
      NRF_RADIO->TASKS_RSSISTOP = 1;
      NRF_RADIO->TASKS_DISABLE = 1;
      scanPhase = 0;
      if (++scanPos >= RF24_SCAN_N) {
        scanPos = 0;
        if (++scanPass >= SCAN_PASSES) {
          scanFinished = true;
          wantRun = false;
          endNow = true;
          return;
        }
      }
      protoTimer(80);
      break;
    }
  }
}

static void onProtoTimer() {
  if (!wantRun || paused) {
    endNow = true;
    return;
  }
  if (st == S_SCAN) {
    scanStep();
  } else if (st == S_IDLE) {
    if (prm.master) startPoll();
  } else if (st == S_RX) {
    if (NRF_RADIO->EVENTS_ADDRESS && !rxExtended) {  // a packet is coming in: let it finish
      rxExtended = true;
      protoTimer(RX_BODY_US);
    } else if (NRF_RADIO->STATE != RADIO_STATE_STATE_Disabled) {
      NRF_RADIO->TASKS_DISABLE = 1;  // -> DISABLED without END = timeout
    }
  }
}

static void onStart() {
  slotActive = true;
  slotCount++;
  closing = false;
  slotEnd = SLOT_LEN_US;
  NRF_TIMER0->EVENTS_COMPARE[0] = 0;
  NRF_TIMER0->EVENTS_COMPARE[1] = 0;
  NRF_TIMER0->CC[0] = slotEnd - EXT_MARGIN_US;
  NRF_TIMER0->INTENSET = TIMER_INTENSET_COMPARE0_Msk;
  NVIC_EnableIRQ(TIMER0_IRQn);
  if (!wantRun || paused || job == J_NONE) {
    endNow = true;
    return;
  }
  radioConfig();
  NVIC_ClearPendingIRQ(RADIO_IRQn);
  NVIC_EnableIRQ(RADIO_IRQn);
  if (job == J_SCAN) {
    scanPhase = 0;
    st = S_SCAN;
    protoTimer(50);
  } else if (prm.master) {
    st = S_IDLE;
    protoTimer(100);
  } else {
    listen(true);
  }
}

static nrf_radio_signal_callback_return_param_t* onSignal(uint8_t sig) {
  ret.callback_action = NRF_RADIO_SIGNAL_CALLBACK_ACTION_NONE;
  switch (sig) {
    case NRF_RADIO_CALLBACK_SIGNAL_TYPE_START:
      onStart();
      break;
    case NRF_RADIO_CALLBACK_SIGNAL_TYPE_RADIO:
      if (NRF_RADIO->EVENTS_DISABLED) {
        NRF_RADIO->EVENTS_DISABLED = 0;
        (void)NRF_RADIO->EVENTS_DISABLED;
        if (slotActive) onDisabled();
      }
      break;
    case NRF_RADIO_CALLBACK_SIGNAL_TYPE_TIMER0:
      if (NRF_TIMER0->EVENTS_COMPARE[1] && (NRF_TIMER0->INTENSET & TIMER_INTENSET_COMPARE1_Msk)) {
        NRF_TIMER0->EVENTS_COMPARE[1] = 0;
        protoTimerOff();
        if (slotActive) onProtoTimer();
      }
      if (NRF_TIMER0->EVENTS_COMPARE[0]) {
        NRF_TIMER0->EVENTS_COMPARE[0] = 0;
        (void)NRF_TIMER0->EVENTS_COMPARE[0];
        if (!slotActive) break;
        if (closing) {
          endNow = true;
        } else if (wantRun && !paused && slotEnd + EXT_LEN_US <= SLOT_MAX_US) {
          extendNow = true;
        } else {
          closing = true;
          NRF_TIMER0->CC[0] = slotEnd - END_MARGIN_US;
        }
      }
      break;
    case NRF_RADIO_CALLBACK_SIGNAL_TYPE_EXTEND_SUCCEEDED:
      slotEnd += EXT_LEN_US;
      NRF_TIMER0->CC[0] = slotEnd - EXT_MARGIN_US;
      break;
    case NRF_RADIO_CALLBACK_SIGNAL_TYPE_EXTEND_FAILED:
      closing = true;
      NRF_TIMER0->CC[0] = slotEnd - END_MARGIN_US;
      break;
  }
  if (endNow) {
    endNow = extendNow = false;
    radioOff();
    NRF_TIMER0->INTENCLR = TIMER_INTENCLR_COMPARE0_Msk | TIMER_INTENCLR_COMPARE1_Msk;
    st = S_IDLE;
    slotActive = false;
    if (wantRun && !paused) {
      ret.callback_action = NRF_RADIO_SIGNAL_CALLBACK_ACTION_REQUEST_AND_END;
      ret.params.request.p_next = &req;
    } else {
      ret.callback_action = NRF_RADIO_SIGNAL_CALLBACK_ACTION_END;
    }
  } else if (extendNow) {
    extendNow = false;
    ret.callback_action = NRF_RADIO_SIGNAL_CALLBACK_ACTION_EXTEND;
    ret.params.extend.length_us = EXT_LEN_US;
  }
  return &ret;
}

// ---------------------------------------------------------------- main context
static uint32_t lastSlotSeen = 0, lastSlotMs = 0, nextAfh = 0;
static uint32_t banUntil[RF24_MAX_CH];

static bool ensureSession() {
  if (!sessionOpen) sessionOpen = sd_radio_session_open(onSignal) == NRF_SUCCESS;
  return sessionOpen;
}
static void kick() {
  req.request_type = NRF_RADIO_REQ_TYPE_EARLIEST;
  req.params.earliest.hfclk = NRF_RADIO_HFCLK_CFG_XTAL_GUARANTEED;
  req.params.earliest.priority = NRF_RADIO_PRIORITY_NORMAL;
  req.params.earliest.length_us = SLOT_LEN_US;
  req.params.earliest.timeout_us = 200000;
  sd_radio_request(&req);  // fails harmlessly while a request is already pending
  lastSlotMs = millis();
}
static void waitSlotEnd() {
  for (int i = 0; i < 80 && slotActive; i++) delay(1);
}

bool scanStart() {
  if (wantRun || slotActive) return false;
  if (!ensureSession()) return false;
  for (uint8_t i = 0; i < RF24_SCAN_N; i++) scanLv[i] = -127;
  scanPos = scanPass = 0;
  scanFinished = false;
  paused = false;
  job = J_SCAN;
  wantRun = true;
  kick();
  return true;
}
bool scanDone() { return scanFinished; }
int8_t scanLevel(uint8_t i) { return i < RF24_SCAN_N ? scanLv[i] : 0; }

static int8_t validPower(int8_t p) {
  static const int8_t ok[] = {-40, -20, -16, -12, -8, -4, 0, 2, 3, 4, 5, 6, 7, 8};
  int8_t best = ok[0];
  for (int8_t v : ok)
    if (v <= p) best = v;
  return best;
}

bool start(const Params& p) {
  stop();
  if (!p.nch || p.nch > RF24_MAX_CH) return false;
  if (!ensureSession()) return false;
  prm = p;
  prm.txPower = validPower(p.txPower);
  txq.head = txq.tail = 0;
  rxq.head = rxq.tail = 0;
  hopIdx = txSn = rxNesn = txLen = pollCtr = lastCtr = missRun = 0;
  needNew = true;
  haveCtr = false;
  hopMask = allMask();
  nGood = nMiss = nCrc = nPolls = rssiSum = 0;
  ackedBytes = rxBytes = 0;
  rssiLast = -127;
  for (uint8_t i = 0; i < RF24_MAX_CH; i++) {
    chPolls[i] = chOk[i] = 0;
    banUntil[i] = 0;
  }
  nextAfh = millis() + 1000;
  paused = false;
  job = J_LINK;
  wantRun = true;
  kick();
  return true;
}

void stop() {
  wantRun = false;
  waitSlotEnd();
  job = J_NONE;
}
bool running() { return wantRun && job == J_LINK; }

void pause(bool on) {
  if (on == paused) return;
  paused = on;
  if (on) waitSlotEnd();
  else if (wantRun) kick();
}

void loop() {
  if (!wantRun || paused) return;
  uint32_t now = millis();
  // The timeslot events (blocked / cancelled) go to the Bluefruit SoC task, not to us:
  // simply ask again when no slot has started for a while.
  if (slotActive || slotCount != lastSlotSeen) {
    lastSlotSeen = slotCount;
    lastSlotMs = now;
  } else if (now - lastSlotMs > 40) {
    kick();
  }
  // adaptive hop set (the master decides, the mask travels in every poll)
  if (job == J_LINK && prm.master && (int32_t)(now - nextAfh) >= 0) {
    nextAfh = now + 1000;
    uint8_t mask = 0, n = 0;
    for (uint8_t i = 0; i < prm.nch; i++) {
      uint16_t p = chPolls[i], o = chOk[i];
      chPolls[i] = chOk[i] = 0;
      if (p >= 8 && o * 2u < p) banUntil[i] = now + 10000;  // more than half lost: rest it for 10 s
      if (banUntil[i] && (int32_t)(now - banUntil[i]) >= 0) banUntil[i] = 0;
      if (!banUntil[i]) {
        mask |= (uint8_t)(1u << i);
        n++;
      }
    }
    if (n < 3 && n < prm.nch) {  // too few left: the problem is not the channels
      mask = allMask();
      for (uint8_t i = 0; i < prm.nch; i++) banUntil[i] = 0;
    }
    hopMask = mask;
  }
}

size_t txSpace() { return txq.space(); }
size_t txWrite(const uint8_t* d, size_t n) { return txq.write(d, (uint16_t)(n > 2048 ? 2048 : n)); }
size_t txPending() { return txq.used(); }
uint32_t txAcked() { return ackedBytes; }
size_t txLeftover(uint8_t* d, size_t max) { return txq.peek(d, (uint16_t)(max > 2048 ? 2048 : max)); }

size_t rxAvailable() { return rxq.used(); }
size_t rxRead(uint8_t* d, size_t max) {
  uint16_t n = rxq.peek(d, (uint16_t)(max > 2048 ? 2048 : max));
  rxq.drop(n);
  return n;
}
uint32_t rxTotal() { return rxBytes; }

void stats(Stats& s) {
  s.good = nGood;
  s.miss = nMiss;
  s.crcErr = nCrc;
  s.polls = nPolls;
  s.slots = slotCount;
  s.rssiSum = rssiSum;
}
uint8_t channel() { return curCh; }
uint8_t channelMask() { return hopMask; }
int8_t lastRssi() { return rssiLast; }

}  // namespace rf24

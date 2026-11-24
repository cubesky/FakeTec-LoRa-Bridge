// ETH (network adapter) mode, see eth.h.
#include "eth.h"

#include "config.h"
#include "display.h"
#include "lora_link.h"
#include "usb_ncm.h"

namespace eth {

#define ETHERTYPE_IP 0x0800
#define ETHERTYPE_ARP 0x0806
#define IP_MAX 1500
#define LEASE_S 600
#define SYNC_TRIES 6

// control frame (LoRa frame type FT_ETH): the complete shared setting
//   0: 1 (STATE)   1: flags   2..5: revision   6..9: master IP   10..13: slave IP   14: prefix
#define C_STATE 1
#define CF_ON 0x01      // ETH mode on
#define CF_ACKREQ 0x02  // answer with your own state
#define CF_ACK 0x04     // this is such an answer
#define CTL_LEN 15

static uint8_t hostMac[6];  // the host's adapter (address string of the USB function)
static uint8_t gateMac[6];  // what the host sees as the other end
static uint8_t seenMac[6];  // source address the host really uses
static bool haveSeenMac = false;

// what the peer runs
static bool peerKnown = false;   // the peer's firmware has ETH mode and told us its state
static uint32_t peerDig = 0;
static uint32_t lastPeerId = 0;
// sending our state
static uint8_t sendTries = 0;
static bool replyPending = false;
static uint32_t nextSend = 0, lastKick = 0;
// address handed to the host
static bool isLeased = false;
static uint32_t shownHost = 0, shownPeer = 0;
static uint8_t shownPrefix = 0;
static bool addrDirty = false;
static uint32_t addrDirtyAt = 0;

static struct {
  uint32_t txPkts, rxPkts, txDrop, rxDrop, filtered, dhcp, arp;
} st;

// ---------------------------------------------------------------- helpers
static inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void put16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}
static inline void put32(uint8_t* p, uint32_t v) {
  put16(p, (uint16_t)(v >> 16));
  put16(p + 2, (uint16_t)v);
}
static inline void le32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
static inline uint32_t gle32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

bool parseIp(const char* s, uint32_t& ip, const char** end) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) {
    if (*s < '0' || *s > '9') return false;
    uint32_t o = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9' && digits < 4) {
      o = o * 10 + (uint32_t)(*s++ - '0');
      digits++;
    }
    if (o > 255) return false;
    v = (v << 8) | o;
    if (i < 3) {
      if (*s != '.') return false;
      s++;
    }
  }
  ip = v;
  if (end) *end = s;
  else if (*s) return false;
  return true;
}
void fmtIp(char* out, size_t n, uint32_t ip) {
  snprintf(out, n, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 255), (unsigned)((ip >> 8) & 255),
           (unsigned)(ip & 255));
}

static bool paired() { return cfg.peerId != 0 && cfg.peerId != 0xFFFFFFFFu; }
bool enabled() { return cfg.ethEnable; }
uint32_t netmask() { return cfg.ethPrefix ? 0xFFFFFFFFu << (32 - cfg.ethPrefix) : 0; }
uint32_t network() { return cfg.ethMasterIp & netmask(); }
uint32_t hostIp() { return (!paired() || link::isMaster()) ? cfg.ethMasterIp : cfg.ethSlaveIp; }
uint32_t peerIp() { return (!paired() || link::isMaster()) ? cfg.ethSlaveIp : cfg.ethMasterIp; }
bool leased() { return isLeased && ncm::ready(); }
bool busy() { return ncm::busy() || replyPending || (sendTries && (int32_t)(millis() - nextSend) >= 0); }

static const char* checkAddresses(uint32_t m, uint32_t s, uint8_t prefix) {
  if (prefix < 8 || prefix > 30) return "prefix 8..30";
  uint32_t mask = 0xFFFFFFFFu << (32 - prefix);
  if ((m & mask) != (s & mask)) return "master and slave IP must be in the same subnet";
  if (m == s) return "master and slave IP must differ";
  uint32_t hm = m & ~mask, hs = s & ~mask;
  if (!hm || hm == ~mask || !hs || hs == ~mask) return "network / broadcast address cannot be used";
  uint8_t a = (uint8_t)(m >> 24);
  if (a == 0 || a == 127 || a >= 224) return "not a usable network";
  return nullptr;
}

uint32_t digest() {
  uint8_t b[14];
  le32(b, cfg.ethRev);
  b[4] = cfg.ethEnable ? 1 : 0;
  b[5] = cfg.ethPrefix;
  le32(b + 6, cfg.ethMasterIp);
  le32(b + 10, cfg.ethSlaveIp);
  return crc32(b, sizeof(b), 0x45544831u);
}
bool synced() { return peerKnown && peerDig == digest(); }

// ---------------------------------------------------------------- shared setting
static void startSync(uint32_t delayMs) {
  sendTries = SYNC_TRIES;
  nextSend = millis() + delayMs;
}
// the stored setting has changed (here or adopted from the peer): make it happen
static void apply() {
  ncm::expose(cfg.ethEnable);
  if (!cfg.ethEnable) isLeased = false;
}
static void changed() {
  cfg.ethRev++;
  configSave();
  apply();
  startSync(300);  // (several commands in a row go out as one)
}

const char* setEnabled(bool on) {
  if (on == (bool)cfg.ethEnable) return nullptr;
  if (on && !paired()) return "not paired";
  if (on && checkAddresses(cfg.ethMasterIp, cfg.ethSlaveIp, cfg.ethPrefix)) return "invalid addresses (AT+ETHCFG)";
  cfg.ethEnable = on ? 1 : 0;
  changed();
  return nullptr;
}

const char* setAddresses(uint32_t m, uint32_t s, uint8_t prefix) {
  const char* why = checkAddresses(m, s, prefix);
  if (why) return why;
  if (m == cfg.ethMasterIp && s == cfg.ethSlaveIp && prefix == cfg.ethPrefix) return nullptr;
  cfg.ethMasterIp = m;
  cfg.ethSlaveIp = s;
  cfg.ethPrefix = prefix;
  changed();
  return nullptr;
}

static size_t buildState(uint8_t* b, uint8_t flags) {
  b[0] = C_STATE;
  b[1] = (uint8_t)(flags | (cfg.ethEnable ? CF_ON : 0));
  le32(b + 2, cfg.ethRev);
  le32(b + 6, cfg.ethMasterIp);
  le32(b + 10, cfg.ethSlaveIp);
  b[14] = cfg.ethPrefix;
  return CTL_LEN;
}

size_t ctlPoll(uint8_t* buf, size_t max) {
  if (max < CTL_LEN || !paired()) return 0;
  if (replyPending) {
    replyPending = false;
    return buildState(buf, CF_ACK);
  }
  uint32_t now = millis();
  if (!sendTries || (int32_t)(now - nextSend) < 0) return 0;
  sendTries--;
  // a sleeping peer answers after its wake-up preamble; while hopping the answer may have to
  // wait for the next slot
  uint32_t retry = 3 * link::dwellMs();
  if (retry < 2500) retry = 2500;
  nextSend = now + retry + link::random32() % 500;
  return buildState(buf, CF_ACKREQ);
}

void onCtl(const uint8_t* pl, size_t n) {
  if (n < CTL_LEN || pl[0] != C_STATE) return;
  bool on = pl[1] & CF_ON;
  uint32_t rev = gle32(pl + 2), m = gle32(pl + 6), s = gle32(pl + 10);
  uint8_t prefix = pl[14];
  peerKnown = true;
  bool same = rev == cfg.ethRev && on == (bool)cfg.ethEnable && m == cfg.ethMasterIp && s == cfg.ethSlaveIp &&
              prefix == cfg.ethPrefix;
  if (same) {
    sendTries = 0;
  } else if ((rev > cfg.ethRev || (rev == cfg.ethRev && !link::isMaster())) && !checkAddresses(m, s, prefix)) {
    // the peer's setting is newer (same age: the master's counts): take it over
    bool was = cfg.ethEnable;
    cfg.ethRev = rev;
    cfg.ethEnable = on ? 1 : 0;
    cfg.ethMasterIp = m;
    cfg.ethSlaveIp = s;
    cfg.ethPrefix = prefix;
    configSave();
    apply();
    sendTries = 0;
    if (was != on) display::toast(on ? "ETH ON (PEER)" : "ETH OFF (PEER)");
    else display::toast("ETH IP CHANGED");
  } else {
    // ours is newer: the peer takes it from the answer, or from the next try
    if (sendTries < 2) startSync(600);
    // (peerDig below is no longer what the peer runs; it tells us again)
  }
  // what the peer runs now: its own state, unless it is about to take ours
  uint8_t b[14];
  le32(b, rev);
  b[4] = on ? 1 : 0;
  b[5] = prefix;
  le32(b + 6, m);
  le32(b + 10, s);
  peerDig = crc32(b, sizeof(b), 0x45544831u);
  if (pl[1] & CF_ACKREQ) replyPending = true;
}

void onPeerDigest(bool present, uint32_t d) {
  if (!present) {  // firmware without ETH mode
    peerKnown = false;
    return;
  }
  peerKnown = true;
  peerDig = d;
  uint32_t now = millis();
  if (d != digest() && !sendTries && !replyPending && now - lastKick > 5000) {
    lastKick = now;
    sendTries = 2;
    nextSend = now + (link::isMaster() ? 0 : 700);  // not both at the same moment
  }
}

// ---------------------------------------------------------------- frames to the host
static uint8_t frameBuf[1514];

static const uint8_t* hostDst() {
  static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  return haveSeenMac ? seenMac : bcast;
}
static uint16_t ipChecksum(const uint8_t* p, size_t n) {
  uint32_t sum = 0;
  for (size_t i = 0; i + 1 < n; i += 2) sum += be16(p + i);
  if (n & 1) sum += (uint32_t)p[n - 1] << 8;
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return (uint16_t)~sum;
}

void onPacket(const uint8_t* ip, size_t n) {
  if (!cfg.ethEnable || !ncm::ready() || n < 20 || n > IP_MAX || (ip[0] >> 4) != 4 || be16(ip + 2) != n) {
    st.rxDrop++;
    return;
  }
  memcpy(frameBuf, hostDst(), 6);
  memcpy(frameBuf + 6, gateMac, 6);
  put16(frameBuf + 12, ETHERTYPE_IP);
  memcpy(frameBuf + 14, ip, n);
  if (ncm::send(frameBuf, 14 + n)) st.rxPkts++;
  else st.rxDrop++;
}

// ---------------------------------------------------------------- ARP
// The host asks for the other host: answer for it, so that no ARP has to cross the radio.
static void onArp(const uint8_t* f, size_t len) {
  if (len < 42) return;
  const uint8_t* a = f + 14;
  if (be16(a) != 1 || be16(a + 2) != ETHERTYPE_IP || a[4] != 6 || a[5] != 4 || be16(a + 6) != 1) return;
  uint32_t spa = be32(a + 14), tpa = be32(a + 24);
  // not for the host's own address: that is its duplicate address probe
  if (tpa != peerIp() || spa == tpa) return;
  uint8_t* r = frameBuf;
  memcpy(r, a + 8, 6);  // to the asking host
  memcpy(r + 6, gateMac, 6);
  put16(r + 12, ETHERTYPE_ARP);
  put16(r + 14, 1);
  put16(r + 16, ETHERTYPE_IP);
  r[18] = 6;
  r[19] = 4;
  put16(r + 20, 2);  // reply
  memcpy(r + 22, gateMac, 6);
  put32(r + 28, tpa);
  memcpy(r + 32, a + 8, 6);
  put32(r + 38, spa);
  memset(r + 42, 0, 18);
  if (ncm::send(r, 60)) st.arp++;
}

// ---------------------------------------------------------------- DHCP server (one client: the host)
static void onDhcp(const uint8_t* udp, size_t ulen) {
  if (ulen < 8 + 240) return;
  const uint8_t* b = udp + 8;
  size_t blen = ulen - 8;
  static const uint8_t cookie[4] = {0x63, 0x82, 0x53, 0x63};
  if (b[0] != 1 || b[1] != 1 || b[2] != 6 || memcmp(b + 236, cookie, 4)) return;
  uint8_t type = 0;
  uint32_t reqIp = 0, serverId = 0;
  for (size_t i = 240; i < blen;) {
    uint8_t o = b[i++];
    if (o == 255) break;
    if (o == 0) continue;
    if (i >= blen) break;
    uint8_t l = b[i++];
    if (i + l > blen) break;
    if (o == 53 && l >= 1) type = b[i];
    else if (o == 50 && l == 4) reqIp = be32(b + i);
    else if (o == 54 && l == 4) serverId = be32(b + i);
    i += l;
  }
  uint32_t ciaddr = be32(b + 12);
  uint32_t mine = hostIp(), server = peerIp();
  uint8_t reply;
  uint32_t yi = mine;
  bool lease = true;
  if (type == 1) {  // DISCOVER
    reply = 2;      // OFFER
  } else if (type == 3) {  // REQUEST
    if (serverId && serverId != server) return;  // it chose somebody else
    uint32_t want = reqIp ? reqIp : ciaddr;
    reply = want == mine ? 5 : 6;  // ACK / NAK: after a change the host starts over
  } else if (type == 8) {  // INFORM
    reply = 5;
    yi = 0;
    lease = false;
  } else {
    return;
  }
  bool nak = reply == 6;
  if (nak) yi = 0;
  // unicast to a host that already has its (right) address, broadcast otherwise
  bool uni = !nak && ciaddr == mine && ciaddr != 0;

  uint8_t* f = frameBuf;
  memset(f, 0, 14 + 20 + 8 + 300);
  if (uni) memcpy(f, b + 28, 6);
  else memset(f, 0xFF, 6);
  memcpy(f + 6, gateMac, 6);
  put16(f + 12, ETHERTYPE_IP);
  uint8_t* d = f + 14 + 20 + 8;
  d[0] = 2;  // BOOTREPLY
  d[1] = 1;
  d[2] = 6;
  memcpy(d + 4, b + 4, 4);      // xid
  memcpy(d + 10, b + 10, 2);    // flags
  if (!nak) memcpy(d + 12, b + 12, 4);  // ciaddr
  put32(d + 16, yi);
  memcpy(d + 24, b + 24, 4);    // giaddr
  memcpy(d + 28, b + 28, 16);   // chaddr
  memcpy(d + 236, cookie, 4);
  uint8_t* o = d + 240;
  *o++ = 53; *o++ = 1; *o++ = reply;
  *o++ = 54; *o++ = 4; put32(o, server); o += 4;
  if (!nak) {
    if (lease) {
      *o++ = 51; *o++ = 4; put32(o, LEASE_S); o += 4;
      *o++ = 58; *o++ = 4; put32(o, LEASE_S / 2); o += 4;
      *o++ = 59; *o++ = 4; put32(o, LEASE_S * 7 / 8); o += 4;
    }
    *o++ = 1; *o++ = 4; put32(o, netmask()); o += 4;
    *o++ = 28; *o++ = 4; put32(o, network() | ~netmask()); o += 4;
  }
  *o++ = 255;
  size_t dlen = (size_t)(o - d);
  if (dlen < 300) dlen = 300;  // BOOTP minimum
  uint8_t* u = f + 14 + 20;
  put16(u, 67);
  put16(u + 2, 68);
  put16(u + 4, (uint16_t)(8 + dlen));  // (checksum 0 = none)
  uint8_t* ip = f + 14;
  ip[0] = 0x45;
  put16(ip + 2, (uint16_t)(20 + 8 + dlen));
  ip[8] = 64;
  ip[9] = 17;
  put32(ip + 12, server);
  put32(ip + 16, uni ? mine : 0xFFFFFFFFu);
  put16(ip + 10, ipChecksum(ip, 20));
  if (ncm::send(f, 14 + 20 + 8 + dlen)) {
    st.dhcp++;
    if (reply == 5 && lease) isLeased = true;
    else if (nak) isLeased = false;
  }
}

// ---------------------------------------------------------------- frames from the host
static void onHostFrame(const uint8_t* f, size_t len) {
  if (len < 14 || !cfg.ethEnable) return;
  if (!(f[6] & 1)) {
    memcpy(seenMac, f + 6, 6);
    haveSeenMac = true;
  }
  uint16_t type = be16(f + 12);
  if (type == ETHERTYPE_ARP) return onArp(f, len);
  if (type != ETHERTYPE_IP) return;  // (IPv6 and the rest stay on the cable)
  const uint8_t* ip = f + 14;
  size_t ilen = len - 14;
  if (ilen < 20 || (ip[0] >> 4) != 4) return;
  size_t ihl = (size_t)(ip[0] & 15) * 4, tot = be16(ip + 2);
  if (ihl < 20 || tot < ihl || tot > ilen || tot > IP_MAX) return;
  bool firstFrag = (be16(ip + 6) & 0x1FFF) == 0;
  if (ip[9] == 17 && firstFrag && tot >= ihl + 8 && be16(ip + ihl + 2) == 67) {
    size_t ulen = be16(ip + ihl + 4);
    if (ulen >= 8 && ulen <= tot - ihl) onDhcp(ip + ihl, ulen);
    return;
  }
  // only what is addressed to somebody on the far side: no broadcasts / multicasts (name
  // lookups, discovery protocols, ...) - they would keep a 300 byte/s link busy for nothing
  uint32_t dst = be32(ip + 16), mask = netmask();
  if (dst == 0xFFFFFFFFu || (dst >> 28) == 0xE || dst == 0 || dst == hostIp() || dst == (network() | ~mask) ||
      dst == network()) {
    st.filtered++;
    return;
  }
  if (link::queuePacket(ip, tot)) st.txPkts++;
  else st.txDrop++;
}

// ---------------------------------------------------------------- setup / loop
void begin() {
  uint32_t id = g_nodeId;
  hostMac[0] = 0x02;  // locally administered
  hostMac[1] = 0x4C;
  gateMac[0] = 0x02;
  gateMac[1] = 0x4D;
  for (int i = 0; i < 4; i++) hostMac[2 + i] = gateMac[2 + i] = (uint8_t)(id >> (24 - 8 * i));
  if (checkAddresses(cfg.ethMasterIp, cfg.ethSlaveIp, cfg.ethPrefix)) cfg.ethEnable = 0;
  ncm::begin(hostMac, cfg.ethEnable);
  ncm::setRxHandler(onHostFrame);
  link::setNetHandler(onPacket);
  lastPeerId = cfg.peerId;
  shownHost = hostIp();
  shownPeer = peerIp();
  shownPrefix = cfg.ethPrefix;
  // announce the setting, so that both ends agree soon after power-up
  if (paired() && cfg.ethEnable) startSync(1500);
}

void loop() {
  uint32_t now = millis();
  if (cfg.peerId != lastPeerId) {  // paired with somebody else / unpaired
    lastPeerId = cfg.peerId;
    peerKnown = false;
    replyPending = false;
    sendTries = 0;
    if (paired() && cfg.ethEnable) startSync(1000);
  }
  // IP packets travel in the framed stream: only when both ends have ETH mode on
  link::setMux(cfg.ethEnable && paired() && synced());

  // the address of our host has changed (setting, or master / slave after a new pairing):
  // drop the media state for a moment, the host then asks again and gets the new one
  if (shownHost != hostIp() || shownPeer != peerIp() || shownPrefix != cfg.ethPrefix) {
    shownHost = hostIp();
    shownPeer = peerIp();
    shownPrefix = cfg.ethPrefix;
    addrDirty = true;
    addrDirtyAt = now;
  }
  if (addrDirty && now - addrDirtyAt > 600) {
    addrDirty = false;
    isLeased = false;
    ncm::bounceLink();
  }
  ncm::loop();
}

void status(Print& o) {
  char a[16], b[16], c[16];
  fmtIp(a, sizeof(a), network());
  fmtIp(b, sizeof(b), hostIp());
  fmtIp(c, sizeof(c), peerIp());
  const ncm::Stats& u = ncm::stats();
  // +ETHST=<on>,<peer in sync>,<USB adapter up>,<host has its address>,<network/prefix>,<host IP>,<peer host IP>
  o.printf("+ETHST=%u,%u,%u,%u,%s/%u,%s,%s\r\n", cfg.ethEnable ? 1 : 0, synced() ? 1 : 0, ncm::ready() ? 1 : 0,
           leased() ? 1 : 0, a, cfg.ethPrefix, b, c);
  o.printf("ETH: rev %lu  peer %s  stream %s  MAC %02X:%02X:%02X:%02X:%02X:%02X\r\n", (unsigned long)cfg.ethRev,
           !peerKnown ? "unknown" : synced() ? "in sync" : "differs", link::muxActive() ? "framed" : "plain",
           hostMac[0], hostMac[1], hostMac[2], hostMac[3], hostMac[4], hostMac[5]);
  o.printf("ETH: to peer %lu  from peer %lu  dropped %lu/%lu  filtered %lu  DHCP %lu  ARP %lu\r\n",
           (unsigned long)st.txPkts, (unsigned long)st.rxPkts, (unsigned long)st.txDrop, (unsigned long)st.rxDrop,
           (unsigned long)st.filtered, (unsigned long)st.dhcp, (unsigned long)st.arp);
  o.printf("ETH: USB frames in %lu  out %lu  dropped %lu  bad %lu\r\n", (unsigned long)u.rxFrames,
           (unsigned long)u.txFrames, (unsigned long)u.txDropped, (unsigned long)u.badNtb);
}

}  // namespace eth

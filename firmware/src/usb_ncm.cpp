// USB CDC-NCM class driver, see usb_ncm.h.
//
// TinyUSB runs in its own task: the callbacks below (d_*) are called from there, everything
// else from the main loop. The two sides only share a few flags:
//   * OUT: a received NTB stays in rxBuf (rxLen != 0) until the main loop has taken the
//     frames out of it, only then the endpoint is armed again - the host is held off by NAK.
//   * IN / notification: started from the main loop whenever the endpoint is idle.
#include "usb_ncm.h"

#include <Adafruit_TinyUSB.h>

#include "device/usbd_pvt.h"
#include "ringbuf.h"

namespace ncm {

#define NTB_MAX 3200          // dwNtbInMaxSize / dwNtbOutMaxSize
#define FRAME_MAX 1514
#define EP_SIZE 64
#define TX_MAX_DGRAM 4
#define TX_DATA_OFS (12 + 8 + 4 * (TX_MAX_DGRAM + 1))  // NTH16 + NDP16 + datagram table
#define REENUM_DELAY_MS 500   // lets the answer to the command that caused it reach the host
#define REENUM_OFF_MS 400     // time off the bus
#define BOUNCE_MS 1500

#define SIG_NTH16 0x484D434Eu  // "NCMH"
#define SIG_NDP16 0x304D434Eu  // "NCM0"
#define SIG_NDP16_CRC 0x314D434Eu

enum : uint8_t {
  REQ_SET_PACKET_FILTER = 0x43,
  REQ_GET_NTB_PARAMETERS = 0x80,
  REQ_GET_NTB_INPUT_SIZE = 0x85,
  REQ_SET_NTB_INPUT_SIZE = 0x86,
};

static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline void wr16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}
static inline void wr32(uint8_t* p, uint32_t v) {
  wr16(p, (uint16_t)v);
  wr16(p + 2, (uint16_t)(v >> 16));
}

// ---------------------------------------------------------------- descriptor
static char macStr[13];
static uint8_t macStrId = 0;

class NcmInterface : public Adafruit_USBD_Interface {
 public:
  uint16_t getInterfaceDescriptor(uint8_t, uint8_t* buf, uint16_t bufsize) override {
    uint8_t itf = 0, epN = 0, epI = 0, epO = 0;
    if (buf) {
      itf = TinyUSBDevice.allocInterface(2);
      epN = TinyUSBDevice.allocEndpoint(TUSB_DIR_IN);
      epI = TinyUSBDevice.allocEndpoint(TUSB_DIR_IN);
      epO = TinyUSBDevice.allocEndpoint(TUSB_DIR_OUT);
    }
    const uint8_t d[] = {
        // interface association: communication + data interface
        8, TUSB_DESC_INTERFACE_ASSOCIATION, itf, 2, TUSB_CLASS_CDC, 0x0D, 0, 0,
        // communication interface (subclass 0x0D = NCM)
        9, TUSB_DESC_INTERFACE, itf, 0, 1, TUSB_CLASS_CDC, 0x0D, 0, _strid,
        5, TUSB_DESC_CS_INTERFACE, 0x00, 0x10, 0x01,                      // header, CDC 1.10
        5, TUSB_DESC_CS_INTERFACE, 0x06, itf, (uint8_t)(itf + 1),         // union
        13, TUSB_DESC_CS_INTERFACE, 0x0F, macStrId, 0, 0, 0, 0,           // Ethernet networking
        (uint8_t)(FRAME_MAX & 0xFF), (uint8_t)(FRAME_MAX >> 8), 0, 0, 0,
        6, TUSB_DESC_CS_INTERFACE, 0x1A, 0x00, 0x01, 0x00,                // NCM 1.00, no optional requests
        7, TUSB_DESC_ENDPOINT, epN, TUSB_XFER_INTERRUPT, EP_SIZE, 0, 50,
        // data interface: alternate 0 without endpoints, alternate 1 = traffic
        9, TUSB_DESC_INTERFACE, (uint8_t)(itf + 1), 0, 0, TUSB_CLASS_CDC_DATA, 0, 1, 0,
        9, TUSB_DESC_INTERFACE, (uint8_t)(itf + 1), 1, 2, TUSB_CLASS_CDC_DATA, 0, 1, 0,
        7, TUSB_DESC_ENDPOINT, epI, TUSB_XFER_BULK, EP_SIZE, 0, 0,
        7, TUSB_DESC_ENDPOINT, epO, TUSB_XFER_BULK, EP_SIZE, 0, 0,
    };
    if (buf) {
      if (bufsize < sizeof(d)) return 0;
      memcpy(buf, d, sizeof(d));
    }
    return sizeof(d);
  }
  void name(const char* s) { setStringDescriptor(s); }
};
static NcmInterface ncmItf;

// The network function is the last one in the configuration descriptor, so it is hidden by
// shortening the descriptor: total length and interface count in the header.
static uint8_t cfgDesc[256];
static uint16_t fullLen = 0, ncmLen = 0;
static uint8_t fullItf = 0;
static bool isExposed = false, wantExposed = false, added = false;
static uint8_t reenum = 0;  // 0 idle, 1 waiting to leave the bus, 2 off the bus
static uint32_t reenumAt = 0;

static void patchDescriptor(bool on) {
  if (!added) return;
  wr16(cfgDesc + 2, on ? fullLen : (uint16_t)(fullLen - ncmLen));
  cfgDesc[4] = on ? fullItf : (uint8_t)(fullItf - 2);
  isExposed = on;
}

// ---------------------------------------------------------------- driver state
static volatile uint8_t epNotif = 0, epIn = 0, epOut = 0, itfNum = 0xFF, dataAlt = 0;
static volatile uint16_t rxLen = 0;       // NTB waiting in rxBuf for the main loop
static volatile bool resetSeen = false;   // bus reset / unplugged: drop what is queued
static volatile bool speedSent = false;
static volatile int8_t linkSent = -1;     // media state the host was told: -1 none yet, 0 down, 1 up
static bool bouncing = false;
static uint32_t bounceUntil = 0;

static uint8_t rxBuf[NTB_MAX] __attribute__((aligned(4)));
static uint8_t txBuf[NTB_MAX] __attribute__((aligned(4)));
static uint8_t notifBuf[16] __attribute__((aligned(4)));
static uint8_t ctlBuf[32] __attribute__((aligned(4)));
static RingBuf<4096> txq;  // frames for the host: length (2 bytes) + frame
static void (*rxHandler)(const uint8_t*, size_t) = nullptr;
static Stats st;

static void armRx(uint8_t rhport) {
  if (dataAlt != 1 || rxLen || !epOut) return;
  if (!usbd_edpt_claim(rhport, epOut)) return;  // busy = already armed
  usbd_edpt_xfer(rhport, epOut, rxBuf, sizeof(rxBuf));
}

// ---------------------------------------------------------------- TinyUSB class driver
static void d_init(void) {
  epNotif = epIn = epOut = 0;
  itfNum = 0xFF;
  dataAlt = 0;
  rxLen = 0;
  speedSent = false;
  linkSent = -1;
  resetSeen = true;
}
static bool d_deinit(void) { return true; }
static void d_reset(uint8_t) { d_init(); }

static uint16_t d_open(uint8_t rhport, tusb_desc_interface_t const* itf, uint16_t max_len) {
  if (itf->bInterfaceClass != TUSB_CLASS_CDC || itf->bInterfaceSubClass != 0x0D) return 0;
  uint16_t len = sizeof(tusb_desc_interface_t);
  const uint8_t* p = tu_desc_next(itf);
  while (len < max_len && tu_desc_type(p) == TUSB_DESC_CS_INTERFACE) {
    len += tu_desc_len(p);
    p = tu_desc_next(p);
  }
  if (len >= max_len || tu_desc_type(p) != TUSB_DESC_ENDPOINT) return 0;
  if (!usbd_edpt_open(rhport, (tusb_desc_endpoint_t const*)p)) return 0;
  uint8_t notif = ((tusb_desc_endpoint_t const*)p)->bEndpointAddress;
  len += tu_desc_len(p);
  p = tu_desc_next(p);
  while (len < max_len && tu_desc_type(p) == TUSB_DESC_INTERFACE) {
    if (((tusb_desc_interface_t const*)p)->bInterfaceClass != TUSB_CLASS_CDC_DATA) return 0;
    len += tu_desc_len(p);
    p = tu_desc_next(p);
  }
  if (len >= max_len || tu_desc_type(p) != TUSB_DESC_ENDPOINT) return 0;
  uint8_t out = 0, in = 0;
  if (!usbd_open_edpt_pair(rhport, p, 2, TUSB_XFER_BULK, &out, &in)) return 0;
  len += 2 * sizeof(tusb_desc_endpoint_t);
  itfNum = itf->bInterfaceNumber;
  epNotif = notif;
  epOut = out;
  epIn = in;
  dataAlt = 0;
  rxLen = 0;
  return len;
}

static bool d_control(uint8_t rhport, uint8_t stage, tusb_control_request_t const* req) {
  if (stage != CONTROL_STAGE_SETUP) return true;
  if (req->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD) {
    if (req->wIndex != (uint16_t)(itfNum + 1)) return false;
    if (req->bRequest == TUSB_REQ_GET_INTERFACE) {
      ctlBuf[0] = dataAlt;
      return tud_control_xfer(rhport, req, ctlBuf, 1);
    }
    if (req->bRequest == TUSB_REQ_SET_INTERFACE && req->wValue < 2) {
      dataAlt = (uint8_t)req->wValue;
      if (dataAlt == 1) {
        speedSent = false;  // the main loop reports speed + media state
        linkSent = -1;
        armRx(rhport);
      }
      return tud_control_status(rhport, req);
    }
    return false;
  }
  if (req->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS || req->wIndex != itfNum) return false;
  switch (req->bRequest) {
    case REQ_GET_NTB_PARAMETERS:
      // (Windows refuses to start the adapter - code 10 - with a datagram divisor of 1: it has to
      // match the alignment)
      memset(ctlBuf, 0, 28);
      wr16(ctlBuf + 0, 28);        // wLength
      wr16(ctlBuf + 2, 1);         // 16 bit NTBs
      wr32(ctlBuf + 4, NTB_MAX);   // dwNtbInMaxSize
      wr16(ctlBuf + 8, 4);         // wNdpInDivisor
      wr16(ctlBuf + 12, 4);        // wNdpInAlignment
      wr32(ctlBuf + 16, NTB_MAX);  // dwNtbOutMaxSize
      wr16(ctlBuf + 20, 4);        // wNdpOutDivisor
      wr16(ctlBuf + 24, 4);        // wNdpOutAlignment
      wr16(ctlBuf + 26, 0);        // wNtbOutMaxDatagrams: no limit
      return tud_control_xfer(rhport, req, ctlBuf, 28);
    case REQ_GET_NTB_INPUT_SIZE:
      memset(ctlBuf, 0, 8);
      wr32(ctlBuf, NTB_MAX);
      return tud_control_xfer(rhport, req, ctlBuf, req->wLength < 8 ? req->wLength : 8);
    case REQ_SET_NTB_INPUT_SIZE:  // our NTBs carry one frame or a few small ones: always fits
      if (req->wLength > sizeof(ctlBuf)) return false;
      return tud_control_xfer(rhport, req, ctlBuf, req->wLength);
    case REQ_SET_PACKET_FILTER:
      return tud_control_status(rhport, req);
    default:
      return false;
  }
}

static bool d_xfer(uint8_t rhport, uint8_t ep, xfer_result_t result, uint32_t n) {
  if (ep == epOut) {
    if (result == XFER_RESULT_SUCCESS && n >= 12 && n <= sizeof(rxBuf)) rxLen = (uint16_t)n;
    else armRx(rhport);
  }
  return true;
}

static const usbd_class_driver_t kDriver = {"NCM", d_init, d_deinit, d_reset, d_open, d_control, d_xfer, nullptr};

extern "C" usbd_class_driver_t const* usbd_app_driver_get_cb(uint8_t* driver_count) {
  *driver_count = 1;
  return &kDriver;
}

// ---------------------------------------------------------------- main loop side
static void processRx() {
  size_t len = rxLen;
  if (rd32(rxBuf) == SIG_NTH16 && rd16(rxBuf + 4) == 12) {
    size_t blk = rd16(rxBuf + 8);
    if (blk && blk < len) len = blk;
    size_t ndp = rd16(rxBuf + 10);
    for (int guard = 0; guard < 8 && ndp >= 12 && ndp + 16 <= len; guard++) {
      uint32_t sig = rd32(rxBuf + ndp);
      size_t nlen = rd16(rxBuf + ndp + 4);
      if ((sig != SIG_NDP16 && sig != SIG_NDP16_CRC) || nlen < 16 || ndp + nlen > len) {
        st.badNtb++;
        break;
      }
      for (size_t e = ndp + 8; e + 4 <= ndp + nlen; e += 4) {
        size_t idx = rd16(rxBuf + e), dl = rd16(rxBuf + e + 2);
        if (!idx || !dl) break;
        if (sig == SIG_NDP16_CRC && dl > 4) dl -= 4;
        if (idx + dl > len || dl < 14 || dl > FRAME_MAX) {
          st.badNtb++;
          continue;
        }
        st.rxFrames++;
        if (rxHandler) rxHandler(rxBuf + idx, dl);
      }
      ndp = rd16(rxBuf + ndp + 6);  // wNextNdpIndex, 0 = last
    }
  } else {
    st.badNtb++;
  }
  rxLen = 0;
  armRx(0);
}

// packs the waiting frames into one NTB and starts the transfer
static void pumpTx() {
  if (txq.empty() || usbd_edpt_busy(0, epIn)) return;
  memset(txBuf, 0, TX_DATA_OFS);
  size_t ofs = TX_DATA_OFS;
  uint8_t n = 0;
  while (n < TX_MAX_DGRAM && txq.used() >= 2) {
    uint8_t h[2] = {0, 0};
    txq.peek(h, 2);
    size_t fl = rd16(h);
    if (fl > FRAME_MAX || txq.used() < fl + 2) {  // cannot happen: start over
      txq.clear();
      break;
    }
    if (ofs + fl > NTB_MAX - 4) break;
    txq.drop(2);
    txq.read(txBuf + ofs, fl);
    wr16(txBuf + 20 + 4 * n, (uint16_t)ofs);
    wr16(txBuf + 22 + 4 * n, (uint16_t)fl);
    n++;
    ofs += fl;
    while (ofs & 3) txBuf[ofs++] = 0;
  }
  if (!n) return;
  // a transfer that ends on a full packet would need a zero length packet: pad instead
  if ((ofs % EP_SIZE) == 0) {
    memset(txBuf + ofs, 0, 4);
    ofs += 4;
  }
  static uint16_t seq = 0;
  wr32(txBuf + 0, SIG_NTH16);
  wr16(txBuf + 4, 12);
  wr16(txBuf + 6, seq++);
  wr16(txBuf + 8, (uint16_t)ofs);
  wr16(txBuf + 10, 12);
  wr32(txBuf + 12, SIG_NDP16);
  wr16(txBuf + 16, 8 + 4 * (TX_MAX_DGRAM + 1));
  if (!usbd_edpt_claim(0, epIn)) return;  // (frames are lost only if the bus went away meanwhile)
  usbd_edpt_xfer(0, epIn, txBuf, (uint16_t)ofs);
  st.txFrames += n;
}

static bool notify(uint8_t request, uint16_t value, const uint8_t* data, uint8_t dlen) {
  if (usbd_edpt_busy(0, epNotif) || !usbd_edpt_claim(0, epNotif)) return false;
  notifBuf[0] = 0xA1;  // class, interface, device to host
  notifBuf[1] = request;
  wr16(notifBuf + 2, value);
  wr16(notifBuf + 4, itfNum);
  wr16(notifBuf + 6, dlen);
  if (dlen) memcpy(notifBuf + 8, data, dlen);
  return usbd_edpt_xfer(0, epNotif, notifBuf, (uint16_t)(8 + dlen));
}

static void pumpNotify(uint32_t now) {
  if (bouncing && (int32_t)(now - bounceUntil) >= 0) bouncing = false;
  int8_t want = bouncing ? 0 : 1;
  if (want && !speedSent) {
    uint8_t sp[8];
    wr32(sp, 12000000);  // downlink / uplink bit rate (full speed USB)
    wr32(sp + 4, 12000000);
    if (notify(0x2A /* CONNECTION_SPEED_CHANGE */, 0, sp, 8)) speedSent = true;
  } else if (linkSent != want) {
    if (notify(0x00 /* NETWORK_CONNECTION */, (uint16_t)want, nullptr, 0)) linkSent = want;
  }
}

void begin(const uint8_t mac[6], bool on) {
  if (added) return;
  snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  TinyUSBDevice.setConfigurationBuffer(cfgDesc, sizeof(cfgDesc));
  macStrId = TinyUSBDevice.addStringDescriptor(macStr);
  ncmItf.name("LoRa Bridge Network");
  uint16_t before = rd16(cfgDesc + 2);
  if (!TinyUSBDevice.addInterface(ncmItf)) return;
  fullLen = rd16(cfgDesc + 2);
  fullItf = cfgDesc[4];
  ncmLen = (uint16_t)(fullLen - before);
  added = true;
  wantExposed = on;
  patchDescriptor(on);
}

void expose(bool on) {
  if (!added || on == wantExposed) return;
  wantExposed = on;
  if (!reenum) {
    reenum = 1;
    reenumAt = millis() + REENUM_DELAY_MS;
  }
}
bool exposed() { return wantExposed; }
bool reenumerating() { return reenum != 0; }
bool ready() { return isExposed && !reenum && dataAlt == 1 && epIn && tud_ready(); }
void bounceLink() {
  if (!ready()) return;  // a host that configures us later asks for its address anyway
  bouncing = true;
  bounceUntil = millis() + BOUNCE_MS;
}
void setRxHandler(void (*cb)(const uint8_t*, size_t)) { rxHandler = cb; }

bool send(const uint8_t* frame, size_t len) {
  if (!ready() || len < 14 || len > FRAME_MAX) return false;
  if (txq.space() < len + 2) {
    st.txDropped++;
    return false;
  }
  uint8_t h[2] = {(uint8_t)len, (uint8_t)(len >> 8)};
  txq.write(h, 2);
  txq.write(frame, len);
  return true;
}

void loop() {
  uint32_t now = millis();
  if (reenum == 1 && (int32_t)(now - reenumAt) >= 0) {
    TinyUSBDevice.detach();
    reenum = 2;
    reenumAt = now + REENUM_OFF_MS;
  } else if (reenum == 2 && (int32_t)(now - reenumAt) >= 0) {
    patchDescriptor(wantExposed);
    TinyUSBDevice.attach();
    reenum = 0;
  }
  if (resetSeen) {
    resetSeen = false;
    txq.clear();
    bouncing = false;
  }
  if (!ready()) return;
  if (rxLen) processRx();
  pumpTx();
  pumpNotify(now);
}

bool busy() { return rxLen != 0 || !txq.empty() || reenum != 0; }
const Stats& stats() { return st; }

}  // namespace ncm

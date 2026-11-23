// Transport + protocol for the FakeTec LoRa bridge over WebUSB or Web Bluetooth.
// Works in the browser (global LoraBridge) and in Node (require) for tests.
//
// The device's WebUSB interface and its BLE UART (Nordic UART Service) are transparent serial
// ports like its USB CDC port.
// Configuration uses the in-band control channel, which never leaves data mode:
//   host -> device : ESC ] L B ; <AT command> BEL
//   device -> host : ESC ] L B ; <response text> BEL
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.LoraBridge = factory();
})(typeof self !== 'undefined' ? self : this, function () {
  const VID = 0x239a;
  const PFX = [0x1b, 0x5d, 0x4c, 0x42, 0x3b]; // ESC ] L B ;
  const BEL = 0x07;
  // Nordic UART Service: the phone / browser writes to RX and gets notifications from TX
  const NUS_SERVICE = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
  const NUS_RX = '6e400002-b5a3-f393-e0a9-e50e24dcca9e';
  const NUS_TX = '6e400003-b5a3-f393-e0a9-e50e24dcca9e';

  class LoraBridge {
    constructor() {
      this.dev = null;          // USBDevice
      this.ble = null;          // BluetoothDevice
      this.kind = null;         // 'usb' | 'ble'
      this.bleChunk = 180;      // bytes per BLE write, lowered if the connection has a small MTU
      this.running = false;
      this.held = 0;
      this.body = null;
      this.waiter = null;
      this.cmdQueue = Promise.resolve();
      this.writeQueue = Promise.resolve();
      this.flushTimer = null;
      this.onData = () => {};   // (Uint8Array) terminal data from the remote side
      this.onClose = () => {};  // (Error|null)
    }

    static get filters() { return [{ vendorId: VID }]; }
    // options for navigator.bluetooth.requestDevice()
    static get bleOptions() { return { filters: [{ services: [NUS_SERVICE] }], optionalServices: [NUS_SERVICE] }; }

    async open(dev) {
      await dev.open();
      if (dev.configuration === null || dev.configuration === undefined) await dev.selectConfiguration(1);
      let found = null;
      for (const itf of dev.configuration.interfaces)
        for (const alt of itf.alternates)
          if (alt.interfaceClass === 0xff) found = { num: itf.interfaceNumber, alt };
      if (!found) throw new Error('no WebUSB interface on this device (firmware 1.1.0 or newer needed)');
      this.ifnum = found.num;
      this.epIn = found.alt.endpoints.find((e) => e.direction === 'in').endpointNumber;
      this.epOut = found.alt.endpoints.find((e) => e.direction === 'out').endpointNumber;
      await dev.claimInterface(this.ifnum);
      // "line state": tells the firmware a client is attached (wakes the LoRa link)
      await dev.controlTransferOut({ requestType: 'class', recipient: 'interface', request: 0x22, value: 1, index: this.ifnum });
      this.dev = dev;
      this.kind = 'usb';
      this.running = true;
      this._readLoop();
    }

    // Bluetooth LE (Web Bluetooth). With the default firmware setting (AT+BLEPAIR=1) there is
    // no operating system pairing: the connection comes up at once, but the device answers
    // nothing until a new host was allowed with its button. With BLEPAIR=2/3 the operating
    // system pairs when notifications are enabled.
    async openBle(device) {
      if (this.bleBusy) throw new Error('a bluetooth connection attempt is already running');
      this.bleBusy = true;
      this.bleAbort = false;
      this.bleTrying = device;
      try { return await this._openBle(device); }
      finally { this.bleBusy = false; this.bleTrying = null; }
    }

    // gives up a running openBle()
    abortBle() {
      this.bleAbort = true;
      try { if (this.bleTrying) this.bleTrying.gatt.disconnect(); } catch (e) {}
    }

    async _openBle(device) {
      const onGone = () => {
        if (this.ble !== device) return;
        const was = this.running;
        this.running = false;
        this.ble = null;
        this.kind = null;
        if (was) this.onClose(new Error('bluetooth disconnected'));
      };
      device.addEventListener('gattserverdisconnected', onGone);
      // A first attempt can fail with "GATT Server is disconnected" (the operating system
      // drops the link while it pairs, or still holds a stale connection). Try again a few
      // times, strictly one attempt at a time.
      const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
      let lastErr = null;
      for (let attempt = 0; attempt < 3; attempt++) {
        try {
          if (this.bleAbort) throw new Error('cancelled');
          const gatt = await device.gatt.connect();
          const svc = await gatt.getPrimaryService(NUS_SERVICE);
          this.bleRx = await svc.getCharacteristic(NUS_RX);
          this.bleTx = await svc.getCharacteristic(NUS_TX);
          await this.bleTx.startNotifications();
          if (this.bleAbort) throw new Error('cancelled');
          lastErr = null;
          break;
        } catch (e) {
          lastErr = e;
          try { device.gatt.disconnect(); } catch (e2) {}
          if (this.bleAbort) { lastErr = new Error('cancelled'); break; }
          if (this.onRetry) this.onRetry(attempt + 1, e);
          await sleep(2500);   // let the operating system finish tearing the link down
        }
      }
      if (lastErr) { device.removeEventListener('gattserverdisconnected', onGone); throw lastErr; }
      this.bleTx.addEventListener('characteristicvaluechanged', (e) => {
        const v = e.target.value;
        this._feed(new Uint8Array(v.buffer, v.byteOffset, v.byteLength));
      });
      this.ble = device;
      this.kind = 'ble';
      this.bleChunk = 180;
      this.running = true;
    }

    async close() {
      if (this.ble) {
        const d = this.ble;
        this.running = false;
        this.ble = null;
        this.kind = null;
        try { d.gatt.disconnect(); } catch (e) {}
        return;
      }
      if (!this.dev) return;
      this.running = false;
      this.kind = null;
      const dev = this.dev;
      this.dev = null;
      try { await dev.controlTransferOut({ requestType: 'class', recipient: 'interface', request: 0x22, value: 0, index: this.ifnum }); } catch (e) {}
      try { await dev.close(); } catch (e) {}
    }

    get connected() { return (!!this.dev || !!this.ble) && this.running; }

    async _readLoop() {
      const dev = this.dev;
      try {
        while (this.running) {
          const r = await dev.transferIn(this.epIn, 64);
          if (r.status === 'stall') { await dev.clearHalt('in', this.epIn); continue; }
          if (r.data && r.data.byteLength) this._feed(new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength));
        }
      } catch (e) {
        if (this.running) {
          this.running = false;
          this.dev = null;
          this.kind = null;
          this.onClose(e);
        }
      }
    }

    _feed(data) {
      const out = [];
      for (const c of data) {
        if (this.body) {
          if (c === BEL) {
            const text = new TextDecoder().decode(new Uint8Array(this.body));
            this.body = null;
            if (this.waiter) this.waiter(text);
          } else this.body.push(c);
          continue;
        }
        if (c === PFX[this.held]) {
          if (++this.held === PFX.length) { this.held = 0; this.body = []; }
          continue;
        }
        if (this.held) {
          out.push(...PFX.slice(0, this.held));
          this.held = 0;
          if (c === 0x1b) { this.held = 1; continue; }
        }
        out.push(c);
      }
      if (out.length) this.onData(new Uint8Array(out));
      clearTimeout(this.flushTimer);
      if (this.held) this.flushTimer = setTimeout(() => {   // a lone ESC was just an ESC
        if (this.held && !this.body) { const h = PFX.slice(0, this.held); this.held = 0; this.onData(new Uint8Array(h)); }
      }, 30);
    }

    // raw bytes to the remote side
    write(bytes) {
      if (typeof bytes === 'string') bytes = new TextEncoder().encode(bytes);
      const run = async () => {
        if (this.dev) { await this.dev.transferOut(this.epOut, bytes); return; }
        if (!this.ble) return;
        for (let i = 0; i < bytes.length;) {
          const part = bytes.slice(i, i + this.bleChunk);
          try {
            await this.bleRx.writeValueWithoutResponse(part);
            i += part.length;
          } catch (e) {
            if (this.bleChunk <= 20) throw e;
            this.bleChunk = 20;   // MTU smaller than hoped: fall back to the minimum
          }
        }
      };
      const p = this.writeQueue.then(run, run);
      this.writeQueue = p.catch(() => {});
      return p;
    }

    // one AT command, resolves with the response text
    cmd(line, timeoutMs = 3000) {
      const run = async () => {
        if (!this.connected) throw new Error('not connected');
        const answer = new Promise((resolve, reject) => {
          const t = setTimeout(() => { this.waiter = null; reject(new Error('no answer from the device')); }, timeoutMs);
          this.waiter = (s) => { clearTimeout(t); this.waiter = null; resolve(s.replace(/\r\n/g, '\n').trim()); };
        });
        const enc = new TextEncoder().encode(line);
        await this.write(new Uint8Array([...PFX, ...enc, BEL]));
        return answer;
      };
      const p = this.cmdQueue.then(run, run);
      this.cmdQueue = p.catch(() => {});
      return p;
    }

    // +ST=mode,role,rssi,snr,batt,usb,txRate,rxRate,peerBatt,queue,channel,freq,peerRssi,peerSnr
    async status() {
      const r = await this.cmd('AT+ST');
      const m = r.match(/\+ST=(.*)/);
      if (!m) return null;
      const f = m[1].trim().split(',');
      return { mode: f[0], role: f[1], rssi: +f[2], snr: +f[3], batt: +f[4], usb: f[5] === '1', txRate: +f[6], rxRate: +f[7],
               peerBatt: +f[8], queue: +f[9], channel: +f[10], freq: +f[11], peerRssi: +f[12], peerSnr: +f[13] };
    }

    // { KEY: 'value', ... } from AT+CFG
    async config() {
      const r = await this.cmd('AT+CFG');
      const cfg = {};
      for (const l of r.split('\n')) {
        const m = l.match(/^\+([A-Z0-9]+)=(.*)$/);
        if (m) cfg[m[1]] = m[2].trim();
      }
      return cfg;
    }
  }
  return LoraBridge;
});
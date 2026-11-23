// Headless test of bridge.js against real hardware, using node-usb's WebUSB implementation.
// Needs two paired bridges plugged in.   node test/usb-test.js
const { WebUSB } = require('usb');
const LoraBridge = require('../bridge.js');

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
(async () => {
  const usb = new WebUSB({ allowAllDevices: true });
  const devs = (await usb.getDevices()).filter((d) => d.vendorId === 0x239a);
  console.log('devices:', devs.map((d) => `${d.productName} ${d.serialNumber}`));
  if (devs.length < 1) throw new Error('no bridge found');
  const a = new LoraBridge();
  await a.open(devs[0]);
  console.log('A info:\n' + (await a.cmd('ATI')));
  console.log('A status:', await a.status());
  const cfg = await a.config();
  console.log('A config keys:', Object.keys(cfg).length, 'FREQ =', cfg.FREQ);

  if (devs.length >= 2) {
    const b = new LoraBridge();
    await b.open(devs[1]);
    const got = { a: '', b: '' };
    a.onData = (d) => (got.a += Buffer.from(d).toString('latin1'));
    b.onData = (d) => (got.b += Buffer.from(d).toString('latin1'));
    for (let i = 0; i < 15 && (await a.status()).mode !== 'LINK'; i++) await sleep(1000);
    console.log('A mode:', (await a.status()).mode, ' B mode:', (await b.status()).mode);
    const msgA = '\x1b[1;32mgreen from A\x1b[0m \x1b alone-esc ok\r\n';
    const msgB = 'plain from B 0123456789\r\n';
    await a.write(msgA);
    await b.write(msgB);
    for (let i = 0; i < 40 && (got.b.length < msgA.length || got.a.length < msgB.length); i++) {
      await sleep(250);
      if (i % 4 === 0) await a.cmd('AT+ST');   // control traffic interleaved with data
    }
    console.log('B received A intact:', got.b === msgA, JSON.stringify(got.b));
    console.log('A received B intact:', got.a === msgB, JSON.stringify(got.a));
    // FAST mode (firmware 1.3.0+): to 2.4 GHz and back
    if ((await a.status()).fast !== null) {
      try {
        console.log('FAST on:', JSON.stringify(await a.setFast(true)), ' status.fast A/B:', (await a.status()).fast, (await b.status()).fast);
        console.log('FAST off:', JSON.stringify(await a.setFast(false)));
      } catch (e) { console.log('FAST failed:', e.message); }
    }
    await b.close();
  }
  await a.close();
  process.exit(0);
})().catch((e) => { console.error('FAILED', e); process.exit(1); });
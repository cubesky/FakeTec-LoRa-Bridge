'use strict';
const $ = (s) => document.querySelector(s);
const bridge = new LoraBridge();
let pollTimer = null;
let busy = false;        // a connect / disconnect is running: only one at a time
let loadedCfg = {};

// ---- terminal: raw pass-through, xterm.js interprets colours / cursor control
const term = new Terminal({
  cursorBlink: true, scrollback: 10000, allowProposedApi: true,
  fontFamily: 'Cascadia Mono, Consolas, "DejaVu Sans Mono", monospace', fontSize: 14,
  theme: { background: '#000000' },
});
const fit = new FitAddon.FitAddon();
term.loadAddon(fit);
term.open($('#term'));
fit.fit();
new ResizeObserver(() => fit.fit()).observe($('#termwrap'));
term.onData((s) => { if (bridge.connected) bridge.write(s).catch(() => {}); });
term.onBinary((s) => { if (bridge.connected) bridge.write(Uint8Array.from(s, (c) => c.charCodeAt(0))).catch(() => {}); });
bridge.onData = (bytes) => term.write(bytes);
bridge.onClose = () => { setConnected(false); modal('连接已断开', ['与设备的连接已断开。'], { error: true }); };

// ---- messages from the client itself go into a modal dialog, never into the terminal
// (the terminal only ever shows what the remote side sent)
const dlg = $('#msg');
let dlgTimer = null;
function modalLine(text, dim) {
  const p = document.createElement('p');
  p.textContent = text;
  if (dim) p.className = 'dim';
  $('#msgbody').append(p);
  p.scrollIntoView({ block: 'nearest' });
}
// opts: busy (spinner, no OK button, Esc does nothing), onCancel (shows a cancel button),
//       error (red title), autoClose (ms)
let dlgCancel = null;
function modal(title, lines, opts = {}) {
  clearTimeout(dlgTimer);
  dlgCancel = opts.onCancel || null;
  $('#msgcancel').hidden = !dlgCancel;
  $('#msgtitle').textContent = title;
  $('#msgbody').innerHTML = '';
  for (const l of [].concat(lines || [])) modalLine(l);
  dlg.classList.toggle('error', !!opts.error);
  dlg.classList.toggle('busy', !!opts.busy);
  $('#msgok').hidden = !!opts.busy;
  if (!dlg.open) dlg.showModal();
  if (opts.autoClose) dlgTimer = setTimeout(closeModal, opts.autoClose);
}
function closeModal() {
  clearTimeout(dlgTimer);
  if (dlg.open) dlg.close();
  term.focus();
}
$('#msgok').addEventListener('click', closeModal);
$('#msgcancel').addEventListener('click', () => { if (dlgCancel) dlgCancel(); });
// Esc must not hide a dialog that reports a running operation
dlg.addEventListener('cancel', (e) => { if (dlg.classList.contains('busy')) e.preventDefault(); });
dlg.addEventListener('close', () => term.focus());
function log(text) { const l = $('#log'); l.textContent = text; }

// ---- settings
const LABELS = {
  FREQ: '基础频率 MHz', BW: '带宽 kHz', SF: '扩频因子', CR: '编码率 4/x', PWR: '发射功率 dBm', SYNC: '同步字 (hex)',
  PRE: '前导码符号数', TCXO: 'TCXO 电压 (0=晶振)', HOP: '跳频', HOPCH: '跳频信道数', HOPSP: '信道间隔 kHz',
  DWELL: '驻留时间 ms (0=自动)', AFH: '自适应跳频', LBT: '先听后发', LBTRSSI: 'LBT RSSI 门限 (0=关)',
  PEER: '对端 ID', ENC: '加密', ACK: '确认重传', RETRY: '重传次数', MAXPL: '每帧最大字节', PKTTO: '组包超时 ms',
  HB: '心跳 s', COMP: '压缩', PSM: '省电待机', SBHB: '待机心跳 s', USBACT: 'USB 激活 (1=插入 0=打开)',
  BLE: '蓝牙', NAME: '蓝牙名称', BLEPIN: '蓝牙固定配对码 (0=关)', BLEPAIR: '蓝牙允许方式 (1=按钮 2=数字比对 3=加密配对 0=开放)',
  OLED: '息屏时间 s (0=常亮)', OLEDTYPE: '屏幕 (0=SSD1306 1=SH1106)', FLIP: '屏幕翻转', BATCAL: '电池校准 x1000', OUT: '输出位',
  FASTEN: '允许 2.4G 高速模式', FASTLOSS: '高速回退丢包门限 %', FASTRSSI: '高速回退 RSSI 门限 dBm', FASTPWR: '2.4G 发射功率 dBm',
  ETH: '网卡模式 (两端同步)', ETHNET: '网段 (地址/前缀长度)', ETHMIP: '主端 IP (主机一侧电脑)', ETHSIP: '从端 IP (从机一侧电脑)',
};
// the three address settings belong together: they are checked and sent to the peer as one
const ETH_ADDR = ['ETHNET', 'ETHMIP', 'ETHSIP'];
const READONLY = new Set(['PEER']);

function renderCfg(cfg) {
  loadedCfg = cfg;
  const box = $('#fields');
  box.className = '';
  box.innerHTML = '';
  for (const [k, v] of Object.entries(cfg)) {
    const row = document.createElement('div');
    row.className = 'row';
    const label = document.createElement('label');
    label.innerHTML = `${LABELS[k] || k} <small>${k}</small>`;
    const input = document.createElement('input');
    input.value = v.split(' ')[0];
    input.dataset.key = k;
    input.dataset.orig = input.value;
    input.spellcheck = false;
    if (READONLY.has(k)) input.disabled = true;
    input.addEventListener('input', () => input.classList.toggle('changed', input.value !== input.dataset.orig));
    row.append(label, input);
    box.append(row);
  }
}

async function reloadCfg() {
  try { renderCfg(await bridge.config()); } catch (e) { log(String(e.message || e)); }
}

async function applyCfg() {
  const changed = [...document.querySelectorAll('#fields input.changed')];
  if (!changed.length) { log('没有修改'); return; }
  const lines = [];
  const run = async (line) => { try { lines.push(`${line}\n${await bridge.cmd(line, 4000)}`); } catch (e) { lines.push(`${line}\n${e.message}`); } };
  const val = (k) => { const i = document.querySelector(`#fields input[data-key="${k}"]`); return i ? i.value.trim() : ''; };
  for (const i of changed) {
    if (ETH_ADDR.includes(i.dataset.key) || i.dataset.key === 'ETH') continue;
    await run(`AT+${i.dataset.key}=${i.value.trim()}`);
  }
  // network + both host addresses in one command (both ends take them over, the hosts get
  // their new address by themselves)
  if (changed.some((i) => ETH_ADDR.includes(i.dataset.key))) await run(`AT+ETHCFG=${val('ETHNET')},${val('ETHMIP')},${val('ETHSIP')}`);
  // last: switching the network adapter on / off re-enumerates the USB device
  const eth = changed.find((i) => i.dataset.key === 'ETH');
  if (eth) await run(`AT+ETH=${eth.value.trim()}`);
  log(lines.join('\n'));
  showPane('ctl');
  if (!eth || bridge.kind !== 'usb') await reloadCfg();
}

// ---- FAST mode (2.4 GHz high speed link) switch
const FAST_WHY = {
  'Bluetooth in use': '有一端正在使用蓝牙连接（高速模式与蓝牙互斥）',
  'LoRa link not up': 'LoRa 链路未处于 LINK 状态', 'not paired': '尚未配对', 'no answer': '对端没有应答（固件低于 1.3.0？）',
  'no 2.4G link': '2.4G 信号不通（距离太远？）', 'packet loss': '2.4G 丢包率过高', 'weak signal': '2.4G 信号太弱',
  '2.4G link lost': '2.4G 链路中断', 'LoRa link down': 'LoRa 链路待机 / 断开', busy: '正在切换，请稍候', timeout: '超时',
};
const fastWhy = (r) => r.split(': ').map((x) => FAST_WHY[x] || (x === 'peer' ? '对端' : x === 'rejected' ? '对端拒绝' : x)).join('：');
let fastState = null;    // last value from AT+ST: null = unknown / not supported
let fastBusy = false;
function showFast(v) {
  fastState = v;
  const b = $('#fast');
  const ble = bridge.kind === 'ble';
  // always usable while connected: when FAST mode is not possible the click explains why
  b.disabled = !bridge.connected || fastBusy;
  b.classList.toggle('on', v === 2);
  b.classList.toggle('neg', v === 1 || fastBusy);
  b.textContent = fastBusy || v === 1 ? '2.4G 协商中…' : v === 2 ? '2.4G 高速：开' : '2.4G 高速';
  b.title = v === null && bridge.connected ? '设备固件不支持高速模式（需要 1.3.0 或更新）'
    : ble ? '通过蓝牙连接时不能使用高速模式（2.4G 射频与蓝牙互斥），请改用 USB 连接'
    : '把数据切换到 2.4GHz 高速链路（AT+FAST），再点一次切回 LoRa';
}
$('#fast').addEventListener('click', async () => {
  if (fastBusy || !bridge.connected) return;
  if (fastState === null || fastState === undefined) {
    // status line without the FAST field: old firmware (or a stale cached script, see index.html)
    let st = null;
    try { st = await bridge.fastStatus(); } catch (e) { /* no answer */ }
    if (!st) { modal('不支持 2.4G 高速模式', ['设备固件不支持高速模式，需要 1.3.0 或更新。'], { error: true }); return; }
    fastState = st.fast;
  }
  if (bridge.kind === 'ble' && fastState !== 2) {
    modal('蓝牙连接下不能使用 2.4G 高速模式', ['2.4G 高速链路与蓝牙共用同一个射频，两端都没有蓝牙连接时才能启用。', '请断开蓝牙，改用 USB 连接本页面后再切换。'], { error: true });
    return;
  }
  const on = fastState !== 2;
  fastBusy = true;
  showFast(fastState);
  try {
    await bridge.setFast(on);
    fastBusy = false;
    showFast(on ? 2 : 0);
  } catch (e) {
    fastBusy = false;
    showFast(0);
    modal(on ? '未能进入 2.4G 高速模式' : '切回 LoRa 失败', [fastWhy(e.message), '数据继续通过 LoRa 传输。'], { error: true });
  }
  poll();
});

// ---- ETH mode (USB network adapter on both ends) switch
let ethState = null;     // last value from AT+ST: null = unknown / firmware without ETH mode
function showEth(v) {
  ethState = v;
  const b = $('#eth');
  b.disabled = !bridge.connected;
  b.classList.toggle('on', v === 2);
  b.classList.toggle('wait', v === 1);
  b.textContent = v === 2 ? '网卡模式：开' : v === 1 ? '网卡模式：等待对端' : '网卡模式';
}
$('#eth').addEventListener('click', async () => {
  if (!bridge.connected) return;
  if (ethState === null || ethState === undefined) {
    modal('不支持网卡模式', ['设备固件不支持网卡模式，需要 1.4.0 或更新。'], { error: true });
    return;
  }
  const on = !ethState;
  try {
    await bridge.setEth(on);
    if (bridge.kind === 'usb') modal(on ? '网卡模式已开启' : '网卡模式已关闭', ['两端的 USB 设备会重新枚举，本页面稍后自动重新连接。'], { busy: true });
    else poll();
  } catch (e) {
    const why = { 'not paired': '尚未配对', 'invalid addresses (AT+ETHCFG)': 'IP 设置无效' }[e.message] || e.message;
    modal('未能切换网卡模式', [why], { error: true });
  }
});

// The USB device left the bus: it re-enumerates when ETH mode is switched (here, on the device
// or by the peer), so look for it again for a few seconds before reporting it as unplugged.
async function usbGone(serial) {
  setConnected(false);
  modal('USB 设备重新连接中', ['设备已从 USB 断开；如果是切换了网卡模式，它会在几秒内回来。'], { busy: true });
  for (let i = 0; i < 20; i++) {
    await new Promise((r) => setTimeout(r, 400));
    let dev = null;
    try { const devs = await navigator.usb.getDevices(); dev = devs.find((d) => d.serialNumber === serial) || null; } catch (e) { /* retry */ }
    if (!dev || busy || bridge.connected) continue;
    try { await bridge.open(dev); setConnected(true); closeModal(); await reloadCfg(); return; } catch (e) { /* not ready yet */ }
  }
  if (!bridge.connected) modal('设备已拔出', ['USB 设备已断开。'], { error: true });
}

// ---- status bar
async function poll() {
  if (!bridge.connected) return;
  try {
    const s = await bridge.status();
    if (!s) return;
    if (!fastBusy) {
      // the device fell back to LoRa by itself: say why (once)
      if (fastState === 2 && s.fast === 0) {
        bridge.fastStatus().then((f) => { if (f && f.reason && !/user/.test(f.reason)) modal('已自动切回 LoRa', [fastWhy(f.reason)], { autoClose: 4000 }); }).catch(() => {});
      }
      showFast(s.fast);
    }
    const m = $('#mode');
    m.textContent = s.mode;
    m.className = 'badge ' + s.mode;
    const role = s.role === 'M' ? '主机' : s.role === 'S' ? '从机' : '';
    const peer = s.peerBatt <= 100 ? ` · 对端 <b>${s.peerBatt}%</b>` : '';
    const fast = s.fast === 2 ? ` · <b>2.4G</b> RSSI <b>${s.fastRssi}</b>` : '';
    showEth(s.eth);
    $('#stext').innerHTML = `${role} · <b>${s.freq.toFixed(3)}</b> MHz ch${s.channel} · RSSI <b>${s.rssi}</b> SNR <b>${s.snr}</b>${fast} · TX <b>${s.txRate}</b> RX <b>${s.rxRate}</b> B/s · Q <b>${s.queue}</b> · 电池 <b>${s.batt}%</b>${s.usb ? ' USB' : ''}${peer}`;
  } catch (e) { /* next round */ }
}

function setConnected(on) {
  const usbBtn = $('#connect'), bleBtn = $('#connectble');
  // while connected, the button of the active transport becomes "disconnect"
  usbBtn.textContent = on && bridge.kind === 'usb' ? '断开 USB' : 'USB 连接';
  bleBtn.textContent = on && bridge.kind === 'ble' ? '断开蓝牙' : '蓝牙连接';
  usbBtn.classList.toggle('primary', !on);
  bleBtn.classList.toggle('primary', !on);
  usbBtn.disabled = busy || !navigator.usb || (on && bridge.kind !== 'usb');
  bleBtn.disabled = busy || !navigator.bluetooth || (on && bridge.kind !== 'ble');
  clearInterval(pollTimer);
  if (on) { pollTimer = setInterval(poll, 2000); poll(); }
  else { $('#mode').textContent = '----'; $('#mode').className = 'badge'; $('#stext').textContent = '未连接'; fastBusy = false; }
  showFast(on ? fastState : null);
  showEth(on ? ethState : null);
}

async function connect(dev) {
  await bridge.open(dev);
  setConnected(true);
  modal('已连接', [`USB：${dev.productName || ''} ${dev.serialNumber || ''}`], { autoClose: 1200 });
  await reloadCfg();
}

async function connectBle(dev) {
  let cancelled = false;
  const onCancel = () => { cancelled = true; bridge.abortBle(); bridge.close(); };
  modal('正在连接蓝牙', [`设备：${dev.name || ''}`], { busy: true, onCancel });
  modalLine('无需在系统里配对。新的电脑 / 手机第一次连接时，设备屏幕会显示 ALLOW?，请在 60 秒内短按设备上的按钮。', true);
  bridge.onRetry = (n, e) => modalLine(`第 ${n} 次尝试未成功（${e.message}），重试…`, true);
  await bridge.openBle(dev);
  // the link is up, but the device answers nothing until the button was pressed
  let ok = false;
  for (let i = 0; i < 45 && bridge.connected && !ok && !cancelled; i++) {
    try { await bridge.cmd('AT', 1500); ok = true; }
    catch (e) { if (i === 1 && !cancelled) modalLine('已连接，等待在设备上短按按钮允许（屏幕显示 ALLOW?）…'); }
  }
  if (cancelled) { const e = new Error('cancelled'); e.name = 'NotFoundError'; throw e; }
  if (!ok) {
    await bridge.close();
    throw new Error('设备没有允许这次连接（未按按钮或已拒绝）');
  }
  setConnected(true);
  modal('已连接', [`蓝牙：${dev.name || ''}`], { autoClose: 1200 });
  await reloadCfg();
}
// ---- wiring
function showPane(id) {
  document.querySelectorAll('.tabs button').forEach((b) => b.classList.toggle('on', b.dataset.pane === id));
  document.querySelectorAll('.pane').forEach((p) => p.classList.toggle('on', p.id === id));
}
document.querySelectorAll('.tabs button').forEach((b) => b.addEventListener('click', () => showPane(b.dataset.pane)));

function normalise(line) {
  const u = line.trim();
  if (/^at/i.test(u)) return u;
  const m = u.match(/^(\S+?)[\s=]+(.*)$/);
  return m ? `AT+${m[1].toUpperCase()}=${m[2].trim()}` : `AT+${u.toUpperCase()}`;
}
async function runAt(line) {
  try { log(`${line}\n${await bridge.cmd(line)}`); } catch (e) { log(`${line}\n${e.message}`); }
}
document.querySelectorAll('button[data-at]').forEach((b) => b.addEventListener('click', () => {
  if (b.classList.contains('danger') && !confirm(`确定执行 ${b.textContent}？`)) return;
  runAt(b.dataset.at);
}));
$('#atsend').addEventListener('click', () => { const v = $('#atline').value; if (v.trim()) runAt(normalise(v)); });
$('#atline').addEventListener('keydown', (e) => { if (e.key === 'Enter') $('#atsend').click(); });
$('#reload').addEventListener('click', reloadCfg);
$('#apply').addEventListener('click', applyCfg);

if (!navigator.usb && !navigator.bluetooth) $('#nousb').style.display = 'block';
setConnected(false);

if (navigator.bluetooth) {
  $('#connectble').addEventListener('click', async () => {
    if (busy) return;
    busy = true;
    setConnected(bridge.connected);
    try {
      if (bridge.connected) { await bridge.close(); setConnected(false); modal('已断开', [], { autoClose: 900 }); return; }
      const dev = await navigator.bluetooth.requestDevice(LoraBridge.bleOptions);
      await connectBle(dev);
    } catch (e) {
      setConnected(bridge.connected);
      busy = false;
      setConnected(bridge.connected);
      if (e.name === 'NotFoundError' || e.message === 'cancelled') closeModal();
      else modal('蓝牙连接失败', [e.message, '请确认设备固件为 1.2.1 或更新；如果系统蓝牙设置里留有这台设备的旧配对记录，请先删除后再连接。'], { error: true });
    } finally {
      busy = false;
      setConnected(bridge.connected);
    }
  });
}

if (navigator.usb) {
  $('#connect').addEventListener('click', async () => {
    if (busy) return;
    busy = true;
    setConnected(bridge.connected);
    try {
      if (bridge.connected) { await bridge.close(); setConnected(false); modal('已断开', [], { autoClose: 900 }); return; }
      const dev = await navigator.usb.requestDevice({ filters: LoraBridge.filters });
      await connect(dev);
    } catch (e) {
      setConnected(bridge.connected);
      if (e.name !== 'NotFoundError') modal('USB 连接失败', [e.message], { error: true });
    } finally {
      busy = false;
      setConnected(bridge.connected);
    }
  });
  navigator.usb.addEventListener('disconnect', (e) => { if (bridge.dev === e.device) { const serial = e.device.serialNumber; bridge.running = false; bridge.dev = null; bridge.kind = null; usbGone(serial); } });
  // reconnect to a device that was authorised before
  navigator.usb.getDevices().then((devs) => { if (devs.length === 1) connect(devs[0]).catch(() => {}); });
}
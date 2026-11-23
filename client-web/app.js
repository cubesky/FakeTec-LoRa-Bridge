'use strict';
const $ = (s) => document.querySelector(s);
const bridge = new LoraBridge();
let pollTimer = null;
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
bridge.onClose = () => { setConnected(false); note('设备已断开'); };

function note(msg) { term.write(`\r\n\x1b[30;46m[bridge]\x1b[0m ${msg}\r\n`); }
function log(text) { const l = $('#log'); l.textContent = text; }

// ---- settings
const LABELS = {
  FREQ: '基础频率 MHz', BW: '带宽 kHz', SF: '扩频因子', CR: '编码率 4/x', PWR: '发射功率 dBm', SYNC: '同步字 (hex)',
  PRE: '前导码符号数', TCXO: 'TCXO 电压 (0=晶振)', HOP: '跳频', HOPCH: '跳频信道数', HOPSP: '信道间隔 kHz',
  DWELL: '驻留时间 ms (0=自动)', AFH: '自适应跳频', LBT: '先听后发', LBTRSSI: 'LBT RSSI 门限 (0=关)',
  PEER: '对端 ID', ENC: '加密', ACK: '确认重传', RETRY: '重传次数', MAXPL: '每帧最大字节', PKTTO: '组包超时 ms',
  HB: '心跳 s', COMP: '压缩', PSM: '省电待机', SBHB: '待机心跳 s', USBACT: 'USB 激活 (1=插入 0=打开)',
  BLE: '蓝牙', NAME: '蓝牙名称', BLEPIN: '蓝牙固定配对码 (0=关)', BLEPAIR: '蓝牙按钮配对',
  OLED: '息屏时间 s (0=常亮)', OLEDTYPE: '屏幕 (0=SSD1306 1=SH1106)', FLIP: '屏幕翻转', BATCAL: '电池校准 x1000', OUT: '输出位',
};
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
  for (const i of changed) {
    const line = `AT+${i.dataset.key}=${i.value.trim()}`;
    try { lines.push(`${line}\n${await bridge.cmd(line)}`); } catch (e) { lines.push(`${line}\n${e.message}`); }
  }
  log(lines.join('\n'));
  showPane('ctl');
  await reloadCfg();
}

// ---- status bar
async function poll() {
  if (!bridge.connected) return;
  try {
    const s = await bridge.status();
    if (!s) return;
    const m = $('#mode');
    m.textContent = s.mode;
    m.className = 'badge ' + s.mode;
    const role = s.role === 'M' ? '主机' : s.role === 'S' ? '从机' : '';
    const peer = s.peerBatt <= 100 ? ` · 对端 <b>${s.peerBatt}%</b>` : '';
    $('#stext').innerHTML = `${role} · <b>${s.freq.toFixed(3)}</b> MHz ch${s.channel} · RSSI <b>${s.rssi}</b> SNR <b>${s.snr}</b> · TX <b>${s.txRate}</b> RX <b>${s.rxRate}</b> B/s · Q <b>${s.queue}</b> · 电池 <b>${s.batt}%</b>${s.usb ? ' USB' : ''}${peer}`;
  } catch (e) { /* next round */ }
}

function setConnected(on) {
  $('#connect').textContent = on ? '断开' : '连接设备';
  $('#connect').classList.toggle('primary', !on);
  clearInterval(pollTimer);
  if (on) { pollTimer = setInterval(poll, 2000); poll(); }
  else { $('#mode').textContent = '----'; $('#mode').className = 'badge'; $('#stext').textContent = '未连接'; }
}

async function connect(dev) {
  await bridge.open(dev);
  setConnected(true);
  note(`已连接 ${dev.productName || ''} ${dev.serialNumber || ''}`);
  await reloadCfg();
  term.focus();
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

if (!navigator.usb) {
  $('#nousb').style.display = 'block';
  $('#connect').disabled = true;
} else {
  $('#connect').addEventListener('click', async () => {
    try {
      if (bridge.connected) { await bridge.close(); setConnected(false); note('已断开'); return; }
      const dev = await navigator.usb.requestDevice({ filters: LoraBridge.filters });
      await connect(dev);
    } catch (e) {
      if (e.name !== 'NotFoundError') note(`\x1b[31m${e.message}\x1b[0m`);
    }
  });
  navigator.usb.addEventListener('disconnect', (e) => { if (bridge.dev === e.device) { bridge.running = false; bridge.dev = null; setConnected(false); note('设备已拔出'); } });
  // reconnect to a device that was authorised before
  navigator.usb.getDevices().then((devs) => { if (devs.length === 1) connect(devs[0]).catch(() => {}); });
}
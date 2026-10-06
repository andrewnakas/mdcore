// mdcore - the demo player page.
// Copyright (c) 2026 mdcore contributors. MIT licence, see LICENSE.
import { MDCore } from './mdcore.js';

const $ = (id) => document.getElementById(id);
const status = (s) => { $('status').textContent = s; };
const md = await MDCore.create($('screen'), {
  wasmUrl: new URL('mdcore.wasm', import.meta.url),
  onSramChange: (data) => saveSram(data),
});
window.mdcore = md; // for tests and tooling

let romKey = null, state = null;

async function sha1(bytes) {
  const h = new Uint8Array(await crypto.subtle.digest('SHA-1', bytes));
  return [...h].map((b) => b.toString(16).padStart(2, '0')).join('');
}

function header(rom) {
  const s = (a, n) => String.fromCharCode(...rom.subarray(a, a + n)).replace(/\s+/g, ' ').trim();
  return { name: s(0x150, 48), serial: s(0x180, 14), region: s(0x1F0, 3) };
}

function saveSram(data) {
  if (!romKey || !data) return;
  try { localStorage.setItem('mdcore-sram-' + romKey, btoa(String.fromCharCode(...data))); } catch (_) { /* storage unavailable */ }
}

function restoreSram() {
  try {
    const v = localStorage.getItem('mdcore-sram-' + romKey);
    if (v) md.setSram(Uint8Array.from(atob(v), (c) => c.charCodeAt(0)));
    return !!v;
  } catch (_) { return false; }
}

async function open(file) {
  if (!file) return;
  const rom = new Uint8Array(await file.arrayBuffer());
  md.stop();
  try {
    md.load(rom);
  } catch (e) {
    status(e.message);
    return;
  }
  const digest = await sha1(rom);
  romKey = digest;
  const h = header(rom);
  const hadSave = md.sramSize() && restoreSram();
  $('info').textContent = `${file.name} · ${rom.length} bytes · sha1 ${digest} · ${h.name} · ${h.serial} · ${h.region}` +
    (md.sramSize() ? ` · battery RAM ${md.sramSize()} bytes${hadSave ? ' (restored)' : ''}` : '');
  $('picker').hidden = true;
  for (const id of ['pause', 'reset', 'save']) $(id).disabled = false;
  $('load').disabled = !state;
  md.start();
  status('running');
}

$('file').addEventListener('change', (e) => open(e.target.files[0]));
$('file2').addEventListener('change', (e) => open(e.target.files[0]));
$('pause').addEventListener('click', () => {
  if (md.running) { md.stop(); $('pause').textContent = 'Resume'; status('paused'); }
  else { md.start(); $('pause').textContent = 'Pause'; status('running'); }
});
$('reset').addEventListener('click', () => md.reset());
$('save').addEventListener('click', () => { state = md.saveState(); $('load').disabled = false; status('state saved'); });
$('load').addEventListener('click', () => status(state && md.loadState(state) ? 'state loaded' : 'no state'));
$('sound').addEventListener('click', async () => {
  if (md.audio) { await md.disableAudio(); $('sound').textContent = 'Sound: off'; }
  else { await md.enableAudio(0.8); $('sound').textContent = 'Sound: on'; }
});
document.addEventListener('visibilitychange', () => { if (document.hidden && romKey && md.sramSize()) saveSram(md.getSram()); });

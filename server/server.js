// FlipBoard Socket.IO server — multi-tenant
//
// Each board authenticates with its API key after connecting.
// Boards sharing the same key are grouped into a room and receive
// broadcasts together. The dashboard and REST API require the same
// key via the X-Api-Key header, so each operator only sees and
// controls their own boards.
//
// Boards push a 'state' event after each content update so the
// dashboard can show a live preview of what is on each screen.
//
// Usage:
//   npm install
//   node server.js
//
// Access the dashboard at http://localhost:3500

const express    = require('express');
const http       = require('http');
const { Server } = require('socket.io');

const app    = express();
const server = http.createServer(app);
const io     = new Server(server, { cors: { origin: '*' } });

const PORT = process.env.PORT || 3500;

// ── Board registry ────────────────────────────────────────────────────────────
// boards: socketId → { id, ip, key, connectedAt, rows[], leds{} }
const boards = new Map();

function room(key)         { return `key:${key}`; }
function boardsForKey(key) { return [...boards.values()].filter(b => b.key === key); }

const emptyState = () => ({
    rows: ['', '', '', '', '', ''],
    demo: false,
    leds: {
        led1: { mode: 'off', brightness: 100, notify: false },
        led2: { mode: 'off', brightness: 100, notify: false },
    },
});

// ── Socket.IO connection handling ─────────────────────────────────────────────

io.on('connection', (socket) => {
    const ip = socket.handshake.address;
    boards.set(socket.id, { id: socket.id, ip, key: null,
        connectedAt: new Date().toISOString(), ...emptyState() });

    socket.on('auth', ({ key } = {}) => {
        if (!key) return;
        const board = boards.get(socket.id);
        if (!board) return;
        board.key = key;
        socket.join(room(key));
        console.log(`[+] board authed  id=${socket.id}  ip=${ip}  key=${key.slice(0,8)}…`);
    });

    // Boards push their current display + LED state after each update.
    socket.on('state', (data) => {
        const board = boards.get(socket.id);
        if (!board || !board.key) return;
        if (Array.isArray(data.rows))       board.rows = data.rows.slice(0, 6).map(String);
        if (typeof data.demo === 'boolean') board.demo = data.demo;
        if (data.leds)                      board.leds = data.leds;
    });

    socket.on('disconnect', () => {
        const board = boards.get(socket.id);
        const key   = board?.key ?? '(unauthed)';
        boards.delete(socket.id);
        console.log(`[-] board disconnected  id=${socket.id}  key=${String(key).slice(0,8)}…`);
    });
});

// ── Helpers ───────────────────────────────────────────────────────────────────

function requireKey(req, res) {
    const key = req.headers['x-api-key'];
    if (!key) { res.status(401).json({ error: 'X-Api-Key header required' }); return null; }
    return key;
}

function emit(key, event, data, targetId = null) {
    if (targetId) {
        io.to(targetId).emit(event, data);
        console.log(`[>] ${event} → target=${targetId.slice(0,8)}…`);
    } else {
        io.to(room(key)).emit(event, data);
        const count = boardsForKey(key).length;
        console.log(`[>] ${event} → key=${key.slice(0,8)}… (${count} board${count!==1?'s':''})`);
    }
}

// ── REST API ──────────────────────────────────────────────────────────────────

app.use(express.json());
app.use(express.urlencoded({ extended: true }));

// GET /api/boards  →  boards connected under the caller's key (includes state)
app.get('/api/boards', (req, res) => {
    const key = requireKey(req, res); if (!key) return;
    const list = boardsForKey(key);
    res.json({ count: list.length, boards: list.map(({ id, ip, connectedAt, rows, demo, leds }) =>
        ({ id, ip, connectedAt, rows, demo, leds })) });
});

// POST /api/cmd  { event, data, target? }
// General targeted command — use target: socketId to address one board,
// omit target to broadcast to all boards under your key.
app.post('/api/cmd', (req, res) => {
    const key = requireKey(req, res); if (!key) return;
    const { event, data, target } = req.body;
    if (!event) return res.status(400).json({ error: 'event required' });
    if (target) {
        const board = boards.get(target);
        if (!board || board.key !== key)
            return res.status(404).json({ error: 'board not found under this key' });
    }
    emit(key, event, data || {}, target || null);
    res.json({ ok: true });
});

// POST /api/row   { row, text }
app.post('/api/row', (req, res) => {
    const key = requireKey(req, res); if (!key) return;
    const { row, text } = req.body;
    if (row === undefined || text === undefined)
        return res.status(400).json({ error: 'row and text required' });
    emit(key, 'set_row', { row: parseInt(row), text: String(text) });
    res.json({ ok: true });
});

// POST /api/rows  { rows: [...] }
app.post('/api/rows', (req, res) => {
    const key = requireKey(req, res); if (!key) return;
    const { rows } = req.body;
    if (!Array.isArray(rows)) return res.status(400).json({ error: 'rows array required' });
    emit(key, 'set_all', { rows: rows.slice(0, 6).map(String) });
    res.json({ ok: true });
});

// POST /api/clear  { row }
app.post('/api/clear', (req, res) => {
    const key = requireKey(req, res); if (!key) return;
    const { row } = req.body;
    if (row === undefined) return res.status(400).json({ error: 'row required' });
    emit(key, 'clear_row', { row: parseInt(row) });
    res.json({ ok: true });
});

// POST /api/wake
app.post('/api/wake', (req, res) => {
    const key = requireKey(req, res); if (!key) return;
    emit(key, 'wake', {});
    res.json({ ok: true });
});

// POST /api/demo  { mode: "on"|"off" }
app.post('/api/demo', (req, res) => {
    const key = requireKey(req, res); if (!key) return;
    const { mode } = req.body;
    if (mode !== 'on' && mode !== 'off')
        return res.status(400).json({ error: 'mode must be on or off' });
    emit(key, 'demo', { mode });
    res.json({ ok: true });
});

// POST /api/timeout  { minutes }
app.post('/api/timeout', (req, res) => {
    const key = requireKey(req, res); if (!key) return;
    const { minutes } = req.body;
    if (minutes === undefined) return res.status(400).json({ error: 'minutes required' });
    emit(key, 'timeout', { minutes: parseInt(minutes) });
    res.json({ ok: true });
});

// POST /api/led/mode  { led: 1|2, mode: "on"|"off"|"flash"|"pulse" }
app.post('/api/led/mode', (req, res) => {
    const key = requireKey(req, res); if (!key) return;
    const { led, mode } = req.body;
    if (!led || !['on','off','flash','pulse'].includes(mode))
        return res.status(400).json({ error: 'led (1|2) and mode (on|off|flash|pulse) required' });
    emit(key, 'led_mode', { led: parseInt(led), mode });
    res.json({ ok: true });
});

// POST /api/led/brightness  { led: 1|2, percent: 0-100 }
app.post('/api/led/brightness', (req, res) => {
    const key = requireKey(req, res); if (!key) return;
    const { led, percent } = req.body;
    if (!led || percent === undefined) return res.status(400).json({ error: 'led and percent required' });
    const pct = parseInt(percent);
    if (isNaN(pct) || pct < 0 || pct > 100)
        return res.status(400).json({ error: 'percent must be 0-100' });
    emit(key, 'led_brightness', { led: parseInt(led), percent: pct });
    res.json({ ok: true });
});

// POST /api/brightness  { percent }
app.post('/api/brightness', (req, res) => {
    const key = requireKey(req, res); if (!key) return;
    const { percent } = req.body;
    if (percent === undefined) return res.status(400).json({ error: 'percent required' });
    const pct = parseInt(percent);
    if (isNaN(pct) || pct < 0 || pct > 100)
        return res.status(400).json({ error: 'percent must be 0-100' });
    emit(key, 'brightness', { percent: pct });
    res.json({ ok: true });
});

// ── Web dashboard ─────────────────────────────────────────────────────────────

app.get('/', (req, res) => res.send(`<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FlipBoard Server</title>
<style>
*{box-sizing:border-box}
body{background:#0e0e0e;color:#ffaa00;font-family:monospace;margin:0 auto;padding:16px;max-width:600px;text-align:center}
h1{font-size:1.1em;letter-spacing:.35em;border-bottom:1px solid #ffaa00;padding-bottom:8px;margin:0 0 14px;text-align:center}
h2{font-size:.85em;letter-spacing:.2em;margin:20px 0 8px;border-left:2px solid #ffaa00;padding-left:8px;text-align:left}
.field{margin-bottom:10px;text-align:left}
label{display:block;font-size:.72em;color:#cc8800;margin-bottom:3px}
input[type=text]{width:100%;background:#1a1a1a;color:#ffaa00;border:1px solid #444;padding:6px 8px;font-family:monospace;font-size:1em}
input[type=text]:focus{outline:none;border-color:#ffaa00}
.row-input{text-transform:uppercase}
button{font-family:monospace;font-size:.85em;padding:9px 18px;cursor:pointer;letter-spacing:.1em;margin-top:6px;margin-right:6px}
.primary{background:#ffaa00;color:#0e0e0e;border:none}
.danger{background:#880000;color:#fff;border:none}
#msg{margin-top:10px;font-size:.82em;min-height:1.1em;text-align:left}
hr{border:none;border-top:1px solid #2a2a2a;margin:18px 0}
/* tabs */
.tab-bar{display:flex;gap:4px;flex-wrap:wrap;align-items:flex-end}
.tab-btn{font-family:monospace;font-size:.78em;padding:6px 12px;cursor:pointer;letter-spacing:.1em;
  border:1px solid #444;border-bottom:none;background:#1a1a1a;color:#888;position:relative;top:1px}
.tab-btn.active{background:#ffaa00;color:#0e0e0e;border-color:#ffaa00}
.tab-x{margin-left:6px;opacity:.5;font-size:.9em}
.tab-x:hover{opacity:1}
.tab-add{font-family:monospace;font-size:.85em;padding:4px 10px;cursor:pointer;
  border:1px dashed #444;background:transparent;color:#666;margin-left:4px;align-self:flex-end;margin-bottom:1px}
.tab-add:hover{color:#ffaa00;border-color:#ffaa00}
.tab-content{border:1px solid #ffaa00;padding:14px;margin-bottom:18px;background:#0e0e0e}
.tab-meta{display:flex;gap:8px;margin-bottom:10px}
.tab-meta .field{flex:1;margin:0}
/* board strip */
.boards-strip{font-size:.72em;color:#666;margin-bottom:10px;text-align:left;min-height:1.2em}
/* OLED preview */
.oled-wrap{background:#080808;border:1px solid #333;padding:10px 12px;margin:6px 0 10px;border-radius:2px}
.oled-row{color:#ffaa00;letter-spacing:.06em;font-size:.82em;line-height:1.65em;
  border-bottom:1px dotted #1a1a1a;white-space:pre;min-height:1.65em;text-align:left}
.oled-row:last-child{border-bottom:none}
/* LED pills */
.led-pills{margin:8px 0 4px;font-size:.72em;text-align:left}
.led-pill{display:inline-block;padding:3px 10px;border:1px solid #444;margin:0 6px 0 0;letter-spacing:.08em}
.led-off{color:#444;border-color:#333}
.led-on{color:#ffaa00;border-color:#ffaa00}
.led-flash{color:#ff8800;border-color:#ff8800}
.led-pulse{color:#44ffcc;border-color:#44ffcc}
.notify-dot{color:#ff4444;margin-left:4px}
</style>
</head>
<body>
<h1>&#9646; FLIPBOARD SERVER</h1>

<div style="display:flex;align-items:flex-end;gap:0;margin-bottom:0">
  <div class="tab-bar" id="groupBar"></div>
  <button class="tab-add" onclick="addGroup()" title="Add board group">+</button>
</div>

<div class="tab-content" id="tabContent">
  <div class="tab-meta">
    <div class="field">
      <label>GROUP NAME</label>
      <input type="text" id="tabLabel" placeholder="Living Room" oninput="saveGroups()">
    </div>
    <div class="field">
      <label>API KEY</label>
      <input type="text" id="tabKey" placeholder="paste api key" autocomplete="off" oninput="saveGroups();refreshBoards()">
    </div>
  </div>

  <div class="boards-strip" id="boardsStrip">No boards connected.</div>

  <div id="preview" style="display:none">
    <h2>BOARD STATE</h2>
    <div class="oled-wrap">
      <div class="oled-row" id="or0">&nbsp;</div>
      <div class="oled-row" id="or1">&nbsp;</div>
      <div class="oled-row" id="or2">&nbsp;</div>
      <div class="oled-row" id="or3">&nbsp;</div>
      <div class="oled-row" id="or4">&nbsp;</div>
      <div class="oled-row" id="or5">&nbsp;</div>
    </div>
    <div class="led-pills" id="ledPills"></div>
  </div>

  <hr>
  <h2>ALL ROWS</h2>
  <form id="allForm">
    <div class="field"><label>ROW 0</label><input class="row-input" type="text" name="r0" maxlength="21" autocomplete="off"></div>
    <div class="field"><label>ROW 1</label><input class="row-input" type="text" name="r1" maxlength="21" autocomplete="off"></div>
    <div class="field"><label>ROW 2</label><input class="row-input" type="text" name="r2" maxlength="21" autocomplete="off"></div>
    <div class="field"><label>ROW 3</label><input class="row-input" type="text" name="r3" maxlength="21" autocomplete="off"></div>
    <div class="field"><label>ROW 4</label><input class="row-input" type="text" name="r4" maxlength="21" autocomplete="off"></div>
    <div class="field"><label>ROW 5</label><input class="row-input" type="text" name="r5" maxlength="21" autocomplete="off"></div>
    <button type="submit" class="primary">SEND ALL ROWS</button>
  </form>

  <hr>
  <h2>BRIGHTNESS</h2>
  <div class="field">
    <label>DISPLAY BRIGHTNESS &mdash; <span id="brightVal">78</span>%</label>
    <input type="range" id="bright" min="0" max="100" value="78"
      style="width:100%;accent-color:#ffaa00"
      oninput="document.getElementById('brightVal').textContent=this.value"
      onchange="cmd('brightness',{percent:parseInt(this.value)})">
  </div>

  <hr>
  <h2>LED INDICATORS</h2>
  <div style="margin-bottom:14px;text-align:left">
    <label style="display:block;font-size:.72em;color:#cc8800;margin-bottom:5px">LED 1</label>
    <div style="display:flex;gap:6px;flex-wrap:wrap;margin-bottom:6px">
      <button class="primary" onclick="cmd('led_mode',{led:1,mode:'off'})">OFF</button>
      <button class="primary" onclick="cmd('led_mode',{led:1,mode:'on'})">ON</button>
      <button class="primary" onclick="cmd('led_mode',{led:1,mode:'flash'})">FLASH</button>
      <button class="primary" onclick="cmd('led_mode',{led:1,mode:'pulse'})">PULSE</button>
    </div>
    <label style="display:block;font-size:.72em;color:#cc8800;margin-bottom:3px">BRIGHTNESS &mdash; <span id="ls1">100</span>%</label>
    <input type="range" id="lb1" min="0" max="100" value="100"
      style="width:100%;accent-color:#ffaa00"
      oninput="document.getElementById('ls1').textContent=this.value"
      onchange="cmd('led_brightness',{led:1,percent:parseInt(this.value)})">
  </div>
  <div style="margin-bottom:14px;text-align:left">
    <label style="display:block;font-size:.72em;color:#cc8800;margin-bottom:5px">LED 2</label>
    <div style="display:flex;gap:6px;flex-wrap:wrap;margin-bottom:6px">
      <button class="primary" onclick="cmd('led_mode',{led:2,mode:'off'})">OFF</button>
      <button class="primary" onclick="cmd('led_mode',{led:2,mode:'on'})">ON</button>
      <button class="primary" onclick="cmd('led_mode',{led:2,mode:'flash'})">FLASH</button>
      <button class="primary" onclick="cmd('led_mode',{led:2,mode:'pulse'})">PULSE</button>
    </div>
    <label style="display:block;font-size:.72em;color:#cc8800;margin-bottom:3px">BRIGHTNESS &mdash; <span id="ls2">100</span>%</label>
    <input type="range" id="lb2" min="0" max="100" value="100"
      style="width:100%;accent-color:#ffaa00"
      oninput="document.getElementById('ls2').textContent=this.value"
      onchange="cmd('led_brightness',{led:2,percent:parseInt(this.value)})">
  </div>

  <hr>
  <h2>CONTROLS</h2>
  <div style="text-align:left">
    <button class="primary" onclick="cmd('wake',{})">WAKE + REPLAY</button>
    <button class="primary" id="demoBtn" onclick="toggleDemo()">START DEMO</button>
    <button class="danger"  onclick="cmd('timeout',{minutes:0})">NEVER OFF</button>
  </div>

  <div id="msg"></div>
</div>

<script>
// Each group = { id, label, key }  stored in localStorage as 'fb_groups'.
const msg = document.getElementById('msg');
let groups = [];
let activeId = null;
let currentBoards = [];

function uid() { return Math.random().toString(36).slice(2, 10); }
function activeGroup() { return groups.find(g => g.id === activeId); }
function currentKey() { return (activeGroup() || {}).key || ''; }

function loadGroups() {
  try { groups = JSON.parse(localStorage.getItem('fb_groups') || '[]'); } catch(e) { groups = []; }
  if (!groups.length) groups = [{ id: uid(), label: 'Board 1', key: '' }];
  activeId = localStorage.getItem('fb_active') || groups[0].id;
  if (!groups.find(g => g.id === activeId)) activeId = groups[0].id;
}

function saveGroups() {
  const g = activeGroup();
  if (g) {
    g.label = document.getElementById('tabLabel').value;
    g.key   = document.getElementById('tabKey').value.trim();
  }
  localStorage.setItem('fb_groups', JSON.stringify(groups));
}

function renderGroupBar() {
  const bar = document.getElementById('groupBar');
  bar.innerHTML = '';
  groups.forEach(g => {
    const btn = document.createElement('button');
    btn.className = 'tab-btn' + (g.id === activeId ? ' active' : '');
    btn.appendChild(document.createTextNode(g.label || 'Unnamed'));
    if (groups.length > 1) {
      const x = document.createElement('span');
      x.className = 'tab-x';
      x.textContent = '×';
      x.onclick = (e) => { e.stopPropagation(); removeGroup(g.id); };
      btn.appendChild(x);
    }
    btn.onclick = () => selectGroup(g.id);
    bar.appendChild(btn);
  });
}

function selectGroup(id) {
  activeId = id;
  localStorage.setItem('fb_active', id);
  const g = activeGroup();
  document.getElementById('tabLabel').value = g ? g.label : '';
  document.getElementById('tabKey').value   = g ? g.key   : '';
  renderGroupBar();
  currentBoards = [];
  document.getElementById('boardsStrip').textContent = 'No boards connected.';
  renderPreview(null);
  syncDemoBtn(false);
  refreshBoards();
}

function addGroup() {
  const g = { id: uid(), label: 'Board ' + (groups.length + 1), key: '' };
  groups.push(g);
  saveGroups();
  selectGroup(g.id);
}

function removeGroup(id) {
  groups = groups.filter(g => g.id !== id);
  saveGroups();
  if (activeId === id) activeId = groups[0].id;
  selectGroup(activeId);
}

async function cmd(event, data) {
  const k = currentKey();
  if (!k) { msg.textContent = 'Enter your API key in the tab above.'; return; }
  try {
    const r = await fetch('/api/cmd', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-Api-Key': k },
      body: JSON.stringify({ event, data: data || {} })
    });
    const j = await r.json();
    msg.textContent = r.ok ? 'OK' : 'Error: ' + JSON.stringify(j);
    // Refresh board state ~800 ms after the command so the ESP has time to
    // process it and push its updated state back to the server.
    if (r.ok) setTimeout(refreshBoards, 800);
  } catch(e) { msg.textContent = 'Failed: ' + e; }
}

function renderPreview(board) {
  document.getElementById('preview').style.display = board ? 'block' : 'none';
  if (!board) return;
  const rows = board.rows || Array(6).fill('');
  for (let i = 0; i < 6; i++) {
    const el = document.getElementById('or' + i);
    el.textContent = (rows[i] || '').trimEnd() || ' ';
  }
  const leds = board.leds || {};
  const led1 = leds.led1 || { mode: 'off', brightness: 100, notify: false };
  const led2 = leds.led2 || { mode: 'off', brightness: 100, notify: false };
  document.getElementById('ledPills').innerHTML = makePill('LED 1', led1) + makePill('LED 2', led2);
}

function makePill(label, l) {
  const mode = (l.mode || 'off').toLowerCase();
  const dot  = l.notify ? '<span class="notify-dot" title="Notify enabled">&#9679;</span>' : '';
  return '<span class="led-pill led-' + mode + '">' + label + ' &#9679; ' +
         mode.toUpperCase() + ' ' + (l.brightness || 0) + '%' + dot + '</span>';
}

document.getElementById('allForm').onsubmit = async e => {
  e.preventDefault();
  const rows = [0,1,2,3,4,5].map(i => (e.target['r'+i].value || '').toUpperCase());
  await cmd('set_all', { rows });
};

function syncDemoBtn(on) {
  const btn = document.getElementById('demoBtn');
  btn.textContent      = on ? 'STOP DEMO' : 'START DEMO';
  btn.style.background = on ? '#cc4400' : '';
}

async function toggleDemo() {
  // Read current state from the first connected board; fall back to button label.
  const board = currentBoards[0];
  const currently = board ? !!board.demo : document.getElementById('demoBtn').textContent === 'STOP DEMO';
  await cmd('demo', { mode: currently ? 'off' : 'on' });
  // Optimistic update — the next poll will correct it if needed.
  syncDemoBtn(!currently);
}

async function refreshBoards() {
  const k = currentKey();
  if (!k) { document.getElementById('boardsStrip').textContent = 'No API key set.'; return; }
  try {
    const r = await fetch('/api/boards', { headers: { 'X-Api-Key': k } });
    if (!r.ok) return;
    const j = await r.json();
    currentBoards = j.boards || [];
    const strip = document.getElementById('boardsStrip');
    if (!currentBoards.length) {
      strip.textContent = 'No boards connected.';
      renderPreview(null);
    } else {
      strip.textContent = currentBoards.map(b => b.ip || b.id.slice(0,8)).join('  ·  ') +
        '  (' + currentBoards.length + ' board' + (currentBoards.length !== 1 ? 's' : '') + ')';
      renderPreview(currentBoards[0]);
      syncDemoBtn(currentBoards[0].demo);
    }
  } catch(e) {}
}

loadGroups();
renderGroupBar();
selectGroup(activeId);
setInterval(refreshBoards, 2000);
</script>
</body>
</html>`));

// ── Start ─────────────────────────────────────────────────────────────────────

server.listen(PORT, () => {
    console.log(`FlipBoard server running on http://localhost:${PORT}`);
    console.log(`Boards authenticate with their API key — each key is a separate tenant.`);
});

// FlipBoard Socket.IO server — multi-tenant
//
// Each board authenticates with its API key after connecting.
// Boards sharing the same key are grouped into a room and receive
// broadcasts together. The dashboard and REST API require the same
// key via the X-Api-Key header, so each operator only sees and
// controls their own boards.
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
// boards: socketId → { ip, key, connectedAt }
const boards = new Map();

function room(key)          { return `key:${key}`; }
function boardsForKey(key)  { return [...boards.values()].filter(b => b.key === key); }

// ── Socket.IO connection handling ─────────────────────────────────────────────

io.on('connection', (socket) => {
    const ip = socket.handshake.address;
    // Board is unauthenticated until it sends the auth event.
    boards.set(socket.id, { ip, key: null, connectedAt: new Date().toISOString() });

    socket.on('auth', ({ key } = {}) => {
        if (!key) return;
        const board = boards.get(socket.id);
        if (!board) return;
        board.key = key;
        socket.join(room(key));
        console.log(`[+] board authed  id=${socket.id}  ip=${ip}  key=${key.slice(0,8)}…`);
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

function emit(key, event, data) {
    io.to(room(key)).emit(event, data);
    const count = boardsForKey(key).length;
    console.log(`[>] ${event} → key=${key.slice(0,8)}… (${count} board${count!==1?'s':''})`);
}

// ── REST API ──────────────────────────────────────────────────────────────────

app.use(express.json());
app.use(express.urlencoded({ extended: true }));

// GET /api/boards  →  boards connected under the caller's key
app.get('/api/boards', (req, res) => {
    const key = requireKey(req, res); if (!key) return;
    const list = boardsForKey(key);
    res.json({ count: list.length, boards: list.map(({ ip, connectedAt }) => ({ ip, connectedAt })) });
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
body{background:#0e0e0e;color:#ffaa00;font-family:monospace;margin:0;padding:16px;max-width:540px}
h1{font-size:1.1em;letter-spacing:.35em;border-bottom:1px solid #ffaa00;padding-bottom:8px;margin:0 0 14px}
h2{font-size:.85em;letter-spacing:.2em;margin:20px 0 8px;border-left:2px solid #ffaa00;padding-left:8px}
.field{margin-bottom:10px}
label{display:block;font-size:.72em;color:#cc8800;margin-bottom:3px}
input[type=text]{width:100%;background:#1a1a1a;color:#ffaa00;border:1px solid #444;padding:6px 8px;font-family:monospace;font-size:1em}
input[type=text]:focus{outline:none;border-color:#ffaa00}
.row-input{text-transform:uppercase}
button{font-family:monospace;font-size:.85em;padding:9px 18px;cursor:pointer;letter-spacing:.1em;margin-top:6px;margin-right:6px}
.primary{background:#ffaa00;color:#0e0e0e;border:none}
.danger{background:#880000;color:#fff;border:none}
#msg{margin-top:10px;font-size:.82em;min-height:1.1em}
#boards{font-size:.78em;color:#888;margin-top:4px}
hr{border:none;border-top:1px solid #2a2a2a;margin:18px 0}
.key-bar{background:#1a1a1a;border:1px solid #333;padding:10px;margin-bottom:16px}
</style>
</head>
<body>
<h1>&#9646; FLIPBOARD SERVER</h1>

<div class="key-bar">
  <div class="field" style="margin:0">
    <label>API KEY — controls which boards you see and send to</label>
    <input type="text" id="apiKey" placeholder="paste your board api key here" autocomplete="off">
  </div>
  <div id="boards" style="margin-top:6px;font-size:.78em;color:#888">Enter key to see connected boards</div>
</div>

<h2>ALL ROWS</h2>
<form id="allForm">
${[0,1,2,3,4,5].map(i=>`<div class="field"><label>ROW ${i}</label><input class="row-input" type="text" name="r${i}" maxlength="21" autocomplete="off"></div>`).join('')}
<button type="submit" class="primary">SEND ALL ROWS</button>
</form>

<hr>
<h2>SINGLE ROW</h2>
<div class="field"><label>ROW NUMBER (0-5)</label><input type="text" id="rowNum" value="0" style="width:60px"></div>
<div class="field"><label>TEXT</label><input class="row-input" type="text" id="rowText" maxlength="21" autocomplete="off"></div>
<button class="primary" onclick="sendRow()">SEND ROW</button>

<hr>
<h2>BRIGHTNESS</h2>
<div class="field">
  <label>BRIGHTNESS — <span id="brightVal">78</span>%</label>
  <input type="range" id="bright" min="0" max="100" value="78"
    style="width:100%;accent-color:#ffaa00"
    oninput="document.getElementById('brightVal').textContent=this.value"
    onchange="api('/api/brightness',JSON.stringify({percent:parseInt(this.value)}))">
</div>

<hr>
<h2>CONTROLS</h2>
<button class="primary" onclick="api('/api/wake','{}')">WAKE + REPLAY</button>
<button class="primary" id="demoBtn" onclick="toggleDemo()">START DEMO</button>
<button class="danger" onclick="api('/api/timeout',JSON.stringify({minutes:0}))">NEVER OFF</button>

<div id="msg"></div>

<script>
const keyEl  = document.getElementById('apiKey');
const msg    = document.getElementById('msg');
let demoOn   = false;

// Persist key in localStorage
keyEl.value = localStorage.getItem('fb_server_key') || '';
keyEl.addEventListener('change', () => {
  localStorage.setItem('fb_server_key', keyEl.value.trim());
  refreshBoards();
});

function getKey() {
  const k = keyEl.value.trim();
  localStorage.setItem('fb_server_key', k);
  return k;
}

async function api(url, body) {
  const k = getKey();
  if (!k) { msg.textContent = 'Enter your API key first.'; return; }
  try {
    const r = await fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-Api-Key': k },
      body
    });
    const j = await r.json();
    msg.textContent = r.ok ? 'OK' : 'Error: ' + JSON.stringify(j);
  } catch(e) { msg.textContent = 'Failed: ' + e; }
}

document.getElementById('allForm').onsubmit = async e => {
  e.preventDefault();
  const rows = [0,1,2,3,4,5].map(i => (e.target['r'+i].value || '').toUpperCase());
  await api('/api/rows', JSON.stringify({ rows }));
};

function sendRow() {
  const row  = parseInt(document.getElementById('rowNum').value);
  const text = document.getElementById('rowText').value.toUpperCase();
  api('/api/row', JSON.stringify({ row, text }));
}

async function toggleDemo() {
  demoOn = !demoOn;
  await api('/api/demo', JSON.stringify({ mode: demoOn ? 'on' : 'off' }));
  const btn = document.getElementById('demoBtn');
  btn.textContent       = demoOn ? 'STOP DEMO' : 'START DEMO';
  btn.style.background  = demoOn ? '#cc4400'   : '';
}

async function refreshBoards() {
  const k = keyEl.value.trim();
  if (!k) { document.getElementById('boards').textContent = 'Enter key to see connected boards'; return; }
  try {
    const r = await fetch('/api/boards', { headers: { 'X-Api-Key': k } });
    if (!r.ok) { document.getElementById('boards').textContent = 'Invalid key or server error.'; return; }
    const j = await r.json();
    document.getElementById('boards').textContent =
      j.count === 0 ? 'No boards connected with this key' :
      j.count + ' board' + (j.count > 1 ? 's' : '') + ' connected: ' +
      j.boards.map(b => b.ip).join(', ');
  } catch(e) {}
}

refreshBoards();
setInterval(refreshBoards, 5000);
</script>
</body>
</html>`));

// ── Start ─────────────────────────────────────────────────────────────────────

server.listen(PORT, () => {
    console.log(`FlipBoard server running on http://localhost:${PORT}`);
    console.log(`Boards authenticate with their API key — each key is a separate tenant.`);
});

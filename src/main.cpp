// main.cpp - Application entry point
//
// Responsibilities:
//   • Connects to WiFi and keeps the board's signal-strength icon updated.
//   • Starts a lightweight HTTP server (port 80) once WiFi is up.
//   • Calls board_tick() every loop to drive the flap animation.
//
// HTTP API (available after WiFi connects):
//   All endpoints require the header:  X-Api-Key: <value from secrets.h>
//
//   GET  /status           → JSON: wifi, ip, rssi, bars, free_heap, uptime_s
//
//   POST /row/<0-5>        → Set a row.  Send text as the request body.
//                            All three curl styles work:
//     curl -X POST http://<ip>/row/0 -H "X-Api-Key: $KEY" \
//          -H "Content-Type: text/plain" -d "HELLO"
//     curl -X POST http://<ip>/row/0 -H "X-Api-Key: $KEY" -d "text=HELLO+WORLD"
//     curl -X POST http://<ip>/row/0 -H "X-Api-Key: $KEY" -d "HELLO WORLD"
//
//   POST /rows             → Set all 6 rows; one line per row, newline-delimited.
//   DELETE /row/<0-5>/clear → Clear a row (animate to all spaces).
//
// Serial output (115200 baud):
//   Boot stats, WiFi connection event, and a status block every 5 s.
//   POST requests print the decoded body so you can verify receipt.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "travel_board.h"
#include "secrets.h"
#include "sio_client.h"
#include "led_indicator.h"

// ─── Security constants ───────────────────────────────────────────────────────
#define MAX_BODY_BYTES  512   // reject bodies larger than this
#define RATE_LIMIT_RPS  10    // max requests per second (global)

// ─── Socket.IO config (persisted in NVS via Preferences) ─────────────────────
struct SioConfig {
    bool     enabled;
    char     host[64];
    uint16_t port;
};

static SioConfig g_sio = { false, "", 3000 };

static void sioLoadConfig() {
    Preferences prefs;
    prefs.begin("sio", true);   // read-only
    g_sio.enabled = prefs.getBool("enabled", false);
    prefs.getString("host", g_sio.host, sizeof(g_sio.host));
    g_sio.port = (uint16_t)prefs.getUInt("port", 3000);
    prefs.end();
}

static void sioSaveConfig(bool enabled, const char* host, uint16_t port) {
    Preferences prefs;
    prefs.begin("sio", false);  // read-write
    prefs.putBool("enabled", enabled);
    prefs.putString("host", host);
    prefs.putUInt("port", port);
    prefs.end();
    Serial.printf("[SIO] config saved  enabled=%d  host=%s  port=%d\n",
                  enabled, host, port);
}

// ─── Wake-source pins ─────────────────────────────────────────────────────────
// Define WAKE_BTN_PIN and/or WAKE_RADAR_PIN in platformio.ini build_flags to
// enable the corresponding wake source.  Both are optional and independent.
//
//   Button : active-low, internal pull-up.  Press triggers wake + replay.
//   Radar  : active-high OUT pin from an mmWave sensor (e.g. LD2410).
//            Rising edge (no presence → presence) triggers wake + replay.
//
// Example platformio.ini build_flags:
//   -DWAKE_BTN_PIN=5
//   -DWAKE_RADAR_PIN=6

// ─── Web UI page ─────────────────────────────────────────────────────────────
static const char kPageUI[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FlipBoard</title>
<style>
*{box-sizing:border-box}
body{background:#0e0e0e;color:#ffaa00;font-family:monospace;margin:0 auto;padding:16px;max-width:520px}
h1{font-size:1.1em;letter-spacing:.35em;border-bottom:1px solid #ffaa00;padding-bottom:8px;margin:0 0 14px}
h2{font-size:.85em;letter-spacing:.2em;color:#ffaa00;margin:20px 0 8px;border-left:2px solid #ffaa00;padding-left:8px}
h3{font-size:.78em;letter-spacing:.15em;color:#cc8800;margin:14px 0 4px}
.tabs{display:flex;gap:4px;margin-bottom:16px}
.tab{flex:1;padding:7px;font-family:monospace;font-size:.8em;letter-spacing:.15em;cursor:pointer;background:#1a1a1a;color:#888;border:1px solid #333;text-align:center}
.tab.active{background:#ffaa00;color:#0e0e0e;border-color:#ffaa00}
.page{display:none}.page.active{display:block}
.field{margin-bottom:10px}
label{display:block;font-size:.72em;color:#cc8800;margin-bottom:3px}
input[type=text]{width:100%;background:#1a1a1a;color:#ffaa00;border:1px solid #444;padding:6px 8px;font-family:monospace;font-size:1em}
input[type=text]:focus{outline:none;border-color:#ffaa00}
.row-input{text-transform:uppercase;letter-spacing:.08em}
hr{border:none;border-top:1px solid #2a2a2a;margin:18px 0}
button{font-family:monospace;font-size:.85em;padding:9px 18px;cursor:pointer;letter-spacing:.1em;margin-top:6px}
.primary{background:#ffaa00;color:#0e0e0e;border:none;margin-right:8px}
.danger{background:#880000;color:#fff;border:none}
#msg{margin-top:10px;font-size:.82em;min-height:1.1em}
pre{background:#141414;border:1px solid #333;padding:10px;overflow-x:auto;font-size:.8em;line-height:1.5;color:#ffcc66;margin:6px 0 14px}
p{margin:4px 0 10px;font-size:.82em;line-height:1.6;color:#cc9933}
code{background:#1a1a1a;padding:1px 5px;color:#ffcc66;font-size:.9em}
table{width:100%;border-collapse:collapse;font-size:.8em;margin:6px 0 14px}
td,th{padding:5px 8px;border:1px solid #333;text-align:left}
th{color:#0e0e0e;background:#cc8800}
tr:nth-child(even) td{background:#141414}
.ep{color:#ffaa00;font-weight:bold}
.method{color:#aaffaa;font-size:.75em;margin-right:6px}
.auth{color:#ff9944;font-size:.72em;float:right}
</style>
</head>
<body>
<h1>&#9646; FLIPBOARD</h1>

<div class="tabs">
  <div class="tab active" onclick="show('board',this)">BOARD</div>
  <div class="tab" onclick="show('settings',this)">SETTINGS</div>
  <div class="tab" onclick="show('docs',this)">API DOCS</div>
</div>

<!-- ── BOARD TAB ───────────────────────────────────────────────── -->
<div id="board" class="page active">
  <div class="field">
    <label>API KEY</label>
    <input type="text" id="key" placeholder="paste api key here">
  </div>
  <hr>
  <form id="f">
    <div class="field"><label>ROW 0</label><input class="row-input" type="text" name="r0" maxlength="21" autocomplete="off"></div>
    <div class="field"><label>ROW 1</label><input class="row-input" type="text" name="r1" maxlength="21" autocomplete="off"></div>
    <div class="field"><label>ROW 2</label><input class="row-input" type="text" name="r2" maxlength="21" autocomplete="off"></div>
    <div class="field"><label>ROW 3</label><input class="row-input" type="text" name="r3" maxlength="21" autocomplete="off"></div>
    <div class="field"><label>ROW 4</label><input class="row-input" type="text" name="r4" maxlength="21" autocomplete="off"></div>
    <div class="field"><label>ROW 5</label><input class="row-input" type="text" name="r5" maxlength="21" autocomplete="off"></div>
    <button type="submit" class="primary">UPDATE BOARD</button>
  </form>
  <div id="msg"></div>
  <hr>
  <div class="field">
    <label>BRIGHTNESS <span id="brightVal">78</span>%</label>
    <input type="range" id="bright" min="0" max="100" value="78" style="width:100%;accent-color:#ffaa00" oninput="document.getElementById('brightVal').textContent=this.value" onchange="setBright()">
  </div>
  <div class="field">
    <label>DISPLAY OFF TIMEOUT (MINUTES, 0 = NEVER)</label>
    <input type="text" id="timeout" placeholder="10" style="width:80px">
    <button type="button" class="primary" style="margin-left:8px" onclick="setTimeo()">SET</button>
  </div>
  <div class="field">
    <label>WAKE DISPLAY</label>
    <button type="button" class="primary" onclick="wakeDisplay()">WAKE + REPLAY</button>
  </div>
  <div class="field">
    <label>DEMO MODE (CYCLES ALL PRESETS EVERY 30 S)</label>
    <button type="button" id="demoBtn" class="primary" onclick="toggleDemo()">START DEMO</button>
  </div>
  <hr>
  <h2>LED INDICATORS</h2>
  <p style="font-size:.78em;color:#888;margin:0 0 10px">Requires LED1_PIN / LED2_PIN build flags. Controls are always visible.</p>
  <h3 style="font-size:.78em;letter-spacing:.15em;color:#cc8800;margin:12px 0 6px">LED 1</h3>
  <div style="display:flex;gap:6px;flex-wrap:wrap;margin-bottom:6px">
    <button class="primary" onclick="ledMode(1,'off')">OFF</button>
    <button class="primary" onclick="ledMode(1,'on')">ON</button>
    <button class="primary" onclick="ledMode(1,'flash')">FLASH</button>
    <button class="primary" onclick="ledMode(1,'pulse')">PULSE</button>
  </div>
  <div class="field">
    <label>LED 1 BRIGHTNESS — <span id="lb1val">100</span>%</label>
    <input type="range" id="lb1" min="0" max="100" value="100"
      style="width:100%;accent-color:#ffaa00"
      oninput="document.getElementById('lb1val').textContent=this.value"
      onchange="ledBright(1,this.value)">
  </div>
  <div class="field">
    <label><input type="checkbox" id="ln1" onchange="ledNotify(1,this.checked)" style="accent-color:#ffaa00">
    &nbsp;NOTIFY ON NEW MESSAGE / WAKE</label>
  </div>
  <h3 style="font-size:.78em;letter-spacing:.15em;color:#cc8800;margin:12px 0 6px">LED 2</h3>
  <div style="display:flex;gap:6px;flex-wrap:wrap;margin-bottom:6px">
    <button class="primary" onclick="ledMode(2,'off')">OFF</button>
    <button class="primary" onclick="ledMode(2,'on')">ON</button>
    <button class="primary" onclick="ledMode(2,'flash')">FLASH</button>
    <button class="primary" onclick="ledMode(2,'pulse')">PULSE</button>
  </div>
  <div class="field">
    <label>LED 2 BRIGHTNESS — <span id="lb2val">100</span>%</label>
    <input type="range" id="lb2" min="0" max="100" value="100"
      style="width:100%;accent-color:#ffaa00"
      oninput="document.getElementById('lb2val').textContent=this.value"
      onchange="ledBright(2,this.value)">
  </div>
  <div class="field">
    <label><input type="checkbox" id="ln2" onchange="ledNotify(2,this.checked)" style="accent-color:#ffaa00">
    &nbsp;NOTIFY ON NEW MESSAGE / WAKE</label>
  </div>
  <hr>
  <button class="danger" onclick="resetWifi()">RESET WIFI SETTINGS</button>
</div>

<!-- ── SETTINGS TAB ─────────────────────────────────────────────── -->
<div id="settings" class="page">

  <h2>SOCKET.IO SERVER</h2>
  <p>Connect the board to a remote Socket.IO server for live push updates. Changes take effect immediately without rebooting.</p>
  <div class="field">
    <label>API KEY</label>
    <input type="text" id="sKey" placeholder="paste api key here">
  </div>
  <hr>
  <div class="field">
    <label>ENABLE SOCKET.IO</label>
    <select id="sEnable" style="background:#1a1a1a;color:#ffaa00;border:1px solid #444;padding:6px 8px;font-family:monospace;font-size:1em;width:100%">
      <option value="0">DISABLED</option>
      <option value="1">ENABLED</option>
    </select>
  </div>
  <div class="field">
    <label>SERVER HOST / IP</label>
    <input type="text" id="sHost" placeholder="192.168.1.10" autocomplete="off">
  </div>
  <div class="field">
    <label>SERVER PORT</label>
    <input type="text" id="sPort" placeholder="3000" style="width:100px">
  </div>
  <button type="button" class="primary" onclick="saveSio()">SAVE + CONNECT</button>
  <div id="sMsg" style="margin-top:10px;font-size:.82em;min-height:1.1em"></div>

  <hr>
  <div class="field">
    <label>SOCKET.IO STATUS</label>
    <div id="sStatus" style="font-size:.82em;color:#888">Loading…</div>
  </div>

</div><!-- /settings -->

<!-- ── DOCS TAB ────────────────────────────────────────────────── -->
<div id="docs" class="page">

  <h2>AUTHENTICATION</h2>
  <p>All endpoints except <code>GET /</code> require the header:</p>
  <pre>X-Api-Key: your-api-key</pre>
  <p>The key is defined in <code>src/secrets.h</code> on the device. Generate one with:</p>
  <pre>openssl rand -hex 16</pre>
  <p>Requests without a valid key return <code>401 Unauthorized</code>. The rate limit is 10 requests per second; excess requests return <code>429</code>.</p>

  <hr>

  <h2>ENDPOINTS</h2>

  <h3><span class="method">GET</span><span class="ep">/status</span> <span class="auth">requires key</span></h3>
  <p>Returns a JSON snapshot of WiFi state and memory.</p>
  <pre>curl http://&lt;ip&gt;/status -H "X-Api-Key: &lt;key&gt;"</pre>
  <pre>{
  "wifi":      "MyNetwork",
  "ip":        "192.168.1.42",
  "rssi":      -62,
  "bars":      2,
  "free_heap": 214320,
  "min_heap":  201440,
  "uptime_s":  47
}</pre>

  <h3><span class="method">POST</span><span class="ep">/row/&lt;0-5&gt;</span> <span class="auth">requires key</span></h3>
  <p>Set a single row. Text is uppercased automatically. Row 0 is the top row.</p>
  <pre>curl -X POST http://&lt;ip&gt;/row/0 \
     -H "X-Api-Key: &lt;key&gt;" \
     -H "Content-Type: text/plain" \
     -d "GATE CHANGE B12"</pre>
  <p>Also accepts form-encoded body: <code>-d "text=DELAYED+20+MIN"</code></p>

  <h3><span class="method">POST</span><span class="ep">/rows</span> <span class="auth">requires key</span></h3>
  <p>Set all 6 rows in one request. Send <code>Content-Type: text/plain</code> with one line per row, newline-separated. Fewer than 6 lines clears the remaining rows.</p>
  <pre>curl -X POST http://&lt;ip&gt;/rows \
     -H "X-Api-Key: &lt;key&gt;" \
     -H "Content-Type: text/plain" \
     -d $'FL 101  LONDON\nFL 202  NEW YORK\nFL 303  PARIS\nFL 404  TOKYO\nFL 505  SYDNEY\nFL 606  DUBAI'</pre>
  <p>Or send from a file (one line per row):</p>
  <pre>curl -X POST http://&lt;ip&gt;/rows \
     -H "X-Api-Key: &lt;key&gt;" \
     -H "Content-Type: text/plain" \
     --data-binary @rows.txt</pre>

  <h3><span class="method">DELETE</span><span class="ep">/row/&lt;0-5&gt;/clear</span> <span class="auth">requires key</span></h3>
  <p>Animate a single row to all spaces (blank it out).</p>
  <pre>curl -X DELETE http://&lt;ip&gt;/row/2/clear \
     -H "X-Api-Key: &lt;key&gt;"</pre>

  <h3><span class="method">POST</span><span class="ep">/wifi/reset</span> <span class="auth">requires key</span></h3>
  <p>Clears stored WiFi credentials and reboots into setup mode. The board opens the <code>FLIPBOARD-XXXX</code> access point within a few seconds.</p>
  <pre>curl -X POST http://&lt;ip&gt;/wifi/reset \
     -H "X-Api-Key: &lt;key&gt;"</pre>

  <h3><span class="method">POST</span><span class="ep">/display/brightness</span> <span class="auth">requires key</span></h3>
  <p>Set display brightness 0–100 (percent). Applied immediately. Survives wake/dim cycles — the panel always returns to this level when woken. Resets to ~78% on reboot.</p>
  <pre>curl -X POST http://&lt;ip&gt;/display/brightness \
     -H "X-Api-Key: &lt;key&gt;" \
     -H "Content-Type: text/plain" \
     -d "50"</pre>

  <h3><span class="method">POST</span><span class="ep">/display/demo</span> <span class="auth">requires key</span></h3>
  <p>Start or stop demo mode. While active, the board cycles through all built-in presets every 30 seconds. Sending content via <code>/row</code> or <code>/rows</code> cancels demo mode automatically. Body: <code>on</code> or <code>off</code>.</p>
  <pre>curl -X POST http://&lt;ip&gt;/display/demo \
     -H "X-Api-Key: &lt;key&gt;" \
     -H "Content-Type: text/plain" \
     -d "on"</pre>

  <h3><span class="method">POST</span><span class="ep">/display/wake</span> <span class="auth">requires key</span></h3>
  <p>Wake the display and replay the current content through the split-flap animation. Equivalent to pressing the wake button or triggering the radar sensor.</p>
  <pre>curl -X POST http://&lt;ip&gt;/display/wake \
     -H "X-Api-Key: &lt;key&gt;"</pre>

  <h3><span class="method">POST</span><span class="ep">/display/timeout</span> <span class="auth">requires key</span></h3>
  <p>Override the idle power-off timeout for this session. Send the number of minutes as a plain text body. Use <code>0</code> to disable power-off entirely. Resets to the 10-minute default on reboot.</p>
  <pre>curl -X POST http://&lt;ip&gt;/display/timeout \
     -H "X-Api-Key: &lt;key&gt;" \
     -H "Content-Type: text/plain" \
     -d "30"</pre>

  <hr>

  <h2>CHARACTERS</h2>
  <p>The display supports: <code>A-Z 0-9</code> and <code>space - : / . !</code><br>
  Lowercase is converted to uppercase. Unsupported characters are stripped.</p>

  <hr>

  <h2>ERROR CODES</h2>
  <table>
    <tr><th>Status</th><th>Meaning</th></tr>
    <tr><td>401</td><td>Missing or wrong X-Api-Key</td></tr>
    <tr><td>400</td><td>Bad row number or empty body</td></tr>
    <tr><td>413</td><td>Body exceeds 512 bytes</td></tr>
    <tr><td>429</td><td>Rate limit exceeded (max 10 req/s)</td></tr>
    <tr><td>404</td><td>Unknown route</td></tr>
  </table>

</div><!-- /docs -->

<script>
const keyEl=document.getElementById('key');
const msg=document.getElementById('msg');
const stored=localStorage.getItem('fb_key')||'';
keyEl.value=stored;
document.getElementById('sKey').value=stored;
function show(id,tab){
  document.querySelectorAll('.page').forEach(p=>p.classList.remove('active'));
  document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));
  document.getElementById(id).classList.add('active');
  tab.classList.add('active');
  if(id==='settings') loadSioConfig();
}
function sioKey(){
  const k=document.getElementById('sKey').value.trim()||keyEl.value.trim();
  localStorage.setItem('fb_key',k);
  return k;
}
async function loadSioConfig(){
  try{
    const r=await fetch('/config/sio',{headers:{'X-Api-Key':sioKey()}});
    if(!r.ok)return;
    const j=await r.json();
    document.getElementById('sEnable').value=j.enabled?'1':'0';
    document.getElementById('sHost').value=j.host||'';
    document.getElementById('sPort').value=j.port||3000;
    document.getElementById('sStatus').textContent=
      j.connected?'Connected':'Not connected';
    document.getElementById('sStatus').style.color=j.connected?'#44ff88':'#888';
  }catch(e){}
}
async function saveSio(){
  const k=sioKey();
  const body=JSON.stringify({
    enabled: document.getElementById('sEnable').value==='1',
    host:    document.getElementById('sHost').value.trim(),
    port:    parseInt(document.getElementById('sPort').value)||3000
  });
  const sMsg=document.getElementById('sMsg');
  sMsg.textContent='Saving…';
  try{
    const r=await fetch('/config/sio',{method:'POST',
      headers:{'Content-Type':'application/json','X-Api-Key':k},body});
    if(r.ok){
      sMsg.textContent='Saved. Reconnecting…';
      setTimeout(loadSioConfig,2000);
    }else{sMsg.textContent='Error '+r.status;}
  }catch(e){sMsg.textContent='Failed: '+e;}
}
document.getElementById('f').onsubmit=async e=>{
  e.preventDefault();
  const k=keyEl.value.trim();
  localStorage.setItem('fb_key',k);
  const body=['r0','r1','r2','r3','r4','r5']
    .map(n=>(e.target[n].value||'').toUpperCase()).join('\n');
  msg.textContent='Sending…';
  try{
    const r=await fetch('/rows',{method:'POST',
      headers:{'Content-Type':'text/plain','X-Api-Key':k},body});
    msg.textContent=r.ok?'Board updated.':'Error '+r.status+' - check API key?';
  }catch(err){msg.textContent='Failed: '+err;}
};
async function resetWifi(){
  if(!confirm('Clear saved WiFi credentials and reboot into setup mode?'))return;
  const k=keyEl.value.trim();
  localStorage.setItem('fb_key',k);
  msg.textContent='Resetting…';
  try{await fetch('/wifi/reset',{method:'POST',headers:{'X-Api-Key':k}});}catch(e){}
  msg.textContent='Board rebooting into WiFi setup mode…';
}
let demoOn=false;
async function toggleDemo(){
  const k=keyEl.value.trim();
  localStorage.setItem('fb_key',k);
  const next=!demoOn;
  msg.textContent=next?'Starting demo…':'Stopping demo…';
  try{
    const r=await fetch('/display/demo',{method:'POST',
      headers:{'Content-Type':'text/plain','X-Api-Key':k},body:next?'on':'off'});
    if(r.ok){
      demoOn=next;
      const btn=document.getElementById('demoBtn');
      btn.textContent=demoOn?'STOP DEMO':'START DEMO';
      btn.style.background=demoOn?'#cc4400':'';
      msg.textContent=demoOn?'Demo mode on – cycling every 30 s.':'Demo mode off.';
    }else{msg.textContent='Error '+r.status;}
  }catch(err){msg.textContent='Failed: '+err;}
}
async function setBright(){
  const k=keyEl.value.trim(); localStorage.setItem('fb_key',k);
  const v=document.getElementById('bright').value;
  try{ await fetch('/display/brightness',{method:'POST',
    headers:{'Content-Type':'text/plain','X-Api-Key':k},body:v}); }
  catch(e){msg.textContent='Brightness failed: '+e;}
}
// Seed slider from live status on load
(async()=>{
  try{
    const k=keyEl.value.trim();
    if(!k) return;
    const r=await fetch('/status',{headers:{'X-Api-Key':k}});
    if(!r.ok) return;
    const j=await r.json();
    if(j.brightness!==undefined){
      document.getElementById('bright').value=j.brightness;
      document.getElementById('brightVal').textContent=j.brightness;
    }
  }catch(e){}
})();
async function wakeDisplay(){
  const k=keyEl.value.trim();
  localStorage.setItem('fb_key',k);
  msg.textContent='Waking…';
  try{
    const r=await fetch('/display/wake',{method:'POST',headers:{'X-Api-Key':k}});
    msg.textContent=r.ok?'Display woken.':'Error '+r.status;
  }catch(err){msg.textContent='Failed: '+err;}
}
async function ledMode(n,mode){
  const k=keyEl.value.trim(); localStorage.setItem('fb_key',k);
  try{ await fetch('/led/'+n+'/mode',{method:'POST',
    headers:{'Content-Type':'text/plain','X-Api-Key':k},body:mode}); }
  catch(e){msg.textContent='LED failed: '+e;}
}
async function ledBright(n,v){
  const k=keyEl.value.trim(); localStorage.setItem('fb_key',k);
  try{ await fetch('/led/'+n+'/brightness',{method:'POST',
    headers:{'Content-Type':'text/plain','X-Api-Key':k},body:v}); }
  catch(e){msg.textContent='LED failed: '+e;}
}
async function ledNotify(n,en){
  const k=keyEl.value.trim(); localStorage.setItem('fb_key',k);
  try{ await fetch('/led/'+n+'/notify',{method:'POST',
    headers:{'Content-Type':'text/plain','X-Api-Key':k},body:en?'on':'off'}); }
  catch(e){msg.textContent='LED failed: '+e;}
}
// Seed LED controls from live status on load
(async()=>{
  try{
    const k=keyEl.value.trim(); if(!k) return;
    const r=await fetch('/led/status',{headers:{'X-Api-Key':k}});
    if(!r.ok) return;
    const j=await r.json();
    ['led1','led2'].forEach((key,i)=>{
      const n=i+1;
      const b=j[key].brightness;
      document.getElementById('lb'+n).value=b;
      document.getElementById('lb'+n+'val').textContent=b;
      document.getElementById('ln'+n).checked=j[key].notify;
    });
  }catch(e){}
})();
async function setTimeo(){
  const k=keyEl.value.trim();
  localStorage.setItem('fb_key',k);
  const v=document.getElementById('timeout').value.trim();
  if(v===''||isNaN(v)){msg.textContent='Enter a number of minutes (0 = never off).';return;}
  msg.textContent='Updating…';
  try{
    const r=await fetch('/display/timeout',{method:'POST',
      headers:{'Content-Type':'text/plain','X-Api-Key':k},body:v});
    msg.textContent=r.ok?'Timeout updated.':'Error '+r.status;
  }catch(err){msg.textContent='Failed: '+err;}
}
</script>
</body>
</html>
)HTML";

// ─── Boot splash (shown while connecting) ────────────────────────────────────
static const char* kBoot[6] = {
    "INITIALIZING",
    "",          // filled at runtime with chip model - see setup()
    "v1",
    "",
    "",
    ""
};

// ─── Config portal splash (shown while waiting for WiFi setup) ───────────────
// Row 2 is patched at runtime with the AP SSID - see setup().
static const char* kPortal[6] = {
    "WIFI SETUP",
    "JOIN NETWORK",
    "",           // filled at runtime with AP name
    "THEN VISIT",
    "192.168.4.1",
    ""
};

// ─── Demo presets ─────────────────────────────────────────────────────────────
#include "presets.h"

// ─── Module state ────────────────────────────────────────────────────────────
static WebServer server(80);

// Rate-limit state: count requests inside the current 1-second window.
static uint32_t g_rateWindowStart = 0;
static uint16_t g_rateCount       = 0;
static uint32_t g_lastWifiCheckMs = 0;

// Demo mode state
static bool     g_demoMode    = false;
static uint8_t  g_demoIndex   = 0;
static uint32_t g_demoLastMs  = 0;
#define DEMO_INTERVAL_MS  (30UL * 1000UL)

// Boot status splash
static char     g_apName[20]          = {};   // "FLIPBOARD-XXXX", set in setup()
static bool     g_bootStatus          = false; // true while the status page is displayed
static uint32_t g_bootStatusUntilMs   = 0;
#define BOOT_STATUS_MS  (60UL * 1000UL)

// Wake-source state
#ifdef WAKE_BTN_PIN
static uint32_t g_lastBtnMs    = 0;      // debounce timestamp
static bool     g_lastBtnState = true;   // last stable state (HIGH = not pressed)
#endif
#ifdef WAKE_RADAR_PIN
static bool g_lastRadarState = false;    // last stable state (LOW = no presence)
#endif

// ─── Helpers ─────────────────────────────────────────────────────────────────

// Returns true when the X-Api-Key header matches the compiled-in API_KEY.
// Call this in write handlers; send 401 and return if it returns false.
static bool authenticated() {
    if (server.header("X-Api-Key") == API_KEY) return true;
    server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    Serial.println("[HTTP] 401 bad/missing API key");
    return false;
}

// Returns true when the global request rate is within RATE_LIMIT_RPS.
// Call this at the top of every handler; send 429 and return if false.
static bool rateLimited() {
    uint32_t now = millis();
    if (now - g_rateWindowStart >= 1000) {
        g_rateWindowStart = now;
        g_rateCount       = 0;
    }
    if (++g_rateCount > RATE_LIMIT_RPS) {
        server.send(429, "application/json", "{\"error\":\"rate limit exceeded\"}");
        Serial.println("[HTTP] 429 rate limited");
        return true;
    }
    return false;
}

// Convert RSSI (dBm) to a 0–3 bar count for the display icon.
static uint8_t rssiToBars(int32_t rssi) {
    if (rssi >= -55) return 3;   // excellent
    if (rssi >= -70) return 2;   // good
    if (rssi >= -85) return 1;   // fair
    return 0;                    // weak / disconnected
}

// Forward declaration — defined after the wake helpers below.
void triggerContentNotify();

// ─── HTTP handlers ───────────────────────────────────────────────────────────

// GET /status
// Returns a JSON snapshot of WiFi state and heap usage.
static void handleStatus() {
    if (rateLimited()) return;
    if (!authenticated()) return;
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"wifi\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"bars\":%d,"
        "\"free_heap\":%lu,\"min_heap\":%lu,\"uptime_s\":%lu,\"brightness\":%u,"
        "\"led1\":{\"mode\":\"%s\",\"brightness\":%u,\"notify\":%s},"
        "\"led2\":{\"mode\":\"%s\",\"brightness\":%u,\"notify\":%s}}",
        WiFi.SSID().c_str(),
        WiFi.localIP().toString().c_str(),
        (int)WiFi.RSSI(),
        rssiToBars(WiFi.RSSI()),
        (unsigned long)ESP.getFreeHeap(),
        (unsigned long)ESP.getMinFreeHeap(),
        millis() / 1000,
        board_get_brightness(),
        led_mode_str(led_get_mode(0)), led_get_brightness(0), led_get_notify(0) ? "true" : "false",
        led_mode_str(led_get_mode(1)), led_get_brightness(1), led_get_notify(1) ? "true" : "false");
    server.send(200, "application/json", buf);
}

// POST /row/<n>
// Reads the text body and calls board_set_row().
//
// Body retrieval strategy - tried in order until one succeeds:
//
//   1. arg("plain")  - populated when Content-Type is text/plain.
//      Used by:  curl -H "Content-Type: text/plain" -d "HELLO WORLD"
//
//   2. arg("text")   - populated when the form body contains "text=VALUE".
//      Used by:  curl -d "text=HELLO+WORLD"
//
//   3. Join all arg names - curl's default form-encoding sends the raw body
//      as a sequence of "keys" (the text split at '=' boundaries).
//      Each word may become a separate argName when spaces are not URL-encoded.
//      Used by:  curl -d "HELLO WORLD"      (least reliable, but handled)
//
// In all cases the text is uppercased and sanitised inside board_set_row().
static void handleSetRow() {
    if (rateLimited()) return;
    if (!authenticated()) return;
    if (server.arg("plain").length() + server.arg("text").length() > MAX_BODY_BYTES) {
        server.send(413, "application/json", "{\"error\":\"body too large\"}");
        return;
    }
    // Extract the row number from the URI: "/row/3" → 3
    int rowNum = server.uri().substring(5).toInt();
    if (rowNum < 0 || rowNum > 5) {
        server.send(400, "text/plain", "row must be 0-5");
        return;
    }

    // Strategy 1: raw body (text/plain content type)
    String body = server.arg("plain");

    // Strategy 2: named form field "text"
    if (body.isEmpty()) {
        body = server.arg("text");
    }

    // Strategy 3: reconstruct from all arg names (default curl form encoding).
    // curl -d "HELLO WORLD" sends with Content-Type: application/x-www-form-urlencoded.
    // The WebServer parses "HELLO WORLD" as a key (no '='), so the text lands in
    // argName(0).  If the server split on spaces, multiple argNames are joined.
    if (body.isEmpty()) {
        for (int i = 0; i < server.args(); i++) {
            String n = server.argName(i);
            if (n == "plain" || n == "text") continue;   // skip special keys
            if (!body.isEmpty()) body += ' ';
            body += n;
        }
    }

    // Log what was received so you can verify via the serial monitor.
    Serial.printf("[HTTP] POST /row/%d  args=%d  body=\"%s\"\n",
                  rowNum, server.args(), body.c_str());

    g_demoMode   = false;
    g_bootStatus = false;
    board_wake();
    board_set_row((uint8_t)rowNum, body.c_str());
    triggerContentNotify();
    server.send(200, "text/plain", "ok");
}

// POST /rows
// Set all 6 rows in one request.  Send Content-Type: text/plain with one line
// per row, separated by newlines.  Fewer than 6 lines clears the remaining rows.
//
// Example:
//   curl -X POST http://<ip>/rows \
//        -H "Content-Type: text/plain" \
//        -d $'FL 101  LONDON\nFL 202  NEW YORK\nFL 303  PARIS\nFL 404  TOKYO\nFL 505  SYDNEY\nFL 606  DUBAI'
static void handleSetAll() {
    if (rateLimited()) return;
    if (!authenticated()) return;
    String body = server.arg("plain");
    if (body.length() > MAX_BODY_BYTES) {
        server.send(413, "application/json", "{\"error\":\"body too large\"}");
        return;
    }
    if (body.isEmpty()) {
        server.send(400, "application/json", "{\"error\":\"empty body - send Content-Type: text/plain\"}");
        return;
    }

    // Split body on '\n', strip '\r', take up to 6 lines.
    String  lines[6];
    uint8_t count = 0;
    int     start = 0;
    for (int i = 0; i <= (int)body.length() && count < 6; i++) {
        if (i == (int)body.length() || body[i] == '\n') {
            lines[count] = body.substring(start, i);
            if (lines[count].endsWith("\r"))
                lines[count].remove(lines[count].length() - 1);
            count++;
            start = i + 1;
        }
    }

    // Build pointer array; missing rows get nullptr → board treats as empty.
    const char* texts[6] = {};
    for (uint8_t i = 0; i < count; i++) texts[i] = lines[i].c_str();

    Serial.printf("[HTTP] POST /rows  %d lines\n", count);
    for (uint8_t i = 0; i < 6; i++)
        Serial.printf("  [%d] \"%s\"\n", i, texts[i] ? texts[i] : "");

    g_demoMode   = false;
    g_bootStatus = false;
    board_wake();
    board_set_all(texts);
    triggerContentNotify();
    server.send(200, "text/plain", "ok");
}

// DELETE /row/<n>/clear
// Animates the target row to all spaces.
static void handleClearRow() {
    if (rateLimited()) return;
    if (!authenticated()) return;
    // URI is "/row/2/clear" → substring(5) = "2/clear" → toInt() = 2
    int rowNum = server.uri().substring(5).toInt();
    if (rowNum < 0 || rowNum > 5) {
        server.send(400, "text/plain", "row must be 0-5");
        return;
    }
    board_wake();
    board_clear_row((uint8_t)rowNum);
    server.send(200, "text/plain", "ok");
}

// POST /display/demo
// Start or stop the demo mode.  Body: "on" to start, "off" to stop.
// While active, the display cycles through all presets every 30 seconds.
// Sending content via /row or /rows cancels demo mode automatically.
static void handleDemo() {
    if (rateLimited()) return;
    if (!authenticated()) return;
    String body = server.arg("plain");
    body.trim();
    // lowercase
    for (int i = 0; i < (int)body.length(); i++)
        body[i] = tolower(body[i]);

    if (body == "on") {
        g_demoMode   = true;
        g_bootStatus = false;
        g_demoIndex  = esp_random() % kPresetCount;
        g_demoLastMs = millis();
        board_wake();
        board_set_all(kPresets[g_demoIndex]);
        Serial.println("[HTTP] POST /display/demo  on");
        server.send(200, "text/plain", "ok");
    } else if (body == "off") {
        g_demoMode = false;
        Serial.println("[HTTP] POST /display/demo  off");
        server.send(200, "text/plain", "ok");
    } else {
        server.send(400, "application/json", "{\"error\":\"body must be 'on' or 'off'\"}");
    }
}

// Called by sio_client.cpp to toggle demo mode without duplicating the logic.
void triggerDemoMode(bool on) {
    g_demoMode   = on;
    g_bootStatus = false;
    if (on) {
        g_demoIndex  = esp_random() % kPresetCount;
        g_demoLastMs = millis();
        board_wake();
        board_set_all(kPresets[g_demoIndex]);
    }
    Serial.printf("[SIO] demo mode %s\n", on ? "on" : "off");
}

// Fire notification flash on any LED that has notify enabled.
// Called on new content from HTTP or Socket.IO.
void triggerContentNotify() {
    for (uint8_t i = 0; i < 2; i++)
        if (led_get_notify(i)) led_notify(i);
}

// Shared wake logic: restore display and replay animation.
static void triggerWake(const char* source) {
    Serial.printf("[WAKE] triggered by %s\n", source);
    board_wake();
    board_replay();
}

// POST /display/wake
// Wake the display and replay the current content via the split-flap animation.
static void handleWake() {
    if (rateLimited()) return;
    if (!authenticated()) return;
    triggerWake("API");
    server.send(200, "text/plain", "ok");
}

// POST /display/brightness
// Set display brightness 0-100 (percent). Applied immediately.
static void handleSetBrightness() {
    if (rateLimited()) return;
    if (!authenticated()) return;
    String body = server.arg("plain");
    body.trim();
    if (body.isEmpty()) {
        server.send(400, "application/json", "{\"error\":\"send brightness 0-100 as plain text\"}");
        return;
    }
    int pct = body.toInt();
    if (pct < 0 || pct > 100) {
        server.send(400, "application/json", "{\"error\":\"brightness must be 0-100\"}");
        return;
    }
    board_set_brightness((uint8_t)pct);
    Serial.printf("[HTTP] POST /display/brightness  %d%%\n", pct);
    server.send(200, "text/plain", "ok");
}

// POST /led/<1|2>/mode   body: on | off | flash | pulse
// POST /led/<1|2>/brightness   body: 0-100
// POST /led/<1|2>/notify   body: on | off
// GET  /led/status
static void handleLedMode() {
    if (rateLimited()) return;
    if (!authenticated()) return;
    // URI: /led/1/mode or /led/2/mode  → digit at position 5
    int n = server.uri().charAt(5) - '1';
    if (n < 0 || n > 1) { server.send(400, "text/plain", "led must be 1 or 2"); return; }
    String body = server.arg("plain"); body.trim();
    for (int i = 0; i < (int)body.length(); i++) body[i] = tolower(body[i]);
    LedMode mode = led_mode_from_str(body.c_str());
    led_set_mode((uint8_t)n, mode);
    Serial.printf("[HTTP] POST /led/%d/mode  %s\n", n + 1, led_mode_str(mode));
    server.send(200, "text/plain", "ok");
}

static void handleLedBrightness() {
    if (rateLimited()) return;
    if (!authenticated()) return;
    int n = server.uri().charAt(5) - '1';
    if (n < 0 || n > 1) { server.send(400, "text/plain", "led must be 1 or 2"); return; }
    String body = server.arg("plain"); body.trim();
    int pct = body.toInt();
    if (pct < 0 || pct > 100) { server.send(400, "text/plain", "brightness must be 0-100"); return; }
    led_set_brightness((uint8_t)n, (uint8_t)pct);
    Serial.printf("[HTTP] POST /led/%d/brightness  %d%%\n", n + 1, pct);
    server.send(200, "text/plain", "ok");
}

static void handleLedNotify() {
    if (rateLimited()) return;
    if (!authenticated()) return;
    int n = server.uri().charAt(5) - '1';
    if (n < 0 || n > 1) { server.send(400, "text/plain", "led must be 1 or 2"); return; }
    String body = server.arg("plain"); body.trim();
    for (int i = 0; i < (int)body.length(); i++) body[i] = tolower(body[i]);
    bool en = (body == "on" || body == "1" || body == "true");
    led_set_notify((uint8_t)n, en);
    Serial.printf("[HTTP] POST /led/%d/notify  %s\n", n + 1, en ? "on" : "off");
    server.send(200, "text/plain", "ok");
}

static void handleLedStatus() {
    if (rateLimited()) return;
    if (!authenticated()) return;
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"led1\":{\"mode\":\"%s\",\"brightness\":%u,\"notify\":%s},"
         "\"led2\":{\"mode\":\"%s\",\"brightness\":%u,\"notify\":%s}}",
        led_mode_str(led_get_mode(0)), led_get_brightness(0), led_get_notify(0) ? "true" : "false",
        led_mode_str(led_get_mode(1)), led_get_brightness(1), led_get_notify(1) ? "true" : "false");
    server.send(200, "application/json", buf);
}

// POST /display/timeout
// Override the idle power-off timeout for this session (not persisted across reboots).
// Body: plain integer number of minutes, e.g. "30" or "0" to disable power-off.
static void handleSetTimeout() {
    if (rateLimited()) return;
    if (!authenticated()) return;
    String body = server.arg("plain");
    body.trim();
    if (body.isEmpty()) {
        server.send(400, "application/json", "{\"error\":\"send minutes as plain text body\"}");
        return;
    }
    int mins = body.toInt();
    if (mins < 0 || mins > 1440) {
        server.send(400, "application/json", "{\"error\":\"minutes must be 0-1440\"}");
        return;
    }
    // 0 = disable power-off by setting an effectively unreachable timeout (49 days).
    uint32_t ms = mins == 0 ? 0xFFFFFFFFUL : (uint32_t)mins * 60UL * 1000UL;
    board_set_off_timeout_ms(ms);
    Serial.printf("[HTTP] POST /display/timeout  %d min\n", mins);
    server.send(200, "text/plain", "ok");
}

// GET /config/sio
// Returns the current Socket.IO configuration and connection state as JSON.
static void handleGetSioConfig() {
    if (rateLimited()) return;
    if (!authenticated()) return;
    String json = "{\"enabled\":";
    json += g_sio.enabled ? "true" : "false";
    json += ",\"host\":\"";
    json += g_sio.host;
    json += "\",\"port\":";
    json += g_sio.port;
    json += ",\"connected\":";
    json += sio_connected() ? "true" : "false";
    json += "}";
    server.send(200, "application/json", json);
}

// POST /config/sio
// Update Socket.IO config, persist to NVS, and reconnect immediately.
// Body (JSON): { "enabled": true|false, "host": "...", "port": N }
static void handlePostSioConfig() {
    if (rateLimited()) return;
    if (!authenticated()) return;
    String body = server.arg("plain");
    if (body.isEmpty()) {
        server.send(400, "application/json", "{\"error\":\"empty body\"}");
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        server.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }
    bool     enabled = doc["enabled"] | false;
    String   host    = doc["host"]    | "";
    uint16_t port    = doc["port"]    | 3000;
    host.trim();
    if (port == 0) port = 3000;

    // Persist
    sioSaveConfig(enabled, host.c_str(), port);
    g_sio.enabled = enabled;
    strncpy(g_sio.host, host.c_str(), sizeof(g_sio.host) - 1);
    g_sio.port = port;

    // Reconnect with new settings
    if (enabled && host.length() > 0) {
        sio_init(g_sio.host, g_sio.port, API_KEY);
    }

    server.send(200, "application/json", "{\"ok\":true}");
}

// GET /
// Serves the browser-based control UI. No authentication required so the
// page loads in any browser; the API key is entered inside the page itself.
static void handleUI() {
    if (rateLimited()) return;
    server.send_P(200, "text/html", kPageUI);
}

// POST /wifi/reset
// Clears stored WiFi credentials from NVS and reboots into the config portal.
static void handleWifiReset() {
    if (rateLimited()) return;
    if (!authenticated()) return;
    server.send(200, "text/plain", "WiFi credentials cleared. Rebooting…");
    delay(500);
    WiFiManager wm;
    wm.resetSettings();
    ESP.restart();
}

// Register all routes and start the server.
// collectHeaders must be called before server.begin() to make request headers
// readable inside handlers via server.header("Content-Type").
static void setupRoutes() {
    static const char* hdrs[] = {"Content-Type", "X-Api-Key"};
    server.collectHeaders(hdrs, 2);

    server.on("/",                  HTTP_GET,  handleUI);
    server.on("/wifi/reset",        HTTP_POST, handleWifiReset);
    server.on("/status",            HTTP_GET,  handleStatus);
    server.on("/rows",              HTTP_POST, handleSetAll);
    server.on("/display/brightness", HTTP_POST, handleSetBrightness);
    server.on("/display/timeout",   HTTP_POST, handleSetTimeout);
    server.on("/display/wake",      HTTP_POST, handleWake);
    server.on("/display/demo",      HTTP_POST, handleDemo);
    server.on("/config/sio",        HTTP_GET,  handleGetSioConfig);
    server.on("/config/sio",        HTTP_POST, handlePostSioConfig);
    server.on("/led/status",        HTTP_GET,  handleLedStatus);
    for (int i = 1; i <= 2; i++) {
        server.on(String("/led/") + i + "/mode",       HTTP_POST, handleLedMode);
        server.on(String("/led/") + i + "/brightness", HTTP_POST, handleLedBrightness);
        server.on(String("/led/") + i + "/notify",     HTTP_POST, handleLedNotify);
    }

    // Register one POST and one DELETE handler per row (rows 0–5).
    for (int i = 0; i <= 5; i++) {
        server.on(String("/row/") + i,            HTTP_POST,   handleSetRow);
        server.on(String("/row/") + i + "/clear", HTTP_DELETE, handleClearRow);
    }

    // Catch-all: return a 404 JSON error for any unregistered route.
    server.onNotFound([]() {
        String msg = "{\"error\":\"not found\",\"method\":\"";
        msg += server.method() == HTTP_GET    ? "GET"
             : server.method() == HTTP_POST   ? "POST"
             : server.method() == HTTP_DELETE ? "DELETE"
             : "OTHER";
        msg += "\",\"uri\":\"";
        msg += server.uri();
        msg += "\"}";
        Serial.printf("[HTTP] 404  %s  %s\n",
                      server.uri().c_str(),
                      server.method() == HTTP_GET ? "GET" : "POST/OTHER");
        server.send(404, "application/json", msg);
    });

    server.begin();
    Serial.println("  HTTP server started on port 80");
}

// ─── Boot status splash ───────────────────────────────────────────────────────
// Displayed on the board immediately after WiFi connects.  Shows hostname, IP,
// SSID, RSSI, Socket.IO state, and brightness.  Held for BOOT_STATUS_MS (60 s)
// then replaced by a random preset.  Cancelled immediately if real content
// arrives via the API, Socket.IO, or demo mode.
static void showBootStatus() {
    static char r0[22], r1[22], r2[22], r3[22], r4[22], r5[22];

    // Row 0: board hostname  e.g. "FLIPBOARD-3A4F"
    snprintf(r0, sizeof(r0), "%.21s", g_apName);

    // Row 1: IP address  e.g. "IP 192.168.1.42"
    snprintf(r1, sizeof(r1), "IP %.17s", WiFi.localIP().toString().c_str());

    // Row 2: WiFi SSID (truncated to 21 chars)
    snprintf(r2, sizeof(r2), "%.21s", WiFi.SSID().c_str());

    // Row 3: RSSI  e.g. "RSSI -62 DBM"
    snprintf(r3, sizeof(r3), "RSSI %d DBM", (int)WiFi.RSSI());

    // Row 4: Socket.IO  e.g. "SIO CONNECTED" / "SIO DISABLED"
    if (g_sio.enabled && strlen(g_sio.host) > 0) {
        snprintf(r4, sizeof(r4), "SIO %.16s", sio_connected() ? "CONNECTED" : "CONNECTING");
    } else {
        snprintf(r4, sizeof(r4), "SIO DISABLED");
    }

    // Row 5: brightness  e.g. "BRIGHTNESS 78"
    snprintf(r5, sizeof(r5), "BRIGHTNESS %u", board_get_brightness());

    const char* rows[6] = { r0, r1, r2, r3, r4, r5 };
    board_wake();
    board_set_all(rows);
    g_bootStatus        = true;
    g_bootStatusUntilMs = millis() + BOOT_STATUS_MS;
}

// ─── Serial status block ─────────────────────────────────────────────────────
// Printed every 5 s by loop() so you can watch WiFi and memory at a glance.
static void printStatus() {
    Serial.println("── status ──────────────────────────");
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("  WiFi      : %s\n",   WiFi.SSID().c_str());
        Serial.printf("  IP        : %s\n",   WiFi.localIP().toString().c_str());
        Serial.printf("  RSSI      : %d dBm (%d bars)\n",
                      WiFi.RSSI(), rssiToBars(WiFi.RSSI()));
    } else {
        Serial.printf("  WiFi      : disconnected (status %d)\n", WiFi.status());
    }
    if (g_sio.enabled) {
        if (sio_connected()) {
            Serial.printf("  Socket.IO : connected  %s:%d\n", g_sio.host, g_sio.port);
        } else {
            Serial.printf("  Socket.IO : connecting  %s:%d\n", g_sio.host, g_sio.port);
        }
    } else {
        Serial.println("  Socket.IO : disabled");
    }
    Serial.printf("  Brightness: %u%%\n",      board_get_brightness());
    Serial.printf("  LED 1     : %s  %u%%  notify=%s\n",
                  led_mode_str(led_get_mode(0)), led_get_brightness(0),
                  led_get_notify(0) ? "on" : "off");
    Serial.printf("  LED 2     : %s  %u%%  notify=%s\n",
                  led_mode_str(led_get_mode(1)), led_get_brightness(1),
                  led_get_notify(1) ? "on" : "off");
    Serial.printf("  Heap free : %lu bytes\n", (unsigned long)ESP.getFreeHeap());
    Serial.printf("  Heap min  : %lu bytes\n", (unsigned long)ESP.getMinFreeHeap());
    Serial.printf("  Uptime    : %lu s\n",     millis() / 1000);
    Serial.println("────────────────────────────────────");
}

// ─── Arduino entry points ────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);   // allow USB-CDC to enumerate before the first print

    Serial.println("\n── boot ─────────────────────────────");
    Serial.printf("  Chip  : %s rev%d\n", ESP.getChipModel(), ESP.getChipRevision());
    Serial.printf("  SDK   : %s\n",        ESP.getSdkVersion());
    Serial.printf("  Heap  : %lu B\n",     (unsigned long)ESP.getFreeHeap());
    Serial.printf("  SDA   : GPIO%d  SCL : GPIO%d\n", I2C_SDA_PIN, I2C_SCL_PIN);
    Serial.println("────────────────────────────────────");

    // Patch the boot splash row 1 with the actual chip model at runtime.
    static char chipRow[22];
    snprintf(chipRow, sizeof(chipRow), "%s BOARD", ESP.getChipModel());
    kBoot[1] = chipRow;

#ifdef WAKE_BTN_PIN
    pinMode(WAKE_BTN_PIN, INPUT_PULLUP);
    Serial.printf("  Wake button : GPIO%d\n", WAKE_BTN_PIN);
#endif
#ifdef WAKE_RADAR_PIN
    pinMode(WAKE_RADAR_PIN, INPUT);
    Serial.printf("  Wake radar  : GPIO%d\n", WAKE_RADAR_PIN);
#endif

    board_init();
    led_init();
    board_set_all(kBoot);
    board_settle();   // snap to final text immediately - loop() won't run during WiFiManager

    // Build a unique AP name from the lower 16 bits of the chip MAC so multiple
    // boards on the same network don't collide in the config portal.
    snprintf(g_apName, sizeof(g_apName), "FLIPBOARD-%04X",
             (uint16_t)(ESP.getEfuseMac() >> 32));

    // Pre-patch the portal splash with the AP name before the callback fires.
    kPortal[2] = g_apName;

    // Load Socket.IO config from NVS so the portal fields show current values.
    sioLoadConfig();

    WiFiManager wm;
    wm.setConfigPortalTimeout(180);   // reboot after 3 min if no one configures

    // ── Socket.IO custom parameters in the captive portal ────────────────────
    // These appear as extra fields below the WiFi credentials form.
    static char sioEnableBuf[4];
    static char sioHostBuf[64];
    static char sioPortBuf[8];
    snprintf(sioEnableBuf, sizeof(sioEnableBuf), "%s", g_sio.enabled ? "yes" : "no");
    snprintf(sioHostBuf,   sizeof(sioHostBuf),   "%s", g_sio.host);
    snprintf(sioPortBuf,   sizeof(sioPortBuf),   "%u", g_sio.port);

    WiFiManagerParameter paramEnable("sio_en",   "Socket.IO enable (yes/no)", sioEnableBuf, 3);
    WiFiManagerParameter paramHost  ("sio_host", "Socket.IO server host/IP",  sioHostBuf,  63);
    WiFiManagerParameter paramPort  ("sio_port", "Socket.IO port",            sioPortBuf,   7);
    wm.addParameter(&paramEnable);
    wm.addParameter(&paramHost);
    wm.addParameter(&paramPort);

    // Called when stored credentials fail and the AP/portal opens.
    wm.setAPCallback([](WiFiManager*) {
        Serial.printf("  No saved WiFi - portal open on AP \"%s\"\n", g_apName);
        board_set_all(kPortal);
        board_set_wifi_bars(0);
        board_settle();   // snap to final text - portal blocks loop() indefinitely
    });

    // Called each time WiFiManager saves new credentials — also save SIO config.
    wm.setSaveConfigCallback([&]() {
        Serial.println("  WiFi credentials saved - connecting…");
        String en   = paramEnable.getValue();
        String host = paramHost.getValue();
        String port = paramPort.getValue();
        en.trim(); en.toLowerCase();
        host.trim();
        bool enabled = (en == "yes" || en == "1" || en == "true");
        uint16_t p   = (uint16_t)port.toInt();
        if (p == 0) p = 3000;
        sioSaveConfig(enabled, host.c_str(), p);
        // Update live config so the connection attempt below uses fresh values.
        g_sio.enabled = enabled;
        strncpy(g_sio.host, host.c_str(), sizeof(g_sio.host) - 1);
        g_sio.port = p;
    });

    Serial.println("  Starting WiFiManager…");
    if (!wm.autoConnect(g_apName)) {
        // Portal timed out with no connection - restart and try again.
        Serial.println("  Config portal timed out - restarting");
        ESP.restart();
    }

    Serial.printf("  Connected!  SSID: %s  IP: %s\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    board_set_wifi_bars(rssiToBars(WiFi.RSSI()));
    setupRoutes();
    showBootStatus();   // display IP / hostname for 60 s, then a random preset

    if (g_sio.enabled && strlen(g_sio.host) > 0) {
        sio_init(g_sio.host, g_sio.port, API_KEY);
    } else {
        Serial.println("  Socket.IO disabled - skipping");
    }
}

void loop() {
    // Let the WebServer process any pending HTTP request.
    // This must run every loop() - long blocking calls will stall it.
    server.handleClient();

    // ── Wake sources ─────────────────────────────────────────────────────────
#ifdef WAKE_BTN_PIN
    {
        bool state = digitalRead(WAKE_BTN_PIN);   // HIGH = not pressed (pull-up)
        uint32_t now = millis();
        // Falling edge with 50 ms debounce
        if (!state && g_lastBtnState && (now - g_lastBtnMs) > 50) {
            g_lastBtnMs = now;
            triggerWake("button");
            triggerContentNotify();
        }
        g_lastBtnState = state;
    }
#endif
#ifdef WAKE_RADAR_PIN
    {
        bool state = digitalRead(WAKE_RADAR_PIN);  // HIGH = presence detected
        // Rising edge only — don't re-trigger while presence is held
        if (state && !g_lastRadarState) { triggerWake("radar"); triggerContentNotify(); }
        g_lastRadarState = state;
    }
#endif

    // Drive the flap animation and push the frame to the display.
    board_tick();
    led_tick();

    // Process incoming Socket.IO events (no-op if disabled).
    if (g_sio.enabled) sio_tick();

    uint32_t now = millis();

    // ── Boot status: transition to a random preset after 60 s ────────────────
    if (g_bootStatus && now >= g_bootStatusUntilMs) {
        g_bootStatus = false;
        g_demoIndex  = esp_random() % kPresetCount;
        board_wake();
        board_set_all(kPresets[g_demoIndex]);
    }

    // ── Demo mode: cycle presets every 30 s ──────────────────────────────────
    if (g_demoMode && (now - g_demoLastMs) >= DEMO_INTERVAL_MS) {
        g_demoLastMs = now;
        uint8_t next;
        do { next = esp_random() % kPresetCount; } while (next == g_demoIndex);
        g_demoIndex = next;
        board_wake();
        board_set_all(kPresets[g_demoIndex]);
    }

    // Every 5 s: refresh the WiFi signal icon and print a status block.
    if (now - g_lastWifiCheckMs >= 5000) {
        g_lastWifiCheckMs = now;
        board_set_wifi_bars(WiFi.status() == WL_CONNECTED
                            ? rssiToBars(WiFi.RSSI()) : 0);
        printStatus();
    }
}

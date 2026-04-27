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
#include "travel_board.h"
#include "secrets.h"

// ─── Security constants ───────────────────────────────────────────────────────
#define MAX_BODY_BYTES  512   // reject bodies larger than this
#define RATE_LIMIT_RPS  10    // max requests per second (global)

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
body{background:#0e0e0e;color:#ffaa00;font-family:monospace;margin:0;padding:16px;max-width:520px}
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
    <label>DISPLAY OFF TIMEOUT (MINUTES, 0 = NEVER)</label>
    <input type="text" id="timeout" placeholder="10" style="width:80px">
    <button type="button" class="primary" style="margin-left:8px" onclick="setTimeo()">SET</button>
  </div>
  <hr>
  <button class="danger" onclick="resetWifi()">RESET WIFI SETTINGS</button>
</div>

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
keyEl.value=localStorage.getItem('fb_key')||'';
function show(id,tab){
  document.querySelectorAll('.page').forEach(p=>p.classList.remove('active'));
  document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));
  document.getElementById(id).classList.add('active');
  tab.classList.add('active');
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

// ─── Demo content (shown once WiFi connects) ─────────────────────────────────
static const char* kDemo[6] = {
    "FL 101  LONDON",
    "FL 202  NEW YORK",
    "FL 303  PARIS",
    "FL 404  TOKYO",
    "FL 505  SYDNEY",
    "FL 606  DUBAI"
};

// ─── Module state ────────────────────────────────────────────────────────────
static WebServer server(80);

// Rate-limit state: count requests inside the current 1-second window.
static uint32_t g_rateWindowStart = 0;
static uint16_t g_rateCount       = 0;
static uint32_t  g_lastWifiCheckMs = 0;

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

// ─── HTTP handlers ───────────────────────────────────────────────────────────

// GET /status
// Returns a JSON snapshot of WiFi state and heap usage.
static void handleStatus() {
    if (rateLimited()) return;
    if (!authenticated()) return;
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"wifi\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"bars\":%d,"
        "\"free_heap\":%lu,\"min_heap\":%lu,\"uptime_s\":%lu}",
        WiFi.SSID().c_str(),
        WiFi.localIP().toString().c_str(),
        (int)WiFi.RSSI(),
        rssiToBars(WiFi.RSSI()),
        (unsigned long)ESP.getFreeHeap(),
        (unsigned long)ESP.getMinFreeHeap(),
        millis() / 1000);
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

    board_wake();
    board_set_row((uint8_t)rowNum, body.c_str());
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

    board_wake();
    board_set_all(texts);
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
    server.on("/display/timeout",   HTTP_POST, handleSetTimeout);

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

// ─── Serial status block ─────────────────────────────────────────────────────
// Printed every 5 s by loop() so you can watch WiFi and memory at a glance.
static void printStatus() {
    Serial.println("── status ──────────────────────────");
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("  WiFi   : %s\n",        WiFi.SSID().c_str());
        Serial.printf("  IP     : %s\n",        WiFi.localIP().toString().c_str());
        Serial.printf("  RSSI   : %d dBm (%d bars)\n",
                      WiFi.RSSI(), rssiToBars(WiFi.RSSI()));
    } else {
        Serial.printf("  WiFi   : disconnected (status %d)\n", WiFi.status());
    }
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

    board_init();
    board_set_all(kBoot);
    board_settle();   // snap to final text immediately - loop() won't run during WiFiManager

    // Build a unique AP name from the lower 16 bits of the chip MAC so multiple
    // boards on the same network don't collide in the config portal.
    static char apName[20];
    snprintf(apName, sizeof(apName), "FLIPBOARD-%04X",
             (uint16_t)(ESP.getEfuseMac() >> 32));

    // Pre-patch the portal splash with the AP name before the callback fires.
    kPortal[2] = apName;

    WiFiManager wm;
    wm.setConfigPortalTimeout(180);   // reboot after 3 min if no one configures

    // Called when stored credentials fail and the AP/portal opens.
    wm.setAPCallback([](WiFiManager*) {
        Serial.printf("  No saved WiFi - portal open on AP \"%s\"\n", kPortal[2]);
        board_set_all(kPortal);
        board_set_wifi_bars(0);
        board_settle();   // snap to final text - portal blocks loop() indefinitely
    });

    // Called each time WiFiManager saves new credentials.
    wm.setSaveConfigCallback([]() {
        Serial.println("  WiFi credentials saved - connecting…");
    });

    Serial.println("  Starting WiFiManager…");
    if (!wm.autoConnect(apName)) {
        // Portal timed out with no connection - restart and try again.
        Serial.println("  Config portal timed out - restarting");
        ESP.restart();
    }

    Serial.printf("  Connected!  SSID: %s  IP: %s\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    board_set_wifi_bars(rssiToBars(WiFi.RSSI()));
    board_set_all(kDemo);
    setupRoutes();
}

void loop() {
    // Let the WebServer process any pending HTTP request.
    // This must run every loop() - long blocking calls will stall it.
    server.handleClient();

    // Drive the flap animation and push the frame to the display.
    board_tick();

    // Every 5 s: refresh the WiFi signal icon and print a status block.
    uint32_t now = millis();
    if (now - g_lastWifiCheckMs >= 5000) {
        g_lastWifiCheckMs = now;
        board_set_wifi_bars(WiFi.status() == WL_CONNECTED
                            ? rssiToBars(WiFi.RSSI()) : 0);
        printStatus();
    }
}

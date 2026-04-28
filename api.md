# FlipBoard API Reference

- [Board HTTP API](#board-http-api) — direct REST calls to the ESP32
- [Socket.IO Server API](#socketio-server-api) — REST + real-time events via the Node.js hub

---

## Board HTTP API

### First-time WiFi setup

WiFi credentials are configured at runtime via a captive portal, not hardcoded.

On first boot (or after a WiFi reset):

1. The board shows **WIFI SETUP** and opens an access point named `FLIPBOARD-XXXX` (the suffix is unique to each board's MAC address).
2. Connect your phone or laptop to that AP.
3. A captive portal page opens automatically, or browse to `192.168.4.1`.
4. Choose your network, enter the password, and optionally configure the Socket.IO server.
5. The board connects, saves credentials to NVS, and starts normally.

On every subsequent boot the saved credentials are used automatically — no portal appears.

To switch networks, use the **Reset WiFi Settings** button in the web UI, or `POST /wifi/reset`.

---

### Authentication

All endpoints except `GET /` require the `X-Api-Key` header. Set the key in `src/secrets.h` and generate a strong value with:

```sh
openssl rand -hex 16
```

```sh
# Set once per shell session
KEY="your-api-key-here"
IP="192.168.1.42"
```

Requests without a valid key return `401`. The rate limit is 10 requests per second — excess returns `429`.

---

### GET /  — Web UI

Open `http://<ip>/` in any browser. No authentication needed to load the page.

Three tabs: **BOARD** (row inputs, brightness, wake, demo), **SETTINGS** (Socket.IO config), **API DOCS** (inline curl reference).

---

### GET /status

Returns a JSON snapshot of WiFi state, memory, brightness, and LED state.

```sh
curl http://$IP/status -H "X-Api-Key: $KEY"
```

```json
{
  "wifi":      "MyNetwork",
  "ip":        "192.168.1.42",
  "rssi":      -62,
  "bars":      2,
  "free_heap": 214320,
  "min_heap":  201440,
  "uptime_s":  47,
  "brightness": 78,
  "led1": { "mode": "pulse", "brightness": 100, "notify": true },
  "led2": { "mode": "off",   "brightness": 100, "notify": false }
}
```

---

### POST /row/\<0-5\>

Set a single row. Row 0 is the top row. Text is uppercased automatically.

```sh
# Recommended — plain text body
curl -X POST http://$IP/row/0 \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: text/plain" \
     -d "GATE CHANGE B12"

# Form field
curl -X POST http://$IP/row/3 \
     -H "X-Api-Key: $KEY" \
     -d "text=DELAYED+20+MIN"

# Bare body (no Content-Type)
curl -X POST http://$IP/row/5 \
     -H "X-Api-Key: $KEY" \
     -d "BOARDING NOW"
```

---

### POST /rows

Set all 6 rows in one request. Send `Content-Type: text/plain` with one line per row, newline-separated. Fewer than 6 lines clears the remaining rows.

```sh
curl -X POST http://$IP/rows \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: text/plain" \
     -d $'FL 101  LONDON\nFL 202  NEW YORK\nFL 303  PARIS\nFL 404  TOKYO\nFL 505  SYDNEY\nFL 606  DUBAI'
```

From a file (one row per line):

```sh
curl -X POST http://$IP/rows \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: text/plain" \
     --data-binary @rows.txt
```

---

### DELETE /row/\<0-5\>/clear

Animate a row to all spaces (blank it out).

```sh
curl -X DELETE http://$IP/row/2/clear \
     -H "X-Api-Key: $KEY"
```

---

### POST /display/brightness

Set display brightness 0–100 (percent). Applied immediately. Survives wake/dim cycles — the panel always restores this level when woken. Resets to ~78% on reboot.

```sh
curl -X POST http://$IP/display/brightness \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: text/plain" \
     -d "60"
```

---

### POST /display/wake

Wake the display from dim or off state and replay the current content through the full split-flap animation.

```sh
curl -X POST http://$IP/display/wake -H "X-Api-Key: $KEY"
```

---

### POST /display/demo

Start or stop demo mode. While active, the board cycles through all built-in presets every 30 seconds. Sending content via `/row` or `/rows` cancels demo mode automatically.

Body: `on` or `off`.

```sh
curl -X POST http://$IP/display/demo \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: text/plain" \
     -d "on"
```

---

### POST /display/timeout

Override the idle power-off timeout for this session. Not persisted across reboots — resets to 10 minutes on reboot.

Body: plain integer number of minutes. Use `0` to disable power-off entirely.

```sh
# Set to 30 minutes
curl -X POST http://$IP/display/timeout \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: text/plain" \
     -d "30"

# Never power off
curl -X POST http://$IP/display/timeout \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: text/plain" \
     -d "0"
```

---

### GET /led/status

Returns current mode, brightness, and notify flag for both LEDs.

```sh
curl http://$IP/led/status -H "X-Api-Key: $KEY"
```

```json
{
  "led1": { "mode": "pulse", "brightness": 100, "notify": true },
  "led2": { "mode": "off",   "brightness": 100, "notify": false }
}
```

---

### POST /led/\<1|2\>/mode

Set the mode for one LED. Modes: `on`, `off`, `flash`, `pulse`.

```sh
curl -X POST http://$IP/led/1/mode \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: text/plain" \
     -d "pulse"
```

---

### POST /led/\<1|2\>/brightness

Set LED brightness 0–100 (percent).

```sh
curl -X POST http://$IP/led/2/brightness \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: text/plain" \
     -d "60"
```

---

### POST /led/\<1|2\>/notify

Enable or disable notification flash on new content or wake. When enabled, the LED flashes for 3 seconds on each trigger, then returns to its previous mode.

Body: `on` or `off`.

```sh
curl -X POST http://$IP/led/1/notify \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: text/plain" \
     -d "on"
```

---

### GET /config/sio

Returns the current Socket.IO configuration and live connection state.

```sh
curl http://$IP/config/sio -H "X-Api-Key: $KEY"
```

```json
{
  "enabled":   true,
  "host":      "192.168.1.10",
  "port":      3500,
  "connected": true
}
```

---

### POST /config/sio

Update Socket.IO config, persist to NVS, and reconnect immediately. No reboot needed.

```sh
curl -X POST http://$IP/config/sio \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: application/json" \
     -d '{"enabled":true,"host":"192.168.1.10","port":3500}'
```

To disable:

```sh
curl -X POST http://$IP/config/sio \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: application/json" \
     -d '{"enabled":false,"host":"","port":3500}'
```

---

### POST /wifi/reset

Clears stored WiFi credentials from NVS and reboots into config portal mode. The board opens the `FLIPBOARD-XXXX` AP within a few seconds.

```sh
curl -X POST http://$IP/wifi/reset -H "X-Api-Key: $KEY"
```

---

### Error responses

| Status | Meaning |
|--------|---------|
| 401 | Missing or wrong `X-Api-Key` |
| 400 | Bad row number, bad range value, or empty body |
| 413 | Body exceeds 512 bytes |
| 429 | Rate limit exceeded (max 10 req/s) |
| 404 | Unknown route |

---

### Character set

The display supports `A–Z`, `0–9`, and `space - : / . !`  
Lowercase is uppercased automatically. Unsupported characters are stripped.

---

---

## Socket.IO Server API

The Node.js server (`server/server.js`) runs on port 3500 (override with the `PORT` env var). It brokers commands to one or more connected boards and exposes a live dashboard at `http://localhost:3500`.

```sh
cd server && npm install && node server.js
```

```sh
# Set once per shell session
KEY="your-api-key-here"
SRV="http://localhost:3500"
```

---

### Authentication

All server REST endpoints require `X-Api-Key: <key>`. The key is the same value compiled into the board's `src/secrets.h`. Each key forms an isolated tenant — you only see and control boards that authenticated with your key.

---

### GET /api/boards

Returns all boards currently connected under your key, including their live display state.

```sh
curl $SRV/api/boards -H "X-Api-Key: $KEY"
```

```json
{
  "count": 1,
  "boards": [
    {
      "id":          "abc12345",
      "ip":          "::ffff:192.168.1.42",
      "connectedAt": "2025-01-01T12:00:00.000Z",
      "rows":        ["FL 101  LONDON", "FL 202  NEW YORK", "", "", "", ""],
      "demo":        false,
      "leds": {
        "led1": { "mode": "pulse", "brightness": 100, "notify": true },
        "led2": { "mode": "off",   "brightness": 100, "notify": false }
      }
    }
  ]
}
```

The `rows` and `demo` fields reflect what the board last pushed — they update every time an animation completes.

---

### POST /api/cmd

General-purpose command. Omit `target` to broadcast to all boards under your key; include a socket `id` from `/api/boards` to address one board.

```sh
# Broadcast to all boards
curl -X POST $SRV/api/cmd \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: application/json" \
     -d '{"event":"set_row","data":{"row":0,"text":"HELLO"}}'

# Target one board
curl -X POST $SRV/api/cmd \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: application/json" \
     -d '{"event":"set_row","data":{"row":0,"text":"HELLO"},"target":"abc12345"}'
```

---

### POST /api/row

Set a single row on all boards under your key.

```sh
curl -X POST $SRV/api/row \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: application/json" \
     -d '{"row":0,"text":"GATE CHANGE B12"}'
```

---

### POST /api/rows

Set all 6 rows.

```sh
curl -X POST $SRV/api/rows \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: application/json" \
     -d '{"rows":["FL 101  LONDON","FL 202  NEW YORK","FL 303  PARIS","FL 404  TOKYO","FL 505  SYDNEY","FL 606  DUBAI"]}'
```

---

### POST /api/clear

Blank a single row.

```sh
curl -X POST $SRV/api/clear \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: application/json" \
     -d '{"row":3}'
```

---

### POST /api/wake

Wake all boards and replay their current content.

```sh
curl -X POST $SRV/api/wake -H "X-Api-Key: $KEY"
```

---

### POST /api/demo

Start or stop demo mode. `mode` must be `"on"` or `"off"`.

```sh
curl -X POST $SRV/api/demo \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: application/json" \
     -d '{"mode":"on"}'
```

---

### POST /api/timeout

Set the idle power-off timeout. Use `0` to disable power-off.

```sh
curl -X POST $SRV/api/timeout \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: application/json" \
     -d '{"minutes":30}'
```

---

### POST /api/brightness

Set display brightness 0–100 on all boards under your key.

```sh
curl -X POST $SRV/api/brightness \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: application/json" \
     -d '{"percent":60}'
```

---

### POST /api/led/mode

Set LED mode on all boards. `led`: `1` or `2`. `mode`: `on`, `off`, `flash`, `pulse`.

```sh
curl -X POST $SRV/api/led/mode \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: application/json" \
     -d '{"led":1,"mode":"flash"}'
```

---

### POST /api/led/brightness

Set LED brightness 0–100 on all boards.

```sh
curl -X POST $SRV/api/led/brightness \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: application/json" \
     -d '{"led":2,"percent":60}'
```

---

### Socket.IO events (server → board)

These are the events the server emits to boards. You can send any of them via `POST /api/cmd` with the matching `data` payload, or call the dedicated REST shortcuts above.

| Event | Payload | Board action |
|-------|---------|--------------|
| `set_row` | `{ row: 0-5, text: string }` | Animate one row |
| `set_all` | `{ rows: string[] }` | Animate all 6 rows |
| `clear_row` | `{ row: 0-5 }` | Animate row to spaces |
| `wake` | `{}` | Wake display, replay animation |
| `demo` | `{ mode: "on"\|"off" }` | Start / stop demo mode |
| `timeout` | `{ minutes: N }` | Set idle power-off timeout |
| `brightness` | `{ percent: 0-100 }` | Set display brightness |
| `led_mode` | `{ led: 1\|2, mode: string }` | Set LED mode |
| `led_brightness` | `{ led: 1\|2, percent: 0-100 }` | Set LED brightness |

---

### Socket.IO events (board → server)

The board pushes these automatically — you do not need to request them.

| Event | Payload | When sent |
|-------|---------|-----------|
| `auth` | `{ key: string }` | Immediately after Socket.IO connect |
| `state` | `{ rows, demo, leds }` | On (re)connect, and each time a flip animation completes |

The `state` payload shape:

```json
{
  "rows": ["FL 101  LONDON", "FL 202  NEW YORK", "", "", "", ""],
  "demo": false,
  "leds": {
    "led1": { "mode": "pulse", "brightness": 100, "notify": true },
    "led2": { "mode": "off",   "brightness": 100, "notify": false }
  }
}
```

The server stores the most recent `state` per board and returns it in `GET /api/boards`, so the dashboard always shows the settled display content rather than mid-animation characters.

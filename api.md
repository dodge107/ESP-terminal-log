# FlipBoard - HTTP API

## First-time WiFi setup

WiFi credentials are configured at runtime via a captive portal, not hardcoded.

On first boot (or after a WiFi reset):

1. The board shows **WIFI SETUP** and opens an access point named `FLIPBOARD-XXXX` (the suffix is unique to each board's MAC address).
2. Connect your phone or laptop to that AP.
3. A captive portal page opens automatically, or browse to `192.168.4.1`.
4. Choose your network, enter the password, and save.
5. The board connects, saves credentials to NVS, and starts normally.

On every subsequent boot the saved credentials are used automatically - no portal appears.

To switch networks, use the **Reset WiFi Settings** button in the web UI, or `POST /wifi/reset`.

---

## Authentication

All endpoints except `GET /` require the `X-Api-Key` header. Set the key in `src/secrets.h` and generate a strong value with:

```sh
openssl rand -hex 16
```

```sh
# Set once per shell session
KEY="your-api-key-here"
IP="192.168.100.23"
```

---

## Endpoints

### GET /  - Web UI

Open `http://<ip>/` in any browser. No authentication needed to load the page.

- **API Key** field - saved to `localStorage`, persists across browser sessions.
- **Six row inputs** - submits all rows at once via `POST /rows`.
- **Reset WiFi Settings** button - clears NVS credentials and reboots into setup mode.

---

### GET /status

Returns a JSON snapshot of WiFi state and memory.

```sh
curl http://$IP/status -H "X-Api-Key: $KEY"
```

```json
{
  "wifi": "MyNetwork",
  "ip": "192.168.100.23",
  "rssi": -62,
  "bars": 2,
  "free_heap": 214320,
  "min_heap": 201440,
  "uptime_s": 47
}
```

---

### POST /row/\<0-5\>

Set a single row. Row numbers are 0 (top) to 5 (bottom). Text is uppercased automatically.

```sh
# Recommended - plain text body
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

Set all 6 rows in one request. Body must be `Content-Type: text/plain` with one line per row, newline-separated. Fewer than 6 lines clears the remaining rows.

```sh
curl -X POST http://$IP/rows \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: text/plain" \
     -d $'FL 101  LONDON\nFL 202  NEW YORK\nFL 303  PARIS\nFL 404  TOKYO\nFL 505  SYDNEY\nFL 606  DUBAI'
```

From a file (6 lines, one per row):

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

### POST /wifi/reset

Clears stored WiFi credentials from NVS and reboots the board into config portal mode. The board will open the `FLIPBOARD-XXXX` AP within a few seconds.

```sh
curl -X POST http://$IP/wifi/reset -H "X-Api-Key: $KEY"
```

---

## Error responses

| Status | Meaning |
|--------|---------|
| 401 | Missing or wrong `X-Api-Key` |
| 400 | Bad row number or empty body |
| 413 | Body exceeds 512 bytes |
| 429 | Rate limit exceeded (max 10 req/s) |
| 404 | Unknown route |

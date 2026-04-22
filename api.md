# ESP Split-Flap Board — HTTP API

## First-time WiFi setup

WiFi credentials are **not** hardcoded — they are configured at runtime via a captive portal.

On first boot (or after credentials are cleared from NVS):

1. The board shows **"WIFI SETUP"** on the display and opens an access point named `FLIPBOARD-XXXX` (where `XXXX` is unique to each board).
2. Connect your phone or laptop to that AP.
3. A captive portal page opens automatically (or browse to `192.168.4.1`).
4. Choose your network, enter the password, and save.
5. The board connects, saves credentials to NVS, and starts normally.

On every subsequent boot the saved credentials are used automatically — no portal appears.

To re-run the portal (e.g. after moving to a new network), flash the firmware with a `wm.resetSettings()` call before `autoConnect()`, or erase NVS with `pio run --target erase`.

---

All requests require the `X-Api-Key` header set to the value defined in `src/secrets.h`.

```sh
# Convenient alias — set once per shell session
KEY="your-api-key-here"
IP="192.168.100.23"
```

---

## GET /status

Returns a JSON snapshot of WiFi state and memory.

```sh
curl http://$IP/status -H "X-Api-Key: $KEY"
```

Response:
```json
{
  "wifi": "Meraki",
  "ip": "192.168.100.23",
  "rssi": -62,
  "bars": 2,
  "free_heap": 214320,
  "min_heap": 201440,
  "uptime_s": 47
}
```

---

## POST /row/\<0-5\>

Set a single row. Row numbers are 0 (top) to 5 (bottom).

**Recommended — plain text body:**
```sh
curl -X POST http://$IP/row/0 \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: text/plain" \
     -d "GATE CHANGE B12"
```

**Form field style:**
```sh
curl -X POST http://$IP/row/3 \
     -H "X-Api-Key: $KEY" \
     -d "text=DELAYED+20+MIN"
```

**Bare body (no content-type):**
```sh
curl -X POST http://$IP/row/5 \
     -H "X-Api-Key: $KEY" \
     -d "BOARDING NOW"
```

Text is uppercased and truncated to fit the display automatically.

---

## POST /rows

Set all 6 rows in one request. Send one line per row, newline-separated.
Fewer than 6 lines leaves the remaining rows unchanged only if you send exactly 6 lines; missing lines are cleared.

```sh
curl -X POST http://$IP/rows \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: text/plain" \
     -d $'FL 101  LONDON\nFL 202  NEW YORK\nFL 303  PARIS\nFL 404  TOKYO\nFL 505  SYDNEY\nFL 606  DUBAI'
```

Or from a file (one line per row, 6 lines):
```sh
curl -X POST http://$IP/rows \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: text/plain" \
     --data-binary @rows.txt
```

---

## DELETE /row/\<0-5\>/clear

Animate a row to all spaces (blank it out).

```sh
curl -X DELETE http://$IP/row/2/clear \
     -H "X-Api-Key: $KEY"
```

---

## Error responses

| Status | Meaning |
|--------|---------|
| 401 | Missing or wrong `X-Api-Key` |
| 413 | Body exceeds 512 bytes |
| 429 | Rate limit exceeded (max 10 req/s) |
| 400 | Bad row number or empty body |
| 404 | Unknown route |

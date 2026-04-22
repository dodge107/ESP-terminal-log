# FlipBoard

FlipBoard is a split-flap departure board simulator for the ESP32, rendered on a 128×64 SSD1306 OLED. It recreates the look and feel of the mechanical Solari boards found in airports and train stations — each character slot cycles independently through the alphabet before locking onto its final glyph, with rows cascading one after another in the classic left-to-right, top-to-bottom sequence.

```
FL 101  LONDON
FL 202  NEW YORK
FL 303  PARIS
FL 404  TOKYO
FL 505  SYDNEY
FL 606  DUBAI
```

The display is divided into six fixed-width rows separated by dotted rules. A WiFi signal indicator sits in the top-right corner. Text on any row — or all rows at once — can be updated remotely over WiFi via a secured HTTP API or through a built-in browser UI served directly from the device.

### Design

The 128×64 pixel display is laid out with an 11-pixel row pitch: 6 pixels of glyph body, 2 pixels of clear space, a 1-pixel dotted separator, and 2 more pixels of clear space before the next row. This fills the display exactly across all six rows with a 2-pixel bottom margin. The font is `u8g2_font_5x7_tr` with a 6-pixel advance, giving up to 21 characters per row from a 2-pixel left margin to the right edge of the display.

The animation engine runs a `FlapSlot` state machine for every character position in the 6×25 grid. When a row is updated, each slot is assigned a flip count of at least one full alphabet lap (37 steps through `A–Z 0–9` plus space) plus the clockwise distance to the target character — so even a one-character change looks mechanically heavy. Slots within a row start 20 ms apart, and rows themselves start 120 ms apart, producing the characteristic cascade. The engine is fully non-blocking: `board_tick()` is called every `loop()` iteration with no `delay()` in the render path.

WiFi credentials are never hardcoded. On first boot the device opens a captive-portal access point; once credentials are saved to NVS the portal never appears again. The HTTP server requires an API key on all write endpoints, enforces a 10 req/s global rate limit, and rejects bodies over 512 bytes.

---

## Hardware

| Component | Details |
|-----------|---------|
| MCU | ESP32-C3 DevKitM-1 or ESP32-S3 DevKitC-1 |
| Display | SSD1306 OLED, 128×64 px, I²C |
| Interface | USB-C (native USB-CDC, no UART bridge needed) |

### Wiring

#### ESP32-C3 DevKitM-1

| OLED pin | ESP32-C3 pin |
|----------|-------------|
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO3 |
| SCL | GPIO4 |

#### ESP32-S3 DevKitC-1

| OLED pin | ESP32-S3 pin |
|----------|-------------|
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO8 |
| SCL | GPIO9 |

The I²C pins can be changed per-board in `platformio.ini` without touching any source files.

---

## Prerequisites

- [VS Code](https://code.visualstudio.com/) with the [PlatformIO extension](https://platformio.org/install/ide?install=vscode)
- Or the [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation/) standalone

---

## Setup

### 1. Clone the repository

```sh
git clone https://github.com/<your-username>/flipboard.git
cd flipboard
```

### 2. Create your secrets file

```sh
cp src/secrets.h.example src/secrets.h
```

`src/secrets.h` is listed in `.gitignore` and will never be committed.

### 3. Generate an API key

```sh
openssl rand -hex 16
```

Paste the output into `src/secrets.h`:

```cpp
#define API_KEY  "your-generated-key-here"
```

### 4. Build and flash

**VS Code:** Open the project folder, select the environment from the PlatformIO toolbar, and click Upload.

**CLI:**

```sh
# ESP32-C3
pio run -e esp32-c3-devkitm-1 --target upload

# ESP32-S3
pio run -e esp32-s3-devkitc-1 --target upload
```

### 5. Monitor serial output

```sh
pio device monitor -e esp32-c3-devkitm-1
```

Output is at 115200 baud. You will see boot stats, WiFi events, and a status block every 5 seconds.

---

## First-time WiFi setup

WiFi credentials are not hardcoded. On first boot the board opens a captive portal:

1. The display shows **WIFI SETUP** with the AP name (e.g. `FLIPBOARD-3A4F`).
2. Connect your phone or laptop to that access point.
3. A configuration page opens automatically, or browse to `192.168.4.1`.
4. Select your network, enter the password, and save.
5. The board connects, stores the credentials in NVS, and starts normally.

On every subsequent boot the saved credentials are used — no portal appears.

To switch networks, use the **Reset WiFi Settings** button in the web UI or call `POST /wifi/reset`.

---

## Web UI

Once connected, open `http://<board-ip>/` in any browser.

The UI has two tabs:

- **BOARD** — enter text for each row and click Update Board. The API key is saved to `localStorage` so you only enter it once per browser.
- **API DOCS** — inline reference for all HTTP endpoints with curl examples.

The board's IP address is printed in the serial monitor after connecting and is also returned by `GET /status`.

---

## REST API

All endpoints except `GET /` require the header `X-Api-Key: <your-key>`.

```sh
KEY="your-api-key-here"
IP="192.168.1.42"
```

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/` | Web UI (no auth required) |
| `GET` | `/status` | WiFi state and memory as JSON |
| `POST` | `/row/<0-5>` | Set one row |
| `POST` | `/rows` | Set all 6 rows (newline-delimited body) |
| `DELETE` | `/row/<0-5>/clear` | Blank a row |
| `POST` | `/wifi/reset` | Clear WiFi credentials and reboot |

### Examples

```sh
# Set a single row
curl -X POST http://$IP/row/0 \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: text/plain" \
     -d "GATE CHANGE B12"

# Set all rows at once
curl -X POST http://$IP/rows \
     -H "X-Api-Key: $KEY" \
     -H "Content-Type: text/plain" \
     -d $'FL 101  LONDON\nFL 202  NEW YORK\nFL 303  PARIS\nFL 404  TOKYO\nFL 505  SYDNEY\nFL 606  DUBAI'

# Get status
curl http://$IP/status -H "X-Api-Key: $KEY"

# Clear row 3
curl -X DELETE http://$IP/row/3/clear -H "X-Api-Key: $KEY"
```

The display accepts `A–Z`, `0–9`, and `space - : / . !`. Lowercase is uppercased automatically; unsupported characters are stripped.

---

## Changing I²C pins

Edit `platformio.ini` — no source changes needed:

```ini
[env:esp32-c3-devkitm-1]
build_flags =
    ${env.build_flags}
    -DI2C_SDA_PIN=3
    -DI2C_SCL_PIN=4
```

---

## Project structure

```
src/
  main.cpp          — WiFi, HTTP server, route handlers, web UI HTML
  travel_board.cpp  — Display driver, split-flap animation engine
  travel_board.h    — Public board API
  secrets.h         — API key (gitignored, create from secrets.h.example)
  secrets.h.example — Template to copy and fill in
platformio.ini      — Build environments for C3 and S3
api.md              — Full API reference
```

---

## Security

### What is implemented

| Measure | Detail |
|---------|--------|
| API key authentication | All write endpoints require `X-Api-Key` header. Requests without a valid key return `401`. |
| Credentials out of source control | `secrets.h` is gitignored. WiFi credentials are stored in device NVS, never in code. |
| Rate limiting | Max 10 requests per second globally. Excess requests return `429`. |
| Body size limit | Request bodies over 512 bytes are rejected with `413`. |
| WiFiManager portal timeout | The setup AP closes and the board reboots after 3 minutes if unconfigured. |

### Known risks and missing mitigations

**No HTTPS / TLS**
All traffic is plain HTTP on port 80. The API key and all row content are transmitted in cleartext and can be read by anyone on the same network segment with a packet capture tool (e.g. Wireshark). The ESP32 Arduino stack does support `WiFiClientSecure` and `WebServerSecure`, but TLS requires a certificate, adds significant flash and RAM overhead, and is not yet implemented here.

*Mitigations short of full TLS:* restrict the board to a trusted VLAN or IoT network segment; use a reverse proxy (nginx, Caddy) on a local server to terminate TLS and forward to the board over the LAN.

**API key sent as a plain HTTP header**
Because there is no TLS, the API key is visible in every request. Anyone who captures one request has the key permanently. Key rotation requires reflashing the firmware.

**No per-IP rate limiting**
The 10 req/s limit is global. A single client can exhaust the allowance, denying other clients. A per-IP counter would require dynamic memory allocation that is awkward on this stack.

**WiFiManager portal has no password**
During the 3-minute setup window, the `FLIPBOARD-XXXX` access point is open with no WPA2 password. Anyone nearby can connect and submit credentials. This is a deliberate usability trade-off in the WiFiManager library.

**API key stored in plaintext flash**
The compiled key sits in the ESP32's flash memory. Anyone with physical access and a flash dumper (e.g. `esptool.py read_flash`) can extract it. ESP32 supports encrypted flash via the eFuse-based flash encryption feature, but it is not enabled here and requires care to avoid permanently bricking the device.

**No CSRF protection on the web UI**
The browser UI submits to `POST /rows` via JavaScript `fetch()`. A malicious page open in another tab on the same browser could make the same request if it knows the board's IP and the API key (which is in `localStorage`). Standard CSRF tokens are not implemented.

**`localStorage` key storage**
The API key is saved to `localStorage` for convenience. Any JavaScript running on the same origin can read it. Since this page is served directly from the board (no CDN, no third-party scripts), the risk is low — but it is not a hardened credential store.

**No authentication on `GET /`**
The web UI page loads without a key so the form is accessible in a browser. The actual data-writing requests still require the key, but the existence and IP of the device are exposed to anyone on the network.

**No network-level access control**
The HTTP server listens on all interfaces on port 80. There is no firewall, IP allowlist, or VPN requirement. Anyone reachable on the same network can attempt requests.

### Recommended deployment posture

- Place the board on a dedicated IoT VLAN with no internet access and no cross-VLAN routing to untrusted devices.
- Treat the API key as a low-value shared secret — sufficient to prevent casual interference, not sufficient to protect sensitive data.
- Do not expose port 80 to the internet directly.

// sio_client.cpp
// Minimal ws:// WebSocket + Engine.IO v4 + Socket.IO v4 client.
// Uses only WiFiClient (plain TCP) — no TLS, no external WebSocket library.
//
// Frame layout (RFC 6455, client→server frames are masked):
//   Byte 0 : FIN(1) | RSV(3) | opcode(4)
//   Byte 1 : MASK(1) | len7(7)   — len7==126 → next 2 bytes; 127 → next 8 bytes
//   [mask key : 4 bytes if MASK set]
//   [payload, XOR'd with mask key in 4-byte rotation]
//
// Engine.IO v4 packet types (first character of text payload):
//   '0' OPEN | '2' PING | '3' PONG | '4' MESSAGE
//
// Socket.IO v4 packet types (second character when EIO type == '4'):
//   '0' CONNECT | '2' EVENT

#include "sio_client.h"
#include "travel_board.h"
#include "led_indicator.h"
#include <WiFi.h>
#include <ArduinoJson.h>

// ─── Constants ────────────────────────────────────────────────────────────────
#define RECONNECT_MS   5000UL
#define RECV_BUF_SIZE  512          // max incoming frame payload we handle
#define WS_OPCODE_TEXT  0x01
#define WS_OPCODE_PING  0x09
#define WS_OPCODE_PONG  0x0A
#define WS_OPCODE_CLOSE 0x08
#define WS_FIN          0x80
#define WS_MASK         0x80

// ─── State ────────────────────────────────────────────────────────────────────
enum WsState { WS_IDLE, WS_HANDSHAKE, WS_OPEN };

static WiFiClient  g_tcp;
static WsState     g_state      = WS_IDLE;
static bool        g_sioConn    = false;   // Socket.IO namespace confirmed
static uint32_t    g_lastTryMs  = 0;
static char        g_host[64]   = {};
static uint16_t    g_port       = 3000;
static char        g_path[64]   = "/socket.io";
static char        g_apiKey[64] = {};
static char        g_recvBuf[RECV_BUF_SIZE + 1];

// Handshake accumulator — filled across multiple sio_tick() calls (non-blocking).
static char     g_hsBuf[256];
static uint16_t g_hsLen      = 0;
static uint32_t g_hsDeadline = 0;

// ─── Forward declarations ─────────────────────────────────────────────────────
static void     doConnect();
static void     sendFrame(const char* data, size_t len, uint8_t opcode = WS_OPCODE_TEXT);
static void     sendText(const char* data) { sendFrame(data, strlen(data)); }
static bool     readHandshake();
static bool     readFrame();
static void     handleTextFrame(const char* payload, size_t len);
static void     dispatch(const char* event, JsonVariantConst data);

// ─── Base64 (used for the Sec-WebSocket-Key header) ──────────────────────────
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64encode(const uint8_t* in, size_t len, char* out) {
    size_t i = 0, o = 0;
    for (; i + 2 < len; i += 3) {
        out[o++] = B64[in[i]   >> 2];
        out[o++] = B64[((in[i] & 3) << 4) | (in[i+1] >> 4)];
        out[o++] = B64[((in[i+1] & 0xF) << 2) | (in[i+2] >> 6)];
        out[o++] = B64[in[i+2] & 0x3F];
    }
    if (i < len) {
        out[o++] = B64[in[i] >> 2];
        if (i + 1 < len) {
            out[o++] = B64[((in[i] & 3) << 4) | (in[i+1] >> 4)];
            out[o++] = B64[(in[i+1] & 0xF) << 2];
        } else {
            out[o++] = B64[(in[i] & 3) << 4];
            out[o++] = '=';
        }
        out[o++] = '=';
    }
    out[o] = '\0';
}

// ─── Public API ───────────────────────────────────────────────────────────────

void sio_init(const char* host, uint16_t port, const char* apiKey, const char* path) {
    strncpy(g_host,   host,   sizeof(g_host)   - 1);
    strncpy(g_apiKey, apiKey, sizeof(g_apiKey) - 1);
    g_port = port;
    strncpy(g_path, path, sizeof(g_path) - 1);
    doConnect();
}

void sio_tick() {
    switch (g_state) {

    case WS_IDLE: {
        uint32_t now = millis();
        if (now - g_lastTryMs >= RECONNECT_MS && !board_is_animating()) {
            g_lastTryMs = now;
            doConnect();
        }
        break;
    }

    case WS_HANDSHAKE:
        if (g_tcp.connected()) {
            readHandshake();   // non-blocking; completes across multiple ticks
        } else {
            Serial.println("[SIO] TCP dropped during handshake");
            g_state = WS_IDLE;
        }
        break;

    case WS_OPEN:
        if (!g_tcp.connected()) {
            Serial.println("[SIO] disconnected");
            g_state   = WS_IDLE;
            g_sioConn = false;
            break;
        }
        while (g_tcp.available()) {
            if (!readFrame()) break;
        }
        break;
    }
}

bool sio_connected() { return g_sioConn; }

bool sio_send(const char* event, const char* jsonData) {
    if (!g_sioConn) return false;
    // Format: 42["event",{...}]
    char buf[512];
    snprintf(buf, sizeof(buf), "42[\"%s\",%s]", event, jsonData ? jsonData : "{}");
    sendText(buf);
    return true;
}

// ─── Connection ───────────────────────────────────────────────────────────────

static void doConnect() {
    g_tcp.stop();
    g_state   = WS_IDLE;
    g_sioConn = false;

    Serial.printf("[SIO] connecting to %s:%d\n", g_host, g_port);
    if (!g_tcp.connect(g_host, g_port)) {
        Serial.println("[SIO] TCP connect failed");
        g_lastTryMs = millis();
        return;
    }

    // Build a random 16-byte key and base64-encode it.
    uint8_t rawKey[16];
    for (int i = 0; i < 16; i++) rawKey[i] = (uint8_t)(esp_random() & 0xFF);
    char key[25];
    b64encode(rawKey, 16, key);

    // Send HTTP/1.1 upgrade request.
    String req;
    req.reserve(256);
    req  = "GET ";
    req += g_path;
    req += "/?EIO=4&transport=websocket HTTP/1.1\r\n";
    req += "Host: ";
    req += g_host;
    req += ":";
    req += g_port;
    req += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: ";
    req += key;
    req += "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    g_tcp.print(req);

    g_hsLen      = 0;
    g_hsDeadline = millis() + 2000;
    g_state      = WS_HANDSHAKE;
}

// ─── Handshake ────────────────────────────────────────────────────────────────

// Called every sio_tick() while in WS_HANDSHAKE state.
// Accumulates response bytes into g_hsBuf across multiple calls — no delay().
// Returns true once the upgrade response is fully received and validated.
static bool readHandshake() {
    // Drain whatever bytes arrived since the last call.
    while (g_tcp.available() && g_hsLen < sizeof(g_hsBuf) - 1) {
        g_hsBuf[g_hsLen++] = (char)g_tcp.read();
        // HTTP headers end with \r\n\r\n — stop once we see that.
        if (g_hsLen >= 4 &&
            g_hsBuf[g_hsLen-4] == '\r' && g_hsBuf[g_hsLen-3] == '\n' &&
            g_hsBuf[g_hsLen-2] == '\r' && g_hsBuf[g_hsLen-1] == '\n') {
            goto done;
        }
    }
    // Deadline guards against a server that stops sending mid-headers.
    if (millis() > g_hsDeadline) {
        Serial.println("[SIO] handshake timeout");
        g_tcp.stop();
        g_state     = WS_IDLE;
        g_lastTryMs = millis();
        g_hsLen     = 0;
        return false;
    }
    return false;   // not complete yet — come back next tick

done:
    g_hsBuf[g_hsLen] = '\0';
    g_hsLen = 0;    // reset for next connection attempt
    if (!strstr(g_hsBuf, "101")) {
        Serial.printf("[SIO] bad handshake: %.80s\n", g_hsBuf);
        g_tcp.stop();
        g_state     = WS_IDLE;
        g_lastTryMs = millis();
        return false;
    }
    Serial.println("[SIO] WebSocket open — sending Socket.IO CONNECT");
    g_state = WS_OPEN;
    sendText("40");   // Socket.IO CONNECT for default namespace
    return true;
}

// ─── WebSocket frame send ─────────────────────────────────────────────────────

static void sendFrame(const char* data, size_t len, uint8_t opcode) {
    if (!g_tcp.connected()) return;

    uint8_t header[10];
    size_t  hlen = 0;
    header[hlen++] = WS_FIN | opcode;

    if (len <= 125) {
        header[hlen++] = WS_MASK | (uint8_t)len;
    } else if (len <= 65535) {
        header[hlen++] = WS_MASK | 126;
        header[hlen++] = (len >> 8) & 0xFF;
        header[hlen++] = len & 0xFF;
    } else {
        return;   // we never send frames this large
    }

    // 4-byte masking key
    uint8_t mask[4];
    uint32_t r = esp_random();
    mask[0] = r & 0xFF; mask[1] = (r >> 8) & 0xFF;
    mask[2] = (r >> 16) & 0xFF; mask[3] = (r >> 24) & 0xFF;
    memcpy(header + hlen, mask, 4);
    hlen += 4;

    g_tcp.write(header, hlen);

    // Write payload masked in chunks to avoid large stack buffers.
    const size_t CHUNK = 64;
    uint8_t buf[CHUNK];
    for (size_t i = 0; i < len; i += CHUNK) {
        size_t n = min(CHUNK, len - i);
        for (size_t j = 0; j < n; j++)
            buf[j] = (uint8_t)data[i + j] ^ mask[(i + j) & 3];
        g_tcp.write(buf, n);
    }
}

// ─── WebSocket frame receive ──────────────────────────────────────────────────

static bool readFrame() {
    if (g_tcp.available() < 2) return false;

    uint8_t b0 = g_tcp.read();
    uint8_t b1 = g_tcp.read();

    uint8_t  opcode  = b0 & 0x0F;
    size_t   payLen  = b1 & 0x7F;

    if (payLen == 126) {
        if (g_tcp.available() < 2) return false;
        payLen = ((uint16_t)g_tcp.read() << 8) | g_tcp.read();
    } else if (payLen == 127) {
        // We won't receive 8-byte length frames; skip.
        return false;
    }

    if (payLen > RECV_BUF_SIZE) {
        // Drain and discard oversized frame.
        for (size_t i = 0; i < payLen; i++) {
            while (!g_tcp.available()) delay(1);
            g_tcp.read();
        }
        return true;
    }

    // Wait for full payload.
    uint32_t deadline = millis() + 2000;
    while ((size_t)g_tcp.available() < payLen && millis() < deadline) delay(1);
    if ((size_t)g_tcp.available() < payLen) return false;

    for (size_t i = 0; i < payLen; i++) g_recvBuf[i] = g_tcp.read();
    g_recvBuf[payLen] = '\0';

    switch (opcode) {
    case WS_OPCODE_TEXT:
        handleTextFrame(g_recvBuf, payLen);
        break;
    case WS_OPCODE_PING:
        sendFrame(g_recvBuf, payLen, WS_OPCODE_PONG);
        break;
    case WS_OPCODE_CLOSE:
        Serial.println("[SIO] server sent close");
        g_tcp.stop();
        g_state   = WS_IDLE;
        g_sioConn = false;
        return false;
    default:
        break;
    }
    return true;
}

// ─── Engine.IO / Socket.IO parsing ───────────────────────────────────────────

static void handleTextFrame(const char* p, size_t len) {
    if (len < 1) return;

    switch (p[0]) {
    case '0':   // Engine.IO OPEN
        break;
    case '2':   // Engine.IO PING — reply PONG
        sendText("3");
        break;
    case '4':   // Engine.IO MESSAGE
        if (len < 2) break;
        if (p[1] == '0') {
            g_sioConn = true;
            Serial.println("[SIO] namespace connected");
            // Identify this board to the server so it joins the right tenant room.
            char authMsg[96];
            snprintf(authMsg, sizeof(authMsg), "42[\"auth\",{\"key\":\"%s\"}]", g_apiKey);
            sendText(authMsg);
        } else if (p[1] == '2') {
            // Socket.IO EVENT — parse JSON array starting after "42"
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, p + 2, len - 2);
            if (err) { Serial.printf("[SIO] JSON err: %s\n", err.c_str()); break; }
            if (!doc.is<JsonArray>() || doc.as<JsonArray>().size() < 1) break;
            const char* event = doc[0];
            dispatch(event, doc[1]);
        }
        break;
    default:
        break;
    }
}

// ─── Event dispatcher ─────────────────────────────────────────────────────────

static void dispatch(const char* event, JsonVariantConst data) {
    Serial.printf("[SIO] event: %s\n", event);

    extern void triggerContentNotify();

    if (strcmp(event, "set_row") == 0) {
        int row = data["row"] | -1;
        const char* text = data["text"] | "";
        if (row < 0 || row > 5) return;
        board_wake();
        board_set_row((uint8_t)row, text);
        triggerContentNotify();
        extern void pushBoardState(); pushBoardState();

    } else if (strcmp(event, "set_all") == 0) {
        JsonArrayConst rows = data["rows"].as<JsonArrayConst>();
        if (rows.isNull()) return;
        static char bufs[6][22];
        const char* ptrs[6] = {};
        uint8_t n = 0;
        for (JsonVariantConst v : rows) {
            if (n >= 6) break;
            const char* s = v.as<const char*>();
            strncpy(bufs[n], s ? s : "", 21);
            bufs[n][21] = '\0';
            ptrs[n] = bufs[n];
            n++;
        }
        board_wake();
        board_set_all(ptrs);
        triggerContentNotify();
        extern void pushBoardState(); pushBoardState();

    } else if (strcmp(event, "clear_row") == 0) {
        int row = data["row"] | -1;
        if (row < 0 || row > 5) return;
        board_wake();
        board_clear_row((uint8_t)row);

    } else if (strcmp(event, "wake") == 0) {
        board_wake();
        board_replay();

    } else if (strcmp(event, "demo") == 0) {
        extern void triggerDemoMode(bool on);
        const char* mode = data["mode"] | "off";
        triggerDemoMode(strcmp(mode, "on") == 0);

    } else if (strcmp(event, "timeout") == 0) {
        int mins = data["minutes"] | -1;
        if (mins < 0 || mins > 1440) return;
        uint32_t ms = mins == 0 ? 0xFFFFFFFFUL : (uint32_t)mins * 60UL * 1000UL;
        board_set_off_timeout_ms(ms);

    } else if (strcmp(event, "brightness") == 0) {
        int pct = data["percent"] | -1;
        if (pct < 0 || pct > 100) return;
        board_set_brightness((uint8_t)pct);

    } else if (strcmp(event, "led_mode") == 0) {
        int led = (data["led"] | -1);
        if (led < 1 || led > 2) return;
        const char* modeStr = data["mode"] | "off";
        led_set_mode((uint8_t)(led - 1), led_mode_from_str(modeStr));

    } else if (strcmp(event, "led_brightness") == 0) {
        int led = (data["led"] | -1);
        if (led < 1 || led > 2) return;
        int pct = data["percent"] | -1;
        if (pct < 0 || pct > 100) return;
        led_set_brightness((uint8_t)(led - 1), (uint8_t)pct);

    }
}

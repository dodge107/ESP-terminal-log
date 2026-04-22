// main.cpp — Application entry point
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

// ─── Boot splash (shown while connecting) ────────────────────────────────────
static const char* kBoot[6] = {
    "INITIALIZING",
    "",          // filled at runtime with chip model — see setup()
    "v1",
    "",
    "",
    ""
};

// ─── Config portal splash (shown while waiting for WiFi setup) ───────────────
// Row 2 is patched at runtime with the AP SSID — see setup().
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
// Body retrieval strategy — tried in order until one succeeds:
//
//   1. arg("plain")  — populated when Content-Type is text/plain.
//      Used by:  curl -H "Content-Type: text/plain" -d "HELLO WORLD"
//
//   2. arg("text")   — populated when the form body contains "text=VALUE".
//      Used by:  curl -d "text=HELLO+WORLD"
//
//   3. Join all arg names — curl's default form-encoding sends the raw body
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
        server.send(400, "application/json", "{\"error\":\"empty body — send Content-Type: text/plain\"}");
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
    board_clear_row((uint8_t)rowNum);
    server.send(200, "text/plain", "ok");
}

// Register all routes and start the server.
// collectHeaders must be called before server.begin() to make request headers
// readable inside handlers via server.header("Content-Type").
static void setupRoutes() {
    static const char* hdrs[] = {"Content-Type", "X-Api-Key"};
    server.collectHeaders(hdrs, 2);

    server.on("/status", HTTP_GET,  handleStatus);
    server.on("/rows",   HTTP_POST, handleSetAll);

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
        Serial.printf("  No saved WiFi — portal open on AP \"%s\"\n", kPortal[2]);
        board_set_all(kPortal);
        board_set_wifi_bars(0);
    });

    // Called each time WiFiManager saves new credentials.
    wm.setSaveConfigCallback([]() {
        Serial.println("  WiFi credentials saved — connecting…");
    });

    Serial.println("  Starting WiFiManager…");
    if (!wm.autoConnect(apName)) {
        // Portal timed out with no connection — restart and try again.
        Serial.println("  Config portal timed out — restarting");
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
    // This must run every loop() — long blocking calls will stall it.
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

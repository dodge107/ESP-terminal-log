#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "travel_board.h"

#define WIFI_SSID "Meraki"
#define WIFI_PASS "@homewithtoast"

static const char* kBoot[6] = {
    "INITIALIZING",
    "ESP32-C3 BOARD",
    "",
    "",
    "",
    ""
};

static const char* kDemo[6] = {
    "FL 101  LONDON",
    "FL 202  NEW YORK",
    "FL 303  PARIS",
    "FL 404  TOKYO",
    "FL 505  SYDNEY",
    "FL 606  DUBAI"
};

static WebServer server(80);
static uint32_t  g_lastWifiCheckMs = 0;
static bool      g_demoShown       = false;

static uint8_t rssiToBars(int32_t rssi) {
    if (rssi >= -55) return 3;
    if (rssi >= -70) return 2;
    if (rssi >= -85) return 1;
    return 0;
}

static void handleStatus() {
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

static void handleSetRow() {
    String path = server.uri();          // /row/0 .. /row/5
    int rowNum = path.substring(5).toInt();
    if (rowNum < 0 || rowNum > 5) {
        server.send(400, "text/plain", "row must be 0-5");
        return;
    }
    String body = server.arg("plain");
    board_set_row((uint8_t)rowNum, body.c_str());
    Serial.printf("  HTTP POST /row/%d  \"%s\"\n", rowNum, body.c_str());
    server.send(200, "text/plain", "ok");
}

static void handleClearRow() {
    int rowNum = server.uri().substring(12).toInt(); // /row/0/clear
    if (rowNum < 0 || rowNum > 5) { server.send(400, "text/plain", "row must be 0-5"); return; }
    board_clear_row((uint8_t)rowNum);
    server.send(200, "text/plain", "ok");
}

static void setupRoutes() {
    server.on("/status", HTTP_GET, handleStatus);
    for (int i = 0; i <= 5; i++) {
        server.on(String("/row/") + i,              HTTP_POST,   handleSetRow);
        server.on(String("/row/") + i + "/clear",   HTTP_DELETE, handleClearRow);
    }
    server.begin();
    Serial.println("  HTTP server started on port 80");
}

static void printStatus() {
    Serial.println("── status ──────────────────────────");
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("  WiFi   : connected to %s\n", WiFi.SSID().c_str());
        Serial.printf("  IP     : %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("  RSSI   : %d dBm (%d bars)\n", WiFi.RSSI(), rssiToBars(WiFi.RSSI()));
    } else {
        Serial.printf("  WiFi   : disconnected (status %d)\n", WiFi.status());
    }
    Serial.printf("  Free heap  : %lu bytes\n", (unsigned long)ESP.getFreeHeap());
    Serial.printf("  Min heap   : %lu bytes\n", (unsigned long)ESP.getMinFreeHeap());
    Serial.printf("  Uptime     : %lu s\n", millis() / 1000);
    Serial.println("────────────────────────────────────");
}

void setup() {
    Serial.begin(115200);
    delay(500); // let USB CDC enumerate before first print
    Serial.println("\n\n── boot ─────────────────────────────");
    Serial.printf("  SDK     : %s\n", ESP.getSdkVersion());
    Serial.printf("  Heap    : %lu bytes\n", (unsigned long)ESP.getFreeHeap());
    Serial.printf("  Chip    : ESP32-C3 rev%d\n", ESP.getChipRevision());
    Serial.println("────────────────────────────────────");
    Serial.printf("  Connecting to %s ...\n", WIFI_SSID);

    board_init();
    board_set_all(kBoot);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void loop() {
    server.handleClient();
    board_tick();

    uint32_t now = millis();
    if (now - g_lastWifiCheckMs >= 5000) {
        g_lastWifiCheckMs = now;

        bool connected = WiFi.status() == WL_CONNECTED;
        board_set_wifi_bars(connected ? rssiToBars(WiFi.RSSI()) : 0);

        if (connected && !g_demoShown) {
            g_demoShown = true;
            Serial.printf("  Connected! IP: %s\n", WiFi.localIP().toString().c_str());
            board_set_all(kDemo);
            setupRoutes();
        }

        printStatus();
    }
}

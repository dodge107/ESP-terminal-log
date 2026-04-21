#include <Arduino.h>
#include <WiFi.h>
#include "travel_board.h"

#define WIFI_SSID "YOUR_SSID"
#define WIFI_PASS "YOUR_PASSWORD"

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

static uint32_t g_lastWifiCheckMs = 0;
static bool     g_demoShown       = false;

static uint8_t rssiToBars(int32_t rssi) {
    if (rssi >= -55) return 3;
    if (rssi >= -70) return 2;
    if (rssi >= -85) return 1;
    return 0;
}

void setup() {
    Serial.begin(115200);
    board_init();
    board_set_all(kBoot);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void loop() {
    board_tick();

    uint32_t now = millis();
    if (now - g_lastWifiCheckMs >= 5000) {
        g_lastWifiCheckMs = now;
        if (WiFi.status() == WL_CONNECTED) {
            board_set_wifi_bars(rssiToBars(WiFi.RSSI()));
            if (!g_demoShown) {
                g_demoShown = true;
                board_set_all(kDemo);
            }
        } else {
            board_set_wifi_bars(0);
        }
    }
}

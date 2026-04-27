#pragma once
#include <Arduino.h>

// ─── Socket.IO client ─────────────────────────────────────────────────────────
// Connects to a plain ws:// Socket.IO v4 server (Engine.IO v4).
// Call sio_init() once after WiFi connects, sio_tick() every loop().
//
// Supported inbound events (server → board):
//   set_row   { "row": 0-5, "text": "..." }
//   set_all   { "rows": ["...", ...] }        up to 6 elements
//   clear_row { "row": 0-5 }
//   wake      {}
//   demo      { "mode": "on"|"off" }
//   timeout   { "minutes": N }

void sio_init(const char* host, uint16_t port, const char* apiKey, const char* path = "/socket.io");
void sio_tick();
bool sio_connected();

#include "travel_board.h"
#include <U8g2lib.h>
#include <Wire.h>

// ESP32-C3 default I2C pins. Adjust for your board (XIAO: SDA=6, SCL=7).
#define I2C_SDA 8
#define I2C_SCL 9

#define NUM_ROWS  6
#define NUM_COLS  20
#define LEFT_PAD  2

// Space comes first so trailing spaces cycle through the full alphabet.
static const char ALPHABET[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const uint8_t ALPHA_LEN = 27;

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, I2C_SCL, I2C_SDA);

static uint16_t g_flipMs   = 60;
static uint8_t  g_wifiBars = 0;

typedef struct {
    char     current;
    char     target;
    uint16_t flipsLeft;
    uint32_t lastFlipMs;
    uint16_t startDelayMs;
    bool     done;
} FlapSlot;

typedef struct {
    FlapSlot slots[NUM_COLS];
    uint32_t startMs;
    bool     active;
} BoardRow;

static BoardRow g_rows[NUM_ROWS];

static uint8_t alphaIdx(char c) {
    for (uint8_t i = 0; i < ALPHA_LEN; i++) {
        if (ALPHABET[i] == c) return i;
    }
    return 0;
}

static char sanitize(char c) {
    if (c >= 'a' && c <= 'z') c -= 32;
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == ' ' || c == '-' || c == ':' || c == '/' || c == '.' || c == '!')
        return c;
    return 0;
}

static void startRow(uint8_t ri, const char* text, uint32_t startMs) {
    BoardRow& row = g_rows[ri];
    row.startMs   = startMs;
    row.active    = true;

    char sanitized[NUM_COLS];
    uint8_t out = 0;
    if (text) {
        for (uint8_t i = 0; text[i] && out < NUM_COLS; i++) {
            char s = sanitize(text[i]);
            if (s) sanitized[out++] = s;
        }
    }
    while (out < NUM_COLS) sanitized[out++] = ' ';

    for (uint8_t j = 0; j < NUM_COLS; j++) {
        FlapSlot& sl  = row.slots[j];
        char     tgt  = sanitized[j];
        uint8_t  ci   = alphaIdx(sl.current);
        uint8_t  ti   = alphaIdx(tgt);
        uint8_t  dist = (ti - ci + ALPHA_LEN) % ALPHA_LEN;

        sl.target       = tgt;
        sl.flipsLeft    = ALPHA_LEN + dist; // ≥1 full lap so even short edits look mechanical
        sl.startDelayMs = (uint16_t)j * 20;
        sl.lastFlipMs   = 0;
        sl.done         = false;
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void board_init() {
    Wire.begin(I2C_SDA, I2C_SCL);
    u8g2.begin();
    u8g2.setFont(u8g2_font_5x7_tr);

    for (uint8_t i = 0; i < NUM_ROWS; i++) {
        g_rows[i].active  = false;
        g_rows[i].startMs = 0;
        for (uint8_t j = 0; j < NUM_COLS; j++) {
            FlapSlot& sl    = g_rows[i].slots[j];
            sl.current      = ' ';
            sl.target       = ' ';
            sl.done         = true;
            sl.flipsLeft    = 0;
            sl.lastFlipMs   = 0;
            sl.startDelayMs = 0;
        }
    }
}

void board_set_row(uint8_t row, const char* text) {
    if (row >= NUM_ROWS) return;
    startRow(row, text, millis());
}

void board_clear_row(uint8_t row) {
    board_set_row(row, "");
}

void board_set_all(const char* texts[6]) {
    uint32_t now = millis();
    for (uint8_t i = 0; i < NUM_ROWS; i++)
        startRow(i, texts ? texts[i] : nullptr, now + (uint32_t)i * 120);
}

void board_set_speed_ms(uint16_t ms) {
    g_flipMs = ms;
}

void board_set_wifi_bars(uint8_t bars) {
    g_wifiBars = bars > 3 ? 3 : bars;
}

// ── Drawing helpers ───────────────────────────────────────────────────────────

static void drawWifi() {
    // Clear 8×7 box so row-1 text can't bleed into the icon area.
    u8g2.setDrawColor(0);
    u8g2.drawBox(119, 0, 8, 7);
    u8g2.setDrawColor(1);

    // Concentric arcs centred at x=123, dot at y=4.
    //   large arc  y=1 : x=120, x=126
    //   medium arc y=2 : x=121, x=125
    //   small arc  y=3 : x=122, x=124
    //   dot        y=4 : x=123
    u8g2.drawPixel(123, 4);

    if (g_wifiBars >= 1) { u8g2.drawPixel(122, 3); u8g2.drawPixel(124, 3); }
    if (g_wifiBars >= 2) { u8g2.drawPixel(121, 2); u8g2.drawPixel(125, 2); }
    if (g_wifiBars >= 3) { u8g2.drawPixel(120, 1); u8g2.drawPixel(126, 1); }
}

// ── Main tick ─────────────────────────────────────────────────────────────────

void board_tick() {
    uint32_t now = millis();

    // Advance animation state
    for (uint8_t i = 0; i < NUM_ROWS; i++) {
        BoardRow& row = g_rows[i];
        if (!row.active) continue;

        bool allDone = true;
        for (uint8_t j = 0; j < NUM_COLS; j++) {
            FlapSlot& sl = row.slots[j];
            if (sl.done) continue;
            allDone = false;

            if (now < row.startMs) continue;
            if ((now - row.startMs) < sl.startDelayMs) continue;
            if ((now - sl.lastFlipMs) < g_flipMs) continue;

            sl.current    = ALPHABET[(alphaIdx(sl.current) + 1) % ALPHA_LEN];
            sl.lastFlipMs = now;

            if (--sl.flipsLeft == 0) {
                sl.current = sl.target;
                sl.done    = true;
            }
        }
        if (allDone) row.active = false;
    }

    // Redraw full buffer
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.setDrawColor(1);

    for (uint8_t i = 0; i < NUM_ROWS; i++) {
        char buf[NUM_COLS + 1];
        for (uint8_t j = 0; j < NUM_COLS; j++) buf[j] = g_rows[i].slots[j].current;
        buf[NUM_COLS] = '\0';
        u8g2.drawStr(LEFT_PAD, 7 + i * 10, buf);

        // Dotted separator below every row except the last
        if (i < NUM_ROWS - 1) {
            uint8_t sy = 8 + i * 10;
            for (uint8_t x = 2; x <= 125; x += 3) u8g2.drawPixel(x, sy);
        }
    }

    drawWifi();
    u8g2.sendBuffer();
}

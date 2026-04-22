// travel_board.cpp — Split-flap notification board driver
//
// Display: SSD1306 OLED, 128×64 px, monochrome, I²C.
// MCU    : ESP32-C3. SDA = GPIO3, SCL = GPIO4 (adjust at the top if needed).
//
// ── Coordinate system ────────────────────────────────────────────────────────
//   Origin (0,0) = top-left corner of the display.
//   x increases rightward  : 0 … 127  (128 pixels wide).
//   y increases DOWNWARD   : 0 … 63   (64 pixels tall).
//   U8g2 drawStr(x, y, s)  : y is the BASELINE — the bottom of uppercase body.
//
// ── Font metrics for u8g2_font_5x7_tr ────────────────────────────────────────
//   Cell width   : 6 px  (5 px glyph + 1 px inter-character gap).
//   Cell height  : 7 px total (fits inside the 10 px row pitch with margin).
//   Ascent       : 6 px above the baseline  → glyph pixels  y−6 … y.
//   Descent      : 0 px for uppercase letters (none of A–Z have descenders).
//   Max columns  : floor((128 − LEFT_PAD) / 6) = floor(126/6) = 21 chars,
//                  placing the last glyph pixel at x = 2 + 21×6 − 1 = 127.
//
// ── Row layout (10 px pitch, 6 rows) ─────────────────────────────────────────
//   TOP_OFFSET = 4 px  → free space above the first row's glyph.
//   ROW_PITCH  = 10 px → distance between successive baselines.
//   ASCENT     = 6 px  → pixels above baseline (= height of uppercase glyph).
//
//   Row i baseline  y = ASCENT + i × ROW_PITCH = 6 + i×11.
//   Separator       y = baseline + SEP_GAP     = 8 + i×11.
//
//   ROW_PITCH = 11  →  6 px glyph + 2 px gap + 1 px sep + 2 px gap = 11 px.
//   2 px of clear space on each side of every separator.
//
//   y= 0  ─── display top / row 0 glyph top ────────────────────────────
//   y= 1   }
//   y= 2   }
//   y= 3   }  row 0 glyph body (6 px, x=2…127)
//   y= 4   }
//   y= 5   }
//   y= 6  ─── row 0 baseline        drawStr y = 6
//   y= 7   }  2 px gap
//   y= 8  ─── separator dots ────── every 3 px, x=2…127
//   y= 9   }  2 px gap
//   y=10  ─── row 1 glyph top ──────────────────────────────────────────
//   y=11   }
//   y=12   }
//   y=13   }  row 1 glyph body
//   y=14   }
//   y=15   }
//   y=17  ─── row 1 baseline        drawStr y = 17
//   y=19  ─── separator dots
//             … pattern repeats (pitch = 11 px) …
//   y=50  ─── row 5 glyph top
//   y=61  ─── row 5 baseline        drawStr y = 61
//   y=62   }  2 px bottom margin
//   y=63  ─── display bottom edge ─────────────────────────────────────
//
// ── WiFi status icon (top-right, drawn last) ─────────────────────────────────
//   Concentric arcs centred at x=123, occupying x=120…126, y=1…5.
//   A 9×12 px black box (x=119…127, y=0…11) is painted first to erase any
//   row-0 text that bleeds into the icon area.
//
//     bars=0 :  only dot  (x=123, y=5)
//     bars=1 :  + small arc  (x=122 & x=124, y=4)
//     bars=2 :  + medium arc (x=121 & x=125, y=3)
//     bars=3 :  + large arc  (x=120 & x=126, y=2)  — strongest signal
//
// ── Alphabet ─────────────────────────────────────────────────────────────────
//   27 characters: ' ' first, then A–Z.
//   Space-first ensures that trailing-space slots cycle the full alphabet
//   before landing on ' ', giving the same mechanical feel as letter slots.

#include "travel_board.h"
#include <U8g2lib.h>
#include <Wire.h>

// ─── Hardware ────────────────────────────────────────────────────────────────
#define I2C_SDA     3          // Adjust for your ESP32-C3 board
#define I2C_SCL     4

// ─── Layout constants ────────────────────────────────────────────────────────
#define NUM_ROWS    6
#define NUM_COLS    21         // 21 × 6 px + 2 px left pad = 128 px (fills display)
#define LEFT_PAD    2          // px gap left of column 0
#define TOP_OFFSET  0          // no top margin — row 0 glyph starts at y=0
#define ASCENT      6          // px above baseline for this font's uppercase
#define ROW_PITCH   11         // px between baselines: 6 glyph + 2 gap + 1 sep + 2 gap
#define SEP_GAP     2          // px of clear space on each side of the separator

// Convenient macros — computed entirely at compile time.
// BASELINE(0) = 0+6 = 6.   BASELINE(5) = 6+5×11 = 61.
// Glyph top row 0 : y = 6−6 = 0  → flush with display top.
// Glyph btm row 5 : y = 61       → bottom margin = 63−61 = 2 px.
#define BASELINE(row)   (TOP_OFFSET + ASCENT + (row) * ROW_PITCH)   // 6 + row×11
#define SEPARATOR(row)  (BASELINE(row) + SEP_GAP)                    // 8 + row×11

// ─── Alphabet ────────────────────────────────────────────────────────────────
// Space is index 0. Slots start on ' ' so every position does a full lap.
static const char    ALPHABET[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const uint8_t ALPHA_LEN  = 27;

// ─── U8g2 display object ─────────────────────────────────────────────────────
// Full-buffer mode: we rebuild the entire 1 024-byte frame buffer every tick
// and push it to the display in one shot.  HW_I2C uses the ESP32-C3 hardware
// I²C peripheral at 400 kHz.
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(
    U8G2_R0,           // no rotation
    U8X8_PIN_NONE,     // no reset pin
    I2C_SCL,
    I2C_SDA
);

// ─── Module-level state ──────────────────────────────────────────────────────
static uint16_t g_flipMs   = 60;    // ms between consecutive glyph advances
static uint8_t  g_wifiBars = 0;     // 0–3; updated by the caller

// ─── Per-slot animation state ────────────────────────────────────────────────
// One FlapSlot per character position in the grid.
typedef struct {
    char     current;       // glyph currently shown on the display
    char     target;        // glyph this slot will land on when done
    uint16_t flipsLeft;     // remaining alphabet steps before locking
    uint32_t lastFlipMs;    // millis() at the last glyph advance
    uint16_t startDelayMs;  // ms after row.startMs before this slot begins
    bool     done;          // true once current == target and animation stopped
} FlapSlot;

// ─── Per-row state ───────────────────────────────────────────────────────────
typedef struct {
    FlapSlot slots[NUM_COLS];
    uint32_t startMs;   // millis() when this row's animation was triggered
    bool     active;    // false when all slots have finished
} BoardRow;

static BoardRow g_rows[NUM_ROWS];

// ─── Helpers ─────────────────────────────────────────────────────────────────

// Returns the index of c inside ALPHABET[], defaulting to 0 (space) if missing.
static uint8_t alphaIdx(char c) {
    for (uint8_t i = 0; i < ALPHA_LEN; i++) {
        if (ALPHABET[i] == c) return i;
    }
    return 0;
}

// Converts c to uppercase and returns it if it is in the allowed character set
// [A–Z, 0–9, space, - : / . !]; otherwise returns 0 (strip this character).
static char sanitize(char c) {
    if (c >= 'a' && c <= 'z') c -= 32;   // to uppercase
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == ' ' || c == '-' || c == ':' ||
        c == '/' || c == '.'  || c == '!')
        return c;
    return 0;
}

// Initialise (or restart) one row's animation.
//   ri      : row index 0–5
//   text    : source string (may be nullptr → treated as "")
//   startMs : millis() value at which the row's animation clock starts;
//             pass millis() + i*120 from board_set_all() to get staggered start
static void startRow(uint8_t ri, const char* text, uint32_t startMs) {
    BoardRow& row = g_rows[ri];
    row.startMs   = startMs;
    row.active    = true;

    // Build the sanitized, uppercased, space-padded target string.
    char sanitized[NUM_COLS];
    uint8_t out = 0;
    if (text) {
        for (uint8_t i = 0; text[i] && out < NUM_COLS; i++) {
            char s = sanitize(text[i]);
            if (s) sanitized[out++] = s;     // only keep allowed characters
        }
    }
    while (out < NUM_COLS) sanitized[out++] = ' '; // pad to full width with spaces

    // Program each slot to flip from its current glyph to the new target.
    for (uint8_t j = 0; j < NUM_COLS; j++) {
        FlapSlot& sl = row.slots[j];
        char    tgt  = sanitized[j];

        uint8_t ci   = alphaIdx(sl.current);
        uint8_t ti   = alphaIdx(tgt);
        // Clockwise distance through the 27-character ring from current → target.
        uint8_t dist = (ti - ci + ALPHA_LEN) % ALPHA_LEN;

        sl.target    = tgt;
        // Force at least one full alphabet lap (27 steps) plus the remaining
        // distance, so even a single-character change looks mechanically heavy.
        sl.flipsLeft    = ALPHA_LEN + dist;
        // Each character column starts 20 ms after the previous one,
        // creating the left-to-right cascade effect within a row.
        sl.startDelayMs = (uint16_t)j * 20;
        sl.lastFlipMs   = 0;   // 0 guarantees the first flip fires immediately
        sl.done         = false;
    }
}

// ─── Public API ──────────────────────────────────────────────────────────────

void board_init() {
    Wire.begin(I2C_SDA, I2C_SCL);
    u8g2.begin();
    u8g2.setFont(u8g2_font_5x7_tr);

    // All slots start as settled spaces so the display is blank on boot.
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

// Start the flap animation on a single row.  The other rows are unaffected.
void board_set_row(uint8_t row, const char* text) {
    if (row >= NUM_ROWS) return;
    startRow(row, text, millis());
}

// Animate a row to all-spaces (visually clears it).
void board_clear_row(uint8_t row) {
    board_set_row(row, "");
}

// Set all 6 rows at once.  Rows start 120 ms apart (row 0 first, row 5 last),
// matching the classic Solari departure-board cascade.
void board_set_all(const char* texts[6]) {
    uint32_t now = millis();
    for (uint8_t i = 0; i < NUM_ROWS; i++)
        startRow(i, texts ? texts[i] : nullptr, now + (uint32_t)i * 120);
}

void board_set_speed_ms(uint16_t ms) { g_flipMs = ms; }

void board_set_wifi_bars(uint8_t bars) { g_wifiBars = bars > 3 ? 3 : bars; }

// ─── Drawing: WiFi icon ───────────────────────────────────────────────────────
// The icon lives in the top-right corner.  It is drawn AFTER the row text so
// it paints over any characters that overflow into the reserved area.
//
// Icon layout (x=120…126, y=2…5):
//
//   x:  119 120 121 122 123 124 125 126 127   (← all cleared to black first)
//   y=2:      [L] [ ] [ ] [ ] [ ] [L]         large arc  — shown when bars≥3
//   y=3:          [M] [ ] [ ] [M]             medium arc — shown when bars≥2
//   y=4:              [S] [ ] [S]  wait no...
//
// Corrected pixel map (centred at x=123):
//
//   y=2 : x=120 ●                     ● x=126   (large arc)
//   y=3 :     x=121 ●           ● x=125         (medium arc)
//   y=4 :         x=122 ●   ● x=124             (small arc)
//   y=5 :               x=123 ●                 (dot — always shown)
//
static void drawWifi() {
    // ── Step 1: erase the reserved zone ──────────────────────────────────────
    // Paint a 9×12 black rectangle covering x=119…127, y=0…11.
    // This is wider and taller than the icon itself so no row-0 glyph pixels
    // (which extend to y=10 with the new TOP_OFFSET=4 layout) survive behind it.
    u8g2.setDrawColor(0);                              // colour 0 = black (erase)
    u8g2.drawBox(119, 0, 9, SEPARATOR(0) + 1);        // clears glyph + 2 px gap + separator row
    u8g2.setDrawColor(1);              // colour 1 = white (draw)

    // ── Step 2: draw icon pixels ─────────────────────────────────────────────
    // Dot — always present regardless of signal strength.
    u8g2.drawPixel(123, 5);

    // Small arc — one bar of signal (x=122 and x=124 at y=4).
    if (g_wifiBars >= 1) {
        u8g2.drawPixel(122, 4);
        u8g2.drawPixel(124, 4);
    }
    // Medium arc — two bars (x=121 and x=125 at y=3).
    if (g_wifiBars >= 2) {
        u8g2.drawPixel(121, 3);
        u8g2.drawPixel(125, 3);
    }
    // Large arc — three bars / full signal (x=120 and x=126 at y=2).
    if (g_wifiBars >= 3) {
        u8g2.drawPixel(120, 2);
        u8g2.drawPixel(126, 2);
    }
}

// ─── Main tick ───────────────────────────────────────────────────────────────
// Call this every loop().  It advances the animation state for all active rows,
// then redraws the entire frame buffer and pushes it to the display.
// No delay() is used; the function is fully non-blocking.
void board_tick() {
    uint32_t now = millis();

    // ── Phase 1: advance animation ────────────────────────────────────────────
    for (uint8_t i = 0; i < NUM_ROWS; i++) {
        BoardRow& row = g_rows[i];
        if (!row.active) continue;   // row is static — nothing to do

        bool allDone = true;
        for (uint8_t j = 0; j < NUM_COLS; j++) {
            FlapSlot& sl = row.slots[j];
            if (sl.done) continue;           // slot already locked
            allDone = false;

            // Gate 1: the row itself may not have started yet (stagger).
            if (now < row.startMs) continue;

            // Gate 2: within the row, each column starts 20 ms after the previous.
            if ((now - row.startMs) < sl.startDelayMs) continue;

            // Gate 3: enforce the flip interval between successive glyph changes.
            if ((now - sl.lastFlipMs) < g_flipMs) continue;

            // Advance one step clockwise through the alphabet ring.
            sl.current    = ALPHABET[(alphaIdx(sl.current) + 1) % ALPHA_LEN];
            sl.lastFlipMs = now;

            // When the counter reaches zero the slot locks on its target.
            if (--sl.flipsLeft == 0) {
                sl.current = sl.target;   // guarantee exact target (no rounding)
                sl.done    = true;
            }
        }
        if (allDone) row.active = false;  // all slots settled — mark row idle
    }

    // ── Phase 2: redraw full frame ────────────────────────────────────────────
    // Clear the 1 024-byte internal buffer to all-black, then repaint everything.
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.setDrawColor(1);              // white-on-black

    for (uint8_t i = 0; i < NUM_ROWS; i++) {

        // Build a null-terminated string from this row's 21 character slots.
        char buf[NUM_COLS + 1];
        for (uint8_t j = 0; j < NUM_COLS; j++) buf[j] = g_rows[i].slots[j].current;
        buf[NUM_COLS] = '\0';

        // Draw the row text.
        //   x = LEFT_PAD (2 px from left edge).
        //   y = BASELINE(i) = 10 + i×10 (baseline, not glyph top).
        //   Glyphs occupy y−6 … y  i.e. pixels y=4…10 for row 0, y=14…20 for row 1, etc.
        //   The full 21-character string reaches x=2 + 21×6 − 1 = 127 (right edge).
        u8g2.drawStr(LEFT_PAD, BASELINE(i), buf);

        // Draw the dotted separator line below this row (not after the last row).
        //   y = SEPARATOR(i) = BASELINE(i) + 1 = 11 + i×10.
        //   Dots every 3 px from x=2 to x=127, leaving a 1 px gap between dots.
        //   This creates a dashed rule: ·  ·  ·  ·  ·  ·  ·  ·  ·  ·  ·  ·  ·
        if (i < NUM_ROWS - 1) {
            const uint8_t sy = SEPARATOR(i);
            for (uint8_t x = LEFT_PAD; x <= 127; x += 4)
                u8g2.drawPixel(x, sy);
        }
    }

    // Draw the WiFi icon last so it sits on top of any row-0 text overflow.
    drawWifi();

    // Push the completed frame buffer to the SSD1306 over I²C (~22 ms at 400 kHz).
    u8g2.sendBuffer();
}

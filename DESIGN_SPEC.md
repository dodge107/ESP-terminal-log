# DESIGN SPEC — ESP32 Travel Board (Variant 1 · "Dense Grid")

A 6-row split-flap notification board on a 128×64 SSD1306 OLED, driven by
an ESP32 using the U8g2 library. Each letter animates independently — cycling
A → Z — until it locks onto its final character, Solari airport-board style.

---

## 1. Hardware

| Item        | Spec                                                            |
| ----------- | --------------------------------------------------------------- |
| MCU         | ESP32 (any dev board with I²C pins, e.g. ESP32-WROOM-32 DevKit) |
| Display     | 0.96" SSD1306 OLED, 128×64, monochrome, I²C                     |
| Bus         | I²C @ 400 kHz                                                   |
| I²C address | `0x3C` (verify with scanner if not detected)                    |
| Wiring      | VCC→3V3, GND→GND, SDA→GPIO21, SCL→GPIO22                        |

## 2. Software stack

- **Framework:** Arduino core for ESP32 (PlatformIO or Arduino IDE).
- **Library:** `U8g2` by olikraus (latest release).
- **Mode:** Full buffer — `U8G2_SSD1306_128X64_NONAME_F_HW_I2C`.
  Full buffer is required because we redraw every frame of the animation.

## 3. Layout — Variant 1 "Dense Grid"

Pixel-accurate targets. Origin (0,0) = top-left.

```
 ┌────────────────────────────────────────────────────────────┐ y=0
 │ ROW 1  text, up to 20 chars, 5×7 font           ▒ wifi ▒   │
 │ · · · · · · · · · · · · · · · · · · · · · · · · · · · · · │ y=9  (dotted)
 │ ROW 2                                                      │
 │ · · · · · · · · · · · · · · · · · · · · · · · · · · · · · │ y=19
 │ ROW 3                                                      │
 │ · · · · · · · · · · · · · · · · · · · · · · · · · · · · · │ y=29
 │ ROW 4                                                      │
 │ · · · · · · · · · · · · · · · · · · · · · · · · · · · · · │ y=39
 │ ROW 5                                                      │
 │ · · · · · · · · · · · · · · · · · · · · · · · · · · · · · │ y=49
 │ ROW 6                                                      │
 └────────────────────────────────────────────────────────────┘ y=63
```

**Dimensions**

- Row pitch: **10 px** (7 px glyph + 1 px descender space + 1 px separator + 1 px gap).
- Row text origin (U8g2 baseline): `y = 7 + i * 10` for `i ∈ 0..5`.
- Left padding: **2 px**.
- Dotted separator between rows `i` and `i+1` at `y = 8 + i*10`, stepping every **3 px** from `x = 2` to `x = 125`.
- No header. No border.

**Font**

- `u8g2_font_5x7_tr` (or `u8g2_font_5x8_tr` if 5×7 reads too tight).
- Character advance: 6 px → **20 characters max per row** (120 px / 6).
- Uppercase only. Characters outside `[A–Z, 0–9, space, - : / . !]` are stripped.

**Wi-Fi glyph**

- Position: top-right, **x = 120, y = 1**, size 7×5 px.
- Three arcs + dot; bars reflect RSSI (0/1/2/3 bars).
- The glyph is drawn **after** row 1 text, and the 8×7 bounding box is cleared
  first so it doesn't collide with long strings.

## 4. Flap animation

### Model

Each row owns N character slots (N = 20). Each slot has:

```c
typedef struct {
    char current;      // currently displayed glyph
    char target;       // what it should land on
    uint16_t flipsLeft; // remaining ticks before lock
    uint32_t lastFlipMs;
    uint16_t startDelayMs; // time from row start until this slot begins
    bool done;
} FlapSlot;
```

Alphabet (27 chars): `" ABCDEFGHIJKLMNOPQRSTUVWXYZ"` — **space comes first**
so trailing spaces flip through the whole alphabet before resting.

### Timing

| Parameter        | Default | Range       | Notes                                    |
| ---------------- | ------- | ----------- | ---------------------------------------- |
| Flip interval    | 60 ms   | 20 – 200 ms | Time between successive glyph changes.   |
| Row start stagger| 120 ms  | fixed       | Row `i` starts at `i * 120 ms`.          |
| Char stagger     | 20 ms   | fixed       | Within a row, slot `j` starts at `j*20ms`|
| Min flips        | 1 full lap + distance to target, so even tiny changes look mechanical |

### Algorithm (per frame, ~30 fps)

```
now = millis()
for each row i:
    for each slot j:
        if slot.done: continue
        elapsed = now - row.startMs
        if elapsed < slot.startDelayMs: continue
        if (now - slot.lastFlipMs) < FLIP_INTERVAL: continue
        slot.current = alphabet[(indexOf(slot.current) + 1) % 27]
        slot.lastFlipMs = now
        slot.flipsLeft--
        if slot.flipsLeft == 0:
            slot.current = slot.target
            slot.done = true
```

Then clear buffer, redraw all rows + separators + wifi, `u8g2.sendBuffer()`.

### Cycle variants (future tweak)

- `sequential` (default) — walks A→B→C…
- `random` — picks random glyph each flip until final 2 flips lock in sequence
- `spin` — flip interval decelerates as `flipsLeft` approaches 0
- `cascade` — larger char-stagger (60 ms) so letters land left→right

## 5. Public API

```c
// travel_board.h
void board_init();
void board_set_row(uint8_t row, const char* text); // 0..5, text auto-uppercased & clamped
void board_clear_row(uint8_t row);
void board_set_all(const char* rows[6]);
void board_tick();           // call every loop(); drives animation + redraw
void board_set_speed_ms(uint16_t ms);
void board_set_wifi_bars(uint8_t bars); // 0..3
```

`board_set_row` restarts the animation only for that row. Unchanged characters
still flip (that's the aesthetic — a new message = the whole row flips).

## 6. Memory & performance

- 6 rows × 20 slots × ~10 bytes = ~1.2 KB RAM for slot state. Fine.
- SSD1306 full buffer = 1024 bytes (128×64 / 8).
- Target redraw rate: 30 fps. Measure `u8g2.sendBuffer()` — if >25 ms, drop to 20 fps.
- Keep `loop()` non-blocking. No `delay()` inside `board_tick()`.

## 7. Acceptance criteria

1. All six rows render simultaneously with the correct font and padding.
2. Setting a row triggers per-letter A→Z flipping that locks onto the final text.
3. Rows start in sequence (row 1 first, row 6 last) with a 120 ms gap.
4. Dotted separators are visible between every row.
5. Wi-Fi glyph appears top-right without colliding with row 1 text.
6. Animation stays smooth (≥20 fps) while rows are flipping.
7. `board_set_speed_ms(20)` makes flaps visibly faster; `200` makes them slow.
8. Characters outside `[A–Z 0–9 space - : / . !]` are silently stripped, never crash.
9. No `delay()` in the render path — other tasks (Wi-Fi, sensors) keep running.

## 8. Out of scope (variant 1)

- Boxed per-character tile divisions (that's variant 2).
- Row numbering gutter (variant 3).
- Mixed font sizes (variant 4).
- Scroll/queue indicators (variant 5).
- Lowercase, symbols beyond the allowed set, non-ASCII.

## 9. Stretch (optional)

- Persist the last 6 rows to NVS so the board re-hydrates on boot.
- Expose HTTP POST `/row/<n>` to set a row over the network.
- Sound hook: trigger a short "tick" on a piezo each flip (GPIO25).
-- 
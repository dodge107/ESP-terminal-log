#pragma once
#include <Arduino.h>

void board_init();
void board_set_row(uint8_t row, const char* text); // row 0..5, auto-uppercased & clamped
void board_clear_row(uint8_t row);
void board_set_all(const char* rows[6]);
void board_settle();                // snap all slots instantly to targets and render
void board_tick();                  // call every loop(); drives animation + redraw
void board_set_speed_ms(uint16_t ms);
void board_set_wifi_bars(uint8_t bars); // 0..3
void board_set_sep_gap(uint8_t px);     // px of clear space on each side of the separator line
void board_wake();                      // call on new data: restores full brightness and resets idle timer

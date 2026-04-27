#pragma once
#include <Arduino.h>

// LED mode — applies to each indicator independently.
typedef enum : uint8_t {
    LED_OFF   = 0,  // always off
    LED_ON    = 1,  // always on at set brightness
    LED_FLASH = 2,  // 500 ms on / 500 ms off
    LED_PULSE = 3,  // sine-wave breathing over 3 s
} LedMode;

// ── Public API ────────────────────────────────────────────────────────────────
void        led_init();
void        led_tick();                               // call every loop()

void        led_set_mode(uint8_t led, LedMode mode); // led = 0 or 1
void        led_set_brightness(uint8_t led, uint8_t percent); // 0-100
void        led_set_notify(uint8_t led, bool enable); // auto-flash on new content/wake
void        led_notify(uint8_t led);                  // trigger one 3-s notification flash

LedMode     led_get_mode(uint8_t led);
uint8_t     led_get_brightness(uint8_t led);
bool        led_get_notify(uint8_t led);

const char* led_mode_str(LedMode mode);    // "off" | "on" | "flash" | "pulse"
LedMode     led_mode_from_str(const char* s);

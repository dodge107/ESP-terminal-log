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
void        led_tick();                                    // call every loop()

void        led_set_mode(uint8_t led, LedMode mode);       // led = 0 or 1
void        led_set_brightness(uint8_t led, uint8_t percent);
void        led_set_override(uint8_t led, LedMode override_mode);
            // Override mode: the mode the LED switches to when new content
            // arrives and nobody is present.  Must differ from base mode to
            // have a visible effect.  Persisted to NVS.

void        led_content_arrived();  // call when new content is set; activates
                                    // override on LEDs where override != mode
void        led_wake();             // call on any wake event; cancels override

LedMode     led_get_mode(uint8_t led);
uint8_t     led_get_brightness(uint8_t led);
LedMode     led_get_override(uint8_t led);
bool        led_is_overriding(uint8_t led);  // true while in override state

const char* led_mode_str(LedMode mode);      // "off"|"on"|"flash"|"pulse"
LedMode     led_mode_from_str(const char* s);

// led_indicator.cpp
//
// Drives up to two PWM indicator LEDs via the ESP32 LEDC peripheral.
// Pins are optional: define LED1_PIN and/or LED2_PIN in platformio.ini
// build_flags.  If a pin is not defined the corresponding LED index is a no-op.
//
// Each LED has two modes:
//   mode          — normal operating state (off / on / flash / pulse)
//   overrideMode  — state while waiting for someone to notice new content
//
// Override activates when led_content_arrived() is called and overrideMode
// differs from mode.  Override deactivates (returns to mode) when led_wake()
// is called — i.e. on any button press, radar detection, or wake API call.
//
// LEDC channels 0 (LED1) and 1 (LED2); 8-bit resolution at 5 kHz.

#include "led_indicator.h"
#include <math.h>
#include <Preferences.h>

// ── Timing constants ─────────────────────────────────────────────────────────
#define FLASH_PERIOD_MS  500UL    // half-period: on for 500 ms, off for 500 ms
#define PULSE_PERIOD_MS  3000UL   // full breathing cycle

// ── LEDC config ───────────────────────────────────────────────────────────────
#define LEDC_FREQ_HZ   5000
#define LEDC_RES_BITS  8          // duty range 0–255

// ── Per-LED state ─────────────────────────────────────────────────────────────
typedef struct {
    uint8_t  pin;           // 0xFF = not fitted
    LedMode  mode;          // normal operating state
    LedMode  overrideMode;  // state while override is active
    uint8_t  brightness;    // user-set 0–100 %
    uint8_t  dutyFull;      // 0–255, derived from brightness
    bool     overriding;    // true while override is active
} LedState;

static LedState g_led[2] = {
    { 0xFF, LED_OFF, LED_OFF, 100, 255, false },
    { 0xFF, LED_OFF, LED_OFF, 100, 255, false },
};

// ── Helpers ───────────────────────────────────────────────────────────────────

static void applyDuty(uint8_t i, uint8_t duty) {
    if (g_led[i].pin == 0xFF) return;
    ledcWrite(g_led[i].pin, duty);
}

static uint8_t scaleDuty(uint8_t i, float fraction) {
    return (uint8_t)(g_led[i].dutyFull * fraction);
}

// ── NVS persistence ───────────────────────────────────────────────────────────
// Namespace "leds", keys: "l0mode" "l0bright" "l0over" "l1mode" …

static void ledSave(uint8_t i) {
    Preferences p;
    p.begin("leds", false);
    char km[8], kb[8], ko[8];
    snprintf(km, sizeof(km), "l%umode",   i);
    snprintf(kb, sizeof(kb), "l%ubright", i);
    snprintf(ko, sizeof(ko), "l%uover",   i);
    p.putUChar(km, (uint8_t)g_led[i].mode);
    p.putUChar(kb, g_led[i].brightness);
    p.putUChar(ko, (uint8_t)g_led[i].overrideMode);
    p.end();
}

static void ledLoad(uint8_t i) {
    Preferences p;
    p.begin("leds", true);
    char km[8], kb[8], ko[8];
    snprintf(km, sizeof(km), "l%umode",   i);
    snprintf(kb, sizeof(kb), "l%ubright", i);
    snprintf(ko, sizeof(ko), "l%uover",   i);
    g_led[i].mode         = (LedMode)p.getUChar(km, (uint8_t)LED_OFF);
    uint8_t bright        = p.getUChar(kb, 100);
    g_led[i].overrideMode = (LedMode)p.getUChar(ko, (uint8_t)LED_OFF);
    p.end();
    g_led[i].brightness = bright;
    g_led[i].dutyFull   = (uint8_t)((uint16_t)bright * 255 / 100);
}

// ── Public API ────────────────────────────────────────────────────────────────

void led_init() {
#ifdef LED1_PIN
    g_led[0].pin = LED1_PIN;
    ledcAttach(LED1_PIN, LEDC_FREQ_HZ, LEDC_RES_BITS);
    ledLoad(0);
    ledcWrite(LED1_PIN, 0);
#endif
#ifdef LED2_PIN
    g_led[1].pin = LED2_PIN;
    ledcAttach(LED2_PIN, LEDC_FREQ_HZ, LEDC_RES_BITS);
    ledLoad(1);
    ledcWrite(LED2_PIN, 0);
#endif
}

void led_tick() {
    uint32_t now = millis();

    for (uint8_t i = 0; i < 2; i++) {
        if (g_led[i].pin == 0xFF) continue;

        LedMode effective = g_led[i].overriding ? g_led[i].overrideMode : g_led[i].mode;

        switch (effective) {
        case LED_OFF:
            applyDuty(i, 0);
            break;

        case LED_ON:
            applyDuty(i, g_led[i].dutyFull);
            break;

        case LED_FLASH: {
            bool on = ((now % (FLASH_PERIOD_MS * 2)) < FLASH_PERIOD_MS);
            applyDuty(i, on ? g_led[i].dutyFull : 0);
            break;
        }

        case LED_PULSE: {
            float phase    = (float)(now % PULSE_PERIOD_MS) / (float)PULSE_PERIOD_MS;
            float fraction = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * phase);
            applyDuty(i, scaleDuty(i, fraction));
            break;
        }
        }
    }
}

void led_set_mode(uint8_t led, LedMode mode) {
    if (led >= 2) return;
    g_led[led].mode      = mode;
    g_led[led].overriding = false;  // cancel active override — base mode changed
    ledSave(led);
}

void led_set_brightness(uint8_t led, uint8_t percent) {
    if (led >= 2) return;
    if (percent > 100) percent = 100;
    g_led[led].brightness = percent;
    g_led[led].dutyFull   = (uint8_t)((uint16_t)percent * 255 / 100);
    ledSave(led);
}

void led_set_override(uint8_t led, LedMode override_mode) {
    if (led >= 2) return;
    g_led[led].overrideMode = override_mode;
    ledSave(led);
}

void led_content_arrived() {
    for (uint8_t i = 0; i < 2; i++) {
        if (g_led[i].pin == 0xFF) continue;
        // Only activate if override would actually look different.
        if (g_led[i].overrideMode != g_led[i].mode) {
            g_led[i].overriding = true;
        }
    }
}

void led_wake() {
    for (uint8_t i = 0; i < 2; i++) {
        g_led[i].overriding = false;
    }
}

LedMode     led_get_mode(uint8_t led)       { return led < 2 ? g_led[led].mode         : LED_OFF; }
uint8_t     led_get_brightness(uint8_t led) { return led < 2 ? g_led[led].brightness    : 0;       }
LedMode     led_get_override(uint8_t led)   { return led < 2 ? g_led[led].overrideMode  : LED_OFF; }
bool        led_is_overriding(uint8_t led)  { return led < 2 ? g_led[led].overriding    : false;   }

const char* led_mode_str(LedMode mode) {
    switch (mode) {
        case LED_ON:    return "on";
        case LED_FLASH: return "flash";
        case LED_PULSE: return "pulse";
        default:        return "off";
    }
}

LedMode led_mode_from_str(const char* s) {
    if (!s) return LED_OFF;
    if (strcmp(s, "on")    == 0) return LED_ON;
    if (strcmp(s, "flash") == 0) return LED_FLASH;
    if (strcmp(s, "pulse") == 0) return LED_PULSE;
    return LED_OFF;
}

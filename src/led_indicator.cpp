// led_indicator.cpp
//
// Drives up to two PWM indicator LEDs via the ESP32 LEDC peripheral.
// Pins are optional: define LED1_PIN and/or LED2_PIN in platformio.ini
// build_flags.  If a pin is not defined the corresponding LED index is a no-op.
//
// Example platformio.ini build_flags:
//   -DLED1_PIN=10
//   -DLED2_PIN=11
//
// LEDC channels 0 (LED1) and 1 (LED2); 8-bit resolution at 5 kHz.

#include "led_indicator.h"
#include <math.h>
#include <Preferences.h>

// ── Timing constants ─────────────────────────────────────────────────────────
#define FLASH_PERIOD_MS   500UL   // half-period: 250 ms on, 250 ms off
#define PULSE_PERIOD_MS  3000UL   // full breathing cycle
#define NOTIFY_DURATION_MS 3000UL // how long a notification flash lasts

// ── LEDC config ───────────────────────────────────────────────────────────────
#define LEDC_FREQ_HZ   5000
#define LEDC_RES_BITS  8          // 0-255 duty range

// ── Per-LED state ─────────────────────────────────────────────────────────────
typedef struct {
    uint8_t  pin;           // 0xFF = not fitted
    LedMode  mode;
    uint8_t  brightness;    // user-set, 0-100 %
    uint8_t  dutyFull;      // 0-255, derived from brightness
    bool     notifyEnabled; // auto-flash on new content / wake event
    bool     notifying;     // currently running a notification override
    LedMode  priorMode;     // mode to restore after notification ends
    uint32_t notifyUntilMs;
} LedState;

static LedState g_led[2] = {
    { 0xFF, LED_OFF, 100, 255, false, false, LED_OFF, 0 },
    { 0xFF, LED_OFF, 100, 255, false, false, LED_OFF, 0 },
};

// ── Helpers ───────────────────────────────────────────────────────────────────

static void applyDuty(uint8_t i, uint8_t duty) {
    if (g_led[i].pin == 0xFF) return;
    ledcWrite(g_led[i].pin, duty);
}

static uint8_t scaleDuty(uint8_t i, float fraction) {
    // fraction 0.0..1.0 → scaled by dutyFull
    return (uint8_t)(g_led[i].dutyFull * fraction);
}

// ── NVS persistence ───────────────────────────────────────────────────────────
// Namespace "leds", keys: "l0mode" "l0bright" "l0notify" "l1mode" …

static void ledSave(uint8_t i) {
    Preferences p;
    p.begin("leds", false);
    char km[8], kb[8], kn[8];
    snprintf(km, sizeof(km), "l%umode",   i);
    snprintf(kb, sizeof(kb), "l%ubright", i);
    snprintf(kn, sizeof(kn), "l%unotify", i);
    p.putUChar(km, (uint8_t)g_led[i].mode);
    p.putUChar(kb, g_led[i].brightness);
    p.putBool (kn, g_led[i].notifyEnabled);
    p.end();
}

static void ledLoad(uint8_t i) {
    Preferences p;
    p.begin("leds", true);
    char km[8], kb[8], kn[8];
    snprintf(km, sizeof(km), "l%umode",   i);
    snprintf(kb, sizeof(kb), "l%ubright", i);
    snprintf(kn, sizeof(kn), "l%unotify", i);
    g_led[i].mode          = (LedMode)p.getUChar(km, (uint8_t)LED_OFF);
    uint8_t bright         = p.getUChar(kb, 100);
    g_led[i].notifyEnabled = p.getBool (kn, false);
    p.end();
    // Apply brightness through the setter logic (updates dutyFull).
    g_led[i].brightness = bright;
    g_led[i].dutyFull   = (uint8_t)((uint16_t)bright * 255 / 100);
}

// ── Public API ────────────────────────────────────────────────────────────────

void led_init() {
#ifdef LED1_PIN
    g_led[0].pin = LED1_PIN;
    ledcAttach(LED1_PIN, LEDC_FREQ_HZ, LEDC_RES_BITS);
    ledLoad(0);
    ledcWrite(LED1_PIN, 0);   // start dark; led_tick() applies mode each loop
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

        // End notification override once the duration expires.
        if (g_led[i].notifying && now >= g_led[i].notifyUntilMs) {
            g_led[i].notifying = false;
            g_led[i].mode      = g_led[i].priorMode;
        }

        LedMode effective = g_led[i].notifying ? LED_FLASH : g_led[i].mode;

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
            float phase   = (float)(now % PULSE_PERIOD_MS) / (float)PULSE_PERIOD_MS;
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
    g_led[led].notifying = false;  // cancel any active notification
    ledSave(led);
}

void led_set_brightness(uint8_t led, uint8_t percent) {
    if (led >= 2) return;
    if (percent > 100) percent = 100;
    g_led[led].brightness = percent;
    g_led[led].dutyFull   = (uint8_t)((uint16_t)percent * 255 / 100);
    ledSave(led);
}

void led_set_notify(uint8_t led, bool enable) {
    if (led >= 2) return;
    g_led[led].notifyEnabled = enable;
    ledSave(led);
}

void led_notify(uint8_t led) {
    if (led >= 2) return;
    if (g_led[led].pin == 0xFF) return;
    if (!g_led[led].notifying)
        g_led[led].priorMode = g_led[led].mode;
    g_led[led].notifying     = true;
    g_led[led].notifyUntilMs = millis() + NOTIFY_DURATION_MS;
}

LedMode led_get_mode(uint8_t led)       { return led < 2 ? g_led[led].mode           : LED_OFF; }
uint8_t led_get_brightness(uint8_t led) { return led < 2 ? g_led[led].brightness      : 0;       }
bool    led_get_notify(uint8_t led)     { return led < 2 ? g_led[led].notifyEnabled   : false;   }

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

/* Debounced press latching for the front-panel buttons. See buttons.h. */
#include "buttons.h"

#if RF_BUTTONS

#include <stdbool.h>

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

/* Fruit Jam wires all three to ground with no external pull, so the pads
 * carry an internal pull-up and a press reads low. Button 1 is also the BOOT
 * button; that is a reset-time function only and does not affect this. */
#ifndef RF_BTN1_PIN
#define RF_BTN1_PIN ADAFRUIT_FRUIT_JAM_BUTTON1_PIN
#endif
#ifndef RF_BTN2_PIN
#define RF_BTN2_PIN ADAFRUIT_FRUIT_JAM_BUTTON2_PIN
#endif
#ifndef RF_BTN3_PIN
#define RF_BTN3_PIN ADAFRUIT_FRUIT_JAM_BUTTON3_PIN
#endif
#ifndef RF_BTN_ACTIVE_LOW
#define RF_BTN_ACTIVE_LOW 1
#endif
/* how long the line must sit released before the button can fire again */
#ifndef RF_BTN_QUIET_MS
#define RF_BTN_QUIET_MS 40
#endif

#if RF_BTN_ACTIVE_LOW
#define RF_BTN_PRESS_EDGE GPIO_IRQ_EDGE_FALL
#else
#define RF_BTN_PRESS_EDGE GPIO_IRQ_EDGE_RISE
#endif
/* both edges are watched: only the press edge latches, but a release that
 * chatters must still hold the button disarmed, and that is what seeing its
 * edges does */
#define RF_BTN_EDGES (GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE)

static const uint8_t btn_pin[RF_BTN_COUNT] = {RF_BTN1_PIN, RF_BTN2_PIN,
                                              RF_BTN3_PIN};
static volatile uint32_t btn_pending;
static volatile uint32_t btn_armed;     /* bit set = may latch a press */
/* bit set = a latched press whose length is not decided yet; it becomes a
 * short or a long event in rf_buttons_take() */
static volatile uint32_t btn_holding;
static volatile uint64_t btn_edge_us[RF_BTN_COUNT];  /* last edge seen  */
static volatile uint64_t btn_press_us[RF_BTN_COUNT]; /* its press edge  */

static inline bool btn_down(int i) {
    return gpio_get(btn_pin[i]) == !RF_BTN_ACTIVE_LOW;
}

/* Every edge pushes the quiet timer out, so a bouncing release keeps the
 * button disarmed until the contact has actually settled. */
static void btn_isr(uint gpio, uint32_t events) {
    uint64_t now = time_us_64();
    for (int i = 0; i < RF_BTN_COUNT; i++) {
        if (btn_pin[i] != gpio) continue;
        btn_edge_us[i] = now;
        if ((events & RF_BTN_PRESS_EDGE) && (btn_armed & (1u << i))) {
            btn_armed &= ~(1u << i);
            btn_press_us[i] = now;
            btn_holding |= 1u << i;
        }
    }
}

void rf_buttons_init(void) {
    for (int i = 0; i < RF_BTN_COUNT; i++) {
        gpio_init(btn_pin[i]);
        gpio_set_dir(btn_pin[i], GPIO_IN);
#if RF_BTN_ACTIVE_LOW
        gpio_pull_up(btn_pin[i]);
#else
        gpio_pull_down(btn_pin[i]);
#endif
    }
    /* let the pulls charge the lines before arming, and drop whatever edge
     * that settling produced - otherwise the board draws a face at boot */
    sleep_ms(5);
    gpio_set_irq_callback(&btn_isr);
    for (int i = 0; i < RF_BTN_COUNT; i++) {
        gpio_acknowledge_irq(btn_pin[i], RF_BTN_EDGES);
        /* a button held at boot stays disarmed until it is let go */
        if (!btn_down(i)) btn_armed |= 1u << i;
        gpio_set_irq_enabled(btn_pin[i], RF_BTN_EDGES, true);
    }
    irq_set_enabled(IO_IRQ_BANK0, true);
}

uint32_t rf_buttons_take(void) {
    const uint64_t quiet = RF_BTN_QUIET_MS * 1000ull;
    const uint64_t hold = RF_BTN_LONG_MS * 1000ull;
    uint64_t now = time_us_64();
    uint32_t st = save_and_disable_interrupts();
    for (int i = 0; i < RF_BTN_COUNT; i++) {
        uint32_t bit = 1u << i;
        bool down = btn_down(i);
        if (btn_holding & bit) {
            if (down) {
                /* still on the button: report the hold the moment it is one,
                 * so the mode it switches changes under the finger */
                if (now - btn_press_us[i] >= hold) {
                    btn_pending |= 1u << RF_BTN_LONG(i);
                    btn_holding &= ~bit;
                }
                continue;
            }
            /* let the release settle, then judge the press by the edges the
             * interrupt timed - which is what a press made during a
             * generation is judged by too, long after the fact */
            if (now - btn_edge_us[i] <= quiet) continue;
            int ev = (btn_edge_us[i] - btn_press_us[i] >= hold)
                         ? RF_BTN_LONG(i)
                         : i;
            btn_pending |= 1u << ev;
            btn_holding &= ~bit;
        }
        if (!(btn_armed & bit) && !down && now - btn_edge_us[i] > quiet)
            btn_armed |= bit;
    }
    uint32_t m = btn_pending;
    btn_pending = 0;
    restore_interrupts(st);
    return m;
}

uint32_t rf_buttons_held(void) {
    uint32_t m = 0;
    for (int i = 0; i < RF_BTN_COUNT; i++)
        if (btn_down(i)) m |= 1u << i;
    return m;
}

#endif /* RF_BUTTONS */

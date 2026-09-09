/* Front-panel buttons (Adafruit Fruit Jam: BUTTON1/2/3 on GP0/GP4/GP5).
 *
 * The board is usable with no host attached: one button draws a new face,
 * one cycles the color grid palette set, one cycles the generation settings, and
 * holding one down is a second, separate event that switches a mode. main.c
 * owns what each one means; this file only turns bouncing switch contacts
 * into at most one clean event per press, short or long.
 *
 * Presses are latched in a GPIO interrupt rather than polled, because a
 * generation blocks the main loop for seconds - a press made while a face is
 * being drawn is still acted on when it finishes. Debounce is arm/disarm:
 * the falling edge that latches a press disarms the button, and only a line
 * that has read released and stayed quiet for RF_BTN_QUIET_MS re-arms it, so
 * neither press nor release bounce can produce a second event.
 *
 * Short and long are told apart by how long the line stayed down. A hold is
 * reported as soon as it passes RF_BTN_LONG_MS, with the finger still on the
 * button, so the mode change lands while the press is happening; the release
 * that follows is then swallowed. A press that came and went while the main
 * loop was busy generating is judged instead by the timestamps the interrupt
 * recorded, so holding a button through a generation still reads as a hold.
 */
#ifndef RF_BUTTONS_H
#define RF_BUTTONS_H

#include <stdint.h>

/* Only the Fruit Jam has buttons wired; other boards build this out. The
 * CMake option of the same name is what normally sets this. */
#ifndef RF_BUTTONS
#if defined(ADAFRUIT_FRUIT_JAM)
#define RF_BUTTONS 1
#else
#define RF_BUTTONS 0
#endif
#endif

#define RF_BTN_COUNT 3
/* bit positions in the rf_buttons_take() mask, in silkscreen order */
#define RF_BTN_FACE 0  /* button 1: draw a new face          */
#define RF_BTN_COLOR 1 /* button 2: next palette set         */
#define RF_BTN_GEN 2   /* button 3: next generation preset   */
/* the same three buttons held down, in the top half of the same mask */
#define RF_BTN_LONG(b) ((b) + RF_BTN_COUNT)

/* how long a button must stay down to report a hold instead of a press */
#ifndef RF_BTN_LONG_MS
#define RF_BTN_LONG_MS 700
#endif

void rf_buttons_init(void);

/* Bitmask of button events since the last call, clearing them: bit RF_BTN_*
 * for a press, bit RF_BTN_LONG(RF_BTN_*) for a hold. One press produces one
 * or the other, never both. Also re-arms any button whose line has been
 * released and quiet, and is where a hold that is still in progress crosses
 * the threshold, so this must be called regularly - it is the debounce's
 * second half and the long-press timer's only tick. */
uint32_t rf_buttons_take(void);

/* Live line state, for the "B" status line; bit set = held down now. */
uint32_t rf_buttons_held(void);

#endif

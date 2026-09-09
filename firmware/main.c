/* pico-faces firmware.
 * USB-CDC protocol:
 *   host -> "G <seed> [k_steps] [class]\n"   (class default: seed % n_cond)
 *   dev  -> "RFI2" | u32 seed | u16 w | u16 h | u16 ch | u16 class
 *           | w*h*ch image bytes (HWC) | u32 crc32 | u32 gen_ms
 * Color grid tuning (RF_COLOR_GRID builds), all re-rendering from the image already
 * in rf_img, so palettes can be tried without regenerating:
 *   host -> "L <panel> <level> <rrggbb>\n"   one palette entry
 *   host -> "W [soft] [q0 q1 q2]\n"          softness/quantiles, then redraw
 *   host -> "S [n]\n"                        named palette set (blank = next)
 * Seed source (blank = just report it):
 *   host -> "R [0|1]\n"                      1 = a random seed per face,
 *                                            0 = count on from the current one
 * Fruit Jam built-in buttons (RF_BUTTONS builds) drive the same three
 * things with no host attached, and holding one down does a second thing:
 * button 1 held toggles automatic refresh (a new face every RF_AUTO_MS with
 * nobody pressing anything), button 2 held toggles the color grid off, back
 * to the single full-size face the model produced, and button 3 held walks
 * the generation presets backwards. "B" prints all of it.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/vreg.h"
#include "pico/rand.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#include "buttons.h"
#include "rf_model.h"
#include "rf_ops.h"
#include "color_grid.h"

extern const uint8_t rf_model_blob[];
extern const uint8_t rf_model_blob_end[];

#ifndef RF_LED_INVERTED
#if defined(ADAFRUIT_FRUIT_JAM)
#define RF_LED_INVERTED 1
#else
#define RF_LED_INVERTED PICO_DEFAULT_LED_PIN_INVERTED
#endif
#endif
#if RF_LED_INVERTED
#define RF_LED_ON 0
#define RF_LED_OFF 1
#else
#define RF_LED_ON 1
#define RF_LED_OFF 0
#endif

/* Either scanout provides the same four-function contract (rf_vga_init,
 * rf_vga_invalidate, rf_vga_dither, rf_step_hook), so the call sites below
 * only care whether one is linked at all. */
#define RF_VIDEO (RF_VGA + RF_DVI)

#define RF_HSTX_KHZ 150000
#if RF_DVI
#if (RF_SYS_KHZ) % (RF_HSTX_KHZ) != 0 || \
    (RF_SYS_KHZ) / (RF_HSTX_KHZ) < 1 || (RF_SYS_KHZ) / (RF_HSTX_KHZ) > 3
#error "RF_DVI needs clk_hstx=150MHz: RF_SYS_KHZ must be 1..3 x 150000"
#endif
#if RF_VGA
#error "RF_DVI and RF_VGA are two scanouts for one framebuffer; pick one"
#endif
#endif

static rf_model_t model;
/* non-static: the display backend reads it live */
uint8_t rf_img[RF_IMG_HW * RF_IMG_HW * RF_IMG_CH];

/* ---- generation settings -------------------------------------------------
 * The three variables rf_generate() takes besides the seed, as a flat list of
 * named presets: button 3 (and nothing else) walks it. The first four hold a
 * class and move along the quality axis, from a soft 2-step pass to the
 * slowest, most strongly guided one, then the unguided face and the 1-step
 * sketch; the rest pin the class, which is the variable you can actually see.
 * Edit the table freely - it is only read here.
 *
 * class: 0 female/neutral, 1 female/smiling, 2 male/neutral, 3 male/smiling,
 * 4 = the trained null class (unconditional; guidance does not apply to it),
 * RF_COND_RANDOM = one of the four labelled classes, drawn from the seed.
 * w: baked guidance strengths are 4, 6 and 8; 0 means a plain, unguided pass.
 * k: Euler steps, a power-of-2 divisor of the baked K (8, 4, 2, 1). */
#define RF_COND_RANDOM 0xff

typedef struct {
    const char *name;
    uint8_t k, cond, w;
} gen_preset_t;

static const gen_preset_t gen_presets[] = {
    {"soft K2 w4 rnd", 2, RF_COND_RANDOM, 4},
    {"strong K8 w8 rnd", 8, RF_COND_RANDOM, 8},
    {"unconditional", 4, 4, 0},
    {"sketch K1", 1, 4, 0},
    {"female neutral", 4, 0, 6},
    {"female smiling", 4, 1, 6},
    {"male neutral", 4, 2, 6},
    {"male smiling", 4, 3, 6},
};
#define N_GEN_PRESETS ((int)(sizeof gen_presets / sizeof gen_presets[0]))

#ifndef RF_RANDOM_SEED
#define RF_RANDOM_SEED 1
#endif
#ifndef RF_SEED0
#define RF_SEED0 1
#endif

static uint64_t cur_seed = RF_SEED0;
/* Where the next unasked-for seed comes from. RF_RANDOM_SEED is only the
 * power-on default; "R 0"/"R 1" moves it at runtime. */
static bool random_seed = RF_RANDOM_SEED;
static int cur_preset;
static bool have_image; /* rf_img holds a face, so a redraw is meaningful */

#if RF_BUTTONS
/* ---- automatic refresh (button 1 held) ----------------------------------
 * The face on screen gets a life span: when it runs out the next one starts
 * on its own, so the board runs unattended. The wait is shown by draining
 * the progress bar, which has nothing else to say between generations.
 */
#ifndef RF_AUTO_MS
#define RF_AUTO_MS 6500
#endif
static bool auto_mode;
static absolute_time_t auto_due; /* when the next automatic face is due */
#endif

static void put_u32(uint32_t v) { fwrite(&v, 4, 1, stdout); }
static void put_u16(uint16_t v) { fwrite(&v, 2, 1, stdout); }

/* raise the flash clock divider before overclocking; must run from SRAM
 * because it changes XIP timing underneath any flash-resident caller */
static void __no_inline_not_in_flash_func(qmi_set_clkdiv)(uint32_t div) {
    uint32_t t = qmi_hw->m[0].timing;
    qmi_hw->m[0].timing = (t & ~QMI_M0_TIMING_CLKDIV_BITS) |
                          (div << QMI_M0_TIMING_CLKDIV_LSB);
    __compiler_memory_barrier();
}

/* Run one generation into rf_img and put it on screen. Returns the elapsed
 * milliseconds; the caller decides whether a host also gets the bytes. */
static uint32_t rf_run(uint64_t seed, int k_steps, int cond, int w_idx) {
    gpio_put(PICO_DEFAULT_LED_PIN, RF_LED_ON);
#if RF_VIDEO
    /* framebuffer aliases rf_arena; engine is about to reuse it */
    extern void rf_vga_invalidate(void), rf_vga_dither(void);
    rf_vga_invalidate();
#endif
    absolute_time_t t0 = get_absolute_time();
    rf_generate(&model, seed, k_steps, cond, w_idx, rf_img, NULL);
    uint32_t ms =
        (uint32_t)(absolute_time_diff_us(t0, get_absolute_time()) / 1000);
#if RF_VIDEO
    rf_vga_dither();
#endif
    gpio_put(PICO_DEFAULT_LED_PIN, RF_LED_OFF);
    have_image = true;
#if RF_BUTTONS
    /* whoever asked for this face - a press, the timer, or a host - it is
     * the one now on screen, so its full time on screen starts here */
    auto_due = make_timeout_time_ms(RF_AUTO_MS);
#endif
    return ms;
}

/* Guidance strength -> index into the model's baked w tables (-1 = plain). */
static int w_index(int w) {
    for (uint32_t j = 0; j < model.n_w; j++)
        if (model.w_q8[j] == (uint32_t)(w * 256)) return (int)j;
    return -1;
}

/* A preset's class for one seed: a fixed class passes straight through, while
 * RF_COND_RANDOM draws one of the four labelled classes. Mixed out of the seed
 * (and the preset's slot, so the two random presets disagree on the same seed)
 * rather than the clock: a new face is a new seed, hence a new class, but the
 * same seed still reproduces the same face. */
static int preset_cond(const gen_preset_t *g, uint64_t seed) {
    if (g->cond != RF_COND_RANDOM) return g->cond;
    uint32_t v = (uint32_t)seed ^ (uint32_t)(seed >> 32);
    v += (uint32_t)(g - gen_presets) * 0x9e3779b9u;
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    return (int)(v & 3u);
}

#if RF_BUTTONS || RF_AUTOGEN
static uint32_t run_preset(void) {
    const gen_preset_t *g = &gen_presets[cur_preset];
    return rf_run(cur_seed, g->k, preset_cond(g, cur_seed), w_index(g->w));
}
#endif

/* Put the preset on screen, under the progress bar. Numbered from 1 there
 * and in print_state(), so the display and the log read the same. */
static void show_preset(void) {
#if RF_DVI
    extern void rf_dvi_readout(int n, int total);
    rf_dvi_readout(cur_preset + 1, N_GEN_PRESETS);
#endif
}

/* One line covering everything the buttons can change, so a host can see the
 * state a button just changed (and "B" can ask for it). */
static void print_state(const char *what, uint32_t ms) {
    const gen_preset_t *g = &gen_presets[cur_preset];
    printf("%s seed=%u preset=%d/%d %s (K=%u class=%u w=%u)",
           what, (unsigned)cur_seed, cur_preset + 1, N_GEN_PRESETS, g->name,
           (unsigned)g->k, (unsigned)preset_cond(g, cur_seed), (unsigned)g->w);
#if RF_DVI && RF_COLOR_GRID
    printf(" color_grid=%s palette=%d/%d %s", rf_color_grid_on ? "on" : "off",
           rf_color_grid_set, rf_color_grid_n_sets, rf_color_grid_set_name(rf_color_grid_set));
#endif
#if RF_BUTTONS
    printf(" refresh=%s", auto_mode ? "auto" : "manual");
#endif
    printf(" random=%s", random_seed ? "on" : "off");
    if (ms) printf(" %ums", (unsigned)ms);
    printf("\n");
}

/* The next seed nobody chose.
 *
 * In random mode it comes from get_rand_32(), which on RP2350 is fed by the
 * hardware TRNG and the per-boot random word - it owes nothing to how long
 * the board has been up. That is the whole point: the old seed was mixed out
 * of time_us_64(), which reads the same on every boot, so an unattended board
 * replayed the same faces in the same order every time it was powered on.
 *
 * Otherwise it just counts on from the current seed, so a board that starts
 * at RF_SEED0 walks one fixed stream, and setting a seed with "G" picks where
 * that stream carries on from.
 *
 * 32 bits either way: that is what the "RFI2" header and print_state() report,
 * so a face worth keeping can always be asked for again with "G <seed>". */
static void new_seed(void) {
    cur_seed = random_seed ? (uint64_t)get_rand_32() : (uint32_t)(cur_seed + 1);
}

#if RF_BUTTONS
/* How much of this face's time is left, as the progress bar; in manual mode
 * there is nothing to count down and the bar goes back to hidden. */
static void auto_show_wait(void) {
#if RF_DVI
    extern void rf_dvi_bar(int num, int den);
    int left = (int)(absolute_time_diff_us(get_absolute_time(), auto_due) /
                     1000);
    if (left < 0) left = 0;
    rf_dvi_bar(left, auto_mode ? RF_AUTO_MS : 0);
#endif
}

/* Button 1 draws a new face, button 2 recolours the one on screen, button 3
 * steps the generation settings; holding 1 or 2 switches a mode instead, and
 * holding 3 steps the settings backwards.
 * Events are latched, so several can be waiting after a long generation; a
 * palette change is free, a grid switch costs a redraw, and everything that
 * needs new pixels shares a single regeneration. */
static void handle_buttons(void) {
    uint32_t b = rf_buttons_take();
    if (!b) return;
    const char *what = NULL; /* stays NULL if the press changed nothing */
    bool regen = false;
#if RF_VIDEO
    bool redraw = false; /* the face on screen, drawn a different way */
#endif

    if (b & (1u << RF_BTN_LONG(RF_BTN_FACE))) {
        auto_mode = !auto_mode;
        /* the face being looked at when the mode came on gets a full turn,
         * however long it had already been up; an empty screen has nothing
         * to wait for, so its first face starts now */
        auto_due = make_timeout_time_ms(RF_AUTO_MS);
        if (auto_mode && !have_image) regen = true;
        auto_show_wait(); /* first countdown, or the bar cleared away */
        what = "MODE";
    }
    if (b & (1u << RF_BTN_LONG(RF_BTN_COLOR))) {
#if RF_DVI && RF_COLOR_GRID
        rf_color_grid_on = !rf_color_grid_on;
        /* the same rf_img either way - the grid is a display-time transform,
         * so this is a redraw, never a regeneration */
        if (have_image) redraw = true;
        else regen = true;
        what = "MODE";
#else
        printf("no color grid in this build\n");
#endif
    }
    if (b & (1u << RF_BTN_COLOR)) {
#if RF_DVI && RF_COLOR_GRID
        /* the four palettes are the grid's; with the grid off there is one
         * face in its own colours and the press has nothing to change */
        if (rf_color_grid_on) {
            rf_color_grid_use_set(rf_color_grid_set + 1);
            if (have_image) redraw = true; /* same face, new palette */
            else regen = true;             /* nothing on screen to recolour */
            what = "PAL";
        }
#else
        printf("no palette sets in this build\n");
#endif
    }
    if (b & (1u << RF_BTN_LONG(RF_BTN_GEN))) {
        /* the same walk as a press, the other way round, so a preset one
         * past the one you wanted is one hold back rather than seven more
         * presses (and seven generations) away */
        cur_preset = (cur_preset + N_GEN_PRESETS - 1) % N_GEN_PRESETS;
        show_preset();
        regen = true;
        what = "GEN";
    }
    if (b & (1u << RF_BTN_GEN)) {
        cur_preset = (cur_preset + 1) % N_GEN_PRESETS;
        /* before the generation, not after: the readout is what says the
         * press landed while the new face is still drawing */
        show_preset();
        regen = true;
        what = "GEN";
    }
    if (b & (1u << RF_BTN_FACE)) {
        new_seed();
        regen = true;
        what = "GEN";
    }

    uint32_t ms = 0;
    if (regen) {
        ms = run_preset();
#if RF_VIDEO
    } else if (redraw) {
        extern void rf_vga_dither(void);
        rf_vga_dither();
#endif
    }
    if (regen) auto_show_wait(); /* the dither hid the bar; the wait is on */
    if (what) print_state(what, ms);
}

/* Automatic mode's whole loop: hold the face until its time is up, then draw
 * the next one from a fresh seed with nobody pressing anything. */
static void handle_auto(void) {
    if (!auto_mode) return;
    if (have_image && !time_reached(auto_due)) {
        auto_show_wait();
        return;
    }
    new_seed();
    uint32_t ms = run_preset();
    auto_show_wait();
    print_state("AUTO", ms);
}
#endif

int main(void) {
#if RF_SYS_KHZ > 150000
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(10);
    /* QSPI stays at RF_SYS_KHZ/4 (75 MHz at 300) - within W25Q32 spec */
    qmi_set_clkdiv(4);
    set_sys_clock_khz(RF_SYS_KHZ, true);
#endif
#if RF_DVI
    /* Must come after set_sys_clock_khz(): the SDK's runtime init leaves
     * clk_hstx glued undivided to clk_sys, and retuning clk_sys neither
     * re-divides it nor updates its recorded frequency. Take the actual
     * clk_sys rather than RF_SYS_KHZ. */
    clock_configure(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS,
                    clock_get_hz(clk_sys), RF_HSTX_KHZ * 1000u);
#endif
    stdio_init_all();
    stdio_set_translate_crlf(&stdio_usb, false);

    extern void rf_par_init(void);
    rf_par_init();
#if RF_VIDEO
    /* Historically this had to come first: scanvideo claims FIXED DMA
     * channels (0..), so the display had to be up before the staging
     * channels were taken from the unused pool. dvi_hstx.c has no such
     * constraint - it claims whatever is free - but the ordering is kept so
     * the scanout is alive (and showing the boot pattern) before anything
     * slower runs. */
    extern void rf_vga_init(void);
    rf_vga_init();
#endif
    show_preset();
#if RF_STAGE_DMA
    extern void rf_stage_init(void);
    rf_stage_init();
#endif

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    /* an output defaults to 0, which on an inverted LED is "lit" - park it */
    gpio_put(PICO_DEFAULT_LED_PIN, RF_LED_OFF);
#if RF_BUTTONS
    rf_buttons_init();
#endif

    int rc = rf_model_load(rf_model_blob,
                           (size_t)(rf_model_blob_end - rf_model_blob), &model);
    /* Before anything is drawn: in random mode this is what makes one
     * power-on differ from the next. Left alone otherwise, so the first face
     * is RF_SEED0 and the stream from it is the same every time. */
    if (random_seed) new_seed();

#if RF_AUTOGEN
    /* Draw something without waiting to be asked, for a board that has no
     * host attached. Costs a boot's worth of silence on USB. */
    if (rc == 0) {
        uint32_t ms = run_preset();
        print_state("BOOT", ms);
    }
#endif

    char line[64];
    int n = 0;
    for (;;) {
#if RF_BUTTONS
        if (rc == 0) {
            handle_buttons();
            handle_auto();
        }
#endif
        /* short enough that a latched press is acted on promptly, since a
         * generation is the only other thing that leaves this loop */
        int ch = getchar_timeout_us(20000);
        if (ch == PICO_ERROR_TIMEOUT) continue;
        if (ch != '\n' && ch != '\r') {
            if (n < (int)sizeof line - 1) line[n++] = (char)ch;
            continue;
        }
        line[n] = 0;
        n = 0;
        if (rc != 0) {
            printf("ERR model load %d\n", rc);
            continue;
        }
        if (line[0] == 'G') {
            char *e1, *e2, *e3, *e4;
            uint64_t seed = strtoull(line + 1, &e1, 0);
            int k_steps = (int)strtol(e1, &e2, 0);
            if (!k_steps) k_steps = 4;
            long cv = strtol(e2, &e3, 0);
            /* golden convention when the class token is absent */
            int cond = (e3 != e2) ? (int)cv : (int)(seed % model.n_cond);
            /* optional guidance strength w (e.g. 4/6/8): matched against
             * the baked w_q8 sets; absent w on a CFG build follows the
             * golden convention so goldens reproduce over USB */
            long wv = strtol(e3, &e4, 0);
            int w_idx = -1;
            if (e4 != e3) {
                w_idx = w_index((int)wv);
            } else if (e3 == e2 && model.n_w) {
                w_idx = (int)(seed % (model.n_w + 1)) - 1;
            }
            uint32_t ms = rf_run(seed, k_steps, cond, w_idx);
            /* so a later button press carries on from what the host asked
             * for rather than from whatever was last pressed */
            cur_seed = seed;
            fwrite("RFI2", 1, 4, stdout);
            put_u32((uint32_t)seed);
            put_u16(RF_IMG_HW);
            put_u16(RF_IMG_HW);
            put_u16(RF_IMG_CH);
            put_u16((uint16_t)cond);
            fwrite(rf_img, 1, sizeof rf_img, stdout);
            put_u32(rf_crc32(rf_img, sizeof rf_img));
            put_u32(ms);
            fflush(stdout);
#if RF_DVI && RF_VGA_TEST
        } else if (line[0] == 'P') { /* boot test pattern 0..2 */
            extern void rf_dvi_test_pattern(int pat);
            int pat = (int)strtol(line + 1, NULL, 0);
            rf_dvi_test_pattern(pat);
            printf("OK pattern %d\n", pat);
#endif
#if RF_DVI && RF_COLOR_GRID
        } else if (line[0] == 'L') { /* palette entry: L <panel> <lvl> <hex> */
            char *e1, *e2;
            long p = strtol(line + 1, &e1, 0);
            long l = strtol(e1, &e2, 0);
            unsigned long rgb = strtoul(e2, NULL, 16);
            if (p < 0 || p >= RF_COLOR_GRID_PANELS || l < 0 ||
                l >= RF_COLOR_GRID_LEVELS) {
                printf("ERR panel 0..%d level 0..%d\n", RF_COLOR_GRID_PANELS - 1,
                       RF_COLOR_GRID_LEVELS - 1);
            } else {
                rf_color_grid_pal[p][l][0] = (uint8_t)(rgb >> 16);
                rf_color_grid_pal[p][l][1] = (uint8_t)(rgb >> 8);
                rf_color_grid_pal[p][l][2] = (uint8_t)rgb;
                printf("OK pal %ld %ld %06lx\n", p, l, rgb & 0xffffff);
            }
        } else if (line[0] == 'W') { /* W [soft] [q0 q1 q2] then re-render */
            extern void rf_vga_dither(void);
            char *e = line + 1, *e2;
            long v = strtol(e, &e2, 0);
            if (e2 != e) rf_color_grid_soft = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            /* quantiles must stay ascending or the band edges collapse */
            long prev = 0;
            for (int k = 0; k < RF_COLOR_GRID_LEVELS - 1; k++) {
                e = e2;
                v = strtol(e, &e2, 0);
                if (e2 == e) break;
                if (v < prev) v = prev;
                if (v > 100) v = 100;
                rf_color_grid_q[k] = (uint8_t)(prev = v);
            }
            rf_vga_dither();
            printf("OK color_grid soft=%u q=%u,%u,%u\n", rf_color_grid_soft,
                   rf_color_grid_q[0], rf_color_grid_q[1], rf_color_grid_q[2]);
        } else if (line[0] == 'S') { /* S [n]: named palette set, blank = next */
            extern void rf_vga_dither(void);
            char *e;
            long v = strtol(line + 1, &e, 0);
            rf_color_grid_use_set(e != line + 1 ? (int)v : rf_color_grid_set + 1);
            if (have_image) rf_vga_dither();
            printf("OK palette %d %s\n", rf_color_grid_set,
                   rf_color_grid_set_name(rf_color_grid_set));
#endif
#if RF_DVI
        } else if (line[0] == 'V') { /* scanout health / restart */
            extern int rf_dvi_bringup_tries, rf_dvi_restart(void);
            extern uint32_t rf_dvi_frame_rate_mhz(uint32_t ms);
            if (line[1] == 'R') rf_dvi_restart();
            printf("dvi bringup=%d refresh=%umHz\n", rf_dvi_bringup_tries,
                   (unsigned)rf_dvi_frame_rate_mhz(500));
#endif
        } else if (line[0] == 'R') { /* R [0|1]: seed at random, or count on */
            char *e;
            long v = strtol(line + 1, &e, 0);
            if (e != line + 1) random_seed = (v != 0);
            print_state("SEED", 0);
        } else if (line[0] == 'B') { /* what the buttons are set to */
            print_state("STATE", 0);
#if RF_BUTTONS
            printf("buttons: 1=new face 2=palette 3=settings; "
                   "hold 1=auto refresh (%us) hold 2=color grid "
                   "hold 3=prev settings; "
                   "held=0x%x\n",
                   (unsigned)(RF_AUTO_MS / 1000), (unsigned)rf_buttons_held());
#else
            printf("buttons: not built in\n");
#endif
            printf("seed: %s; \"R 0\"/\"R 1\" switches, \"G <seed>\" sets one\n",
                   random_seed ? "random per face"
                               : "counting on from the last");
        } else if (line[0] == 'I') { /* info */
            printf("pico-faces K=%u dim=%u depth=%u cond=%u ch=%u blob=%u "
                   "sys=%ukHz hstx=%ukHz meas=%ukHz\n",
                   model.K, model.dim, model.depth, model.n_cond, model.img_ch,
                   (unsigned)(rf_model_blob_end - rf_model_blob),
                   (unsigned)(clock_get_hz(clk_sys) / 1000u),
                   (unsigned)(clock_get_hz(clk_hstx) / 1000u),
                   (unsigned)frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_HSTX));
        }
    }
}

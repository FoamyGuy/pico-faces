/* Color grid filter: posterise one generated face and recolour it four ways.
 *
 * The display shows the SAME image in a 2x2 grid, each panel run through its
 * own palette, so the effect costs no extra generation time - it all happens
 * in rf_vga_dither(), and rf_color_grid_on turns it off there for the plain
 * full-size face. The look is a gradient map: luminance is split into
 * RF_COLOR_GRID_LEVELS bands whose edges come from the image's own histogram
 * (so a dark face and a bright one posterise in the same places), and each
 * band is painted with a flat palette colour. rf_color_grid_soft bleeds a little
 * of the next colour across each band, trading flatness for modelling.
 *
 * The tables live in color_grid.c and are writable: viewer/color_grid_tune.py can
 * rewrite them in place (--emit) or push them to a running board (--push).
 *
 * color_grid.c also carries the script's named sets as a small read-only table,
 * so button 2 (and the "S" command) can cycle whole palettes on the device
 * with no host attached - see rf_color_grid_use_set().
 */
#ifndef RF_COLOR_GRID_H
#define RF_COLOR_GRID_H

#include <stdbool.h>
#include <stdint.h>

/* 1 = build the 2x2 recoloured grid in, 0 = the original single face filling
 * the band, and no way back. */
#ifndef RF_COLOR_GRID
#define RF_COLOR_GRID 1
#endif

/* Which of the two an RF_COLOR_GRID build is showing right now: true = the grid,
 * false = the single face at full size, straight out of the model. Holding
 * button 2 flips it; rf_vga_dither() reads it, so a flip only costs a redraw
 * of the face already in rf_img. It is ignored (and the grid code compiled
 * out) when RF_COLOR_GRID is 0. */
extern bool rf_color_grid_on;

/* The only layout knob. 192 gives the original 384-square band; 216 gives a
 * 432-square one. Both tile RF_ZHW evenly and leave the progress bar room. */
#ifndef RF_PANEL
#define RF_PANEL 192
#endif

#define RF_COLOR_GRID_PANELS 4
#define RF_COLOR_GRID_LEVELS 4

extern uint8_t rf_color_grid_pal[RF_COLOR_GRID_PANELS][RF_COLOR_GRID_LEVELS][3];
extern uint8_t rf_color_grid_q[RF_COLOR_GRID_LEVELS - 1]; /* band splits, percent */
extern uint8_t rf_color_grid_soft;                    /* 0 flat .. 255 smooth */

/* The named sets from viewer/color_grid_tune.py, in the script's own order.
 * rf_color_grid_set is which one is loaded; it is only a label, since "L" (and
 * color_grid_tune.py --push) may have edited rf_color_grid_pal since. */
extern const int rf_color_grid_n_sets;
extern int rf_color_grid_set;
/* Copy set i into rf_color_grid_pal, wrapping i into range. The caller re-runs
 * rf_vga_dither() to see it; nothing is regenerated. */
void rf_color_grid_use_set(int i);
const char *rf_color_grid_set_name(int i);

/* Recompute the band edges from this image's luminance histogram. Call once
 * per image, before any rf_color_grid_map(). */
void rf_color_grid_levels(const uint8_t *img);
/* One source pixel (RF_IMG_CH bytes) -> one recoloured pixel (RF_IMG_CH). */
void rf_color_grid_map(int panel, const uint8_t *src, uint8_t *out);

#endif

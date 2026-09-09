/* Color grid filter tables and gradient map. See color_grid.h. */
#include <string.h>

#include "rf_model.h"
#include "color_grid.h"

/* The grid is what the board boots showing; button 2 held turns it off. */
bool rf_color_grid_on = true;

/* Four palettes, darkest band first, one per panel of the grid. A set shares
 * its near-black ink so the eyes and hairline read the same in all four; the
 * loud colours are the middle bands, which is where a face's skin and hair
 * land. Everything between the markers below is rewritten by
 * viewer/color_grid_tune.py --emit, so design sets there (where you can see them
 * side by side) rather than hand-editing hex. */
/* ---- BEGIN GENERATED: viewer/color_grid_tune.py --set palette1 --emit ---- */
uint8_t rf_color_grid_pal[RF_COLOR_GRID_PANELS][RF_COLOR_GRID_LEVELS][3] = {
    {{0x1b, 0x0a, 0x1e}, {0xe6, 0x00, 0x7e}, {0xff, 0xd4, 0x00}, {0x22, 0xc1, 0xc3}},
    {{0x06, 0x13, 0x1f}, {0x00, 0x57, 0xff}, {0x00, 0xe5, 0xa0}, {0xff, 0xf3, 0x5c}},
    {{0x1f, 0x06, 0x06}, {0xff, 0x2e, 0x00}, {0xff, 0x9e, 0x00}, {0xff, 0xe9, 0xc4}},
    {{0x0d, 0x05, 0x18}, {0x6a, 0x00, 0xf4}, {0xe0, 0x40, 0xfb}, {0xb8, 0xff, 0x5c}},
};
uint8_t rf_color_grid_q[RF_COLOR_GRID_LEVELS - 1] = {18, 46, 76};
uint8_t rf_color_grid_soft = 64;
/* ---- END GENERATED ---- */

/* ---- named palette sets -------------------------------------------------
 * The same sets viewer/color_grid_tune.py offers as --set, in its order, so the
 * device can cycle whole palettes with no host attached (button 2, or "S").
 * Index 0 is what the generated block above is baked with; if you --emit a
 * custom palette, the first press leaves it for set 1 and it is gone until
 * the next flash. Keep this list and the script's SETS in step - or paste a
 * new set into both.
 * Generated from viewer/color_grid_tune.py SETS.
 */
static const uint8_t color_grid_sets[][RF_COLOR_GRID_PANELS][RF_COLOR_GRID_LEVELS][3] = {
    { /* palette1 */
        {{0x1b, 0x0a, 0x1e}, {0xe6, 0x00, 0x7e}, {0xff, 0xd4, 0x00}, {0x22, 0xc1, 0xc3}},
        {{0x06, 0x13, 0x1f}, {0x00, 0x57, 0xff}, {0x00, 0xe5, 0xa0}, {0xff, 0xf3, 0x5c}},
        {{0x1f, 0x06, 0x06}, {0xff, 0x2e, 0x00}, {0xff, 0x9e, 0x00}, {0xff, 0xe9, 0xc4}},
        {{0x0d, 0x05, 0x18}, {0x6a, 0x00, 0xf4}, {0xe0, 0x40, 0xfb}, {0xb8, 0xff, 0x5c}},
    },
    { /* palette2 */
        {{0x14, 0x10, 0x0c}, {0xc8, 0x10, 0x2e}, {0xf5, 0xf0, 0xe1}, {0xe8, 0xc5, 0x47}},
        {{0x0c, 0x10, 0x14}, {0x1d, 0x3f, 0x8f}, {0xf5, 0xf0, 0xe1}, {0xe8, 0xc5, 0x47}},
        {{0x0c, 0x14, 0x10}, {0x2e, 0x8b, 0x57}, {0xf5, 0xf0, 0xe1}, {0xe8, 0xc5, 0x47}},
        {{0x14, 0x0c, 0x14}, {0x6b, 0x2d, 0x5c}, {0xf5, 0xf0, 0xe1}, {0xe8, 0xc5, 0x47}},
    },
    { /* palette3 */
        {{0x0a, 0x1a, 0x0a}, {0xff, 0x2d, 0x95}, {0xff, 0xe1, 0x4d}, {0x1f, 0x6b, 0x3a}},
        {{0x0a, 0x1a, 0x0a}, {0xff, 0x6a, 0x00}, {0xff, 0xf2, 0xb0}, {0x1f, 0x6b, 0x3a}},
        {{0x0a, 0x1a, 0x0a}, {0x00, 0xb3, 0xff}, {0xd6, 0xfa, 0xff}, {0x1f, 0x6b, 0x3a}},
        {{0x0a, 0x1a, 0x0a}, {0xb0, 0x26, 0xff}, {0xff, 0xd6, 0xff}, {0x1f, 0x6b, 0x3a}},
    },
    { /* palette4 */
        {{0x06, 0x13, 0x1f}, {0x00, 0x57, 0xff}, {0x00, 0xe5, 0xa0}, {0xff, 0xf3, 0x5c}},
        {{0x1a, 0x00, 0x16}, {0xff, 0x00, 0x80}, {0x00, 0xff, 0xf0}, {0xff, 0xff, 0xff}},
        {{0x00, 0x1a, 0x0e}, {0x00, 0xff, 0x6a}, {0x00, 0x3c, 0xff}, {0xff, 0xfb, 0x00}},
        {{0x12, 0x00, 0x14}, {0xff, 0x4d, 0x00}, {0xff, 0x00, 0xc8}, {0x8c, 0xff, 0xff}},
    },
    { /* palette5 */
        {{0x0a, 0x0a, 0x0a}, {0x3d, 0x3d, 0x3d}, {0x8a, 0x8a, 0x8a}, {0xf0, 0xf0, 0xf0}},
        {{0x0a, 0x10, 0x18}, {0x2a, 0x4a, 0x6a}, {0x6a, 0x9a, 0xc0}, {0xe8, 0xf4, 0xff}},
        {{0x18, 0x0a, 0x0a}, {0x6a, 0x2a, 0x2a}, {0xc0, 0x8a, 0x6a}, {0xff, 0xf0, 0xe0}},
        {{0x0a, 0x18, 0x0f}, {0x2a, 0x6a, 0x3d}, {0x8a, 0xc0, 0xa0}, {0xe8, 0xff, 0xf0}},
    },
};
static const char *const color_grid_set_names[] = {"palette1", "palette2", "palette3", "palette4", "palette5"};
const int rf_color_grid_n_sets =
    (int)(sizeof color_grid_sets / sizeof color_grid_sets[0]);
int rf_color_grid_set; /* index 0 = the baked palette above */

const char *rf_color_grid_set_name(int i) {
    if (i < 0 || i >= rf_color_grid_n_sets) return "?";
    return color_grid_set_names[i];
}

void rf_color_grid_use_set(int i) {
    i %= rf_color_grid_n_sets;
    if (i < 0) i += rf_color_grid_n_sets;
    memcpy(rf_color_grid_pal, color_grid_sets[i], sizeof rf_color_grid_pal);
    rf_color_grid_set = i;
}

/* Band edges in luminance; edge[0] is 0 and edge[LEVELS] is 256 always. */
static uint16_t edge[RF_COLOR_GRID_LEVELS + 1] = {0, 64, 128, 192, 256};

static inline int luma(const uint8_t *p) {
#if RF_IMG_CH == 3
    return (77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8;
#else
    return p[0];
#endif
}

void rf_color_grid_levels(const uint8_t *img) {
    static uint16_t hist[256]; /* 16384 pixels: fits a u16 bin */
    memset(hist, 0, sizeof hist);
    const int n = RF_IMG_HW * RF_IMG_HW;
    for (int i = 0; i < n; i++) hist[luma(img + (size_t)i * RF_IMG_CH)]++;

    edge[0] = 0;
    edge[RF_COLOR_GRID_LEVELS] = 256;
    int acc = 0, v = 0;
    for (int k = 0; k < RF_COLOR_GRID_LEVELS - 1; k++) {
        /* rf_color_grid_q ascends, so v only ever moves forward */
        int target = n * rf_color_grid_q[k] / 100;
        while (v < 256 && acc + hist[v] <= target) acc += hist[v++];
        edge[k + 1] = (uint16_t)v;
    }
}

void rf_color_grid_map(int panel, const uint8_t *src, uint8_t *out) {
    int y = luma(src);
    int L = RF_COLOR_GRID_LEVELS - 1;
    while (L > 0 && y < edge[L]) L--;
    const uint8_t *a = rf_color_grid_pal[panel][L];
    const uint8_t *b = rf_color_grid_pal[panel][L + 1 < RF_COLOR_GRID_LEVELS ? L + 1 : L];
    /* position within the band, then softened toward pure flat */
    int lo = edge[L], hi = edge[L + 1];
    int t = (hi > lo) ? (y - lo) * 256 / (hi - lo) : 0;
    t = t * rf_color_grid_soft >> 8;
#if RF_IMG_CH == 3
    for (int c = 0; c < 3; c++)
        out[c] = (uint8_t)(a[c] + (((int)b[c] - a[c]) * t >> 8));
#else
    /* gray build: keep the posterisation, drop the colour */
    int g = 0;
    static const int wgt[3] = {77, 150, 29};
    for (int c = 0; c < 3; c++)
        g += wgt[c] * (a[c] + (((int)b[c] - a[c]) * t >> 8));
    out[0] = (uint8_t)(g >> 8);
#endif
}

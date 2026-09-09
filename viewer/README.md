# viewer/

`view_serial.py` — PC-side serial client for a flashed Pico 2.
`color_grid_tune.py` — designs the palettes for the firmware's 2x2 color grid
(see [the section below](#color_grid_tunepy)).

## view_serial.py

PC-side serial client for a flashed Pico 2. Requests a generation over
USB-CDC (virtual com port), prints the device-reported CRC and timing, saves the frame as a
PNG, and can byte-compare it against a released golden.

```
# generate and look at one face
python viewer/view_serial.py --port COM10 --seed 42 --show

# prove the board reproduces the release bit-for-bit
python viewer/view_serial.py --port COM10 --seed 1 \
    --expect checkpoints/m3_decD_deep_full/goldens/golden_1.rgb
```
Needs `pyserial` + `pillow` (both in [requirements.txt](../requirements.txt)).

(`COM10` on Windows; `/dev/ttyACM0`-style on Linux.)

## Parameters

| flag | range / values | meaning |
|---|---|---|
| `--port` | required | serial port of the flashed Pico 2 |
| `--seed` | 0 … 2³²−1 (default 1) | PRNG seed for the latent noise. The same seed produces a **bit-identical** image on every board, the desktop engine, and the numpy simulator |
| `--steps` | 8, 4, 2, 1 (default 4) | Euler integration steps. 8 = the native baked schedule (best quality); 4/2/1 stride it (faster, progressively softer). Only power-of-2 divisors of 8 are valid — the engine rescales the folded step size by an exact shift |
| `--class` | 0–3, else unconditional | if omitted, the device will default to `seed % 5` |
| `--cfg` | 4, 6, 8; other = plain | classifier-free guidance strength `w`. Only the baked values 4/6/8 exist (folded per-w tables); any other value falls back to plain sampling. Requires `--class`. Guided sampling runs the DiT twice per step (~2× generation time). With BOTH `--class` and `--cfg` absent, the device uses the  convention `w_idx = seed % 4 − 1` |
| `--out` | path (default `out/`) | output directory; saves `seed_<N>.png` |
| `--show` | flag | after saving, upscale the frame to 512×512 (nearest-neighbor, so you see the real pixels) and open it in the system image viewer |
| `--expect` | path to `.rgb`/`.gray` | byte-compare the received image against a golden file and print BYTE-EXACT / MISMATCH |

## What the class value encodes

The models are conditioned on **gender × smile**, with labels derived from
the FFHQ facial-attribute annotations
([ffhq-features-dataset](https://github.com/DCGM/ffhq-features-dataset)):

| class | meaning |
|---|---|
| 0 | female, neutral |
| 1 | female, smiling |
| 2 | male, neutral |
| 3 | male, smiling |
| 4 (or any other value / absent guidance) | unconditional — the trained null class (also used internally as the CFG negative) |

Conditioning is soft: classes steer identity/expression statistics, and
higher `--cfg` values enforce them (and overall structure) more strongly.
`w=4` is the balanced default; `w=8` is the strongest, most-typical look.

# color_grid_tune.py

On a `RF_COLOR_GRID` build (the default for the Fruit Jam / DVI firmware) the
display shows the one generated face four times, each panel through its own
palette. Reflashing to compare two pinks is miserable, so this script runs the
firmware's filter on the host — the same integer luminance, the same histogram
band edges, the same softening, and optionally the same RGB332
Floyd–Steinberg the panel dithers with. Its output is **byte-identical** to
what `firmware/color_grid.c` computes, so what you preview is what you get.

```
# grab one face, then iterate on it offline (no board needed after this)
python viewer/view_serial.py --port /dev/ttyACM0 --seed 3
python viewer/color_grid_tune.py --image out/seed_3.png --show

# look at all the built-in sets at once, or sweep one knob
python viewer/color_grid_tune.py --image out/seed_3.png --sheet --show
python viewer/color_grid_tune.py --image out/seed_3.png --sweep soft --show
python viewer/color_grid_tune.py --image out/seed_3.png --sweep q --show

# random candidates; it prints each one as pasteable --palette lines
python viewer/color_grid_tune.py --image out/seed_3.png --random 12 --show

# bake the winner into firmware/color_grid.c, or try it on a running board first
python viewer/color_grid_tune.py --set palette2 --emit
python viewer/color_grid_tune.py --set palette2 --push --port /dev/ttyACM0
```

A golden works as a source too (`--image checkpoints/<model>/goldens/golden_3.rgb`),
and `--port` without `--image` generates a fresh face first.

| flag | meaning |
|---|---|
| `--image` | source face: a PNG, or a raw `.rgb`/`.gray` |
| `--port` | generate a fresh face from a board instead (also used by `--push`) |
| `--set` | named palette set: `palette1`, `palette2`, `palette3`, `palette4`, `palette5` |
| `--palette` | one panel's colours, darkest first; repeat 4x to replace `--set` |
| `--soft` | 0 = flat posterise … 255 = smooth gradient map (default 64) |
| `--q` | band split quantiles, percent of pixels, ascending (default `18,46,76`) |
| `--panel` | `RF_PANEL`: grid cell size, 192 or 216 (default 192) |
| `--sheet` / `--sweep` / `--random` | render many candidates side by side |
| `--dither` / `--no-dither` | simulate the panel's RGB332 dither; on by default for a single render, off for multi-cell output (it is a Python pixel loop) |
| `--emit` | rewrite the generated block in `firmware/color_grid.c` |
| `--push` | send the palette over `L`/`W` to the board on `--port` and redraw |

Adding a set is four rows of four hex colours in `SETS` at the top of the
script. Darkest band first: the first colour is the ink that draws the eyes
and hairline, the middle two carry hair and skin, the last is the highlight.

The firmware carries the same sets (`color_grid_sets[]` in `firmware/color_grid.c`),
so a Fruit Jam can cycle them from button 2 or the `S [n]` serial command with
no host running. Adding one to `SETS` here does not put it on the device —
paste it into that table too.

Needs `numpy` + `pillow` (both in [requirements.txt](../requirements.txt)).

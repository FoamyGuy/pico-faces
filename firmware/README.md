# firmware/

RP2350 (Pico 2) firmware. Compiles the [engine](../engine/) sources
unchanged and adds the device-specific machinery:

| file | role |
|---|---|
| `CMakeLists.txt` | Pico SDK build; `-DMODEL_BIN=<path>` selects the blob to embed |
| `main.c` | boot (VREG 1.30 V → 300 MHz, QMI divider kept flash-legal, `clk_hstx` for the DVI path), USB-CDC protocol, per-stage DWT timing |
| `model_blob.S` | `.incbin`s `model.bin` into `.rodata` (XIP flash) |
| `stage.c` | paced DMA weight streaming from the uncached XIP alias into the SRAM ping-pong arena |
| `par.c` | `par_for()`: core-1 dispatch over disjoint row halves via the inter-core FIFO, no locks |
| `vga_noirq.c` | interrupt-free VGA scanout (default, `RF_VGA=2`) |
| `vga.c` | pico-extras scanvideo fallback (`RF_VGA=1`, costs ~10–15% generation time in IRQs) |
| `dvi_hstx.c` | interrupt-free DVI/HSTX scanout for the Fruit Jam (`RF_DVI=1`) |
| `color_grid.c` / `color_grid.h` | the 2x2 grid's palettes and gradient map (`RF_COLOR_GRID=1`, DVI builds) |
| `buttons.c` / `buttons.h` | debounced press latching for the Fruit Jam's three front-panel buttons (`RF_BUTTONS=1`) |

## Clocks

`RF_SYS_KHZ` (default 300000) is normally free — drop it to 150000 if a board
proves unstable. **`RF_DVI=1` couples it to the video timing.** HSTX is DDR and
spends 10 TMDS bits on a pixel, so `pixel clock = clk_hstx / 5`, and the mode
chosen for the Fruit Jam (640x480 in 952x525 at 60 Hz) needs `clk_hstx` at 
exactly 150 MHz. `clk_hstx` is an integer 1..3 divide of `clk_sys` with no 
fractional part, so `clk_sys` must be 1, 2 or 3 x 150 MHz: **300000 (div 2)
or 150000 (div 1)**. Any other `RF_SYS_KHZ` fails the build — in CMake, and
again as a `#error` in `main.c` for a hand-driven cmake. `RF_DVI` and `RF_VGA`
are mutually exclusive.

## Display backends

Both scanouts implement the same four functions — `rf_vga_init`,
`rf_vga_invalidate`, `rf_vga_dither` and the engine's `rf_step_hook` override.
`main.c` and the engine do not care which is linked. Both are built the
same way: a static DMA command list that loops in hardware, with zero
interrupts and zero CPU in steady state, so the display never competes with
generation.

`dvi_hstx.c` (`-DRF_DVI=1 -DRF_VGA=0 -DPICO_BOARD=adafruit_fruit_jam`) is the
Fruit Jam one. HSTX does the TMDS encoding in hardware. Its framebuffer is the
image band only, RGB332, 4 pixels per 32-bit word: the background and
the progress bar are `TMDS_REPEAT` runs, which cost one FIFO word each rather
than a screen of pixels.

`rf_vga_invalidate()` must point the image band at the static preview rows
*before* `rf_generate()` runs, and `rf_vga_dither()` rewrites each row's
commands as well as its pixels before pointing the band back. A garbage
command reaches the expander as a command, and a desynced expander never
recovers.

The same rule applies to anything drawn while it is *on* screen, and the
progress bar is the only such thing: its two `TMDS_REPEAT` runs must always
add up to `IMG_W`, and the scanout reads those words out of memory as it
draws each of the bar's rows, so a frame can catch a new "done" beside an old
"remaining" — a line the wrong number of pixels long, which is a line of
broken sync, which a monitor answers by dropping out for a moment.
`bar_set()` therefore keeps two bar lines and swaps between them the way the
image band is swapped: one aligned 32-bit `READ_ADDR` store per line slot,
every slot keeping its transfer count. Pixel-only writes (the preview rows,
the readout glyphs) need none of this — they cannot make a line the wrong
length, and can at worst tear one frame of content.

## Front-panel buttons (`RF_BUTTONS`, Fruit Jam)

The Fruit Jam has three buttons, so the board runs as a standalone piece with
nothing plugged into it but a monitor:

| button | pin | pressed | held (`RF_BTN_LONG_MS`, 700 ms) |
|---|---|---|---|
| 1 | GP0 (also BOOT) | draw a **new face** — a fresh seed, current settings | **automatic refresh** on/off |
| 2 | GP4 | **next palette set** — recolours the face already on screen | **color grid** on/off |
| 3 | GP5 | **next generation preset** — then redraws the same seed with it | **previous** generation preset |

A hold is reported the moment it passes `RF_BTN_LONG_MS`, with the finger
still on the button, so the mode changes while the press is happening and the
release that follows is swallowed. A press that came and went while a
generation held the main loop is judged after the fact, from the timestamps
the interrupt recorded, so holding a button through a generation still counts
as a hold.

**Automatic refresh** (hold button 1) turns the board into an unattended
piece: each face stays up for `RF_AUTO_MS` (45 s) and then the next one
starts on its own, from a fresh seed and the current settings, with nobody
pressing anything (where that seed comes from is
[`RF_RANDOM_SEED`](#the-seed-rf_random_seed) — by default a random one, so an
unattended board does not replay the same run of faces after a power cycle). The wait is the progress bar draining — it has nothing
else to say between generations — so the screen shows how long this face has
left. Anything that draws a face restarts the clock, a press or a host `G`
alike, so the face you just asked for gets its full turn. Holding button 1
again goes back to manual and the bar disappears.

Button 2 pressed costs nothing but a re-run of `rf_vga_dither()`: the
generated face stays in `rf_img`, only the gradient map changes, so it lands
instantly. The sets are the named ones from `viewer/color_grid_tune.py`
(`palette1`, `palette2`, `palette3`, `palette4`, `palette5`), carried in `color_grid.c` as a
read-only table beside the live palette; keep the two lists in step if you add
one.

**The color grid** (hold button 2) is what the 2x2 recoloured grid can be
turned off with: `rf_color_grid_on` goes false and the band shows the single
full-size face the model produced, in its own colours, exactly as an
`RF_COLOR_GRID=0` build does. It is a display-time switch — the same `rf_img`,
one more `rf_vga_dither()` — so nothing regenerates and it lands instantly.
The four palettes belong to the grid, so with the grid off a *short* press on
button 2 has nothing to change and does nothing at all.

Button 3 walks `gen_presets[]` in `main.c` — the flat list of the three
variables `rf_generate()` takes besides the seed (`k_steps`, class,
guidance `w`). The first four entries hold a class and move along the quality
axis; the rest pin the class, which is the variable you can actually see. It is
an ordinary table, so re-order it or add to it freely. Holding button 3 walks
the same list the other way, so a preset you have just stepped past is one
hold back rather than a lap of the table — and a lap is a generation apiece.
Holding the seed fixed is deliberate: it makes the settings the only thing
that changed between two screens.

`B\n` over USB prints the state all three buttons share — including which
mode each of the two toggles is in — which is also what the firmware prints
unprompted after each press that changed something (`GEN`, `PAL`, `MODE`, or
`AUTO` for a face the timer asked for).

Presses are latched in a GPIO interrupt, not polled, because a generation
holds the main loop for seconds — a button pressed while a face is drawing is
still acted on when it finishes (one press per button; they do not queue up).
Debounce is arm/disarm rather than a delay: the edge that latches a press
disarms the button, and only a line that has read released and stayed quiet
for `RF_BTN_QUIET_MS` (40) re-arms it, so neither press nor release bounce can
fire twice — and the settled release is also what a press is measured against
to decide whether it was a hold. This is the only interrupt the firmware enables, and it only fires
when someone touches the board, so the "zero IRQs in steady state" property of
the scanout is intact.

`RF_BUTTONS` defaults on for `-DPICO_BOARD=adafruit_fruit_jam` and off
everywhere else (no other board here has buttons wired); `-DRF_BTN1_PIN=...`,
`-DRF_BTN2_PIN`, `-DRF_BTN3_PIN` and `-DRF_BTN_ACTIVE_LOW=0` re-point it at a
board that does. `RF_AUTOGEN` follows it, so a button build also draws one
face at boot rather than waiting to be asked — the board has something on
screen before the first press. `-DRF_AUTOGEN=0` restores the original silent
boot (USB enumerates either way; the first generation just runs first).

Button 1 is also the BOOT button; that is a reset-time function and does not
collide with reading it as an ordinary input while the firmware runs.

## The seed (`RF_RANDOM_SEED`)

Every face is a seed. `G <seed>` names one, but a board with no host attached
has to pick its own — at boot, on a button 1 press, and on every automatic
refresh — and where it picks from is what `RF_RANDOM_SEED` chooses.

`RF_RANDOM_SEED=1` (the default) draws it from `get_rand_32()`, which on the
RP2350 is fed by the hardware TRNG and the per-boot random word. Nothing about
it depends on how long the board has been up, so **each power-on is a
different run of faces**. This is a fix as much as an option: the seed used to
be mixed out of `time_us_64()`, and since the clock starts at zero on every
boot, an unattended board replayed the same faces in the same order every
time — the boot face especially, which was always seed 1.

`RF_RANDOM_SEED=0` boots deterministic instead. The seed starts at `RF_SEED0`
(default 1) and each new face is the next one up, so the board walks one fixed
stream in the same order every time — useful when you want two boards, or two
runs, showing the same thing.

The build flag is only the power-on default. `R 1` and `R 0` switch modes on a
running board, and `G <seed>` sets a specific seed in either mode — in fixed
mode it also picks up where the stream carries on from. `B` reports which mode
is live, and every `print_state()` line carries `random=on|off`.

Seeds stay inside 32 bits whichever mode is on, because that is what the
`RFI2` header and the status lines report — so a face worth keeping can always
be asked for again with `G <seed>`.

No button was reassigned for this: all three buttons already do two things
each. It is a build flag and a serial command.

## The color grid (`RF_COLOR_GRID`, DVI only)

`RF_COLOR_GRID=1` (the default on the DVI path) draws the **one** generated face
four times in a 2x2 grid, each panel through its own palette. It is purely a
display-time transform inside `rf_vga_dither()` — the same single pass over
the same number of band pixels — so it costs no extra generation time and no
extra RAM. `RF_COLOR_GRID=0` builds the grid out entirely and restores the
original single face filling the band.

On a build that has it, `rf_color_grid_on` picks between the two at runtime and
holding button 2 flips it; `RF_COLOR_GRID=1` is only what the board boots
showing. `rf_vga_dither()` reads the flag fresh each time, so the switch is a
redraw of the face already in `rf_img`, and the compiler still folds the grid
away when `RF_COLOR_GRID` is 0.

`color_grid.c` turns a pixel's luminance into a colour: the band is split into
four luminance ranges at population quantiles taken from *that image's own*
histogram (so a dark face and a bright one posterise in the same places), and
each range is painted with a flat palette colour. `rf_color_grid_soft` bleeds a
little of the next colour across each band — 0 is a hard posterise, 255 a
smooth gradient map that keeps the face's modelling.

`RF_PANEL` is the size of one grid cell and the only layout knob; everything
else in `dvi_hstx.c` derives from it.

| `RF_PANEL` | band | origin | bar | framebuffer (of 256 KB arena) |
|---|---|---|---|---|
| 192 (default) | 384x384 | 128, 48 | y 450 | 167 KB |
| 216 | 432x432 | 104, 24 | y 462 | 209 KB |

192 reproduces the original layout pixel for pixel. Other values need to keep
the band a multiple of 4 wide and of `RF_ZHW` (16) tall, and to leave the
progress bar room underneath; `static_assert`s in `dvi_hstx.c` check all
three, and the `rf_arena` fit.

Design palettes with `viewer/color_grid_tune.py` rather than by reflashing — it
runs a byte-identical copy of this filter on the host, so what it previews is
what the device computes.

```
cmake -S firmware -B build/fw -DRF_DVI=1 -DRF_VGA=0 \
      -DPICO_BOARD=adafruit_fruit_jam -DRF_PANEL=216
```

## Build

```
bash scripts/build_firmware.sh m3_decD_deep_full
```

Requirements: [Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.2.0
(+ [pico-extras](https://github.com/raspberrypi/pico-extras) for the VGA
paths only — neither the headless nor the DVI build needs it),
`arm-none-eabi-gcc`, cmake. Point
`PICO_SDK_PATH` / `PICO_EXTRAS_PATH` at your copies. The UF2 lands in
`uf2/pico_faces_<model>.uf2`.

## USB protocol

`I\n` → one line of model geometry and measured clocks.

`G <seed> [k_steps] [class] [w]\n` → `RFI2` header (w, h, channels, class)
+ raw image bytes + CRC32 + timing. `w` = guidance strength (4/6/8; absent
or unmatched = plain).

`V\n` (DVI builds) → `dvi bringup=<n> refresh=<mHz>`; `VR\n` restarts the
scanout first.

`L <panel> <level> <rrggbb>\n` and `W [soft] [q0 q1 q2]\n` (`RF_COLOR_GRID`
builds) set one palette entry, and the softness/quantiles. `W` then re-runs
`rf_vga_dither()` on the image already in memory, so a whole palette can be
tried on a displayed face without regenerating it — that is what
`viewer/color_grid_tune.py --push` drives.

`S [n]\n` (`RF_COLOR_GRID` builds) loads named palette set `n`, or the next one
when `n` is absent — the same cycle button 2 walks, and it redraws the same
way.

`R [0|1]\n` → where the next unasked-for seed comes from: `1` draws it at
random, `0` counts on from the current one. Blank just reports the state. See
[The seed](#the-seed-rf_random_seed) below.

`B\n` → the seed, generation preset and palette set the buttons are on, which
way the two held-button modes are switched, plus which buttons are held down
right now.

You can use the script in `viewer/view_serial.py` to communicate with the device.

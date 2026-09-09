cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "")

# RF_VGA=2 (default): interrupt-free scanout (vga_noirq.c) - static DMA
# rings feeding scanvideo's PIO programs; zero display IRQs/CPU.
# RF_VGA=1: the original scanvideo path (IRQ-driven, kept as fallback).
# RF_VGA=0: headless (USB-CDC only), no display, no pico-extras needed.
if(NOT DEFINED RF_VGA)
    set(RF_VGA 2)
endif()

# RF_DVI=1: DVI over HSTX (firmware/dvi_hstx.c), for the Adafruit Fruit Jam
# Needs -DPICO_BOARD=adafruit_fruit_jam
if(NOT DEFINED RF_DVI)
    set(RF_DVI 0)
endif()
if(RF_DVI AND RF_VGA)
    message(FATAL_ERROR
        "RF_DVI=1 and RF_VGA=${RF_VGA} are mutually exclusive: they are two "
        "scanouts for one framebuffer. Pass -DRF_VGA=0 with -DRF_DVI=1.")
endif()
# RF_COLOR_GRID=1 (default on the DVI path): show the one generated face four
# times in a 2x2 grid, each panel recoloured by firmware/color_grid.c.
# RF_COLOR_GRID=0 restores the original single face filling the band.
# RF_PANEL is the size of one grid cell.
if(NOT DEFINED RF_COLOR_GRID)
    set(RF_COLOR_GRID 1)
endif()
if(NOT DEFINED RF_PANEL)
    set(RF_PANEL 192)
endif()

# RF_BUTTONS=1 (default on the Fruit Jam): drive the firmware from the three
# built-in buttons - GP0 new face, GP4 next palette set, GP5 next
# generation preset, and holding GP0 or GP4 down switches a mode (automatic
# refresh, and the color grid).
if(NOT DEFINED RF_BUTTONS)
    if(PICO_BOARD STREQUAL "adafruit_fruit_jam")
        set(RF_BUTTONS 1)
    else()
        set(RF_BUTTONS 0)
    endif()
endif()
if(NOT DEFINED RF_AUTOGEN)
    set(RF_AUTOGEN ${RF_BUTTONS})
endif()

# RF_RANDOM_SEED=1 (default): pick the seed at random, so a board shows a
# different run of faces every time it is powered on. The entropy is the
# RP2350's hardware TRNG plus its per-boot random word (pico_rand).
# RF_RANDOM_SEED=0 boots deterministic instead: the seed starts at
# RF_SEED0 and each new face is the next one up, so the same board draws the
# same stream in the same order every time. It is only the power-on default -
# "R 0"/"R 1" over USB switches modes on a running board, and "G <seed>" sets
# a specific seed either way.
if(NOT DEFINED RF_RANDOM_SEED)
    set(RF_RANDOM_SEED 1)
endif()
if(NOT DEFINED RF_SEED0)
    set(RF_SEED0 1)
endif()

if(RF_DVI AND NOT PICO_BOARD STREQUAL "adafruit_fruit_jam")
    message(FATAL_ERROR
        "RF_DVI=1 needs -DPICO_BOARD=adafruit_fruit_jam (got '${PICO_BOARD}'): "
        "dvi_hstx.c uses that board's HSTX pin map and DVI pair polarity.")
endif()

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
# pico-extras only supplies the VGA/scanvideo path; a headless or, DVI/HSTX
# build must not require it
if(RF_VGA)
    include($ENV{PICO_EXTRAS_PATH}/external/pico_extras_import.cmake)
endif()

project(pico_faces C CXX ASM)
pico_sdk_init()

# model blob baked into flash (.rodata); override with -DMODEL_BIN=...
if(NOT MODEL_BIN)
    set(MODEL_BIN ${CMAKE_CURRENT_SOURCE_DIR}/../build/model_random.bin)
endif()

# RF_STAGE_DMA=1: overlap weight staging with compute via paced DMA
# (firmware/stage.c). Default 0 = synchronous memcpy staging (the weak
# implementation in kernels_ref.c), byte-identical results either way.
if(NOT DEFINED RF_STAGE_DMA)
    set(RF_STAGE_DMA 0)
endif()

add_executable(pico_faces
    main.c
    par.c
    model_blob.S
    ../engine/src/graph.c
    ../engine/src/dit.c
    ../engine/src/vae_dec.c
    ../engine/src/kernels_ref.c
    ../engine/src/prng.c
)
# per-model rf_cfg.h sits next to the blob; must come before the engine
# default on the include path (rf_model.h uses an angle include)
if(NOT RF_CFG_DIR)
    get_filename_component(RF_CFG_DIR ${MODEL_BIN} DIRECTORY)
endif()
target_include_directories(pico_faces PRIVATE ${RF_CFG_DIR} ../engine/include)
# engine callbacks run ~1.5KB of locals (softmax tables) on both stacks
target_compile_definitions(pico_faces PRIVATE
    PICO_STACK_SIZE=0x1000 PICO_CORE1_STACK_SIZE=0x1000)
set_source_files_properties(model_blob.S PROPERTIES
    COMPILE_DEFINITIONS "MODEL_BIN=${MODEL_BIN}"
    OBJECT_DEPENDS "${MODEL_BIN}")

# RF_SYS_KHZ: 300000 default (vreg 1.30V, QMI div 4 keeps QSPI at 75 MHz);
# build with -DRF_SYS_KHZ=150000 if a board proves unstable
if(NOT RF_SYS_KHZ)
    set(RF_SYS_KHZ 300000)
endif()
if(RF_DVI)
    math(EXPR rf_hstx_div "${RF_SYS_KHZ} / 150000")
    math(EXPR rf_hstx_rem "${RF_SYS_KHZ} % 150000")
    if(rf_hstx_rem OR rf_hstx_div LESS 1 OR rf_hstx_div GREATER 3)
        message(FATAL_ERROR
            "RF_DVI needs clk_hstx = 150 MHz (30 MHz pixel clock), but "
            "RF_SYS_KHZ=${RF_SYS_KHZ} is not 1, 2 or 3 x that. Use 300000 "
            "(the default, div 2) or 150000 (div 1).")
    endif()
endif()
target_compile_definitions(pico_faces PRIVATE
    RF_SYS_KHZ=${RF_SYS_KHZ} RF_DVI=${RF_DVI} RF_HOT_IN_RAM=1
    RF_BUTTONS=${RF_BUTTONS} RF_AUTOGEN=${RF_AUTOGEN}
    RF_RANDOM_SEED=${RF_RANDOM_SEED} RF_SEED0=${RF_SEED0})
if(RF_BUTTONS)
    target_sources(pico_faces PRIVATE buttons.c)
endif()
foreach(btn RF_BTN1_PIN RF_BTN2_PIN RF_BTN3_PIN RF_BTN_ACTIVE_LOW
            RF_BTN_LONG_MS RF_AUTO_MS)
    if(DEFINED ${btn})
        target_compile_definitions(pico_faces PRIVATE ${btn}=${${btn}})
    endif()
endforeach()

target_link_libraries(pico_faces pico_stdlib pico_multicore hardware_vreg
    hardware_clocks pico_rand)
if(RF_STAGE_DMA)
    target_sources(pico_faces PRIVATE stage.c)
    target_link_libraries(pico_faces hardware_dma)
    target_compile_definitions(pico_faces PRIVATE RF_STAGE_DMA=1)
    if(RF_STAGE_HZ)
        target_compile_definitions(pico_faces PRIVATE RF_STAGE_HZ=${RF_STAGE_HZ})
    endif()
endif()
# RF_VGA=1: scanvideo (IRQ-driven, proven). RF_VGA=2: interrupt-free
# scanout - scanvideo's PIO programs + pins, our static DMA rings
# (firmware/vga_noirq.c); no scanvideo runtime linked.
if(RF_VGA EQUAL 2)
    target_sources(pico_faces PRIVATE vga_noirq.c)
    pico_generate_pio_header(pico_faces
        ${PICO_EXTRAS_PATH}/src/common/pico_scanvideo/scanvideo.pio)
    pico_generate_pio_header(pico_faces
        ${PICO_EXTRAS_PATH}/src/rp2_common/pico_scanvideo_dpi/timing.pio)
    target_include_directories(pico_faces PRIVATE
        ${PICO_EXTRAS_PATH}/src/common/pico_scanvideo/include
        ${PICO_EXTRAS_PATH}/src/rp2_common/pico_scanvideo_dpi/include)
    target_link_libraries(pico_faces hardware_pio hardware_dma)
    target_compile_definitions(pico_faces PRIVATE RF_VGA=2)
    if(RF_VGA_TEST) # boot test pattern through the fb path (debug aid)
        target_compile_definitions(pico_faces PRIVATE RF_VGA_TEST=1)
    endif()
elseif(RF_DVI)
    # DVI over HSTX. the TMDS encoding is in hardware and the
    # whole frame is a static DMA command list.
    target_sources(pico_faces PRIVATE dvi_hstx.c)
    target_link_libraries(pico_faces hardware_dma)
    target_compile_definitions(pico_faces PRIVATE
        RF_COLOR_GRID=${RF_COLOR_GRID} RF_PANEL=${RF_PANEL})
    if(RF_VGA_TEST) # boot test pattern through the fb path (debug aid)
        target_compile_definitions(pico_faces PRIVATE RF_VGA_TEST=1)
    endif()
elseif(RF_VGA)
    target_sources(pico_faces PRIVATE vga.c)
    target_link_libraries(pico_faces pico_scanvideo_dpi)
    target_compile_definitions(pico_faces PRIVATE
        RF_VGA=1 PICO_SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS=210)
endif()
pico_enable_stdio_usb(pico_faces 1)
pico_enable_stdio_uart(pico_faces 0)
pico_add_extra_outputs(pico_faces)

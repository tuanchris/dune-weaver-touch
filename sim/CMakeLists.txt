# Desktop simulator: the firmware's ui/app/render/net layers compiled
# unmodified against LVGL 9 + SDL2, with sim/shim standing in for ESP-IDF.
# Build:  cmake -S sim -B sim/build && cmake --build sim/build -j
# Run:    ./sim/build/dwt_sim   (tables via DWT_SIM_TABLES, see stub/)
cmake_minimum_required(VERSION 3.16)
project(dwt_sim C)

# Panel variant, matching the PlatformIO envs. "7" compiles the UI with the
# 7" board's 800x480 geometry so the layout can be worked on without hardware:
#   cmake -S sim -B sim/build -DDWT_BOARD=7
set(DWT_BOARD "5b" CACHE STRING "Panel variant: 5b (1024x600) or 7 (800x480)")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(LVGL_DIR ${REPO}/managed_components/lvgl__lvgl)
if(NOT EXISTS ${LVGL_DIR}/lvgl.h)
    message(FATAL_ERROR "LVGL not found — run `pio run` once to fetch managed components")
endif()

find_library(SDL2_LIB SDL2 PATHS /opt/homebrew/lib /usr/local/lib REQUIRED)

file(GLOB_RECURSE LVGL_SRC ${LVGL_DIR}/src/*.c)
file(GLOB PAGE_SRC ${REPO}/src/ui/pages/*.c)
# Both type scales (5B x1.5 and 7 x1.0); theme.h references one set,
# the linker drops the other. Globbed so a new size needs no edit here.
file(GLOB FONT_SRC ${REPO}/src/ui/fonts/*.c)
set(DEVICE_SRC
    ${FONT_SRC}
    ${REPO}/src/ui/screen_sleep.c
    ${REPO}/src/ui/theme.c
    ${REPO}/src/ui/ui.c
    ${PAGE_SRC}
    ${REPO}/src/app/jobs.c
    ${REPO}/src/app/sd_catalog.c
    ${REPO}/src/app/state.c
    ${REPO}/src/render/thr_preview.c
    ${REPO}/src/net/fw_client.c
    ${REPO}/src/net/settings.c
)

add_executable(dwt_sim
    main_sim.c
    shim/shim.c
    stub/wifi_stub.c
    stub/discovery_stub.c
    stub/board_stub.c
    stub/ota_stub.c
    vendor/cJSON.c
    ${DEVICE_SRC}
    ${LVGL_SRC}
)

target_include_directories(dwt_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}         # lv_conf.h
    ${CMAKE_CURRENT_SOURCE_DIR}/shim    # esp_*.h, freertos/, nvs*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor  # cJSON.h
    ${LVGL_DIR}                         # lvgl.h (+ drivers/... for main_sim)
    ${REPO}/src                         # ui/, app/, net/, render/
    /opt/homebrew/include               # SDL2/
)

if(DWT_BOARD STREQUAL "7")
    target_compile_definitions(dwt_sim PRIVATE BOARD_WAVESHARE_7)
    message(STATUS "dwt_sim: 7\" panel variant (800x480)")
endif()

target_compile_definitions(dwt_sim PRIVATE
    LV_CONF_INCLUDE_SIMPLE
    LV_LVGL_H_INCLUDE_SIMPLE
    SDL_MAIN_HANDLED
    SIM_FS_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/simfs"
)

# Device sources get the /storage -> sim/simfs path remap; shims/stubs do not.
set_source_files_properties(${DEVICE_SRC} PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/shim/sim_remap.h")

target_link_libraries(dwt_sim PRIVATE ${SDL2_LIB} pthread m)

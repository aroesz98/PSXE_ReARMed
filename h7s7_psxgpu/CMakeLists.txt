# SPDX-License-Identifier: Apache-2.0
#
# PSX GPU for the STM32H7S78-DK: the emulator on the i.MX RT1050 sends its GPU
# commands over Ethernet, this board rasterizes them (PSXE/dev/gpu.c, the same
# file the RT1050 build uses) and shows the picture.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(h7s7_psxgpu LANGUAGES C)

set(PSXE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(NEOCHROM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../h7s7_neochrom")

# -----------------------------------------------------------------------------
# NemaGFX - the NeoChrom driver library (see h7s7_neochrom/CMakeLists.txt)
# -----------------------------------------------------------------------------
set(NEMAGFX_ROOT "$ENV{NEMAGFX_ROOT}" CACHE PATH "NemaGFX package root")
if(NOT NEMAGFX_ROOT)
  if(DEFINED ZEPHYR_LVGL_MODULE_DIR AND EXISTS "${ZEPHYR_LVGL_MODULE_DIR}/libs/nema_gfx")
    set(NEMAGFX_ROOT "${ZEPHYR_LVGL_MODULE_DIR}/libs/nema_gfx")
  else()
    message(FATAL_ERROR "NemaGFX not found: run 'west update' (lvgl module) or pass -DNEMAGFX_ROOT=<dir>")
  endif()
endif()

if(EXISTS "${NEMAGFX_ROOT}/include/cortex_m55_m7/nema_core.h")
  set(NEMAGFX_INC "${NEMAGFX_ROOT}/include/cortex_m55_m7")
else()
  set(NEMAGFX_INC "${NEMAGFX_ROOT}/include")
endif()

if(CONFIG_FP_HARDABI)
  set(NEMAGFX_LIB "${NEMAGFX_ROOT}/lib/core/cortex_m7/gcc/libnemagfx-float-abi-hard.a")
else()
  set(NEMAGFX_LIB "${NEMAGFX_ROOT}/lib/core/cortex_m7/gcc/libnemagfx.a")
endif()

if(NOT EXISTS "${NEMAGFX_INC}/nema_core.h" OR NOT EXISTS "${NEMAGFX_LIB}")
  message(FATAL_ERROR "Incomplete NemaGFX package in ${NEMAGFX_ROOT}")
endif()
message(STATUS "NemaGFX: ${NEMAGFX_LIB}")

add_library(nemagfx STATIC IMPORTED GLOBAL)
set_target_properties(nemagfx PROPERTIES IMPORTED_LOCATION "${NEMAGFX_LIB}")

# the HAL Ethernet driver's descriptor rings (the same value for the HAL and for us)
zephyr_compile_definitions(ETH_RX_DESC_CNT=64 ETH_TX_DESC_CNT=4)

target_include_directories(app PRIVATE
  src
  src/shim
  "${PSXE_ROOT}/PSXE"
  "${PSXE_ROOT}/PSXE/dev"
  "${PSXE_ROOT}/PSXE/link"
  "${NEOCHROM_ROOT}/src"
  "${NEMAGFX_INC}"
)
target_link_libraries(app PUBLIC nemagfx)

# gpu.h is read by every file here: on its own, without the emulator's ic.h/cpu.h
target_compile_definitions(app PRIVATE PSX_GPU_STANDALONE=1 PSXE_GPU_REMOTE=0 PSX_PROFILE=1)

target_sources(app PRIVATE
  src/main.c
  src/eth_raw.c
  src/present.c
  src/shim/psx_shim.c
  "${NEOCHROM_ROOT}/src/nema_hal_zephyr.c"
  "${NEOCHROM_ROOT}/src/font8x8.c"
  "${PSXE_ROOT}/PSXE/dev/gpu.c"
)

# the rasterizer: built as on the RT1050 (-O3, unrolled), on its own
set_source_files_properties("${PSXE_ROOT}/PSXE/dev/gpu.c" PROPERTIES
  COMPILE_OPTIONS "-O3;-funroll-loops;-Wno-unused-function;-Wno-unused-variable;-Wno-sign-compare;-Wno-missing-braces")

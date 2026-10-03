cmake_minimum_required(VERSION 3.22)
project(wl4 C)
# WL4_PORT_DIR/build/wasm2c is produced by port/build_wasm.sh from the user's own ROM.
set(W2C ${WL4_PORT_DIR}/build/wasm2c)
if(NOT EXISTS ${W2C}/wl4.c)
  message(FATAL_ERROR "Run port/build_wasm.sh first (needs your ROM): ${W2C}/wl4.c missing")
endif()
execute_process(COMMAND python3 ${WL4_PORT_DIR}/tools/gen_stubs.py ${W2C}/wl4.h ${CMAKE_BINARY_DIR}/stubs.c)
file(GLOB W2C_RT ${W2C}/wasm-rt-*.c)
list(FILTER W2C_RT EXCLUDE REGEX "-win\\.c$")   # impl, mem-impl, os-unix, exceptions-impl (names vary by wabt version)
add_library(wl4 SHARED
  jni.c
  ${WL4_PORT_DIR}/host/host.c
  ${WL4_PORT_DIR}/hal/hal.c ${WL4_PORT_DIR}/hal/ppu.c ${WL4_PORT_DIR}/hal/bios.c
  ${CMAKE_BINARY_DIR}/stubs.c
  ${W2C}/wl4.c ${W2C_RT})
target_include_directories(wl4 PRIVATE ${W2C} ${WL4_PORT_DIR}/host)
target_compile_options(wl4 PRIVATE -O2 -w -fno-strict-aliasing -DWASM_RT_TRAP_HANDLER=wl4_trap)
target_link_libraries(wl4 log m)

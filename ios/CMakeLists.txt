cmake_minimum_required(VERSION 3.22)
# Configure with the Xcode generator for iOS, e.g. (see .github/workflows/wl4-ios.yml):
#   cmake -S ios -B ios/build -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
#         -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DWL4_PORT_DIR=$PWD/port -DWL4_VERSION=us
#   cmake --build ios/build --config Release -- CODE_SIGNING_ALLOWED=NO
project(wl4ios C OBJC)

set(WL4_PORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../port" CACHE PATH "port/ directory of the repo")
set(WL4_VERSION "us" CACHE STRING "ROM version the wasm2c output was built from: us | jp")
option(WL4_W2C_MMAP "Let wasm-rt reserve guard pages with mmap (faster, needs a big virtual reservation). OFF = explicit bounds checks." OFF)
set(CMAKE_C_FLAGS_RELEASE "-O2")                 # not -O3: wl4.c is huge, -O3 costs CI minutes/RAM for nothing

set(W2C ${WL4_PORT_DIR}/build/wasm2c)            # produced by port/build_wasm.sh from the user's own ROM
if(NOT EXISTS ${W2C}/wl4.c)
  message(FATAL_ERROR "Run port/build_wasm.sh first (needs your ROM): ${W2C}/wl4.c missing")
endif()

if(WL4_VERSION STREQUAL "us")
  set(WL4_EXPECT_CODE "AWAE")
elseif(WL4_VERSION STREQUAL "jp")
  set(WL4_EXPECT_CODE "AWAJ")
else()
  message(FATAL_ERROR "WL4_VERSION must be us or jp")
endif()
file(WRITE ${CMAKE_BINARY_DIR}/gen/wl4_config.h "#define WL4_EXPECT_CODE \"${WL4_EXPECT_CODE}\"\n")

execute_process(COMMAND python3 ${WL4_PORT_DIR}/tools/gen_stubs.py ${W2C}/wl4.h ${CMAKE_BINARY_DIR}/gen/stubs.c
                RESULT_VARIABLE STUBS_RC)
if(NOT STUBS_RC EQUAL 0)
  message(FATAL_ERROR "gen_stubs.py failed")
endif()

file(GLOB W2C_RT ${W2C}/wasm-rt-*.c)
list(FILTER W2C_RT EXCLUDE REGEX "-win\\.c$")   # impl, mem-impl, os-unix, exceptions-impl (names vary by wabt version)

# ---- portable core: identical to the Android build ----
set(CORE_SRCS
  ${WL4_PORT_DIR}/host/host.c ${WL4_PORT_DIR}/host/audio.c
  ${WL4_PORT_DIR}/hal/hal.c ${WL4_PORT_DIR}/hal/ppu.c ${WL4_PORT_DIR}/hal/bios.c
  ${CMAKE_BINARY_DIR}/gen/stubs.c
  ${W2C}/wl4.c ${W2C_RT}
  Sources/WL4Log.c)
set(CORE_FLAGS "-w -fno-strict-aliasing -DWASM_RT_TRAP_HANDLER=wl4_trap -DWL4_TRACE=1")
if(NOT WL4_W2C_MMAP)
  # iOS: no guard-page reservation (big PROT_NONE mapping + SIGSEGV-based traps); the explicit-bounds-check mode is
  # slower but needs no signal handler and no giant mapping.
  set(CORE_FLAGS "${CORE_FLAGS} -DWASM_RT_USE_MMAP=0")
endif()
set_source_files_properties(${CORE_SRCS} PROPERTIES COMPILE_FLAGS "${CORE_FLAGS}")

# ---- Objective-C shell ----
set(UI_SRCS
  Sources/main.m Sources/AppDelegate.m Sources/WL4Session.m
  Sources/MenuViewController.m Sources/GameViewController.m Sources/GameView.m Sources/PadView.m)
set_source_files_properties(${UI_SRCS} PROPERTIES COMPILE_FLAGS "-fobjc-arc")

add_executable(wl4 MACOSX_BUNDLE ${CORE_SRCS} ${UI_SRCS})
target_include_directories(wl4 PRIVATE ${W2C} ${WL4_PORT_DIR}/host ${CMAKE_CURRENT_SOURCE_DIR}/Sources ${CMAKE_BINARY_DIR}/gen)
target_link_libraries(wl4 PRIVATE
  "-framework UIKit" "-framework Foundation" "-framework QuartzCore" "-framework CoreGraphics"
  "-framework AudioToolbox" "-framework AVFoundation" "-framework GameController" "-framework UniformTypeIdentifiers")

set_target_properties(wl4 PROPERTIES
  OUTPUT_NAME "WL4"
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.wl4.port"
  XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "14.0"
  XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
  XCODE_ATTRIBUTE_ENABLE_BITCODE "NO"
  XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"        # unsigned IPA: TrollStore fake-signs on install, AltStore/Sideloadly sign on their side
  XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
  XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "")

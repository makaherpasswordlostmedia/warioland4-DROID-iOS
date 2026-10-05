#!/usr/bin/env bash
# decomp C  ->  wasm32  ->  wasm2c C   (input for the Android NDK build)
#
#   WASI_SDK=/path/to/wasi-sdk  WABT=/path/to/wabt  ./port/build_wasm.sh [us|jp]
#
# Needs the user's own ROM at <repo>/baserom_<ver>.gba and `make extract` to have run
# (the decomp #includes extracted palettes/graphics).  Nothing ROM-derived is committed.
set -euo pipefail
VER=${1:-us}
REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$REPO/port/build}
: "${WASI_SDK:?set WASI_SDK}"; : "${WABT:?set WABT}"
# OPT_LEVEL controls -O passed to wasi-clang.  Default 2.  Set to 0 or 1 when
# the runner runs out of memory during -O2 optimization of the giant wl4.c.
OPT_LEVEL=${OPT_LEVEL:-2}
CC="$WASI_SDK/bin/clang --target=wasm32"
ROM=$REPO/baserom_$VER.gba
[ -f "$ROM" ] || { echo "missing $ROM"; exit 1; }
ROMSIZE=$(wc -c < "$ROM" | tr -d ' ')      # portable: BSD stat has no -c
GLOBAL_BASE=167772160        # 0x0A000000: above the GBA address space (ROM ends at 0x0A000000)
MEM_BYTES=235995136          # 0x0E010000: covers ROM mirror, wasm data and 64KB SRAM at 0x0E000000
STACK=1048576

rm -rf "$OUT"; mkdir -p "$OUT/obj" "$OUT/wasm2c"
python3 "$REPO/port/tools/portify.py" "$REPO" "$OUT/p"
cp "$ROM" "$OUT/baserom.gba"
cp "$OUT/p/ASM_SITES.txt" "$OUT/ASM_SITES.txt"

CFLAGS="-O${OPT_LEVEL} -ffreestanding -fno-builtin -nostdinc -w -Wno-error=implicit-function-declaration -Wno-error=incompatible-function-pointer-types -Wno-error=int-conversion -Wno-error=return-mismatch -DNON_MATCHING -DNONMATCHING -DVERSION_$(echo "$VER" | tr a-z A-Z) -include types.h -include oam.h -include port.h \
        -I$OUT/p/include -I$REPO -fno-strict-aliasing -fwrapv"
: > "$OUT/FAILED.txt"; ok=0; bad=0
while IFS= read -r f; do
  o="$OUT/obj/$(echo "${f#$OUT/p/src/}" | tr / _).o"
  if $CC $CFLAGS -c "$f" -o "$o" 2>>"$OUT/errors.log"; then ok=$((ok+1)); else bad=$((bad+1)); echo "${f#$OUT/p/}" >> "$OUT/FAILED.txt"; fi
done < <(find "$OUT/p/src" -name '*.c' | sort)
echo "compiled $ok files, $bad failed (see $OUT/FAILED.txt, errors.log)"

# Wario's pose/handler tables are raw ARM addresses in the blobs; wasm call_indirect needs real table
# indices.  Generate C tables (names only) from the user's ROM.  Must precede the nm step below so
# gen_blobs.py skips the aliases for these symbols.
python3 "$REPO/port/tools/gen_wario_tables.py" "$REPO" "$ROM" "$OUT/wario_tables.c"
$CC $CFLAGS -c "$OUT/wario_tables.c" -o "$OUT/obj/wario_tables.o"

# ROM blobs: alias every named blob that no compiled C data file already defines
"$WASI_SDK/bin/llvm-nm" --defined-only -g "$OUT"/obj/*.o 2>/dev/null | awk 'NF==3{print $3}' > "$OUT/defined.txt"
python3 "$REPO/port/tools/gen_blobs.py" "$REPO" "$OUT/blobs.S" "$ROMSIZE" "$OUT/defined.txt"
$CC -x assembler -I"$OUT" -c "$OUT/blobs.S" -o "$OUT/obj/blobs.o"

# C sources that replace ARM-only parts (m4a mixer/sequencer, crt0, BIOS stubs) go in port/rt/*.c
for f in "$REPO"/port/rt/*.c; do [ -f "$f" ] && $CC $CFLAGS -mbulk-memory -c "$f" -o "$OUT/obj/rt_$(basename "$f").o"; done

# asm/data/*.s tables (palette-fade amounts/step limits, demo configs, ...) are not blobs and were never assembled:
# their symbols resolved to address 0 and the file-select fade-in never finished (black screen).  Emit them as C.
python3 "$REPO/port/tools/gen_asm_data.py" "$REPO" "$OUT/asm_data.c"
$CC $CFLAGS -c "$OUT/asm_data.c" -o "$OUT/obj/asm_data.o"

"$WASI_SDK/bin/wasm-ld" "$OUT"/obj/*.o -o "$OUT/wl4.wasm" --no-entry --allow-undefined --export-memory \
  --export=AgbMain --export=rom_image --export=InterruptCallbackCallVBlank --export=InterruptCallbackCallHBlank --export=InterruptCallbackCallVCount \
  --global-base=$GLOBAL_BASE -z stack-size=$STACK --initial-memory=$MEM_BYTES --max-memory=$MEM_BYTES --gc-sections
"$WABT/bin/wasm2c" "$OUT/wl4.wasm" --module-name=wl4 -o "$OUT/wasm2c/wl4.c"
cp "$WABT"/share/wabt/wasm2c/wasm-rt*.[ch] "$WABT"/share/wabt/wasm2c/*.inc "$WABT/include/wasm-rt.h" "$OUT/wasm2c/" 2>/dev/null || true
# The install layout of the wasm-rt files differs between wabt versions; WABT_SRC (a wabt checkout) fills in whatever is missing.
if [ -n "${WABT_SRC:-}" ]; then
  cp -n "$WABT_SRC"/wasm2c/wasm-rt*.[ch] "$WABT_SRC"/wasm2c/*.inc "$OUT/wasm2c/" 2>/dev/null || true
fi
for need in wasm-rt.h wasm-rt-impl.c; do [ -f "$OUT/wasm2c/$need" ] || { echo "missing wasm2c runtime file $need (set WABT_SRC to a wabt checkout)"; exit 1; }; done
python3 "$REPO/port/tools/patch_w2c_io.py" "$OUT/wasm2c/wl4.c"
python3 "$REPO/port/tools/patch_w2c_trace.py" "$OUT/wasm2c/wl4.c"
python3 "$REPO/port/tools/patch_w2c_div.py" "$OUT/wasm2c/wl4.c"
echo "done: $OUT/wasm2c/wl4.c"

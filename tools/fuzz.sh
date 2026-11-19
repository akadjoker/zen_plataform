#!/bin/sh
# usage: tools/fuzz.sh [target...]
#
# Runs the libFuzzer targets of tests/fuzz with ASan + UBSan, from a fixed seed, for a time budget each:
#   fuzz_images       png_decode (clipboard images) and framebuffer_load_bmp
#   fuzz_paths        path_* and the fs_* / io_* calls on odd names (in a sandbox under /dev/shm)
#   fuzz_x11_events   what another X client sends a window: ClientMessage, SelectionNotify, ConfigureNotify, XDND drops
#   fuzz_clipboard_x11  the clipboard against a hostile owner of the selection (any property, INCR of any shape)
# Needs clang with libFuzzer (compiler-rt) and, for fuzz_x11_events, xvfb-run (started through tools/xvfb_run.sh).
# The start corpus is tests/fuzz/corpus/<name> (small files; every input that ever broke something is kept there, and
# runs as an ordinary test: test_fuzz_*). A crash, hang or leak is written to $ZP_FUZZ_DIR/work_<target>/ as
# crash-* / leak-* / timeout-*: copy it into tests/fuzz/corpus/<name> once the bug is fixed.
#
# Environment:
#   ZP_FUZZ_SECONDS  time budget of each target (default 120)
#   ZP_FUZZ_SEED     the seed (default 1)
#   ZP_FUZZ_DIR      build and work folder (default ${TMPDIR:-/tmp}/zp-fuzz-$USER); keep it small, delete it when done
#   ZP_JOBS          compiler processes (default: half the processors)

set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIR=${ZP_FUZZ_DIR:-${TMPDIR:-/tmp}/zp-fuzz-$(id -un)}
SECS=${ZP_FUZZ_SECONDS:-120}
SEED=${ZP_FUZZ_SEED:-1}
JOBS=${ZP_JOBS:-$(($(nproc) / 2 > 0 ? $(nproc) / 2 : 1))}
TARGETS=${*:-fuzz_images fuzz_paths fuzz_x11_events fuzz_clipboard_x11}
command -v clang > /dev/null || { echo "fuzz.sh: clang not found" >&2; exit 2; }
SAN="-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined"
CFLAGS="-g -O1 -fno-omit-frame-pointer -DPLATFORM_STATIC -I$ROOT/include -I$ROOT/src"
FS="$ROOT/src/os.c $ROOT/src/error.c $ROOT/src/log.c $ROOT/src/io.c $ROOT/src/io_posix.c $ROOT/src/fs_posix.c"
mkdir -p "$DIR" || exit 2
FAILED=0

for t in $TARGETS; do
    case $t in
        fuzz_images) corpus=images; srcs="$ROOT/src/png.c $ROOT/src/image.c $ROOT/src/draw2d.c $FS"; run= ;;
        fuzz_paths) corpus=paths; srcs="$FS"; run= ;;
        fuzz_x11_events | fuzz_clipboard_x11)
            corpus=x11_events; [ $t = fuzz_clipboard_x11 ] && corpus=clipboard_x11; run=$ROOT/tools/xvfb_run.sh
            # the library itself, with the sanitizers: the harness drives its public API
            if [ ! -f "$DIR/clang/libplatform.a" ] || [ -n "$(find "$ROOT/src" "$ROOT/include" -newer "$DIR/clang/libplatform.a" -name '*.[ch]' | head -n 1)" ]; then
                CC=clang cmake -S "$ROOT" -B "$DIR/clang" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPLATFORM_BUILD_TESTS=OFF -DPLATFORM_BUILD_EXAMPLES=OFF \
                    "-DCMAKE_C_FLAGS=-g -O1 -fno-omit-frame-pointer -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined" > "$DIR/clang.log" 2>&1 &&
                    cmake --build "$DIR/clang" -j "$JOBS" --target platform >> "$DIR/clang.log" 2>&1 || { echo "FAIL $t: the library did not build, see $DIR/clang.log"; FAILED=1; continue; }
            fi
            srcs="$DIR/clang/libplatform.a -lX11 -lXrandr -lGL -ldl -lpthread" ;;
        *) echo "fuzz.sh: unknown target $t" >&2; exit 2 ;;
    esac
    # shellcheck disable=SC2086
    clang $CFLAGS $SAN "$ROOT/tests/fuzz/$t.c" $srcs -o "$DIR/$t" > "$DIR/$t.build.log" 2>&1 || { echo "FAIL $t: did not build, see $DIR/$t.build.log"; FAILED=1; continue; }
    work=$DIR/work_$t
    mkdir -p "$work"
    echo "== $t: ${SECS}s, seed $SEED"
    # shellcheck disable=SC2086
    (cd "$work" && $run "$DIR/$t" -seed="$SEED" -max_total_time="$SECS" -max_len=65536 -rss_limit_mb=1500 -timeout=20 \
        -artifact_prefix="$work/" "$ROOT/tests/fuzz/corpus/$corpus" > "$work/log" 2>&1)
    if grep -q "^SUMMARY: \|ERROR: " "$work/log"; then
        echo "FAIL $t: $(grep -m1 'SUMMARY: ' "$work/log" | cut -c1-200)"; echo "     inputs in $work"; FAILED=1
    else
        echo "PASS $t: $(grep -E '^Done|DONE' "$work/log" | tail -n 1 | cut -c1-120)"
    fi
done
[ $FAILED -eq 0 ] || exit 1

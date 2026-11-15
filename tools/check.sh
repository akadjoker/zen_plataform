#!/bin/sh
# usage: tools/check.sh [stage...]
#
# One command that says whether the tree is good. The stages, in this order:
#   release  Release build, every test
#   asan     Debug build with -DPLATFORM_SANITIZE=ON (ASan + UBSan; a UBSan report fails the test), every test
#   tsan     ThreadSanitizer build, the tests that run threads (TSAN_TESTS below)
#   mingw    cross-compile for Windows with -Wall -Wextra, compile and link only (nothing is run: no wine here)
# With no argument all four run, one after the other. Every test starts through tools/xvfb_run.sh (invisible Xvfb,
# no Wayland, scratch settings) and the tests run one at a time. Each stage prints one line, PASS or FAIL with the
# counts; the exit status is not zero if any stage failed. A stage whose tool is missing FAILs, it does not skip
# itself: leave a stage out by naming the ones you want.
#
# Environment:
#   ZP_BUILD_DIR  where the build trees and logs go (default: ${TMPDIR:-/tmp}/zp-check-$USER); kept between runs,
#                 so a second run only rebuilds what changed (the mingw tree is rebuilt, see below)
#   ZP_JOBS       compiler processes (default: half the processors)
#   TSAN_TESTS    the ctest -R regex of the tsan stage
#
# The mingw stage fails on any warning outside the vendored stb headers: it is the only stage that compiles the
# Windows backend on Linux, and a warning there (a wrong format, an unused result) is a bug nobody sees until a
# Windows build.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${ZP_BUILD_DIR:-${TMPDIR:-/tmp}/zp-check-$(id -un)}
JOBS=${ZP_JOBS:-$(($(nproc) / 2 > 0 ? $(nproc) / 2 : 1))}
TSAN_TESTS=${TSAN_TESTS:-'test_thread|test_io|test_crash|test_process|test_gl_worker_x11|test_x11_lifecycle|test_multiwindow_x11'}
RUN=$ROOT/tools/xvfb_run.sh

STAGES=${*:-release asan tsan mingw}
for s in $STAGES; do
    case $s in
        release|asan|tsan|mingw) ;;
        *) echo "unknown stage '$s' (release asan tsan mingw)" >&2; exit 2 ;;
    esac
done

mkdir -p "$BUILD" || exit 2
GEN=
command -v ninja > /dev/null && GEN="-G Ninja"
FAILED=0
SUMMARY=

now() { date +%s; }
load() { cut -d' ' -f1-3 /proc/loadavg; }

# configure_and_build <dir> <log> <cmake args...>: 0 when both worked
configure_and_build() {
    dir=$1; log=$2; shift 2
    # shellcheck disable=SC2086
    cmake -S "$ROOT" -B "$dir" $GEN -DPLATFORM_BUILD_EXAMPLES=OFF "$@" > "$log" 2>&1 || return 1
    cmake --build "$dir" -j "$JOBS" >> "$log" 2>&1 || return 1
}

# report <stage> <ok: 0 or 1> <detail> <start time>
report() {
    if [ "$2" = 0 ]; then word=PASS; else word=FAIL; FAILED=1; fi
    line=$(printf '%-8s %s  %s  (%ss, load %s)' "$1" "$word" "$3" "$(($(now) - $4))" "$(load)")
    echo "$line"
    SUMMARY="$SUMMARY$line
"
}

# test_stage <name> <cmake args...>: build, then ctest one test at a time through the wrapper.
# In the tsan stage ctest runs under setarch -R and only the tests of TSAN_TESTS.
test_stage() {
    name=$1; shift
    t0=$(now)
    dir=$BUILD/$name
    if [ "$name" = tsan ] && ! setarch "$(uname -m)" -R true 2> /dev/null; then
        report "$name" 1 "setarch -R does not work here, and TSan needs it on this kernel" "$t0"; return
    fi
    configure_and_build "$dir" "$dir.build.log" "$@" || {
        report "$name" 1 "the build failed, see $dir.build.log" "$t0"; return; }
    if [ "$name" = tsan ]; then
        "$RUN" setarch "$(uname -m)" -R ctest --test-dir "$dir" --output-on-failure -R "$TSAN_TESTS" > "$dir.test.log" 2>&1
    else
        "$RUN" ctest --test-dir "$dir" --output-on-failure > "$dir.test.log" 2>&1
    fi
    rc=$?
    total=$(sed -n 's/^.* tests failed out of \([0-9]*\).*/\1/p' "$dir.test.log" | tail -n 1)
    nfailed=$(sed -n 's/^.*, \([0-9]*\) tests failed out of.*/\1/p' "$dir.test.log" | tail -n 1)
    skipped=$(grep -c '\*\*\*Skipped' "$dir.test.log")
    if [ $rc -ne 0 ] || [ "${nfailed:--1}" != 0 ] || [ "${total:-0}" = 0 ]; then
        failing=$(sed -n 's/^ *[0-9]* - \(test_[a-z0-9_]*\).*/\1/p' "$dir.test.log" | sort -u | tr '\n' ' ')
        report "$name" 1 "${nfailed:-?} of ${total:-?} tests failed: ${failing:-see the log} ($dir.test.log)" "$t0"
    else
        report "$name" 0 "$total tests, $skipped skipped" "$t0"
    fi
}

mingw_stage() {
    t0=$(now)
    dir=$BUILD/mingw
    if ! command -v x86_64-w64-mingw32-gcc > /dev/null; then
        report mingw 1 "x86_64-w64-mingw32-gcc not found" "$t0"; return
    fi
    mkdir -p "$BUILD/mingw-tc"
    cat > "$BUILD/mingw-tc/toolchain.cmake" <<'TC'
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
TC
    # A clean tree every time: an up-to-date object prints no warning, and this stage is about the warnings.
    rm -rf "$dir"
    configure_and_build "$dir" "$dir.build.log" -DCMAKE_TOOLCHAIN_FILE="$BUILD/mingw-tc/toolchain.cmake" \
        -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_FLAGS=-Wall -Wextra" || {
        report mingw 1 "the build failed, see $dir.build.log" "$t0"; return; }
    # one line per warning ("path:line:col: warning: text"); the vendored stb headers are not ours to edit
    sort -u "$dir.build.log" | grep ' warning: ' | grep -v '/stb_[a-z_]*\.h:' > "$dir.warnings.txt"
    n=$(wc -l < "$dir.warnings.txt")
    if [ "$n" -ne 0 ]; then
        report mingw 1 "$n warnings with -Wall -Wextra outside the stb headers, see $dir.warnings.txt" "$t0"
    else
        report mingw 0 "compiled and linked with -Wall -Wextra, no warnings (nothing run)" "$t0"
    fi
}

echo "check.sh: $ROOT, builds in $BUILD, $JOBS jobs, load $(load)"
for s in $STAGES; do
    case $s in
        release) test_stage release -DCMAKE_BUILD_TYPE=Release ;;
        asan)    test_stage asan -DCMAKE_BUILD_TYPE=Debug -DPLATFORM_SANITIZE=ON ;;
        tsan)    test_stage tsan -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_C_FLAGS=-fsanitize=thread -g -O1 -fno-omit-frame-pointer" \
                     -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread ;;
        mingw)   mingw_stage ;;
    esac
done

echo "----"
printf '%s' "$SUMMARY"
if [ $FAILED -ne 0 ]; then echo "check.sh: FAILED"; exit 1; fi
echo "check.sh: all stages passed"

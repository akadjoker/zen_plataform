#!/bin/sh
# usage: tools/bench_wayland.sh [-n runs] [stage...]
#
# zen_platform against SDL2 and GLFW 3 on a headless Wayland compositor: the small compositor of tests/test_input_wayland.c
# built as a program of its own (bench/compare/wl_compositor: wl_compositor, xdg_wm_base, wl_seat, wl_shm and one wl_output;
# nothing is ever shown), with the same programs as tools/bench_compare.sh (bench/compare) built without X11: start-up,
# memory, an idle poll, a window create + destroy, the wait and wake. The event modes need XTest and stay X11 only. Every
# window has a GL context (the Wayland backend of zen_platform has only RENDER_GL windows so far), on Mesa's software
# renderer (LIBGL_ALWAYS_SOFTWARE=1: the compositor has no buffer sharing). The numbers come out as markdown tables on
# stdout and in $ZP_BENCH_DIR/results.md.
#
# Stages (default: all): startup mem idle cycle wait
#
# Environment:
#   ZP_BENCH_SRC   source tree to build (default: this repository)
#   ZP_BENCH_DIR   build tree, scratch and results.md (default: ${TMPDIR:-/tmp}/zp-bench-wl-$USER)
#   ZP_BENCH_RUNS  runs per timing, the table shows the median (default 11; -n overrides)
#   ZP_BENCH_LIBS  the libraries to run, a subset of "zp glfw sdl2" (default: those that were built)
#   GLFW_ROOT      a prefix holding usr/include/GLFW and usr/lib/x86_64-linux-gnu/libglfw.so of the WAYLAND build of GLFW (an
#                  unpacked libglfw3-wayland and libglfw3-dev, say): the system's libglfw3 is X11 only
#   VALGRIND       a valgrind binary (default: the one on the PATH); VALGRIND_LIB its libexec/valgrind. Without it the
#                  instruction counts are left out
# Nothing is installed: GLFW and valgrind can be packages unpacked with dpkg-deb -x into a folder of their own. The compositor
# runs in a private XDG_RUNTIME_DIR, HOME is scratch, DISPLAY is unset (SDL and GLFW cannot fall back to X11), and the
# compositor is stopped by the PID this script recorded.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
RUNS=${ZP_BENCH_RUNS:-11}
if [ "${1:-}" = -n ]; then RUNS=$2; shift 2; fi
STAGES=${*:-startup mem idle cycle wait}
SRC=${ZP_BENCH_SRC:-$ROOT}
DIR=${ZP_BENCH_DIR:-${TMPDIR:-/tmp}/zp-bench-wl-$(id -un)}
VG=${VALGRIND:-$(command -v valgrind || true)}
mkdir -p "$DIR/home" || exit 2
export HOME=$DIR/home
unset LD_PRELOAD DISPLAY XMODIFIERS
export BENCH_SETLOCALE=1 SDL_VIDEODRIVER=wayland LIBGL_ALWAYS_SOFTWARE=1
[ -n "${GLFW_ROOT:-}" ] && export LD_LIBRARY_PATH=$GLFW_ROOT/usr/lib/x86_64-linux-gnu
OUT=$DIR/results.md
: > "$OUT"

GEN=
command -v ninja > /dev/null && GEN="-G Ninja"
GLFW_ARG=
[ -n "${GLFW_ROOT:-}" ] && GLFW_ARG=-DZP_COMPARE_GLFW_ROOT=$GLFW_ROOT
build=$DIR/build
# shellcheck disable=SC2086
cmake -S "$SRC" -B "$build" $GEN -DCMAKE_BUILD_TYPE=None "-DCMAKE_C_FLAGS=-O2 -DNDEBUG" -DPLATFORM_LINUX_BACKEND=wayland \
    -DPLATFORM_BUILD_TESTS=OFF -DPLATFORM_BUILD_EXAMPLES=OFF -DPLATFORM_BUILD_COMPARE=ON $GLFW_ARG > "$build.log" 2>&1 &&
    cmake --build "$build" -j "$(($(nproc) / 2 > 0 ? $(nproc) / 2 : 1))" >> "$build.log" 2>&1 ||
    { echo "bench_wayland.sh: build failed, see $build.log" >&2; exit 2; }
B=$build/bench/compare
LIBS=zp
[ -x "$B/bench_glfw" ] && LIBS="$LIBS glfw"
[ -x "$B/bench_sdl2" ] && LIBS="$LIBS sdl2"
[ -n "${ZP_BENCH_LIBS:-}" ] && LIBS=$ZP_BENCH_LIBS
RUN=$B/bench_once

# the compositor, in a runtime directory of its own
XDG_RUNTIME_DIR=$(mktemp -d "${TMPDIR:-/tmp}/zp-wl-XXXXXX") || exit 2
export XDG_RUNTIME_DIR
"$B/wl_compositor" > "$XDG_RUNTIME_DIR/name" 2> "$XDG_RUNTIME_DIR/compositor.err" &
COMPOSITOR=$!
trap 'kill "$COMPOSITOR" 2> /dev/null; wait "$COMPOSITOR" 2> /dev/null; rm -rf "$XDG_RUNTIME_DIR"' EXIT
trap 'exit 130' INT TERM
i=0
while [ ! -s "$XDG_RUNTIME_DIR/name" ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
[ -s "$XDG_RUNTIME_DIR/name" ] || { echo "bench_wayland.sh: the compositor did not start" >&2; exit 2; }
WAYLAND_DISPLAY=$(cat "$XDG_RUNTIME_DIR/name")
export WAYLAND_DISPLAY

say() { printf '%s\n' "$*" | tee -a "$OUT"; }
median() {
    grep -o "$1=[-0-9.]*" | cut -d= -f2 | sort -n | awk '{a[NR]=$1} END{if (NR) printf "%s", a[int((NR+1)/2)]; else printf "n/a"}'
}
rep() { n=$1; shift; i=0; while [ "$i" -lt "$n" ]; do "$@" 2> /dev/null; i=$((i + 1)); done; }
ir() {
    [ -n "$VG" ] || { printf 'n/a'; return; }
    "$VG" --tool=callgrind "--callgrind-out-file=$DIR/cg.out" --cache-sim=no "$@" 2>&1 > /dev/null | grep -o 'Collected : [0-9]*' | grep -o '[0-9]*$'
}
diff_per() { case "$1$2" in *n/a*) printf 'n/a' ;; *) awk -v a="$1" -v b="$2" -v n="$3" 'BEGIN{printf "%.0f", (b - a) / n}' ;; esac; }
row() { printf '| %s |' "$1"; shift; for v in "$@"; do printf ' %s |' "$v"; done; printf '\n'; }
header() {
    say ""; say "### $1"; say ""
    h="| $2 |"; s="|---|"
    for l in $LIBS; do h="$h $l |"; s="$s---:|"; done
    say "$h"; say "$s"
}
table_row() {
    lab=$1; cmd=$2; cells=""
    for l in $LIBS; do cells="$cells$(eval "$cmd")|"; done
    # shellcheck disable=SC2086
    set -- $(printf '%s' "$cells" | tr '|' ' ')
    say "$(row "$lab" "$@")"
}
want() { case " $STAGES " in *" $1 "*) return 0 ;; esac; return 1; }

# the first run of a fresh $HOME fills the driver's caches: one throw-away run of each program, native and counted
for l in $LIBS; do
    "$B/bench_$l" startup gl > /dev/null 2>&1
    ir "$B/bench_$l" startup gl > /dev/null
done

say "## Machine"
say ""
say "- $(uname -srm), $(nproc) logical CPUs, load average $(cut -d' ' -f1-3 /proc/loadavg) at the start"
say "- $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //')"
say "- $(gcc --version | head -n1), flags -O2 -DNDEBUG for the library and the programs; SDL2 and GLFW are the distribution's binaries"
say "- compositor: bench/compare/wl_compositor (the test compositor of tests/test_input_wayland.c, one 1280x800 output), Mesa software rendering"
for l in $LIBS; do
    case $l in
        glfw) v=$(strings -a "$(ldd "$B/bench_glfw" | awk '/libglfw/{print $3}')" 2> /dev/null | grep -m1 -E '^3\.[0-9]+\.[0-9]+ Wayland'); say "- glfw: ${v:-3.x}" ;;
        sdl2) say "- sdl2: $(pkg-config --modversion sdl2 2> /dev/null), video driver wayland" ;;
        zp) say "- zp: $(git -C "$SRC" rev-parse --short HEAD 2> /dev/null) (static library, Wayland backend)" ;;
    esac
done
say "- runs per timing: $RUNS (median); the application calls setlocale(LC_CTYPE, \"\") for all three"
[ -n "$VG" ] && say "- instructions: $("$VG" --version) callgrind, user space, whole process (the compositor is another process and is not counted)" || say "- instructions: no valgrind, not measured"

if want startup; then
    header "Start-up, gl" "metric"
    table_row "ms to window created (first configure seen) + first loop turn (median)" "rep $RUNS $RUN $B/bench_\$l startup gl | median startup_ms"
    table_row "  of which init (ms)" "rep $RUNS $RUN $B/bench_\$l startup gl | median init_ms"
    table_row "  of which window create (ms)" "rep $RUNS $RUN $B/bench_\$l startup gl | median window_ms"
    table_row "instructions (whole process)" "ir $B/bench_\$l startup gl"
    table_row "syscalls" "strace -f -o $DIR/st.txt $B/bench_\$l startup gl > /dev/null 2>&1; wc -l < $DIR/st.txt | tr -d ' '"
fi

if want mem; then
    header "Memory after init, a window and 1000 idle turns, gl" "metric"
    table_row "RSS (kB)" "$B/bench_\$l mem gl 2> /dev/null | median rss_kb"
    table_row "PSS (kB)" "$B/bench_\$l mem gl 2> /dev/null | median pss_kb"
    table_row "PSS anonymous (kB)" "$B/bench_\$l mem gl 2> /dev/null | median pss_anon_kb"
    table_row "shared libraries mapped" "$B/bench_\$l mem gl 2> /dev/null | median libs"
    table_row "heap: malloc calls" "LD_PRELOAD=$B/libmcount.so $B/bench_\$l mem gl 2>&1 >/dev/null | grep -o 'allocs=[0-9]*' | cut -d= -f2"
    table_row "heap: peak live bytes (approximate)" "LD_PRELOAD=$B/libmcount.so $B/bench_\$l mem gl 2>&1 >/dev/null | grep -o 'peak_live=[0-9]*' | cut -d= -f2"
fi

if want idle; then
    header "Idle poll (an event loop turn with no event), gl" "metric"
    table_row "ns per turn, 1e6 turns (median of 5)" "rep 5 $B/bench_\$l idle 1000000 gl | median ns_per_poll"
    table_row "instructions per turn" "diff_per \$(ir $B/bench_\$l idle 0 gl) \$(ir $B/bench_\$l idle 20000 gl) 20000"
fi

if want cycle; then
    header "Window create + destroy, gl, 100 cycles" "metric"
    table_row "us per cycle (median of 5)" "rep 5 $B/bench_\$l cycle 100 gl | median us_per_cycle"
    table_row "CPU us per cycle" "rep 5 $B/bench_\$l cycle 100 gl | median cpu_us_per_cycle"
    table_row "instructions per cycle" "diff_per \$(ir $B/bench_\$l cycle 10 gl) \$(ir $B/bench_\$l cycle 20 gl) 10"
fi

if want wait; then
    header "Sleeping between events: one wake + wait cycle on one thread, gl" "metric"
    table_row "ns per cycle, 200000 cycles (median of 5)" "rep 5 $B/bench_\$l wake 200000 gl | median ns_per_cycle"
    table_row "instructions per cycle" "diff_per \$(ir $B/bench_\$l wake 0 gl) \$(ir $B/bench_\$l wake 20000 gl) 20000"
    header "Wake latency: another thread posts 2 ms after the main one went to sleep with no limit, 1000 times, gl" "metric"
    table_row "median (us, median of 5 runs)" "rep 5 $B/bench_\$l latency 1000 gl | median median_us"
    table_row "99th percentile (us, median of 5 runs)" "rep 5 $B/bench_\$l latency 1000 gl | median p99_us"
    table_row "worst of 1000 (us, median of 5 runs)" "rep 5 $B/bench_\$l latency 1000 gl | median worst_us"
    header "Idle: 10 s in the wait with nothing to wake it, gl" "metric"
    table_row "wall s" "$B/bench_\$l idlecpu 10 gl | median wall_s"
    table_row "CPU ms" "$B/bench_\$l idlecpu 10 gl | median cpu_ms"
    table_row "times the wait returned" "$B/bench_\$l idlecpu 10 gl | median returns"
fi
say ""
say "Results also in $OUT"

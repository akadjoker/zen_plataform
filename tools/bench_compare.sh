#!/bin/sh
# usage: tools/bench_compare.sh [-n runs] [stage...]
#
# zen_platform against GLFW 3 and SDL2 on the same invisible Xvfb, with the same programs (bench/compare): start-up,
# memory, an idle poll, a window create + destroy, events taken from the X connection, binary sizes. The numbers come
# out as markdown tables on stdout and in $ZP_BENCH_DIR/results.md; docs/COMPARACAO_CORE.md says how to read them.
#
# Stages (default: all): startup mem idle cycle events wait size
#
# Environment:
#   ZP_BENCH_SRC   source tree to build (default: this repository); point it at an older checkout (with bench/compare
#                  copied in) to get the "before" column of a change
#   ZP_BENCH_DIR   build trees, scratch and results.md (default: ${TMPDIR:-/tmp}/zp-bench-$USER)
#   ZP_BENCH_RUNS  runs per timing, the table shows the median (default 11; -n overrides)
#   ZP_BENCH_LIBS the libraries to run, a subset of "zp glfw sdl2" (default: those that were built)
#   GLFW_ROOT      a prefix holding usr/include/GLFW and usr/lib/x86_64-linux-gnu/libglfw.so (an unpacked libglfw3 and
#                  libglfw3-dev package, say); without it the system's GLFW is used if there is one
#   VALGRIND       a valgrind binary (default: the one on the PATH); VALGRIND_LIB its libexec/valgrind. Without it the
#                  instruction counts are left out
# GLFW and SDL2 are only built here when they are found; neither is part of the normal build. Everything runs through
# tools/xvfb_run.sh (an invisible Xvfb started with -noreset, no Wayland, scratch settings): nothing opens on a screen.
# The instruction counts are those of the whole process under valgrind --tool=callgrind (perf needs privileges this
# machine does not give), as a difference between two runs of the same program (N events or turns, and none).

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
if [ -z "${ZP_BENCH_INSIDE:-}" ]; then
    exec env ZP_BENCH_INSIDE=1 ZP_XVFB_ARGS=-noreset "$ROOT/tools/xvfb_run.sh" "$0" "$@"
fi

RUNS=${ZP_BENCH_RUNS:-11}
if [ "${1:-}" = -n ]; then RUNS=$2; shift 2; fi
STAGES=${*:-startup mem idle cycle events wait size}
SRC=${ZP_BENCH_SRC:-$ROOT}
DIR=${ZP_BENCH_DIR:-${TMPDIR:-/tmp}/zp-bench-$(id -un)}
VG=${VALGRIND:-$(command -v valgrind || true)}
mkdir -p "$DIR/home" || exit 2
export HOME=$DIR/home
unset LD_LIBRARY_PATH LD_PRELOAD
export BENCH_SETLOCALE=1
# No input-method server runs in the Xvfb. With XMODIFIERS=@im=ibus (the usual desktop setting) GLFW and SDL end up with
# no input method at all and zen_platform falls back to Xlib's built-in compose one, which is more work: the "ibus"
# start-up table shows it. Everything else runs with XMODIFIERS unset, so the three libraries open the same input method.
unset XMODIFIERS
OUT=$DIR/results.md
: > "$OUT"

GEN=
command -v ninja > /dev/null && GEN="-G Ninja"
GLFW_ARG=
[ -n "${GLFW_ROOT:-}" ] && GLFW_ARG=-DZP_COMPARE_GLFW_ROOT=$GLFW_ROOT

build() { # build <dir> <extra cmake args>: -O2 for the library and the programs alike
    d=$1; shift
    # shellcheck disable=SC2086
    cmake -S "$SRC" -B "$d" $GEN -DCMAKE_BUILD_TYPE=None "-DCMAKE_C_FLAGS=-O2 -DNDEBUG" -DPLATFORM_BUILD_TESTS=OFF \
        -DPLATFORM_BUILD_EXAMPLES=OFF -DPLATFORM_BUILD_COMPARE=ON $GLFW_ARG "$@" > "$d.log" 2>&1 &&
        cmake --build "$d" -j "$(($(nproc) / 2 > 0 ? $(nproc) / 2 : 1))" >> "$d.log" 2>&1 || { echo "bench_compare.sh: build failed, see $d.log" >&2; exit 2; }
}
build "$DIR/static" -DBUILD_SHARED_LIBS=OFF
B=$DIR/static/bench/compare
LIBS=zp
[ -x "$B/bench_glfw" ] && LIBS="$LIBS glfw"
[ -x "$B/bench_sdl2" ] && LIBS="$LIBS sdl2"
[ -n "${ZP_BENCH_LIBS:-}" ] && LIBS=$ZP_BENCH_LIBS
RUN=$B/bench_run


say() { printf '%s\n' "$*" | tee -a "$OUT"; }

# median <key>: the median of the key=value fields on stdin
median() {
    grep -o "$1=[-0-9.]*" | cut -d= -f2 | sort -n | awk '{a[NR]=$1} END{if (NR) printf "%s", a[int((NR+1)/2)]; else printf "n/a"}'
}
# rep <n> cmd...: the command n times, stdout kept
rep() { n=$1; shift; i=0; while [ "$i" -lt "$n" ]; do "$@" 2> /dev/null; i=$((i + 1)); done; }
# ir cmd...: instructions of the whole process
ir() {
    [ -n "$VG" ] || { printf 'n/a'; return; }
    "$VG" --tool=callgrind "--callgrind-out-file=$DIR/cg.out" --cache-sim=no "$@" 2>&1 > /dev/null | grep -o 'Collected : [0-9]*' | grep -o '[0-9]*$'
}
# ir_events kind n batch lib: instructions of the events mode with a wrapper
ir_events() {
    [ -n "$VG" ] || { printf 'n/a'; return; }
    "$RUN" events "$1" "$2" "$3" "$VG" --tool=callgrind "--callgrind-out-file=$DIR/cg.out" --cache-sim=no "$B/bench_$4" events "$1" "$2" "$3" nogl 2>&1 > /dev/null | grep -o 'Collected : [0-9]*' | grep -o '[0-9]*$'
}
# diff_per <a> <b> <n>: (b - a) / n
diff_per() { case "$1$2" in *n/a*) printf 'n/a' ;; *) awk -v a="$1" -v b="$2" -v n="$3" 'BEGIN{printf "%.0f", (b - a) / n}' ;; esac; }
# row <label> <values...>
row() { printf '| %s |' "$1"; shift; for v in "$@"; do printf ' %s |' "$v"; done; printf '\n'; }
header() { # header <title> <unit/column text>
    say ""; say "### $1"; say ""
    h="| $2 |"; s="|---|"
    for l in $LIBS; do h="$h $l |"; s="$s---:|"; done
    say "$h"; say "$s"
}
table_row() { # table_row <label> <command-producing-a-cell using \$l>   (cmd is eval'd once per library)
    lab=$1; cmd=$2; cells=""
    for l in $LIBS; do cells="$cells$(eval "$cmd")|"; done
    # shellcheck disable=SC2086
    set -- $(printf '%s' "$cells" | tr '|' ' ')
    say "$(row "$lab" "$@")"
}
want() { case " $STAGES " in *" $1 "*) return 0 ;; esac; return 1; }

# The first GL context of a fresh $HOME, and the first one under valgrind, fill the driver's caches (some 45 M
# instructions more than every later one): one throw-away run of each program, with and without GL, natively and
# counted, so the first library measured is not the one that pays.
for l in $LIBS; do
    "$B/bench_$l" startup nogl > /dev/null 2>&1
    "$B/bench_$l" startup gl > /dev/null 2>&1
    ir "$B/bench_$l" startup gl > /dev/null
done

say "## Machine"
say ""
say "- $(uname -srm), $(nproc) logical CPUs, load average $(cut -d' ' -f1-3 /proc/loadavg) at the start"
say "- $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //')"
say "- $(gcc --version | head -n1), flags -O2 -DNDEBUG for the library and the programs; GLFW and SDL2 are the distribution's binaries"
say "- Xvfb $(dpkg-query -W -f='${Version}' xvfb 2> /dev/null || echo '?'), screen 1280x800x24, -noreset; locale of the process: ${LANG:-C}"
for l in $LIBS; do
    case $l in
        glfw) v=$(strings -a "$(ldd "$B/bench_glfw" | awk '/libglfw/{print $3}')" 2> /dev/null | grep -m1 -E '^3\.[0-9]+\.[0-9]+ X11') ; say "- glfw: ${v:-3.x}" ;;
        sdl2) say "- sdl2: $(pkg-config --modversion sdl2 2> /dev/null)" ;;
        zp) say "- zp: $(git -C "$SRC" rev-parse --short HEAD 2> /dev/null) (static library)" ;;
    esac
done
say "- runs per timing: $RUNS (median); startup and events are run with the application calling setlocale(LC_CTYPE, \"\") for all three (zen_platform does it itself), as an application that takes text input must"
[ -n "$VG" ] && say "- instructions: $("$VG" --version) callgrind, user space, whole process" || say "- instructions: no valgrind, not measured"

if want startup; then
    for loc in user C ibus; do
        unset LC_ALL; unset XMODIFIERS
        case $loc in
            C) LC_ALL=C; export LC_ALL; title="locale C (no compose table to load)" ;;
            ibus) XMODIFIERS=@im=ibus; export XMODIFIERS; title="locale ${LANG:-C}, XMODIFIERS=@im=ibus with no server" ;;
            *) title="locale ${LANG:-C} (the input method loads its compose table)" ;;
        esac
        for gl in nogl gl; do
            [ $loc != user ] && [ $gl = gl ] && continue
            header "Start-up, $gl, $title" "metric"
            table_row "ms to window viewable + first loop turn (median)" "rep $RUNS $RUN once $B/bench_\$l startup $gl | median startup_ms"
            table_row "  of which init (ms)" "rep $RUNS $RUN once $B/bench_\$l startup $gl | median init_ms"
            table_row "  of which window create (ms)" "rep $RUNS $RUN once $B/bench_\$l startup $gl | median window_ms"
            table_row "instructions (whole process)" "ir $B/bench_\$l startup $gl"
            table_row "X requests that wait for a reply" "strace -f -o $DIR/st.txt $B/bench_\$l startup $gl > /dev/null 2>&1; grep -c 'events=POLLIN}\], 1, -1' $DIR/st.txt"
            table_row "syscalls" "strace -f -o $DIR/st.txt $B/bench_\$l startup $gl > /dev/null 2>&1; wc -l < $DIR/st.txt | tr -d ' '"
        done
    done
    unset LC_ALL XMODIFIERS
fi

if want mem; then
    for gl in nogl gl; do
        header "Memory after init, a window and 1000 idle turns, $gl" "metric"
        table_row "RSS (kB)" "$B/bench_\$l mem $gl | median rss_kb"
        table_row "PSS (kB)" "$B/bench_\$l mem $gl | median pss_kb"
        table_row "PSS anonymous (kB)" "$B/bench_\$l mem $gl | median pss_anon_kb"
        table_row "shared libraries mapped" "$B/bench_\$l mem $gl | median libs"
        table_row "heap: malloc calls" "LD_PRELOAD=$DIR/static/bench/compare/libmcount.so $B/bench_\$l mem $gl 2>&1 >/dev/null | grep -o 'allocs=[0-9]*' | cut -d= -f2"
        table_row "heap: bytes asked for" "LD_PRELOAD=$DIR/static/bench/compare/libmcount.so $B/bench_\$l mem $gl 2>&1 >/dev/null | grep -o ' bytes=[0-9]*' | cut -d= -f2"
        table_row "heap: peak live bytes (approximate)" "LD_PRELOAD=$DIR/static/bench/compare/libmcount.so $B/bench_\$l mem $gl 2>&1 >/dev/null | grep -o 'peak_live=[0-9]*' | cut -d= -f2"
    done
fi

if want idle; then
    header "Idle poll (an event loop turn with no event), no GL" "metric"
    table_row "ns per turn, 1e6 turns (median)" "rep 5 $B/bench_\$l idle 1000000 nogl | median ns_per_poll"
    table_row "instructions per turn" "diff_per \$(ir $B/bench_\$l idle 0 nogl) \$(ir $B/bench_\$l idle 20000 nogl) 20000"
fi

if want cycle; then
    for gl in nogl gl nogl-C; do
        n=1000; ni=50; [ $gl = gl ] && { n=100; ni=10; }
        loc=; [ $gl = nogl-C ] && { gl=nogl; LC_ALL=C; export LC_ALL; loc=", locale C (no compose table, so an input context costs less)"; }
        header "Window create + destroy, $gl$loc" "metric"
        table_row "us per cycle, $n cycles (median of 5)" "rep 5 $B/bench_\$l cycle $n $gl | median us_per_cycle"
        table_row "CPU us per cycle" "rep 5 $B/bench_\$l cycle $n $gl | median cpu_us_per_cycle"
        table_row "instructions per cycle" "diff_per \$(ir $B/bench_\$l cycle $ni $gl) \$(ir $B/bench_\$l cycle $((2 * ni)) $gl) $ni"
        unset LC_ALL
    done
fi

if want events; then
    for kind in motion key; do
        n=100000; ni=20000; ek=motion; [ $kind = key ] && { n=20000; ni=10000; ek=text; }
        for batch in 100 0; do
            [ $batch = 0 ] && desc="all $n waiting at the first turn (a frame that took long)" || desc="$batch waiting at each turn (a frame in which $batch arrived)"
            header "Events, $kind, $desc, no GL" "metric"
            table_row "events delivered to the application" "rep 3 $RUN events $kind $n $batch $B/bench_\$l events $kind $n $batch nogl | median $ek"
            table_row "CPU ms in the turns, $n events (median of 3)" "rep 3 $RUN events $kind $n $batch $B/bench_\$l events $kind $n $batch nogl | median cpu_ms"
            table_row "instructions per injected event" "diff_per \$(ir_events $kind 0 $batch \$l) \$(ir_events $kind $ni $batch \$l) $ni"
        done
    done
fi

if want wait; then
    header "Sleeping between events: one wake + wait cycle on one thread, no GL" "metric"
    table_row "ns per cycle, 200000 cycles (median of 5)" "rep 5 $B/bench_\$l wake 200000 nogl | median ns_per_cycle"
    table_row "instructions per cycle" "diff_per \$(ir $B/bench_\$l wake 0 nogl) \$(ir $B/bench_\$l wake 20000 nogl) 20000"
    header "Wake latency: another thread posts 2 ms after the main one went to sleep with no limit, 1000 times, no GL" "metric"
    table_row "median (us, median of 5 runs)" "rep 5 $B/bench_\$l latency 1000 nogl | median median_us"
    table_row "99th percentile (us, median of 5 runs)" "rep 5 $B/bench_\$l latency 1000 nogl | median p99_us"
    table_row "worst of 1000 (us, median of 5 runs)" "rep 5 $B/bench_\$l latency 1000 nogl | median worst_us"
    header "Idle: 10 s in the wait with nothing to wake it, no GL" "metric"
    table_row "wall s" "$B/bench_\$l idlecpu 10 nogl | median wall_s"
    table_row "CPU ms" "$B/bench_\$l idlecpu 10 nogl | median cpu_ms"
    table_row "times the wait returned" "$B/bench_\$l idlecpu 10 nogl | median returns"
fi

if want size; then
    build "$DIR/shared" -DBUILD_SHARED_LIBS=ON
    Bs=$DIR/shared/bench/compare
    stripped() { cp "$1" "$DIR/strip.tmp" && strip --strip-all "$DIR/strip.tmp" 2> /dev/null; wc -c < "$DIR/strip.tmp" | tr -d ' '; }
    kb() { awk -v b="$1" 'BEGIN{printf "%.0f", b / 1024}'; }
    say ""; say "### Binary sizes (stripped, kB)"; say ""
    say "| file | kB |"; say "|---|---:|"
    say "$(row 'minimal program, zen_platform linked statically' "$(kb "$(stripped "$B/bench_zp")")")"
    say "$(row 'libplatform.a (archive, with its debug info stripped)' "$(cp "$DIR/static/libplatform.a" "$DIR/strip.a"; strip --strip-debug "$DIR/strip.a"; kb "$(wc -c < "$DIR/strip.a")")")"
    say "$(row 'minimal program, zen_platform shared (program alone)' "$(kb "$(stripped "$Bs/bench_zp")")")"
    say "$(row 'libplatform.so (shared)' "$(kb "$(stripped "$DIR/shared/libplatform.so")")")"
    if [ -x "$B/bench_glfw" ]; then
        g=$(ldd "$B/bench_glfw" | awk '/libglfw/{print $3}')
        say "$(row 'minimal program, GLFW (program alone)' "$(kb "$(stripped "$B/bench_glfw")")")"
        say "$(row 'libglfw.so (distribution, stripped)' "$(kb "$(stripped "$g")")")"
    fi
    if [ -x "$B/bench_sdl2" ]; then
        s=$(ldd "$B/bench_sdl2" | awk '/libSDL2/{print $3}')
        say "$(row 'minimal program, SDL2 (program alone)' "$(kb "$(stripped "$B/bench_sdl2")")")"
        say "$(row 'libSDL2.so (distribution, stripped)' "$(kb "$(stripped "$s")")")"
    fi
fi
say ""
say "Results also in $OUT"

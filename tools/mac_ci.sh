#!/bin/bash
# usage: tools/mac_ci.sh build <name> <cmake args...>
#        tools/mac_ci.sh warnings <name>
#        tools/mac_ci.sh test <name> [ctest args...]
#        tools/mac_ci.sh summary
#
# The shell of .github/workflows/mac.yml, in a file so that it can be run (and was checked) on another machine: the macOS job
# starts by hand and the owner allows few runs, so what it does must not be written into YAML that only a runner can try.
# bash 3.2 is what macOS has; nothing here needs more. Run from the root of the repository.
#
#   build <name> ...   configures build-<name> (Unix Makefiles) with the given cmake arguments and builds it, going on after an
#                      error (make -k) so that one run shows every file that does not compile and not only the first. If the
#                      parallel pass had errors it builds again one file at a time: only the files that failed are compiled
#                      (the others are up to date), and their errors come out one after the other, not mixed with those of
#                      the compilers that ran beside them. Logs: ci-logs/<name>.configure.log, .build.log, .build-errors.log.
#   warnings <name>    the warnings of our own code in ci-logs/<name>.build.log (the vendored stb headers are left out, WARN_FILTER);
#                      exit status 1 if there are any.
#   test <name> ...    every test of build-<name>, verbose, each with its own timeout (a test's TIMEOUT property wins over
#                      ZP_CI_TEST_TIMEOUT); a failed or timed out test does not stop the others. Logs: ci-logs/<name>.ctest.log,
#                      .LastTest.log (ctest's own), .tests.txt (one line per test).
#   summary            markdown for the job summary: for every configuration the number of errors (the first ones, as the compiler
#                      printed them), the warnings, and the tests that failed.
#
# ZP_CI_JOBS (4), ZP_CI_LOGS (ci-logs), ZP_CI_GENERATOR (Unix Makefiles), ZP_CI_TEST_TIMEOUT (150), WARN_FILTER (stb_).

set -u
LOGS=${ZP_CI_LOGS:-ci-logs}
JOBS=${ZP_CI_JOBS:-4}
GEN=${ZP_CI_GENERATOR:-Unix Makefiles}
TEST_TIMEOUT=${ZP_CI_TEST_TIMEOUT:-150}
WARN_FILTER=${WARN_FILTER:-stb_}

cmd_build() {
    local name=$1 dir rc
    shift
    dir=build-$name
    mkdir -p "$LOGS"
    echo "== configure $dir: cmake -S . -B $dir -G \"$GEN\" $*"
    cmake -S . -B "$dir" -G "$GEN" "$@" 2>&1 | tee "$LOGS/$name.configure.log"
    rc=${PIPESTATUS[0]}
    if [ "$rc" != 0 ]; then
        echo "== configure of $dir failed ($rc)"
        return 1
    fi
    echo "== build $dir: $JOBS jobs, going on after errors"
    cmake --build "$dir" -j"$JOBS" -- -k 2>&1 | tee "$LOGS/$name.build.log"
    rc=${PIPESTATUS[0]}
    if [ "$rc" != 0 ]; then
        echo "== the build of $dir had errors: again, one file at a time, so that the errors of each file come out together"
        cmake --build "$dir" -- -k 2>&1 | tee "$LOGS/$name.build-errors.log"
        echo "== $dir: $(grep -c ' error: ' "$LOGS/$name.build-errors.log") lines with ' error: ' in the sequential pass"
        return 1
    fi
    return 0
}

cmd_warnings() {
    local name=$1 log n
    log=$LOGS/$name.build.log
    if [ ! -f "$log" ]; then
        echo "no $log: nothing was built"
        return 1
    fi
    grep 'warning:' "$log" | grep -v "$WARN_FILTER" | sort -u > "$LOGS/$name.warnings.txt"
    n=$(wc -l < "$LOGS/$name.warnings.txt" | tr -d ' ')
    if [ "$n" != 0 ]; then
        cat "$LOGS/$name.warnings.txt"
        echo "$n distinct warnings of our own code (above)"
        return 1
    fi
    echo "no warnings of our own code"
    return 0
}

cmd_test() {
    local name=$1 dir rc
    shift
    dir=build-$name
    mkdir -p "$LOGS"
    if [ ! -d "$dir" ]; then
        echo "no $dir: nothing was built"
        return 1
    fi
    ctest --test-dir "$dir" -V --no-tests=error --timeout "$TEST_TIMEOUT" "$@" 2>&1 | tee "$LOGS/$name.ctest.log"
    rc=${PIPESTATUS[0]}
    cp "$dir/Testing/Temporary/LastTest.log" "$LOGS/$name.LastTest.log" 2> /dev/null || true
    grep -E '^ *[0-9]+/[0-9]+ Test +#[0-9]+: ' "$LOGS/$name.ctest.log" > "$LOGS/$name.tests.txt" || true
    return "$rc"
}

# the first lines of a file that match, at most $3
first() {
    grep -E "$1" "$2" 2> /dev/null | sort -u | head -n "$3"
}

cmd_summary() {
    local log name n w errs
    echo "## Result of each configuration"
    echo
    for log in "$LOGS"/*.configure.log; do
        [ -f "$log" ] || continue
        name=$(basename "$log" .configure.log)
        echo "### $name"
        if [ ! -f "$LOGS/$name.build.log" ]; then
            echo "- configure failed, nothing was built (ci-logs/$name.configure.log)"
            echo
            continue
        fi
        errs=$(grep -E '(error|fatal error): ' "$LOGS/$name.build.log" | sort -u | wc -l | tr -d ' ')
        w=$(grep 'warning:' "$LOGS/$name.build.log" | grep -v "$WARN_FILTER" | sort -u | wc -l | tr -d ' ')
        echo "- build: $errs distinct error lines, $w distinct warnings of our own code"
        if [ "$errs" != 0 ]; then
            echo
            echo '```'
            first '(error|fatal error): ' "$LOGS/$name.build.log" 40 | sed "s#$PWD/##" | cut -c1-300
            echo '```'
        fi
        if [ -f "$LOGS/$name.ctest.log" ]; then
            n=$(grep -E 'tests passed, [0-9]+ tests failed out of' "$LOGS/$name.ctest.log" | tail -n 1)
            echo "- tests: ${n:-ctest did not get to its summary line}"
            if grep -q 'The following tests FAILED' "$LOGS/$name.ctest.log"; then
                echo
                echo '```'
                awk '/The following tests FAILED/ {p = 1} /^Errors while running/ {p = 0} p' "$LOGS/$name.ctest.log" | head -n 40
                echo '```'
            fi
        else
            echo "- tests: not run"
        fi
        echo
    done
}

case ${1:-} in
    build) shift; cmd_build "$@" ;;
    warnings) shift; cmd_warnings "$@" ;;
    test) shift; cmd_test "$@" ;;
    summary) cmd_summary ;;
    *) sed -n '2,6p' "$0" >&2; exit 2 ;;
esac

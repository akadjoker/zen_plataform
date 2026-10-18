#!/bin/sh
# Runs X11 tests the way a GNOME session has them, Xwayland with ibus as the input
# method, on a display of their own: a headless gnome-shell on a private D-Bus with a
# HOME of its own (its settings, its ibus address files), a rootful Xwayland on it and
# an ibus-x11 for that display. The session in use (its keyboard, its focus, its ibus)
# is never touched, and XTest keys go nowhere else.
#
#   tests/run_x11_isolated.sh build/tests/test_input_x11 [more test programs]
#
# Each program runs twice: with ibus (XMODIFIERS=@im=ibus) and with no input method.
# There is no window manager: a test that needs the focus takes it (XSetInputFocus).

set -u
[ $# -gt 0 ] || { echo "usage: $0 test-program..." >&2; exit 2; }
for t in "$@"; do
    [ -x "$t" ] || { echo "not a program: $t" >&2; exit 2; }
done
for p in gnome-shell Xwayland dbus-run-session; do
    command -v $p > /dev/null || { echo "SKIP: no $p" >&2; exit 77; }
done

N=77
while [ -e /tmp/.X11-unix/X$N ] || [ -e /tmp/.X$N-lock ]; do N=$((N + 1)); done
WL=zen-x11-$N
T=$(mktemp -d)
H=$T/home
mkdir -p $H/.config $H/.cache $H/.local/share
IBUS_DIR=$H/.config/ibus/bus

cleanup() {
    if [ -s $T/bus ]; then
        BUS=$(cat $T/bus)
        PIDS=""
        for p in $(ls /proc | grep -E '^[0-9]+$'); do
            tr '\0' '\n' 2> /dev/null < /proc/$p/environ | grep -qxF "DBUS_SESSION_BUS_ADDRESS=$BUS" && PIDS="$PIDS $p"
        done
        [ -n "$PIDS" ] && kill $PIDS 2> /dev/null
        sleep 2
        for p in $PIDS; do [ -d /proc/$p ] && kill -9 $p 2> /dev/null; done
    fi
    [ -n "${SESSION_PID:-}" ] && kill $SESSION_PID 2> /dev/null
    rm -rf $T
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# Everything below runs with the private bus, so the cleanup finds it by that address.
# DISPLAY=:$N names the address file the session's ibus-daemon writes (":0" otherwise).
env -u WAYLAND_DISPLAY -u DISPLAY HOME=$H XDG_CONFIG_HOME=$H/.config XDG_CACHE_HOME=$H/.cache \
    XDG_DATA_HOME=$H/.local/share dbus-run-session -- sh -c '
    echo "$DBUS_SESSION_BUS_ADDRESS" > "$1/bus"
    export DISPLAY=:$2
    exec gnome-shell --wayland --headless --no-x11 --wayland-display=$3 --virtual-monitor 1280x720
' sh $T $N $WL > $T/shell.log 2>&1 &
SESSION_PID=$!
i=0
until [ -S "$XDG_RUNTIME_DIR/$WL" ] && [ -s $T/bus ]; do
    i=$((i + 1)); [ $i -gt 100 ] && { echo "gnome-shell did not start" >&2; cat $T/shell.log >&2; exit 1; }
    sleep 0.1
done
export DBUS_SESSION_BUS_ADDRESS=$(cat $T/bus)

env WAYLAND_DISPLAY=$WL DISPLAY= Xwayland :$N -geometry 1024x768 -noreset -nolisten tcp > $T/xwayland.log 2>&1 &
i=0
until [ -S /tmp/.X11-unix/X$N ]; do
    i=$((i + 1)); [ $i -gt 100 ] && { echo "Xwayland did not start" >&2; cat $T/xwayland.log >&2; exit 1; }
    sleep 0.1
done

IM=none
if [ -x /usr/libexec/ibus-x11 ]; then
    i=0
    until ls $IBUS_DIR/*-unix-$N > /dev/null 2>&1; do
        i=$((i + 1)); [ $i -gt 100 ] && break
        sleep 0.1
    done
    ADDR=$(grep -h '^IBUS_ADDRESS=' $IBUS_DIR/*-unix-$N 2> /dev/null | cut -d= -f2-)
    if [ -n "$ADDR" ]; then
        env -u WAYLAND_DISPLAY HOME=$H DISPLAY=:$N IBUS_ADDRESS="$ADDR" /usr/libexec/ibus-x11 > $T/ibus-x11.log 2>&1 &
        # ready when it is the display's XIM server and the daemon has an engine;
        # a program started before gets Xlib's own input method instead
        i=0
        until DISPLAY=:$N xprop -root XIM_SERVERS 2> /dev/null | grep -q @server=ibus &&
            [ -n "$(IBUS_ADDRESS="$ADDR" ibus engine 2> /dev/null)" ]; do
            i=$((i + 1)); [ $i -gt 100 ] && { echo "ibus-x11 did not register" >&2; cat $T/ibus-x11.log >&2; exit 1; }
            sleep 0.1
        done
        sleep 0.5
        IM=ibus
    fi
fi
[ $IM = ibus ] || echo "no ibus: running with no input method only" >&2

MODES=none
[ $IM = ibus ] && MODES="ibus none"
FAILED=0
for t in "$@"; do
    for m in $MODES; do
        echo "== $t (XMODIFIERS=@im=$m, DISPLAY=:$N)"
        env -u WAYLAND_DISPLAY DISPLAY=:$N XMODIFIERS=@im=$m timeout 120 "$t"
        r=$?
        [ $r -eq 0 ] || [ $r -eq 77 ] || FAILED=1
    done
done
exit $FAILED

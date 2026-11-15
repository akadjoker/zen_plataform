#!/bin/sh
# usage: tools/xvfb_run.sh <command...>
#
# The one way tests and programs of this repository are started. The command runs
# on a private, invisible Xvfb with no Wayland and no session of the user:
#  - WAYLAND_DISPLAY is unset, and XDG_RUNTIME_DIR too: with only the first one
#    unset libwayland falls back to the socket in XDG_RUNTIME_DIR and the program
#    opens real windows on the user's desktop (a past round did exactly that);
#  - the DISPLAY of the caller is never used (xvfb-run -a picks a free server of
#    its own), so no window can appear on a real screen;
#  - XDG_DATA_HOME and XDG_CONFIG_HOME point to a throw-away folder, so the tests
#    that write settings, recent files or a library never touch the user's own.
# ZP_XDG_DIR keeps that folder (and its content) in a place of the caller's choice.

command -v xvfb-run > /dev/null || { echo "xvfb_run.sh: xvfb-run not found" >&2; exit 127; }
[ $# -gt 0 ] || { echo "usage: $0 command..." >&2; exit 2; }

if [ -n "${ZP_XDG_DIR:-}" ]; then
    X=$ZP_XDG_DIR
    mkdir -p "$X/cfg" || exit 1
else
    X=$(mktemp -d "${TMPDIR:-/tmp}/zp-xdg.XXXXXX") || exit 1
    mkdir -p "$X/cfg"
    trap 'rm -rf "$X"' EXIT
    trap 'exit 130' INT TERM
fi

# ZP_PRIVATE_X tells a test that the X server it finds is the throw-away one: a test that changes the keyboard layout
# (test_input_x11, setxkbmap) runs only then, never on the display of whoever is working.
env -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR XDG_DATA_HOME="$X" XDG_CONFIG_HOME="$X/cfg" ZP_PRIVATE_X=1 \
    xvfb-run -a -s "-screen 0 1280x800x24" "$@"

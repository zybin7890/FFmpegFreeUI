#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
set -eu
app=$(realpath "${1:-build/ffmpegfreeui}")
output=$(realpath "${2:-.}")
runtime=$(mktemp -d)
weston_pid=
xvfb_pid=
cleanup() {
    if [ -n "$weston_pid" ]; then kill "$weston_pid" 2>/dev/null || true; fi
    if [ -n "$xvfb_pid" ]; then kill "$xvfb_pid" 2>/dev/null || true; fi
    # The test owns this newly created directory; no user data is removed.
    rmdir "$runtime" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
chmod 700 "$runtime"
export XDG_RUNTIME_DIR="$runtime"
Xvfb :97 -screen 0 1440x1000x24 >"$output/xvfb.log" 2>&1 &
xvfb_pid=$!
weston --backend=headless-backend.so --use-pixman --width=1440 --height=1000 --socket=fui-test --idle-time=0 >"$output/weston.log" 2>&1 &
weston_pid=$!
i=0
while [ ! -S "$runtime/fui-test" ]; do
    i=$((i+1)); if [ "$i" -ge 100 ]; then cat "$output/weston.log"; exit 1; fi
    sleep 0.1
done
unset QT_QPA_PLATFORM
DISPLAY=:97 WAYLAND_DISPLAY=fui-test XDG_SESSION_TYPE=wayland "$app" --render "$output/wayland.png" --report "$output/wayland.json"
DISPLAY=:97 WAYLAND_DISPLAY=fui-test XDG_SESSION_TYPE=wayland FUI_DISPLAY_BACKEND=x11 "$app" --render "$output/x11.png" --capture-page 1 --report "$output/x11.json"
python3 - "$output" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
for name, expected in [('wayland', 'wayland'), ('x11', 'xcb')]:
    data = json.loads((p / (name + '.json')).read_text())
    assert data['platform'] == expected, data
    assert data['visible'] and data['imageSaved'] and data['fields'] == 186, data
    print('PASS', name, data)
PY

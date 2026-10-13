#!/usr/bin/env bash
set -euo pipefail

if [[ $(uname -s) != Linux ]]; then
  printf '%s\n' 'This headless UI test is Linux-only.' >&2
  exit 77
fi

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
for tool in Xvfb openbox xauth xprop python3 pkg-config; do
  if ! command -v "$tool" >/dev/null; then
    printf 'Missing headless UI dependency: %s\n' "$tool" >&2
    exit 1
  fi
done
if ! pkg-config --exists xtst; then
  printf '%s\n' 'Missing XTest development files (pkg-config: xtst).' >&2
  exit 1
fi

profile=$(mktemp -d /tmp/smashed-pumpkin-display-check-XXXXXX)
xvfb_pid=
wm_pid=
cleanup() {
  local status=$?
  local pid running
  running=" $(jobs -pr | tr '\n' ' ') "
  for pid in "$wm_pid" "$xvfb_pid"; do
    [[ -n "$pid" ]] || continue
    if [[ "$running" == *" $pid "* ]]; then kill "$pid" 2>/dev/null || true; fi
    wait "$pid" 2>/dev/null || true
  done
  if (( status != 0 )); then
    cat "$profile"/*.log >&2
  fi
  rm -rf -- "${profile:?}"
  return "$status"
}
trap cleanup EXIT

export XAUTHORITY="$profile/Xauthority"
touch "$XAUTHORITY"
cookie=$(python3 -c 'import secrets; print(secrets.token_hex(16))')
# The server reads the cookie before allocating its display number; add the
# actual client entry after -displayfd returns it. No host Xauthority is used.
xauth -f "$XAUTHORITY" add :0 MIT-MAGIC-COOKIE-1 "$cookie"
Xvfb -displayfd 3 -screen 0 1600x1200x24 -nolisten tcp -auth "$XAUTHORITY" \
  3>"$profile/display" >"$profile/xvfb.log" 2>&1 &
xvfb_pid=$!
for _ in {1..100}; do
  [[ -s "$profile/display" ]] && break
  kill -0 "$xvfb_pid" 2>/dev/null || break
  sleep 0.05
done
if [[ ! -s "$profile/display" ]]; then
  printf '%s\n' 'Xvfb did not allocate a display within 5 seconds.' >&2
  exit 1
fi
DISPLAY=":$(cat "$profile/display")"
export DISPLAY
xauth -f "$XAUTHORITY" add "$DISPLAY" MIT-MAGIC-COOKIE-1 "$cookie"
unset WAYLAND_DISPLAY
export GDK_BACKEND=x11
mkdir -m 700 "$profile/runtime"
env HOME="$profile" XDG_CONFIG_HOME="$profile/config" XDG_CACHE_HOME="$profile/cache" \
  XDG_RUNTIME_DIR="$profile/runtime" openbox --config-file "$root/tests/ui-openbox.xml" \
  >"$profile/openbox.log" 2>&1 &
wm_pid=$!
ready=false
for _ in {1..100}; do
  state=$(xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null || true)
  if [[ "$state" == *"window id #"* ]]; then ready=true; break; fi
  kill -0 "$wm_pid" 2>/dev/null || break
  sleep 0.05
done
if [[ "$ready" != true ]]; then
  printf '%s\n' 'Openbox did not initialize within 5 seconds.' >&2
  exit 1
fi

# Pointer events target this private X server, never the user's desktop.
export SMASHED_PRIVATE_X11_POINTER=1
printf 'Headless UI display: %s (Xvfb + Openbox)\n' "$DISPLAY"
bash "$root/build-aux/test-window-ui.sh" "$@"

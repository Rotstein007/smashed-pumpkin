#!/usr/bin/env bash
set -euo pipefail

if [[ $(uname -s) != Linux ]]; then
  printf '%s\n' 'This isolated desktop UI smoke test is Linux-only.' >&2
  exit 77
fi

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build=${1:-"$root/buildDir"}
scope=${2:-all}
case "$scope" in all|minimap|worlds-map-access|releases|i18n-boundaries|i18n-loading|i18n-chinese|language-settings) ;; *) printf 'Unknown UI test scope: %s\n' "$scope" >&2; exit 2 ;; esac
if [[ "$build" != /* ]]; then
  build="$PWD/$build"
fi
meson compile -C "$build" smashed-pumpkin
# Every scope discovers its language selector from this isolated catalog root.
meson compile -C "$build" i18n-test-catalog i18n-simplified-fixture i18n-traditional-fixture
if [[ "$scope" == i18n-chinese ]]; then
  meson compile -C "$build" smashed-pumpkin-gmo
fi

profile=$(mktemp -d /tmp/smashed-pumpkin-ui-check-XXXXXX)
cleanup_profile() {
  local test_status=$?
  # Never delete anything except the exact temporary directory created above.
  if [[ "$profile" != /tmp/smashed-pumpkin-ui-check-* ||
        "$(realpath -- "$profile")" != "$profile" ]]; then
    printf 'Refusing to clean unexpected test profile: %s\n' "$profile" >&2
    return 1
  fi
  # D-Bus activation can leave private portal helpers alive after the bus exits.
  # Stop only processes whose environment identifies this exact test profile.
  # PID file descriptors prevent a recycled PID from targeting another process.
  python3 - "$profile" <<'PY' || return 1
import os
from pathlib import Path
import select
import signal
import sys
import time

profile = sys.argv[1]
markers = {
    f"HOME={profile}".encode(),
    f"XDG_RUNTIME_DIR={profile}/runtime".encode(),
}
fds = []
try:
    for path in Path('/proc').iterdir():
        if not path.name.isdigit() or int(path.name) == os.getpid():
            continue
        fd = None
        try:
            fd = os.pidfd_open(int(path.name))
            environment = (path / 'environ').read_bytes().split(b'\0')
            if not markers.intersection(environment):
                continue
            signal.pidfd_send_signal(fd, signal.SIGTERM)
            fds.append(fd)
            fd = None
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            pass
        finally:
            if fd is not None:
                os.close(fd)
    # Let helpers finish their own cleanup before removing the private profile.
    pending = set(fds)
    deadline = time.monotonic() + 3
    while pending and time.monotonic() < deadline:
        ready, _, _ = select.select(list(pending), [], [], max(0, deadline - time.monotonic()))
        pending.difference_update(ready)
    for fd in pending:
        try:
            signal.pidfd_send_signal(fd, signal.SIGKILL)
        except ProcessLookupError:
            pass
finally:
    for fd in fds:
        os.close(fd)
PY
  # Retry only this verified directory while terminated helpers finish exiting.
  local attempt
  for attempt in 1 2 3; do
    if rm -rf -- "${profile:?}" && [[ ! -e "$profile" ]]; then
      printf '%s\n' 'PASS: isolated UI test profile and private helpers were cleaned up'
      return "$test_status"
    fi
    printf 'Retrying isolated profile cleanup (%s/3)\n' "$attempt" >&2
  done
  printf 'Failed to clean isolated test profile: %s\n' "$profile" >&2
  return 1
}
trap cleanup_profile EXIT
mkdir -m 700 "$profile/runtime"
# A private bus also needs a private runtime directory: a test document portal
# must never mount over (and subsequently unmount) the desktop's doc filesystem.
wayland_display=${WAYLAND_DISPLAY:-}
desktop_runtime=${XDG_RUNTIME_DIR:-}
desktop_doc_mount=
if [[ -n "$desktop_runtime" ]] && command -v findmnt >/dev/null; then
  desktop_doc_mount=$(findmnt -rn -M "$desktop_runtime/doc" -o ID || true)
fi
if [[ -n "$wayland_display" && "$wayland_display" != /* ]]; then
  : "${XDG_RUNTIME_DIR:?The desktop runtime directory is required for Wayland}"
  wayland_display="$XDG_RUNTIME_DIR/$wayland_display"
fi
objects=()
for object in "$build"/src/smashed-pumpkin.p/*.o; do
  case "$object" in
    */main.c.o|*/server.c.o) ;;
    *) objects+=("$object") ;;
  esac
done
packages=(gtk4 libadwaita-1 libsoup-3.0 gmodule-2.0)
test_defines=()
if pkg-config --exists xtst; then
  packages+=(xtst)
  test_defines+=(-DSMASHED_TEST_XTEST)
fi
if pkg-config --exists x11; then
  packages+=(x11)
fi
read -r -a flags <<< "$(pkg-config --cflags --libs "${packages[@]}")"
# Reuse the application's compiler and flags, including C standard, sanitizers
# and link options. A failed introspection must fail the test, not a subshell.
meson introspect --targets "$build" > "$profile/targets.json"
python3 - "$profile/targets.json" "$profile" "$build" <<'PY'
import json
from pathlib import Path
import sys

metadata, profile, build = map(Path, sys.argv[1:])
target = next(item for item in json.loads(metadata.read_text())
              if item['name'] == 'smashed-pumpkin')
compile_source = next(item for item in target['target_sources']
                      if item.get('language') == 'c')
link_source = next(item for item in target['target_sources'] if 'linker' in item)
link_flags = [str(build / flag) if not flag.startswith('-') and not Path(flag).is_absolute()
              else flag for flag in link_source['parameters']]
for name, values in [('compiler', compile_source['compiler']),
                     ('compile-flags', compile_source['parameters']),
                     ('link-flags', link_flags)]:
    if not values or any('\n' in value for value in values):
        raise ValueError(f'Invalid {name} in Meson introspection')
    (profile / name).write_text('\n'.join(values) + '\n')
PY
mapfile -t compiler < "$profile/compiler"
mapfile -t compile_flags < "$profile/compile-flags"
mapfile -t link_flags < "$profile/link-flags"
"${compiler[@]}" -Wall -Wextra -Werror "$root/tests/fake-pumpkin.c" -o "$profile/fake-pumpkin"
read -r -a glib_flags <<< "$(pkg-config --cflags --libs glib-2.0)"
"${compiler[@]}" -Wall -Wextra -Werror "$root/tests/fake-managed-server.c" \
  "${glib_flags[@]}" -o "$profile/fake-managed-server"
# Exercise the production shutdown logic with a shorter test-only deadline.
read -r -a gio_flags <<< "$(pkg-config --cflags gio-2.0)"
"${compiler[@]}" "${compile_flags[@]}" -DPUMPKIN_STOP_TIMEOUT_SECONDS=1 \
  "${gio_flags[@]}" -c "$root/src/server.c" -o "$profile/server.o"
objects+=("$profile/server.o")
"${compiler[@]}" "${compile_flags[@]}" "$root/tests/test-gtk-frames.c" \
  "${flags[@]}" -o "$profile/test-gtk-frames"
"${compiler[@]}" "${compile_flags[@]}" "${test_defines[@]}" \
  -I"$root/src" "$root/tests/test-window-ui.c" "$root/tests/ui-marketplace-fixture.c" "${objects[@]}" \
  -Wl,--wrap=pumpkin_marketplace_request_async \
  "${link_flags[@]}" "${flags[@]}" -o "$profile/test-window-ui"

# Never use the developer's servers, credentials, config or autostart entries.
# Existing wording assertions retain an explicit English baseline. Translation
# scopes use a non-C UTF-8 locale and catalog roots in the isolated build tree.
test_locale=C
test_language=en
test_catalog_root="$build/tests/i18n"
if [[ "$scope" == i18n-loading || "$scope" == i18n-chinese ]]; then
  test_locale=en_US.UTF-8
  if [[ "$scope" == i18n-chinese ]]; then
    test_language=zh_CN
    test_catalog_root="$build/po"
  else
    test_language=de
  fi
  if [[ "$(LC_ALL="$test_locale" locale charmap 2>/dev/null)" != UTF-8 ]]; then
    printf '%s\n' 'The translated UI test requires en_US.UTF-8 (not a translated system locale).' >&2
    exit 77
  fi
fi
test_environment=(env -u G_DEBUG LC_ALL="$test_locale" LANGUAGE="$test_language" TZ=UTC \
  GIO_USE_VFS=local GIO_USE_PROXY_RESOLVER=dummy GSETTINGS_BACKEND=memory \
  GTK_A11Y=none GSK_RENDERER="${SMASHED_UI_TEST_RENDERER:-cairo}" \
  SMASHED_PUMPKIN_LOCALEDIR="$test_catalog_root" \
  SMASHED_PUMPKIN_MESSAGE_LOCALEDIR="$build/src/message-locales" \
  XDG_RUNTIME_DIR="$profile/runtime" WAYLAND_DISPLAY="$wayland_display" \
  HOME="$profile" XDG_DATA_HOME="$profile/data" \
  XDG_CONFIG_HOME="$profile/config" XDG_CACHE_HOME="$profile/cache")
if ! "${test_environment[@]}" dbus-run-session -- timeout 10s env \
  G_DEBUG=fatal-warnings "$profile/test-gtk-frames"; then
  printf '%s\n' 'FAIL: GTK frame preflight failed before application tests. Check the GTK/libadwaita runtime pair; GTK 4.24.1 has a reproducible popover frame regression. See the UI test instructions in README.md.' >&2
  exit 1
fi
"${test_environment[@]}" \
  dbus-run-session -- timeout 90s env \
  FAKE_PUMPKIN_BINARY="$profile/fake-pumpkin" \
  FAKE_MANAGED_PUMPKIN_BINARY="$profile/fake-managed-server" \
  SMASHED_UI_TEST_SCOPE="$scope" \
  G_DEBUG=fatal-warnings \
  "$profile/test-window-ui"
if [[ -n "$desktop_doc_mount" ]]; then
  current_doc_mount=$(findmnt -rn -M "$desktop_runtime/doc" -o ID || true)
  if [[ "$current_doc_mount" != "$desktop_doc_mount" ]]; then
    printf '%s\n' 'FAIL: the desktop document portal mount changed during the UI test.' >&2
    exit 1
  fi
  printf '%s\n' 'PASS: the desktop document portal mount is unchanged'
fi
if [[ "$scope" == language-settings ]]; then
  printf '%s\n' 'Only language settings were exercised, using an isolated profile and a fake local server.'
  exit 0
fi
if [[ "$scope" == i18n-loading ]]; then
  printf '%s\n' 'Only translation loading was exercised, using an isolated test catalog; no server was started.'
  exit 0
fi
if [[ "$scope" == i18n-chinese ]]; then
  printf '%s\n' 'The production Simplified Chinese catalog, dynamic UI and raw-log/typed-event boundaries were exercised; no server was started.'
  exit 0
fi
if [[ "$scope" == i18n-boundaries ]]; then
  printf '%s\n' 'Only i18n logic boundaries and updates were exercised, using isolated fake servers and local HTTP fixtures.'
  exit 0
fi
if [[ "$scope" == releases ]]; then
  printf '%s\n' 'Only release-channel settings were exercised; no server was started.'
  exit 0
fi
if [[ "$scope" == worlds-map-access ]]; then
  printf '%s\n' 'Only world loading and minimap access were exercised; no server was started.'
  exit 0
fi
if [[ "$scope" == minimap ]]; then
  printf '%s\n' 'Screenshot: /tmp/smashed-minimap.png (only the minimap was exercised)'
  exit 0
fi
printf '%s\n' 'Screenshots: /tmp/smashed-startup-settings.png, /tmp/smashed-plugin-actions.png,' \
  '             /tmp/smashed-stats-hover.png, /tmp/smashed-plugin-details.png,' \
  '             /tmp/smashed-logs-light.png, /tmp/smashed-logs-dark.png,' \
  '             /tmp/smashed-players.png, /tmp/smashed-players-dark.png,' \
  '             /tmp/smashed-players-compact.png, /tmp/smashed-players-empty.png,' \
  '             /tmp/smashed-players-whitelist.png, /tmp/smashed-players-bans.png,' \
  '             /tmp/smashed-players-bans-compact.png,' \
  '             /tmp/smashed-worlds-light.png, /tmp/smashed-worlds-dark.png,' \
  '             /tmp/smashed-worlds-compact.png, /tmp/smashed-worlds-error.png,' \
  '             /tmp/smashed-player-details.png, /tmp/smashed-player-manage.png'

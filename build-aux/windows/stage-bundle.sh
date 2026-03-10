#!/usr/bin/env bash
set -euo pipefail

stage_root="${1:-package-root}"

if [[ ! -d build ]]; then
  echo "Expected Meson build directory at ./build" >&2
  exit 1
fi

if [[ -z "${MINGW_PREFIX:-}" ]]; then
  echo "MINGW_PREFIX is not set." >&2
  exit 1
fi

bundle_root="${PWD}/${stage_root}"
bundle_bin="${bundle_root}/bin"
runtime_prefix="${MINGW_PREFIX}"

copy_dir_contents() {
  local src="$1"
  local dest="$2"

  if [[ ! -d "$src" ]]; then
    return
  fi

  mkdir -p "$dest"
  cp -R "$src"/. "$dest"/
}

normalize_meson_install_layout() {
  local app_path
  local nested_prefix

  app_path="$(find "$bundle_root" -path '*/bin/smashed-pumpkin.exe' -print -quit)"
  if [[ -z "$app_path" ]]; then
    echo "Could not locate smashed-pumpkin.exe in staged Meson install." >&2
    exit 1
  fi

  nested_prefix="$(dirname "$(dirname "$app_path")")"
  if [[ "$nested_prefix" == "$bundle_root" ]]; then
    return
  fi

  copy_dir_contents "$nested_prefix" "$bundle_root"
  rm -rf "$nested_prefix"
  find "$bundle_root" -depth -mindepth 1 -type d -empty -delete
}

copy_runtime_deps() {
  local copied=1

  while [[ "$copied" -ne 0 ]]; do
    copied=0

    while IFS= read -r binary; do
      while IFS= read -r dep; do
        [[ -n "$dep" ]] || continue
        [[ -f "$dep" ]] || continue

        case "$dep" in
          "${runtime_prefix}"/bin/*)
            local target="${bundle_bin}/$(basename "$dep")"
            if [[ ! -f "$target" ]]; then
              cp -f "$dep" "$target"
              copied=1
            fi
            ;;
        esac
      done < <(ldd "$binary" | awk '{print $3}')
    done < <(find "$bundle_bin" -maxdepth 1 \( -name '*.exe' -o -name '*.dll' \) -print)
  done
}

rm -rf "$bundle_root"
meson install -C build --destdir "$bundle_root"
normalize_meson_install_layout

mkdir -p "$bundle_bin"

for helper in \
  gspawn-win64-helper.exe \
  gspawn-win64-helper-console.exe \
  gdk-pixbuf-query-loaders.exe
do
  if [[ -f "${runtime_prefix}/bin/${helper}" ]]; then
    cp -f "${runtime_prefix}/bin/${helper}" "$bundle_bin/"
  fi
done

copy_runtime_deps

copy_dir_contents "${runtime_prefix}/lib/gio/modules" \
  "${bundle_root}/lib/gio/modules"
copy_dir_contents "${runtime_prefix}/lib/gdk-pixbuf-2.0" \
  "${bundle_root}/lib/gdk-pixbuf-2.0"
copy_dir_contents "${runtime_prefix}/lib/gtk-4.0" \
  "${bundle_root}/lib/gtk-4.0"

copy_dir_contents "${runtime_prefix}/etc/fonts" \
  "${bundle_root}/etc/fonts"
copy_dir_contents "${runtime_prefix}/etc/gtk-4.0" \
  "${bundle_root}/etc/gtk-4.0"
copy_dir_contents "${runtime_prefix}/etc/pango" \
  "${bundle_root}/etc/pango"

copy_dir_contents "${runtime_prefix}/share/glib-2.0/schemas" \
  "${bundle_root}/share/glib-2.0/schemas"
copy_dir_contents "${runtime_prefix}/share/fontconfig" \
  "${bundle_root}/share/fontconfig"
copy_dir_contents "${runtime_prefix}/share/themes" \
  "${bundle_root}/share/themes"
copy_dir_contents "${runtime_prefix}/share/gtk-4.0" \
  "${bundle_root}/share/gtk-4.0"
copy_dir_contents "${runtime_prefix}/share/libadwaita-1" \
  "${bundle_root}/share/libadwaita-1"
copy_dir_contents "${runtime_prefix}/share/icons/Adwaita" \
  "${bundle_root}/share/icons/Adwaita"
copy_dir_contents "${runtime_prefix}/share/icons/hicolor" \
  "${bundle_root}/share/icons/hicolor"
copy_dir_contents "${runtime_prefix}/share/mime" \
  "${bundle_root}/share/mime"

if [[ -x "${runtime_prefix}/bin/glib-compile-schemas.exe" ]] \
  && [[ -d "${bundle_root}/share/glib-2.0/schemas" ]]; then
  "${runtime_prefix}/bin/glib-compile-schemas.exe" \
    "${bundle_root}/share/glib-2.0/schemas"
fi

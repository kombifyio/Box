#!/usr/bin/env bash
# Host preview of the Box UI 2 (see preview.c).
#
#   CC="zig cc" tests/ui_preview/build.sh <lvgl-v9.3.0-src> <out-dir> [locale]
#
# Builds LVGL once into <out-dir>/obj (incremental), compiles the real UI
# sources with stubbed ESP-IDF headers, runs the preview and writes one PPM
# per mockup frame into <out-dir>. tools/ppm2png.mjs turns them into PNGs.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
fw="$(cd "$here/../.." && pwd)"
lvgl="$(cd "$1" && pwd)"
out="$2"
loc="${3:-}"
CC="${CC:-cc}"
mkdir -p "$out/obj/lvgl" "$out/obj/ui"

exe=preview
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) exe=preview.exe ;; esac

flags=(-O1 -w -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -I"$here" -I"$lvgl" -I"$lvgl/src")
uiflags=(-include "$here/compat.h" -I"$here/stubs")
for c in kbx_audio kbx_core kbx_net kbx_sensors kbx_ui kbx_voice kbx_wake; do uiflags+=(-I"$fw/components/$c/include"); done

# LVGL objects (only missing ones)
mapfile -t srcs < <(cd "$lvgl" && find src -name '*.c' | sort)
printf '%s\n' "${srcs[@]}" | xargs -P "${JOBS:-8}" -I{} sh -c '
  o="$0/obj/lvgl/$(echo {} | tr / _).o"
  [ -f "$o" ] || '"$CC"' '"${flags[*]}"' -c "$1/{}" -o "$o"' "$out" "$lvgl"

ui=(kbx_ui/ui_scene.c kbx_ui/ui_overlays.c kbx_ui/ui_style.c kbx_ui/aura_orb.c kbx_ui/display_profile.c
    kbx_core/i18n.c kbx_core/i18n_table.c
    kbx_ui/assets/kombify_rosette_104.c kbx_ui/assets/kombify_rosette_124.c kbx_ui/assets/kombify_rosette_167.c
    kbx_ui/assets/fonts/kbx_lat_14.c kbx_ui/assets/fonts/kbx_lat_16.c kbx_ui/assets/fonts/kbx_lat_18.c kbx_ui/assets/fonts/kbx_lat_20.c kbx_ui/assets/fonts/kbx_lat_28.c
    kbx_ui/assets/fonts/kbx_zh_14.c kbx_ui/assets/fonts/kbx_zh_16.c kbx_ui/assets/fonts/kbx_zh_18.c kbx_ui/assets/fonts/kbx_zh_20.c
    kbx_ui/assets/fonts/kbx_ar_14.c kbx_ui/assets/fonts/kbx_ar_16.c kbx_ui/assets/fonts/kbx_ar_18.c kbx_ui/assets/fonts/kbx_ar_20.c
    kbx_ui/assets/fonts/kbx_num_56.c)
objs=()
for s in "${ui[@]}"; do
  o="$out/obj/ui/$(echo "$s" | tr / _).o"
  $CC "${flags[@]}" "${uiflags[@]}" -c "$fw/components/$s" -o "$o"
  objs+=("$o")
done
$CC "${flags[@]}" "${uiflags[@]}" -c "$here/preview.c" -o "$out/obj/ui/preview.o"
# response file: the LVGL object list exceeds the Windows command line
(cd "$out/obj/lvgl" && ls *.o | sed "s|^|$out/obj/lvgl/|") > "$out/obj/lvgl.rsp"
$CC -o "$out/$exe" "$out/obj/ui/preview.o" "${objs[@]}" @"$out/obj/lvgl.rsp" -lm
"$out/$exe" "$out" $loc

#!/usr/bin/env bash
# Dolby Vision RPUs through the final Apple series (mpv 0316, FFmpeg 0304):
# the enhancement layer kind mpv reports (video-params/dolby-vision-el, the
# "dovi el kind" region of video/mp_image.c, compiled VERBATIM) agrees with
# dovi_tool on every RPU in libdovi's test assets, and FFmpeg's
# ff_dovi_attach_rpu_buffer() writes each parsed RPU back so that it parses
# to the same metadata. See test_dovi_rpu_layers.c.
#
# Builds a host libavcodec (libavutil and the HEVC and AV1 decoders only)
# from the patched FFmpeg tree: a few minutes.
# Usage: test_dovi_rpu_layers.sh [patched-mpv-tree [patched-ffmpeg-tree [dovi_tool-checkout]]]
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mpv_dir="${1:-}"
ffmpeg_dir="${2:-}"
dovi_dir="${3:-}"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$mpv_dir" ]]; then
    mpv_dir="$workdir/mpv"
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv apple "$mpv_dir")
fi
if [[ -z "$ffmpeg_dir" ]]; then
    ffmpeg_dir="$workdir/ffmpeg"
    (cd "$root" && python3 scripts/patches.py fetch-pinned ffmpeg apple "$ffmpeg_dir")
fi
read -r dovi_url dovi_ref dovi_commit < <(python3 - "$root/versions.json" <<'PY'
import json, sys
up = json.load(open(sys.argv[1]))["components"]["libdovi"]["upstream"]
print(up["url"], up["ref"], up["commit"])
PY
)
if [[ -z "$dovi_dir" ]]; then
    dovi_dir="$workdir/dovi_tool"
    git clone -q --depth 1 --branch "$dovi_ref" "$dovi_url" "$dovi_dir"
fi
head="$(git -C "$dovi_dir" rev-parse HEAD)"
if [[ "$head" != "$dovi_commit" ]]; then
    echo "dovi_tool checkout is at $head, versions.json records $dovi_commit" >&2
    exit 1
fi

python3 "$root/scripts/extract.py" region "$mpv_dir/video/mp_image.c" \
    "$workdir/dovi_el_kind.inc" --start "dovi el kind" \
    --require "mp_dovi_el_kind" --require "mp_dovi_el_from_av"

build="$workdir/build"
mkdir -p "$build"
(cd "$build" && "$ffmpeg_dir/configure" --disable-everything --disable-programs \
    --disable-doc --disable-avformat --disable-avdevice --disable-avfilter \
    --disable-swscale --disable-swresample --disable-autodetect \
    --enable-decoder=hevc,av1 >/dev/null && make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
    libavutil/libavutil.a libavcodec/libavcodec.a >/dev/null)

cc -O1 -std=c11 -w -I"$workdir" -I"$build" -I"$ffmpeg_dir" \
    -o "$workdir/test" "$root/scripts/test_dovi_rpu_layers.c" \
    "$build/libavcodec/libavcodec.a" "$build/libavutil/libavutil.a" -lm -lpthread

"$workdir/test" "$dovi_dir"/assets/tests/*.bin

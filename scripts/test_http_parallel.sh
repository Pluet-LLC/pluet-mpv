#!/usr/bin/env bash
# mpv patch 0318 (stream/http_parallel.c): one http(s) file through several
# connections. Builds the patched FFmpeg's http and tcp protocols for the host
# (as test_http_persistent.sh does) and runs test_http_parallel.c against
# rate-limited HTTP/1.1 servers on loopback; prints the measured speedup.
# Usage: test_http_parallel.sh [patched-mpv] [patched-ffmpeg]
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
mpv_dir="${1:-}"
ffmpeg_dir="${2:-}"
if [[ -z "$mpv_dir" ]]; then
    mpv_dir="$workdir/mpv"
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv apple "$mpv_dir")
fi
if [[ -z "$ffmpeg_dir" ]]; then
    ffmpeg_dir="$workdir/ffmpeg"
    (cd "$root" && python3 scripts/patches.py fetch-pinned ffmpeg apple "$ffmpeg_dir")
fi
ffmpeg_dir="$(cd "$ffmpeg_dir" && pwd)"
build="$workdir/build"
mkdir -p "$build"
(cd "$build" && "$ffmpeg_dir/configure" --disable-everything --disable-programs --disable-doc \
    --disable-autodetect --disable-asm --disable-avdevice --disable-avfilter \
    --disable-swscale --disable-swresample \
    --enable-protocol=http --enable-protocol=tcp >"$workdir/configure.log" 2>&1) \
    || { cat "$workdir/configure.log"; exit 1; }
make -C "$build" -j"$(getconf _NPROCESSORS_ONLN)" libavformat/libavformat.a libavcodec/libavcodec.a \
    libavutil/libavutil.a >"$workdir/make.log" 2>&1 || { tail -50 "$workdir/make.log"; exit 1; }
cc -O2 -g -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare -pthread \
    -I"$build" -I"$ffmpeg_dir" -I"$mpv_dir" -o "$workdir/test" "$root/scripts/test_http_parallel.c" \
    "$mpv_dir/stream/http_parallel.c" \
    "$build/libavformat/libavformat.a" "$build/libavcodec/libavcodec.a" "$build/libavutil/libavutil.a" \
    -lm -lpthread
"$workdir/test"

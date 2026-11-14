#!/usr/bin/env bash
# Compile vo_avfoundation's vsync snap (the marked "avf vsync snap" region,
# patch 0307) VERBATIM from the final Apple series and drive it with synthetic
# clocks: every phase across a refresh at 23.976 fps on 23.976 Hz with
# Matroska's 1 ms rounding (never a skip next to a repeat, under either rule a
# layer could use for its boundary), slow drift across a boundary (one clean
# repeat or skip per crossing), the 24/60, 23.976/59.94, 25/60, 30/60 and
# 25/50 cadences, a mode change, a seek, no refresh clock, the enqueue
# pacing under a 14 ppm drift, the auto pace rule, 3 hours of drift with
# every repeat or skip at a cut, and the cut/still classifier. Pass an
# already-patched mpv tree for offline use; otherwise fetch and patch the
# pinned source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv apple "$source_dir")
fi

python3 "$root/scripts/extract.py" region "$source_dir/video/out/vo_avfoundation.m" \
    "$workdir/avf_vsync_snap.inc" --start "avf vsync snap" \
    --require "avf_vsync_snap_refresh"

cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_avf_vsync_snap.c" -lm
"$workdir/test"

#!/usr/bin/env bash
# Compile vo_avfoundation's subtitle refresh match (the marked "avf subtitle
# refresh match" region, patch 0312, measurement only) VERBATIM from the
# final Apple series and check its judgement: in step, a subtitle a refresh
# late and early, a resent buffer, transparent samples, a flush, a sample
# shown at once, and the lead model's zones. Pass an already-patched mpv tree
# for offline use; otherwise fetch and patch the pinned source.
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
    "$workdir/avf_subref.inc" --start "avf subtitle refresh match" \
    --require "avf_subref_judge"
python3 "$root/scripts/extract.py" region "$source_dir/video/out/vo_avfoundation.m" \
    "$workdir/avf_restart_stamp.inc" --start "avf restart stamp" \
    --require "avf_host_restart_stamp_ns"

cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_avf_subref.c"
"$workdir/test"

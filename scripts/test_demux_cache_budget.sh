#!/usr/bin/env bash
# mpv patch 0320 (--demuxer-max-total-bytes): compile the demux cache budget
# region of the patched demux/demux.c and play packets through it: ahead and
# behind together stay within the one limit, the forward cache reads into the
# part of the back buffer that is not held, what was kept behind is never
# pushed out by reading ahead, both settle at total - max_bw and max_bw, and
# with the limit off the budget is upstream's to the byte. Pass an
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
python3 "$root/scripts/extract.py" region "$source_dir/demux/demux.c" "$workdir/demux_cache_budget.inc" \
    --start "demux cache budget"
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function \
    -I"$workdir" "$root/scripts/test_demux_cache_budget.c" -o "$workdir/test"
"$workdir/test"

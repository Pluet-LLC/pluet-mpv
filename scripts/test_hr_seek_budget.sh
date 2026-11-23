#!/usr/bin/env bash
# --hr-seek-budget (patch 0314): hr_seek_budget_applies() and
# hr_seek_budget_landing() from player/playloop.c and kf_index_around() from
# demux/demux.c, compiled VERBATIM from the final Apple series against fakes.
# Checks: within the budget a seek stays exact; beyond it, it lands on the
# nearer keyframe; a forward skip never lands behind where it started (nor a
# backward one ahead); +exact, +keyframes and the player's own seeks are not
# touched; the default (0) is off, i.e. today's behaviour. The option's table
# entry, the seek command's flag and the gate in mp_seek() are checked in the
# source too. Pass an already-patched mpv tree for offline use; otherwise
# fetch and patch the pinned source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv apple "$source_dir")
fi

E="$root/scripts/extract.py"
playloop="$source_dir/player/playloop.c"
core="$source_dir/player/core.h"
python3 "$E" type "$core" "$workdir/budget.inc" --name seek_type
python3 "$E" type "$core" "$workdir/budget.inc" --append --name seek_precision
python3 "$E" type "$core" "$workdir/budget.inc" --append --name seek_flags
python3 "$E" type "$core" "$workdir/budget.inc" --append --name seek_params
python3 "$E" type "$playloop" "$workdir/budget.inc" --append --name hr_seek_budget_landing
python3 "$E" symbol "$playloop" "$workdir/budget.inc" --append \
    --fn hr_seek_budget_applies --return "bool"
python3 "$E" symbol "$playloop" "$workdir/budget.inc" --append \
    --fn hr_seek_budget_landing --return "enum hr_seek_budget_landing"
python3 "$E" symbol "$source_dir/demux/demux.c" "$workdir/kf_index.inc" \
    --fn kf_index_around --return void
python3 "$E" range "$source_dir/options/options.c" "$workdir/option.inc" \
    --first '{"hr-seek-budget"' --last 'M_RANGE(0, 3600)},' --last-inclusive

fail() { echo "FAIL: $*" >&2; exit 1; }

# Default off: no explicit default, so the option is 0.
grep -q '\.hr_seek_budget' "$source_dir/options/options.c" &&
    fail "explicit default for hr_seek_budget; the default must stay 0 (off)"
# Only the seek command's relative and absolute forms carry the flag.
n=$(grep -c 'MPSEEK_FLAG_DELAY | MPSEEK_FLAG_BUDGET' "$source_dir/player/command.c")
[[ "$n" == 2 ]] || fail "MPSEEK_FLAG_BUDGET on $n queue_seek calls, expected 2"
grep -q 'MPSEEK_FLAG_BUDGET' "$source_dir/player/loadfile.c" &&
    fail "a player-made seek (loadfile.c) carries MPSEEK_FLAG_BUDGET"
# mp_seek() consults the budget only through the gate.
grep -q 'if (hr_seek_budget_applies(opts->hr_seek_budget, &seek) &&' "$playloop" ||
    fail "mp_seek() does not gate the budget on hr_seek_budget_applies()"

cc -O1 -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_hr_seek_budget.c" -lm
"$workdir/test"

#!/usr/bin/env bash
# The lossless spatial route and Force Dolby MAT (mpv patch 0317,
# SpatialAudioBridge patch 0001): SpatialAudioBridge's renderer and ao_spatial's
# Swift side, built from the pinned and patched sources with the Rust core,
# against a capturing output (test_spatial_lossless.swift). No audio is played.
# macOS only. Usage: test_spatial_lossless.sh [patched-mpv] [iec-truehd-atmos-or-dts-x ...]
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mpv="${1:-}"
if [[ -z "$mpv" ]]; then
    mpv="$work/mpv"
    python3 "$root/scripts/patches.py" fetch-pinned mpv apple "$mpv"
fi
arch="$(uname -m)"
rust_target="$([[ "$arch" == arm64 ]] && echo aarch64-apple-darwin || echo x86_64-apple-darwin)"
python3 "$root/scripts/build_spatial.py" --source "$work/sab" --output "$work/spatial" --mpv "$mpv" \
    --target "$arch-apple-macos26.0" --rust-target "$rust_target" --sdk "$(xcrun --sdk macosx --show-sdk-path)" > "$work/build.log" 2>&1 \
    || { cat "$work/build.log"; exit 1; }
sab="$work/sab/Sources"
names=(Types Decoder APACEncoder SampleBuffer IEC61937Unpacker SpatialAudioRenderer SpatialAudioPassthroughSink
       SpatialAudioTranscoder FragmentTrim)
sources=()
for name in "${names[@]}"; do sources+=("$sab/SpatialAudioBridge/$name.swift"); done
xcrun --sdk macosx swiftc -swift-version 6 -O -parse-as-library -target "$arch-apple-macos26.0" \
    -I "$work/spatial/include" "${sources[@]}" "$sab/SpatialAudioPlayback/AtmosOutputActivator.swift" \
    "$mpv/audio/out/ao_spatial.swift" "$work/spatial/SpatialResources.swift" "$root/scripts/test_spatial_lossless.swift" \
    "$work/spatial/libspatial_audio_core.a" -framework AVFoundation -framework Accelerate -framework AudioToolbox \
    -o "$work/test"
"$work/test" ${2:+"${@:2}"}

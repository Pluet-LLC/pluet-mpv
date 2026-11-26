# Apple

The apple group publishes one XCFramework zip per library, and the per-platform
thin zips `make build use-prebuilt` restores, to the release of the publish run
that built them (`binaries-<first 12 hex of the commit>`; those published
before per-run releases stay in
[`binaries-apple`](https://github.com/edde746/mpv-build/releases/tag/binaries-apple)).
`Package.swift` is rendered from the `platforms.apple` section of
`artifacts.json` and exposes them as the `MPVKit` product. The build driver is
the SwiftPM package in `Sources/BuildScripts/` (`make build`); the publish job
merges the per-platform legs with `scripts/merge_xcframeworks.py`.

Libraries this repository does not compile come from the prebuilt releases
`versions.json` pins (mostly `mpvkit/*-build`). The build checks each archive's
SHA-256 against `versions.json` before unpacking it, and each source clone's
commit after cloning, and stops on a mismatch or a pin without one. Their licence texts, and those
of everything built here, ship as the `_MPVKit` target's resources
(`Sources/_MPVKit/licenses/`), which Xcode embeds in every app as
`MPVKit__MPVKit.bundle/licenses/`; see
[Licence texts](../README.md#licence-texts).

## Testing a local build in the Demo apps

The Demo projects (`Demo/Demo.xcworkspace`) reference this package by local
path. To run them against a local `make build`, point the binary targets in
`Package.swift` at the zips it wrote, one per framework:

```swift
.binaryTarget(name: "Libmpv", path: "dist/release/Libmpv.xcframework.zip"),
.binaryTarget(name: "Libavcodec", path: "dist/release/Libavcodec.xcframework.zip"),
// ... Libavdevice, Libavformat, Libavfilter, Libavutil, Libswresample, Libswscale
```

Do not commit that edit: `keys.py verify` requires `Package.swift` to match
`artifacts.json`.

## Static pkg-config consumers

Configure the install paths in `pkgconfig-example` and select an Xcode
compatible with the compiler that built the binaries. `mpv.pc` adds the Swift
runtime and compatibility-library flags, but its `swift_toolchain` value is a
placeholder. Set it in the installed file or on the command line:

```bash
swift_compiler="$(xcrun --find swiftc)"
swift_toolchain="$(dirname "$(dirname "$(dirname "$swift_compiler")")")"
pkg-config --define-variable="swift_toolchain=$swift_toolchain" --static --libs mpv
```

Use the metadata of the target slice: device and simulator Swift libraries
live in different toolchain directories. The SwiftPM wrapper contains Swift
source for the same reason, so C and Objective-C consumers also get the Swift
runtime and back-deployment libraries.

## Starting a file quickly on iOS and tvOS

Measured on an iPhone Air, a local file plays 45-55 ms after `loadfile`
(80 ms for 4K HEVC); `mpv_create` and `mpv_initialize` take under 8 ms
together. Three things outside libmpv decide whether it stays there:

- **Activate the audio session before `loadfile`**, off the main thread
  (`AVAudioSession.setCategory`, then `setActive(true)`). An audio output
  that finds the session inactive activates it itself, on mpv's core thread:
  42 ms the first time in a process and about 26 ms after the last output
  deactivated it, during which nothing else of the file starts. The first
  video frame no longer waits for that (patch 0046), the start of playback
  does.
- **Do not hold the main thread.** `vo=avfoundation` reads and changes the
  display layer on the main thread and waits for it: once when the VO is
  created (patch 0047; seven times before it) and once for each change of the
  layer's size, scale or gravity. A main thread that works in pieces with
  gaps costs nothing; one that is busy for 100 ms delays the first frame by
  up to that long. Never call libmpv from the main thread in a way that
  waits for the VO.
- **Network files pay per connection.** FFmpeg's http protocol opens a new
  connection for every seek, and a Matroska file muxed by mkvmerge has its
  Cues and Tags at the end: three connections before the first frame against
  one for a file with only the Cues there, 220 ms against 90 ms with 50 ms
  per response. An MP4 with its `moov` at the end pays the same.

## AVPlayer passthrough output

`ao_avfoundation`'s compressed (AVPlayer) sink needs a host that services the
main dispatch queue: AVPlayer attaches its item on the main thread. A host
whose main thread never does (a command-line client blocked in
`mpv_wait_event()`) gets no resource-loader call; the output seeks to the start
after about 2 s and falls back to PCM after about 4.4 s, and both log lines say
AVPlayer made no resource requests. Run the main loop on the main thread
(`NSApplication`, `RunLoop.main.run()` or `dispatchMain()`) and drive libmpv
from another thread.

A passthrough output that cannot play its stream after opening (this sink, or
`ao_spatial`'s renderer) calls `ao_request_pcm_fallback()`. The player decodes
that track to PCM and seeks back to the playback position. The fallback
belongs to the track: the next track or file, or a later output reload, tries
passthrough again. Patches 0040-0042 carry the details.

With `--audio-spdif-backoff=yes` (patch 0303; off by default, which is 0040's
behaviour) the player also remembers the failure, per codec and per player
instance, with the audio route it happened on: later files and output
reloads decode that codec to PCM instead of paying the failure's seconds
again. An entry lapses when the route changes, on iOS and tvOS also when the
route reports a device change (a receiver power-cycled, a cable replugged),
after `--audio-spdif-backoff-expiry` seconds (default 600), and once
passthrough of any codec has played on that route for 10 seconds. It stays
off until a device shows how tvOS reports route changes around a receiver
waking and around the PCM output's own session setup.

Mid-stream, the sink watches its own playhead (patch 0302). While the core
has it playing and at least a second of stream is buffered, a playhead that
stands still for `--ao-avfoundation-compressed-stall-timeout` seconds
(default 4, 0 turns it off) gets one restart of the player, and the PCM
fallback if it is still stuck a timeout later. On iOS and tvOS the sink also
follows the audio session: an interruption holds the watchdog, and after the
interruption ends or the route changes, a player the system left stopped is
resumed and watched again. AVAudioSession errors are logged at verbose level.
Pausing, resuming and stopping reset the watchdog's window, and a start or
resume also ends an interruption the system never reported the end of. While
AVPlayer reports that it is waiting to play (rebuffering), the allowance is
four times the timeout, so a slow source is not pushed to PCM, but a wait
that never ends still falls back.

## Dolby Vision profile 7

VideoToolbox takes profile 8.1, so FFmpeg converts a profile 7 stream's RPUs
through libdovi by default. The HEVC decoder's `dv_conversion_mode` AVOption
picks the handling: `auto` (the default), `dv81`, `hevc_strip` (drop the RPUs,
decode the base layer as HEVC) or `disabled` (hand profile 7 over unchanged).
From mpv, `--vd-lavc-dv-conversion=<default|auto|disabled|dv81|hevc-strip>`
sets it on the decoders that have it, and
`--vd-lavc-o=dv_conversion_mode=<value>` sets it too. Left unset, FFmpeg reads
`PLUET_DV_CONVERSION_MODE`, else `PLEZY_DV_CONVERSION_MODE`, once when the
hardware decoder opens (FFmpeg patch 0300, mpv patch 0301).

Neither VideoToolbox nor FFmpeg decodes the enhancement layer: a minimal one
(MEL) loses nothing, a full one (FEL) loses picture. mpv's
`video-params/dolby-vision-el` (and `video-dec-params/...`) says which the
current frame's RPU describes, `mel` or `fel`, decided as libdovi's
`is_mel()` (mpv patch 0316), so a player can ask whether to show Dolby
Vision without the layer or, with `--vd-lavc-dv-conversion=hevc-strip` and
`--vf=format:dolbyvision=no`, the HDR10 base.

Dolby Vision profile 10 (AV1) has no VideoToolbox session of its own. The
AV1 decoders export each frame's RPU as `AV_FRAME_DATA_DOVI_RPU_BUFFER` in
the HEVC decoder's layout, written back from the parsed metadata and kept
only when FFmpeg parses it again, and on a VideoToolbox frame the native
decoder attaches it to the pixel buffer as `DolbyVisionRPUData`, as a Dolby
Vision HEVC session's output carries it (FFmpeg patch 0304).
Software-decoded 10.0 frames get it from vo_avfoundation's profile 5 path;
10.1 and 10.4 decoded in software present their base layer.

HDR10+ needs nothing of its own: FFmpeg parses the ST 2094-40 SEIs into
`AV_FRAME_DATA_DYNAMIC_HDR_PLUS` (hwaccel included, with FFmpeg 0016
forwarding the prefix SEIs to VideoToolbox), and vo_avfoundation attaches
each frame's metadata to its display sample as
`kCMSampleAttachmentKey_HDR10PlusPerFrameData` (mpv 0004, part 0030).

The apple libdovi prebuilt (3.3.2) aborts the process if its parser panics,
so VideoToolbox converts only RPUs whose size, prefix, final byte and CRC32
libdovi would accept; any other RPU is dropped (the frame decodes through its
base layer), logged once (FFmpeg patch 0301). That keeps damaged streams away
from the parser. It cannot stop a crafted RPU with a correct CRC32.

## Known limits of the Pluet patches

- **vo_avfoundation's bounded waits (mpv 0300).** preinit waits at most 500 ms
  for the main thread to attach the timebase, register the observers and read
  the layer's geometry, and the OSD layer's creation waits as long. When a
  wait times out (the host's main thread is blocked), the VO goes on: frames
  are enqueued against a layer that may not have its timebase yet and without
  layer geometry, until the late main-thread block runs and the VO adopts its
  result on the next VOCTRL_CHECK_EVENTS. Playback is degraded for that time,
  not hung.
- **Cached prebuilt archives.** The build checks a prebuilt archive's SHA-256
  before unpacking it. A cached archive whose hash still matches is not
  unpacked again, so what was extracted from it earlier in `dist/` is trusted
  as it is; `make clean` starts over.
- **`use-prebuilt` binaries.** The per-platform zips `make build use-prebuilt`
  restores from `artifacts.json` (`prebuilt.<platform>`) carry no hash there,
  and the Swift driver does not verify them; only the third-party prebuilts
  in `versions.json` are verified.
- **First CI run after these patches.** Every Apple content key changes (new
  patches, the driver, the prebuilt hashes in `versions.json`), so the first
  publish run builds every Apple library from source.
- **Network for tests.** `scripts/test_dovi_rpu_check.sh` clones dovi_tool at
  the pinned commit for libdovi's test RPUs, and, like every harness run
  without a source tree, fetches the pinned FFmpeg. `test_vt_dovi_conversion_mode.sh`
  and `test_tx_neon_imdct.sh` build a minimal libavcodec or libavutil on the
  host (a minute or two each).

## Spatial audio (`ao=spatial`)

`ao_spatial` (patch 0039) plays TrueHD Atmos and DTS:X by handing mpv's IEC
61937 bursts to SpatialAudioBridge's `SpatialAudioPassthroughSink`, which
unpacks, converts and queues APAC on the output's synchronizer.

- **Where it exists.** OS 26 on arm64 tvOS and its simulator, and on iOS and
  its simulators. macOS, Mac Catalyst, arm64e and Intel tvOS keep their other
  audio outputs: macOS links LuaJIT, and LuaJIT, C++, Objective-C and the
  core's Rust would need four unwind personality routines, one more than the
  compact-unwind format allows.
- **Fallback.** TrueHD/DTS-HD and conversion failures fall back to PCM. With
  `ao=spatial,` on a slice without the driver, the trailing comma enables
  ordinary output probing; no other Apple output takes TrueHD/DTS-HD
  passthrough, so mpv disables SPDIF and decodes to PCM. That path was checked
  by reading the source, not run. On macOS, CoreAudio hands non-PCM to
  `coreaudio_exclusive`, which accepts SPDIF, so a macOS host must not request
  TrueHD/DTS-HD passthrough itself.
- **Clock.** The driver owns the synchronizer's rate. The clock starts once the
  sink is ready, stops with 250 ms of converted audio left, and resumes with
  500 ms queued, or 250 ms when the sink takes no more. A slow conversion
  holds playback instead of letting the renderer drop late audio. These are
  scheduling policies, not timing guarantees.
- **Buffering.** About half a second the sink cannot take yet waits in the
  output and counts against `free_samples`; the sink buffers a second ahead of
  the renderer. Silence converts to nothing, so the output opts out of
  `audio-stream-silence`. An IEC 61937 pause burst or format change starts a
  new timeline; any other sink failure requests the PCM fallback.
- **Audio session.** Like mpv's other Apple outputs, it sets the session's
  category, mode and multichannel support and activates it when it opens, then
  leaves it to the host.
- **Lossless 7.1.4 (patch 0317, SpatialAudioBridge 0001).**
  `--ao-spatial-output=auto|apac|lpcm` (default `auto`). With `lpcm` the
  renderer enqueues the rendered 7.1.4 as 32-bit float linear PCM in Apple's
  Atmos 7.1.4 layout (`kAudioChannelLayoutTag_Atmos_7_1_4`: L R C LFE Ls Rs
  Rls Rrs Vhl Vhr Ltr Rtr) instead of encoding APAC, beside the same Atmos
  activator; `auto` does so where the audio session's route is HDMI and keeps
  APAC elsewhere (AirPlay, headphones, built-in speakers). The PCM is the
  render rounded to the 24-bit grid Dolby MAT carries (so the system's
  conversion to 24-bit is exact; on a TrueHD Atmos film excerpt the rounding
  changed the render by -331 dB, while APAC measured 49.1 dB
  signal-to-error), and the first audio is enqueued
  after 0.03 s of input instead of APAC's 1.46 s. A renderer that turns
  linear PCM down makes the output reload with APAC for the rest of the
  player's life; a stream without objects still falls back to decoding.
  Public API only. Not yet heard through a receiver: whether tvOS's MAT
  output carries the twelve channels unchanged needs a receiver's channel
  check.
- **Force Dolby MAT (patch 0317).** `--ao-spatial-force-mat` (default no)
  also takes decoded PCM of more than two channels (bed-only TrueHD, DTS-HD
  MA without DTS:X, multichannel PCM, anything else decoded to 5.1 or more):
  the 7.1.4 speakers only are offered (mpv remaps or downmixes nothing
  else), float at 48 kHz, each channel unchanged in its 7.1.4 slot (a 5.1
  with back surrounds at the sides), through the same lossless renderer and
  activator. mpv's own volume already applies to decoded PCM, so the
  renderer stays at unity there. Stereo, AC-3 and E-AC-3 go to the next
  output as before.
- **Layout.** `--ao-spatial-layout=7.1.4|9.1.6` (default 7.1.4). 9.1.6 is
  16 channels in `kAudioChannelLayoutTag_Atmos_9_1_6`, linear PCM or APAC;
  the output then asks the session for 16 output channels
  (`setPreferredOutputNumberOfChannels`). SAB measured Apple TV's MAT output
  at 12 channels with APAC; whether 16-channel linear PCM keeps 16 is for a
  receiver to show. On iOS and tvOS the output logs the session's rendering
  mode and output channels when it opens.
- **`spatial-audio-output`.** What the output sends: `apac`, `lpcm` or
  `pcm` (Force Dolby MAT); unavailable otherwise. It changes with the audio
  reconfig event, as `current-ao` does.

### How it is built

`scripts/build_spatial.py` compiles the SpatialAudioBridge commit pinned in
`versions.json` with `cargo --locked` and the Rust version in
`toolchain/apple.txt`. The Rust core and Swift sink are folded into `Libmpv`;
that pin and `build_spatial.py` are part of libmpv's content key only. There is
no separate SpatialAudioBridge package or application callback. The binary
includes the silent Atmos clip and its licence notices.

The Rust core is built as a fat-LTO staticlib and prelinked into one object in
which only `sab_*` and a weak `rust_eh_personality` stay global, so it does not
collide with libdovi's Rust runtime. The build checks those exports for every
slice.

### Its tests

| Script | What it checks | Runs in |
| --- | --- | --- |
| `scripts/test_ao_spatial.sh` | The driver against a real synchronizer and a fake sink: admission, clock start/hold/resume, pause, silence, gaps, EOF, the PCM fallback. | `ci.yml`, `publish.yml` |
| `scripts/test_spatial_feed.sh` | The production Swift bridge with the pinned sink and IEC unpacker against a fake renderer. | `ci.yml`, `publish.yml` |
| `scripts/test_spatial_archives.sh <patched-mpv> <platform>` | The installed slices: exports, a link with force-loaded libdovi, an Objective-C consumer through `mpv.pc`. | `publish.yml`, after each platform build |
| `scripts/test_spatial_swiftpm.sh <release-dir>` | An Objective-C-only app on iOS and tvOS linked through the packaged SwiftPM product (needs XcodeGen). | `publish.yml`, after packaging |
| `scripts/test_spatial_renderer.sh <patched-mpv> <built-spatial-dir> <iec-file> <1=truehd\|2=dtshd> <expect-failure:0\|1> [libdovi-archive]` | A real, muted renderer on macOS. Make the IEC file with FFmpeg's `spdif` muxer (`-dtshd_rate 768000` for DTS-HD). `SPATIAL_LOSSLESS=1` plays the rendered 7.1.4 as linear PCM. | manual |
| `scripts/test_spatial_lossless.sh [patched-mpv] [iec-truehd-atmos]` | SpatialAudioBridge's real renderer and decoder with the Rust core against a capturing output: linear PCM is the render bit for bit, in the Atmos 7.1.4 layout, with exact timestamps; without the option it is APAC; Force Dolby MAT's slot mapping; the failure kinds. With a TrueHD Atmos IEC file it also compares the whole render and prints APAC's signal-to-error and each route's start. | `ci.yml` |

None of these opens an audio device or establishes receiver behaviour.

## Network files through several connections (patch 0318)

A large http(s) file is read through several connections
(`stream/http_parallel.c`). The file is cut into chunks
(`--stream-http-chunk-size`, 2 MiB); a worker per connection
(`--stream-http-connections`, 4; 1 turns it off) fetches chunks with ranged
requests into connections + 3 chunk buffers (14 MiB at the defaults) that
the reader copies from as bytes arrive. Until the reader has read on 4 MiB
from one position (a file's headers, a start, a seek), it is served as by one
connection, on one open-ended reply that runs on across chunks; then the chunks ahead of it are fetched
too, one per connection, each a bounded request that the next one reuses the
connection for (FFmpeg 0080/0081). A seek stops fetches the reader left and
keeps their bytes. Failed fetches are retried with backoff after FFmpeg's own
reconnects; a server that refuses another connection while others work is
read through as many as work.

Only read-only http(s) (and dav/webdav) files of known size, at least 32 MiB
and seekable, never loopback hosts (an app's own byte server). HLS, live
streams and every other protocol read as before; the demuxer gets the same
bytes, so seeks land where they did.

Measured with mpv on a 24 Mbit/s Matroska file from a throttled LAN server,
1 -> 4 connections: each connection limited to 1.5 MB/s with 60 ms before
each reply, start 2.20 -> 1.44 s, cache after 10 s 0.1 -> 6 s,
cache-pauses in 30 s 7 -> 1, a resume at 50 s as fast as before; one 4 MB/s
line shared by all connections, start 1.14 -> 0.78 s, a resume as fast as
before; unthrottled, the same.
`scripts/test_http_parallel.sh` checks every byte through the stream layer's
walk, retries, a server's connection limit and seeks, and prints the speedup.

## Decoded PCM's rate (patch 0319)

`ao_audiounit` asks the session for the stream's own sample rate
(`setPreferredSampleRate`) before activating it, so 96 kHz and 192 kHz
TrueHD or DTS-HD MA decoded to PCM keep their rate where the route takes it.
When the route keeps another rate, the output opens at the route's rate, so
mpv's resampler converts once and `audio-out-params` says so, instead of the
output unit converting unseen. The log line `route: <asked> Hz asked, <out>
Hz out; <n> of <max> channels` says what the route gave.


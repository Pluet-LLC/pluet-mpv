@preconcurrency import AVFoundation
import Accelerate

// SpatialAudioBridge's real renderer, with the pinned sources and their
// Pluet patch, against a capturing output instead of AVSampleBufferAudioRenderer
// (see test_spatial_lossless.sh). Checks that:
//  - ao_spatial's lossless route (APACConfiguration.losslessPCM) enqueues the
//    decoder's rendered 7.1.4 unchanged, as 32-bit float linear PCM in Apple's
//    Atmos 7.1.4 layout, with exact timestamps, and reports progress like APAC;
//  - without the option it is APAC as before (with a TrueHD Atmos file, the
//    APAC route's signal-to-error against the same render is printed);
//  - Force Dolby MAT's PCMSlotDecoder puts each decoded channel in its 7.1.4
//    slot unchanged, whatever the byte split;
//  - ao_spatial's failure kinds: a renderer refusal of linear PCM is
//    MP_SPATIAL_OUTPUT_FAILED (3), a stream without objects stays
//    MP_SPATIAL_FAILED (2).
// Optional argument: an IEC 61937 TrueHD Atmos file (ffmpeg -f spdif), for the
// rendered comparison and the time each route takes to be ready to play.

final class CaptureOutput: SpatialAudioRendererOutput, @unchecked Sendable {
    var buffers: [CMSampleBuffer] = []
    var refuse = false
    var isReadyForMoreMediaData: Bool { true }
    var hasSufficientMediaDataForReliablePlaybackStart: Bool { !buffers.isEmpty }
    var status: AVQueuedSampleBufferRenderingStatus { refuse ? .failed : (buffers.isEmpty ? .unknown : .rendering) }
    var error: (any Error)? { refuse ? NSError(domain: AVFoundationErrorDomain, code: -11821) : nil }
    var isMuted = false
    var volume: Float = 1
    func enqueue(_ buffer: CMSampleBuffer) { buffers.append(buffer) }
    func flush() { buffers.removeAll() }
    func flush(fromSourceTime time: CMTime, completionHandler: @escaping @Sendable (Bool) -> Void) { completionHandler(false) }
}

nonisolated(unsafe) var failures = 0
func check(_ ok: Bool, _ what: String) {
    if ok { print("ok: \(what)") } else { failures += 1; print("FAIL: \(what)") }
}

func floats(_ buffer: CMSampleBuffer) -> [Float] {
    guard let block = CMSampleBufferGetDataBuffer(buffer) else { return [] }
    let length = CMBlockBufferGetDataLength(block)
    var out = [Float](repeating: 0, count: length / 4)
    out.withUnsafeMutableBytes { _ = CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: length, destination: $0.baseAddress!) }
    return out
}

/// A decoder that hands out fixed 12-channel blocks, as the Rust renderer does.
final class FixedDecoder: SpatialAudioRendererDecoding {
    let blocks: [PCMBlock]
    var used = false
    init(_ blocks: [PCMBlock]) { self.blocks = blocks }
    func decode(_ bytes: UnsafeRawBufferPointer, end: Bool) throws -> [PCMBlock] {
        defer { used = true }
        return used || end ? [] : blocks
    }
}

func render(_ blocks: [PCMBlock], lossless: Bool) async throws -> (buffers: [CMSampleBuffer], progress: SpatialAudioRendererProgress?) {
    var configuration = APACConfiguration()
    configuration.losslessPCM = lossless
    let output = CaptureOutput()
    let synchronizer = AVSampleBufferRenderSynchronizer()
    let renderer = try SpatialAudioRenderer(codec: .trueHDAtmos, configuration: configuration, synchronizer: synchronizer,
                                            output: output, makeDecoder: { FixedDecoder(blocks) })
    let start = CMTime(value: 7, timescale: 2)
    try await renderer.begin(at: start)
    let progress = await renderer.progress()
    try await renderer.enqueue(Data([0]), time: start)
    try await renderer.endOfStream()
    var last: SpatialAudioRendererProgress?
    for await value in progress {
        last = value
        if value.isFinal { break }
    }
    // The renderer flushes its output when released.
    return withExtendedLifetime(renderer) { (output.buffers, last) }
}

@main
struct Main {
    static func main() async throws {
        // 1. Linear PCM keeps the rendered feeds exactly.
        let frames = 4800 + 333
        var samples = [Float](repeating: 0, count: frames * 12)
        for i in samples.indices { samples[i] = sinf(Float(i) * 0.0137) * 0.9 + (i % 12 == 3 ? 0.05 : 0) }
        let blocks = [PCMBlock(samples: Array(samples[..<(1000 * 12)]), frames: 1000, sampleRate: 48_000, channels: 12),
                      PCMBlock(samples: Array(samples[(1000 * 12)...]), frames: frames - 1000, sampleRate: 48_000, channels: 12)]
        let pcm = try await render(blocks, lossless: true)
        let out = pcm.buffers.flatMap(floats)
        check(out == samples, "linear PCM output is bit-identical to the rendered 7.1.4 (\(out.count) of \(samples.count) samples)")
        let first = pcm.buffers.first.flatMap { CMSampleBufferGetFormatDescription($0) }
        let asbd = first.flatMap { CMAudioFormatDescriptionGetStreamBasicDescription($0)?.pointee }
        check(asbd?.mFormatID == kAudioFormatLinearPCM && asbd?.mChannelsPerFrame == 12 && asbd?.mSampleRate == 48_000
              && asbd?.mBitsPerChannel == 32 && (asbd!.mFormatFlags & kAudioFormatFlagIsFloat) != 0,
              "format is 48 kHz 12-channel float linear PCM")
        var layoutSize = 0
        let layout = first.flatMap { CMAudioFormatDescriptionGetChannelLayout($0, sizeOut: &layoutSize)?.pointee }
        check(layout?.mChannelLayoutTag == kAudioChannelLayoutTag_Atmos_7_1_4, "channel layout is Atmos 7.1.4")
        var expected = CMTime(value: 7, timescale: 2)
        var contiguous = true
        for buffer in pcm.buffers {
            contiguous = contiguous && CMSampleBufferGetPresentationTimeStamp(buffer) == expected
            expected = expected + CMTime(value: Int64(CMSampleBufferGetNumSamples(buffer)), timescale: 48_000)
        }
        check(contiguous, "timestamps start at the timeline and run without a gap (\(pcm.buffers.count) buffers)")
        check(pcm.progress?.isFinal == true && pcm.progress?.submittedEnd == CMTime(value: 7, timescale: 2) + CMTime(value: Int64(frames), timescale: 48_000),
              "progress reaches the timeline plus every frame")

        // 2. Without the option the same feeds are still encoded as APAC.
        let apac = try await render(blocks, lossless: false)
        let apacFormat = apac.buffers.first.flatMap(CMSampleBufferGetFormatDescription)
        check(apacFormat.map(CMFormatDescriptionGetMediaSubType) == kAudioFormatAPAC, "without losslessPCM the renderer enqueues APAC")

        // 3. Force Dolby MAT: 5.1 (side) and 5.1 (back) in their 7.1.4 slots, split anywhere.
        for (name, slots) in [("5.1(side)", [0, 1, 2, 3, 4, 5]), ("5.1(back) as side", [0, 1, 2, 3, 4, 5]),
                              ("7.1", [0, 1, 2, 3, 6, 7, 4, 5])] {
            let channels = slots.count
            let n = 777
            var input = [Float](repeating: 0, count: n * channels)
            for i in input.indices { input[i] = Float(i % 97) / 97 - 0.5 }
            let bytes = input.withUnsafeBytes { Data($0) }
            let decoder = PCMSlotDecoder(slots: slots)
            var decoded: [PCMBlock] = []
            var offset = 0
            for size in [1, 3, 5, 4096, 7, 100_000] where offset < bytes.count {
                let end = min(bytes.count, offset + size)
                decoded += try bytes[offset..<end].withUnsafeBytes { try decoder.decode($0, end: false) }
                offset = end
            }
            let flat = decoded.flatMap(\.samples)
            var ok = decoded.allSatisfy { $0.channels == 12 && $0.sampleRate == 48_000 } && flat.count == n * 12
            for f in 0..<n where ok {
                for s in 0..<12 {
                    let source = slots.firstIndex(of: s)
                    ok = ok && flat[f * 12 + s] == (source.map { input[f * channels + $0] } ?? 0)
                }
            }
            check(ok, "Force Dolby MAT \(name): every channel in its 7.1.4 slot, bit-identical, other slots silent")
        }

        // 4. Failure kinds.
        check(mpSpatialFailureKind(SpatialAudioError.unsupported("plain TrueHD"), lossless: true) == 2,
              "a stream without objects falls back to decoding (lossless route)")
        check(mpSpatialFailureKind(SpatialAudioError.unsupported("plain TrueHD"), lossless: false) == 2,
              "a stream without objects falls back to decoding (APAC route)")
        check(mpSpatialFailureKind(SpatialAudioError.encoding("The audio renderer could not play APAC"), lossless: true) == 3,
              "a renderer refusing linear PCM reopens with APAC")
        check(mpSpatialFailureKind(NSError(domain: AVFoundationErrorDomain, code: -11821), lossless: true) == 3,
              "an AVFoundation renderer error on linear PCM reopens with APAC")
        check(mpSpatialFailureKind(SpatialAudioError.encoding("x"), lossless: false) == 2, "an APAC failure decodes as before")
        check(mpSpatialFailureKind(SpatialAudioError.discontinuity("pause burst"), lossless: true) == 1, "a gap begins a new timeline")

        // 5. A real TrueHD Atmos stream, if given: identical render either way, readiness.
        let arguments = CommandLine.arguments
        if arguments.count > 1 {
            let bursts = try Data(contentsOf: URL(fileURLWithPath: arguments[1]))
            var unpacker = IEC61937Unpacker()
            let stream = try unpacker.unpack(bursts)
            // The renderer's own decode, straight.
            let decoder = try Decoder(codec: .trueHDAtmos, layout: .sevenOneFour)
            var reference: [Float] = []
            for start in stride(from: 0, to: stream.count, by: 65_536) {
                let end = min(stream.count, start + 65_536)
                for block in try stream[start..<end].withUnsafeBytes({ try decoder.decode($0, end: false) }) { reference += block.samples }
            }
            for block in try decoder.decode(UnsafeRawBufferPointer(start: nil, count: 0), end: true) { reference += block.samples }
            for lossless in [true, false] {
                var configuration = APACConfiguration()
                configuration.losslessPCM = lossless
                let output = CaptureOutput()
                let synchronizer = AVSampleBufferRenderSynchronizer()
                let renderer = try SpatialAudioRenderer(codec: .trueHDAtmos, configuration: configuration,
                                                        synchronizer: synchronizer, output: output)
                try await renderer.begin(at: .zero)
                // Readiness: how much audio goes in before the first buffer comes out.
                var fed = 0
                let chunk = 4_000
                var firstOut: Int?
                while fed < stream.count {
                    let end = min(stream.count, fed + chunk)
                    try await renderer.enqueue(stream.subdata(in: fed..<end), time: .zero)
                    fed = end
                    if firstOut == nil, !output.buffers.isEmpty { firstOut = fed }
                }
                try await renderer.endOfStream()
                let captured = withExtendedLifetime(renderer) { output.buffers }
                let inputSeconds = Double(firstOut ?? fed) / Double(stream.count) * Double(reference.count / 12) / 48_000
                if lossless {
                    let out = captured.flatMap(floats)
                    check(out == reference, "TrueHD Atmos: linear PCM output equals the renderer's 7.1.4 bit for bit (\(out.count / 12) frames)")
                    print(String(format: "info: linear PCM: first audio enqueued after %.2f s of input", inputSeconds))
                } else if let format = captured.first.flatMap(CMSampleBufferGetFormatDescription) {
                    let snr = try apacSNR(captured, format: format, reference: reference)
                    print(String(format: "info: APAC: first packet after %.2f s of input; 7.1.4 signal-to-error %.1f dB", inputSeconds, snr))
                }
            }
        }
        if failures > 0 { print("\(failures) failed"); exit(1) }
        print("spatial lossless: all passed")
    }

    /// Decodes APAC sample buffers back to PCM and compares them with the reference, aligned by the
    /// best lag within the encoder's priming.
    static func apacSNR(_ buffers: [CMSampleBuffer], format: CMAudioFormatDescription, reference: [Float]) throws -> Double {
        var source = CMAudioFormatDescriptionGetStreamBasicDescription(format)!.pointee
        var destination = AudioStreamBasicDescription(mSampleRate: 48_000, mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, mBytesPerPacket: 48, mFramesPerPacket: 1,
            mBytesPerFrame: 48, mChannelsPerFrame: 12, mBitsPerChannel: 32, mReserved: 0)
        var converter: AudioConverterRef?
        guard AudioConverterNew(&source, &destination, &converter) == noErr, let converter else { return .nan }
        defer { AudioConverterDispose(converter) }
        var cookieSize = 0
        if let cookie = CMAudioFormatDescriptionGetMagicCookie(format, sizeOut: &cookieSize) {
            AudioConverterSetProperty(converter, kAudioConverterDecompressionMagicCookie, UInt32(cookieSize), cookie)
        }
        var layoutSize = 0
        if let layout = CMAudioFormatDescriptionGetChannelLayout(format, sizeOut: &layoutSize) {
            AudioConverterSetProperty(converter, kAudioConverterInputChannelLayout, UInt32(layoutSize), layout)
            AudioConverterSetProperty(converter, kAudioConverterOutputChannelLayout, UInt32(layoutSize), layout)
        }
        var decoded: [Float] = []
        for buffer in buffers {
            guard let block = CMSampleBufferGetDataBuffer(buffer) else { continue }
            let length = CMBlockBufferGetDataLength(block)
            let packet = UnsafeMutableRawPointer.allocate(byteCount: length, alignment: 16)
            defer { packet.deallocate() }
            CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: length, destination: packet)
            var description = AudioStreamPacketDescription(mStartOffset: 0, mVariableFramesInPacket: 0, mDataByteSize: UInt32(length))
            final class Feed { var given = false; var data: UnsafeMutableRawPointer; var size: UInt32; var desc: UnsafeMutablePointer<AudioStreamPacketDescription>
                init(_ d: UnsafeMutableRawPointer, _ s: UInt32, _ p: UnsafeMutablePointer<AudioStreamPacketDescription>) { data = d; size = s; desc = p } }
            try withUnsafeMutablePointer(to: &description) { descriptionPointer in
                let feed = Feed(packet, UInt32(length), descriptionPointer)
                var out = [Float](repeating: 0, count: 1024 * 12)
                var count: UInt32 = 1024
                try out.withUnsafeMutableBytes { raw in
                    var list = AudioBufferList(mNumberBuffers: 1, mBuffers: AudioBuffer(mNumberChannels: 12,
                        mDataByteSize: UInt32(raw.count), mData: raw.baseAddress))
                    let status = AudioConverterFillComplexBuffer(converter, { _, packets, data, descriptions, context in
                        let feed = Unmanaged<Feed>.fromOpaque(context!).takeUnretainedValue()
                        if feed.given { packets.pointee = 0; return 0x6E65_6564 }
                        feed.given = true
                        packets.pointee = 1
                        data.pointee.mBuffers = AudioBuffer(mNumberChannels: 12, mDataByteSize: feed.size, mData: feed.data)
                        descriptions?.pointee = feed.desc
                        return noErr
                    }, Unmanaged.passUnretained(feed).toOpaque(), &count, &list, nil)
                    guard status == noErr || status == 0x6E65_6564 else { throw SpatialAudioError.coreMedia(status) }
                }
                decoded += out.prefix(Int(count) * 12)
            }
        }
        // Find the decoder's delay on a quarter second of the front left channel, then compare all of it.
        let window = 12_000, skip = 4_800
        var bestLag = 0, bestError = Double.infinity
        for lag in 0...4096 where (skip + lag + window) * 12 <= decoded.count && (skip + window) * 12 <= reference.count {
            var error = 0.0
            for f in skip..<(skip + window) {
                let d = Double(reference[f * 12]) - Double(decoded[(f + lag) * 12])
                error += d * d
            }
            if error < bestError { bestError = error; bestLag = lag }
        }
        let n = min(decoded.count / 12 - bestLag, reference.count / 12)
        var signal = 0.0, error = 0.0
        for i in 0..<(n * 12) {
            let r = Double(reference[i]), d = Double(decoded[i + bestLag * 12])
            signal += r * r; error += (r - d) * (r - d)
        }
        let best = 10 * log10(signal / max(error, 1e-30))
        return best
    }
}

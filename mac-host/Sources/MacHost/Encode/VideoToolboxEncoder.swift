import CoreVideo
import CoreMedia
import VideoToolbox

struct EncodedFrame {
    var annexB: Data
    var isKeyframe: Bool
    var hasParamSets: Bool
    var pts: CMTime
    var encodeDoneUs: UInt64
}

/// VideoToolbox H.264 hardware encoder (spec §11/§12): realtime, no frame
/// reordering, short GOP, hardware required. Output is annex-B per frame with
/// SPS/PPS inlined on keyframes, ready for packetization.
final class VideoToolboxEncoder {
    enum EncoderError: LocalizedError {
        case createFailed(OSStatus)
        var errorDescription: String? {
            if case .createFailed(let status) = self {
                return "VTCompressionSessionCreate failed (\(status)) — hardware H.264 encoder unavailable."
            }
            return "Encoder error"
        }
    }

    let width: Int
    let height: Int
    let fps: Int
    let bitrate: Int

    var onEncodedFrame: ((EncodedFrame) -> Void)?

    private var session: VTCompressionSession?
    private let lock = NSLock()
    private var pendingKeyframeRequest = false

    private(set) var encodedFrames = 0
    private(set) var keyframes = 0
    private(set) var bytesOut = 0

    private static let startCode = Data([0x00, 0x00, 0x00, 0x01])
    /// kCMSampleBufferAttachmentKey_NotSync (constant value per CoreMedia)
    private static let notSyncKey = "NotSync" as CFString

    private static let outputCallback: VTCompressionOutputCallback = { refcon, _, status, _, sampleBuffer in
        guard status == noErr, let refcon, let sampleBuffer, sampleBuffer.isValid else { return }
        let encoder = Unmanaged<VideoToolboxEncoder>.fromOpaque(refcon).takeUnretainedValue()
        encoder.handleOutput(sampleBuffer)
    }

    init(width: Int, height: Int, fps: Int, bitrate: Int, requireHardware: Bool = true) throws {
        self.width = width
        self.height = height
        self.fps = fps
        self.bitrate = bitrate

        var spec: [CFString: Any] = [
            kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: true
        ]
        if requireHardware {
            spec[kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder] = true
        }

        var newSession: VTCompressionSession?
        let status = VTCompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            width: Int32(width), height: Int32(height),
            codecType: kCMVideoCodecType_H264,
            encoderSpecification: spec as CFDictionary,
            imageBufferAttributes: nil,
            compressedDataAllocator: nil,
            outputCallback: Self.outputCallback,
            refcon: Unmanaged.passUnretained(self).toOpaque(),
            compressionSessionOut: &newSession)
        guard status == noErr, let s = newSession else { throw EncoderError.createFailed(status) }
        session = s

        // Low-latency configuration (spec §12)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_ProfileLevel,
                             value: kVTProfileLevel_H264_High_AutoLevel)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AverageBitRate,
                             value: NSNumber(value: bitrate))
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_ExpectedFrameRate,
                             value: NSNumber(value: fps))
        // GOP: 60 fps → keyframe every 1–2 s; 120 fps → 1 s (spec §12)
        let gopLength = fps <= 60 ? fps * 2 : fps
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_MaxKeyFrameInterval,
                             value: NSNumber(value: gopLength))
        VTCompressionSessionPrepareToEncodeFrames(s)
    }

    deinit {
        if let session {
            VTCompressionSessionInvalidate(session)
        }
    }

    /// Encode one frame. In-flight frames are bounded by the caller (see HP
    /// session queue policy, spec §10).
    func encode(_ pixelBuffer: CVPixelBuffer, pts: CMTime, forceKeyframe: Bool = false) {
        guard let session else { return }
        var frameProps: CFDictionary?
        if forceKeyframe {
            frameProps = [kVTEncodeFrameOptionKey_ForceKeyFrame: true] as CFDictionary
        }
        VTCompressionSessionEncodeFrame(
            session,
            imageBuffer: pixelBuffer,
            presentationTimeStamp: pts,
            duration: CMTime.invalid,
            frameProperties: frameProps,
            sourceFrameRefcon: nil,
            infoFlagsOut: nil)
    }

    /// Keyframe is applied on the next encode call (spec §12 triggers:
    /// packet-loss burst, desync, resolution change, recovery, resume).
    func requestKeyFrame() { shouldForceKeyframeOnNextEncode = true }

    var shouldForceKeyframeOnNextEncode: Bool {
        get {
            lock.lock(); defer { lock.unlock() }
            let v = pendingKeyframeRequest
            pendingKeyframeRequest = false
            return v
        }
        set { lock.lock(); pendingKeyframeRequest = newValue; lock.unlock() }
    }

    // MARK: Output handling — AVCC → annex-B

    private func handleOutput(_ sampleBuffer: CMSampleBuffer) {
        guard let format = CMSampleBufferGetFormatDescription(sampleBuffer) else { return }

        // 1) Slice NALs (AVCC length-prefixed) → annex-B
        var nalHeaderLength: Int32 = 4
        var paramSetCount: Int = 0
        _ = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            format, parameterSetIndex: 0, parameterSetPointerOut: nil,
            parameterSetSizeOut: nil, parameterSetCountOut: &paramSetCount,
            nalUnitHeaderLengthOut: &nalHeaderLength)

        var sliceNALs = Data()
        if let blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) {
            var totalLength: Int = 0
            var dataPointer: UnsafeMutablePointer<CChar>?
            let st = CMBlockBufferGetDataPointer(
                blockBuffer, atOffset: 0, lengthAtOffsetOut: nil,
                totalLengthOut: &totalLength, dataPointerOut: &dataPointer)
            if st == noErr, let dataPointer, totalLength > 0 {
                let nalLenBytes = Int(nalHeaderLength)
                var offset = 0
                while offset + nalLenBytes <= totalLength {
                    var nalLen = 0
                    for i in 0..<nalLenBytes {
                        nalLen = nalLen << 8 | Int(dataPointer[offset + i])
                    }
                    offset += nalLenBytes
                    guard nalLen > 0, offset + nalLen <= totalLength else { break }
                    sliceNALs.append(Self.startCode)
                    let nalStart = (UnsafeRawPointer(dataPointer)! + offset)
                        .assumingMemoryBound(to: UInt8.self)
                    sliceNALs.append(contentsOf: UnsafeBufferPointer(start: nalStart, count: nalLen))
                    offset += nalLen
                }
            }
        }

        // 2) Keyframe detection via NAL type scan: type 5 = IDR slice. The
        //    NotSync attachment is unreliable across encoder configurations.
        let isKeyframe = Self.isIDRFrame(sliceNALs)

        // 3) Keyframes carry SPS/PPS inline so any client can (re)start decoding.
        var annexB = Data()
        var hasParamSets = false
        if isKeyframe, paramSetCount > 0 {
            for i in 0..<paramSetCount {
                var ptr: UnsafePointer<UInt8>?
                var size: Int = 0
                guard CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    format,
                    parameterSetIndex: i,
                    parameterSetPointerOut: &ptr,
                    parameterSetSizeOut: &size,
                    parameterSetCountOut: nil,
                    nalUnitHeaderLengthOut: nil) == noErr,
                    let ptr, size > 0 else { continue }
                annexB.append(Self.startCode)
                annexB.append(contentsOf: UnsafeBufferPointer(start: ptr, count: size))
            }
            hasParamSets = annexB.count > 0
        }
        annexB.append(sliceNALs)

        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        lock.lock()
        encodedFrames += 1
        if isKeyframe { keyframes += 1 }
        bytesOut += annexB.count
        lock.unlock()

        onEncodedFrame?(EncodedFrame(
            annexB: annexB, isKeyframe: isKeyframe, hasParamSets: hasParamSets,
            pts: pts, encodeDoneUs: WireHeader.nowUs()))
    }

    /// A frame is a keyframe when it does not depend on earlier frames.
    static func dependsOnOthers(_ sampleBuffer: CMSampleBuffer) -> Bool {
        guard let attachments = CMSampleBufferGetSampleAttachmentsArray(
            sampleBuffer, createIfNecessary: false) else { return true }
        let array = attachments as NSArray
        guard let dict = array.firstObject as? NSDictionary else { return true }
        let notSync = dict[notSyncKey] as? Bool
        return notSync ?? true
    }

    /// Scans an annex-B buffer for an IDR slice (NAL type 5).
    static func isIDRFrame(_ annexB: Data) -> Bool {
        let b = [UInt8](annexB)
        var i = 0
        while i + 4 <= b.count {
            if b[i] == 0, b[i + 1] == 0, b[i + 2] == 0, b[i + 3] == 1 {
                if i + 4 < b.count, b[i + 4] & 0x1F == 5 { return true }
                i += 4
            } else {
                i += 1
            }
        }
        return false
    }
}

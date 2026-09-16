package com.streamtablet.video

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.os.Handler
import android.os.HandlerThread
import android.view.Choreographer
import android.media.MediaFormat
import android.util.Log
import android.view.Surface
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * Video decoder that supports multiple codecs (AV1, HEVC, H.264)
 */
class VideoDecoder(
    private val surface: Surface,
    private val width: Int,
    private val height: Int,
    private val codecType: CodecType,
    private val csd: ByteArray? = null,
    private val targetFps: Int = 60,
    private val framePacing: Boolean = true,
    private val displayRefreshHz: Float = 0f
) {
    companion object {
        private const val TAG = "VideoDecoder"
        private const val INPUT_TIMEOUT_US = 10000L   // 10ms for input
        private const val OUTPUT_TIMEOUT_US = 5000L   // 5ms for output (balance latency vs CPU)
        private const val OUTPUT_QUEUE_LIMIT = 2
    }

    enum class CodecType(val mimeType: String, val displayName: String) {
        AV1(MediaFormat.MIMETYPE_VIDEO_AV1, "AV1"),
        HEVC(MediaFormat.MIMETYPE_VIDEO_HEVC, "HEVC"),
        H264(MediaFormat.MIMETYPE_VIDEO_AVC, "H.264");

        companion object {
            fun fromId(id: Int): CodecType = when (id) {
                0 -> AV1
                1 -> HEVC
                2 -> H264
                else -> AV1  // Default to AV1
            }
        }
    }

    // Set from the launching Intent; see StreamActivity.
    var forceSoftware: Boolean = false

    private var codec: MediaCodec? = null
    @Volatile
    private var isRunning = false
    @Volatile
    private var codecFailed = false
    private val frameQueue = LinkedBlockingQueue<FrameData>(60)  // ~500ms at 120fps
    private var decoderThread: Thread? = null
    private var outputThread: Thread? = null

    // Statistics
    private var framesSubmitted = 0
    private var framesDropped = 0
    @Volatile
    private var framesDecoded = 0L
    private var lastStatsLog = System.currentTimeMillis()

    // Presentation cadence tracking
    private var lastRenderNs = 0L
    private var renderGapSumUs = 0L
    private var renderGapCount = 0
    private var renderGapMaxUs = 0L
    private var renderLateFrames = 0
    private var targetGapUs = 8333L   // refined from the measured stream rate

    // Wall-clock decode latency: queueInputBuffer -> matching dequeueOutputBuffer.
    // CPU percentage cannot see time spent blocked on the hardware decode block,
    // so this is the only way to compare codecs honestly. The encoder emits no
    // B-frames (max_b_frames=0), so output order matches input order and a FIFO
    // of submit timestamps pairs them exactly — the PTS cannot be used as a key
    // because it is millisecond-quantised and collides at 120fps.
    private val submitTimesNs = ConcurrentLinkedQueue<Long>()

    // --- vsync-aligned presentation ---
    // Releasing a decoded frame the instant it is ready means it lands on
    // whichever vsync happens to be next, so decode-time variance and the
    // source/panel clock drift both show up as judder. Instead the output loop
    // parks finished buffers here and a Choreographer callback releases one per
    // vsync with a presentation timestamp. Slip then lands on a frame boundary,
    // where it is invisible, instead of mid-motion.
    //
    // The queue is deliberately tiny: it exists to absorb a late frame, not to
    // buffer. Moonlight uses the same limit.
    private val outputBufferQueue = java.util.concurrent.LinkedBlockingQueue<Int>(OUTPUT_QUEUE_LIMIT)
    private var choreographerThread: HandlerThread? = null
    private var vsyncPeriodNs = 8_333_333L
    @Volatile
    private var pacingActive = false
    private var decLatSumUs = 0L
    private var decLatCount = 0
    private var decLatMaxUs = 0L
    private val decLatBuckets = IntArray(12)   // 0-2,2-4,...,20-22,22+ ms

    fun getFramesDecoded(): Long = framesDecoded

    data class FrameData(
        val data: ByteArray,
        val timestamp: Long,
        val isKeyframe: Boolean = false
    )

    fun start() {
        if (isRunning) return

        try {
            // Find decoder for this codec type
            val decoderName = findDecoder()
            if (decoderName == null) {
                Log.e(TAG, "No ${codecType.displayName} decoder found")
                throw RuntimeException("${codecType.displayName} decoder not available")
            }

            Log.i(TAG, "Using decoder: $decoderName for ${codecType.displayName}")

            val isSoftware = decoderName.startsWith("c2.android.") ||
                             decoderName.startsWith("OMX.google.")

            // Create format
            val format = MediaFormat.createVideoFormat(codecType.mimeType, width, height).apply {
                // KEY_LOW_LATENCY tells a decoder to emit each frame without
                // waiting for more input. On a software component that removes
                // the pipelining it relies on to use more than one core, and on
                // this device it stalls outright. KEY_PRIORITY=0 asks for
                // realtime scheduling, which is meaningful for a hardware
                // component and counterproductive for a threaded software one.
                if (!isSoftware) {
                    setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
                    setInteger(MediaFormat.KEY_PRIORITY, 0)
                } else {
                    Log.i(TAG, "Software decoder: omitting low-latency/realtime hints " +
                            "so it can pipeline across cores")
                }

                // Codec configuration record. MediaCodec submits csd-0 to the
                // decoder itself, so it must not also be queued as input.
                csd?.let {
                    setByteBuffer("csd-0", java.nio.ByteBuffer.wrap(it))
                    Log.i(TAG, "Supplied csd-0: ${it.size} bytes")
                }
            }

            // Create and configure codec
            val newCodec = MediaCodec.createByCodecName(decoderName)
            newCodec.configure(format, surface, null, 0)
            newCodec.start()
            codec = newCodec

            isRunning = true
            codecFailed = false

            // Start decoder threads
            decoderThread = Thread({ decoderLoop() }, "${codecType.displayName}DecoderInput").apply { start() }
            outputThread = Thread({ outputLoop() }, "${codecType.displayName}DecoderOutput").apply { start() }

            if (framePacing) startChoreographer()

            Log.i(TAG, "${codecType.displayName} decoder started: ${width}x${height}")

        } catch (e: Exception) {
            Log.e(TAG, "Failed to start ${codecType.displayName} decoder", e)
            throw e
        }
    }

    private fun findDecoder(): String? {
        val codecList = MediaCodecList(MediaCodecList.ALL_CODECS)
        var hwLowLatency: String? = null
        var hw: String? = null
        var softwareDecoder: String? = null

        Log.i(TAG, "Searching for ${codecType.displayName} decoders...")

        for (codecInfo in codecList.codecInfos) {
            if (codecInfo.isEncoder) continue
            for (type in codecInfo.supportedTypes) {
                if (!type.equals(codecType.mimeType, ignoreCase = true)) continue

                val isHw = codecInfo.isHardwareAccelerated

                // Does this decoder actually advertise low-latency operation?
                // Setting KEY_LOW_LATENCY on a decoder that does not is a no-op.
                // MediaTek ships the capability as separate components
                // (c2.mtk.av1.decoder.lowlatency et al) rather than as a mode on
                // the standard one, so the only way to get it is to pick that
                // component by name.
                val caps = try { codecInfo.getCapabilitiesForType(type) } catch (e: Exception) { null }

                val lowLatency = try {
                    android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R &&
                        caps?.isFeatureSupported(
                            MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency) == true
                } catch (e: Exception) {
                    false
                }

                // Decoders advertise the resolutions and rates they can handle.
                // Selecting one that cannot does not fail cleanly — MediaCodec
                // accepts a handful of frames and then wedges, producing no
                // output and no error. Checking up front is the only way to see
                // it. (c2.android.av1-dav1d caps at 2048x2048 and 245760
                // blocks/sec; a 2960x1848@120 stream is ~10x that.)
                val capable = try {
                    caps?.videoCapabilities?.areSizeAndRateSupported(
                        width, height, targetFps.toDouble()) ?: true
                } catch (e: Exception) {
                    false
                }

                Log.i(TAG, "Found ${codecType.displayName} decoder: ${codecInfo.name} " +
                        "(hw=$isHw, lowLatency=$lowLatency, capable=$capable)")

                if (!capable) {
                    Log.w(TAG, "  skipping ${codecInfo.name}: cannot do ${width}x${height}@${targetFps}")
                    continue
                }

                if (isHw && lowLatency && hwLowLatency == null) hwLowLatency = codecInfo.name
                else if (isHw && hw == null) hw = codecInfo.name
                else if (!isHw && softwareDecoder == null) softwareDecoder = codecInfo.name
            }
        }

        // Escape hatch for testing the software path. dav1d has predictable
        // timing where MediaTek's AV1 block shows size-independent latency
        // spikes; the tablet has cores to spare.
        if (System.getenv("STREAM_TABLET_FORCE_SW") != null || forceSoftware) {
            softwareDecoder?.let {
                Log.w(TAG, "Forcing software decoder: $it")
                return it
            }
        }

        hwLowLatency?.let {
            Log.i(TAG, "Selected hardware low-latency decoder: $it")
            return it
        }
        hw?.let {
            Log.w(TAG, "No low-latency ${codecType.displayName} decoder; using $it " +
                    "(expect higher and less consistent decode latency)")
            return it
        }
        if (softwareDecoder != null) {
            Log.w(TAG, "No hardware ${codecType.displayName} decoder found, using software: $softwareDecoder")
        }
        return softwareDecoder
    }

    private var receivedFirstKeyframe = false
    private var submitFrameCount = 0
    private var keyframeRequestCallback: (() -> Unit)? = null

    fun setKeyframeRequestCallback(callback: () -> Unit) {
        keyframeRequestCallback = callback
    }

    fun submitFrame(data: ByteArray, timestamp: Long, isKeyframe: Boolean = false) {
        if (!isRunning || codecFailed) return

        submitFrameCount++
        framesSubmitted++

        // Wait for first keyframe before queueing
        if (!receivedFirstKeyframe) {
            if (isKeyframe) {
                receivedFirstKeyframe = true
                Log.i(TAG, "First keyframe received, size=${data.size}")
            } else {
                // Request keyframe after waiting too long
                if (submitFrameCount % 30 == 0) {
                    keyframeRequestCallback?.invoke()
                }
                return
            }
        }

        // Drop frames if queue is full to maintain low latency
        if (frameQueue.remainingCapacity() == 0) {
            frameQueue.poll()
            framesDropped++
        }

        frameQueue.offer(FrameData(data, timestamp, isKeyframe))

        // Log stats every 5 seconds
        val now = System.currentTimeMillis()
        if (now - lastStatsLog >= 5000) {
            val decoded = framesDecoded
            Log.i(TAG, "Decoder stats (${codecType.displayName}): submitted=$framesSubmitted, dropped=$framesDropped, totalDecoded=$decoded, queue=${frameQueue.size}")
            framesSubmitted = 0
            framesDropped = 0
            lastStatsLog = now
        }
    }

    private fun decoderLoop() {
        while (isRunning && !codecFailed) {
            try {
                val frame = frameQueue.poll(100, TimeUnit.MILLISECONDS) ?: continue
                val codec = this.codec ?: break

                val inputIndex = codec.dequeueInputBuffer(INPUT_TIMEOUT_US)
                if (inputIndex >= 0) {
                    val inputBuffer = codec.getInputBuffer(inputIndex)
                    inputBuffer?.clear()
                    inputBuffer?.put(frame.data)

                    // BUFFER_FLAG_KEY_FRAME is an *output* flag — MediaCodec sets
                    // it on buffers it returns to say "this was a sync frame". On
                    // queueInputBuffer the only meaningful flags are
                    // CODEC_CONFIG, END_OF_STREAM and PARTIAL_FRAME. Hardware
                    // components ignore the stray bit; stricter ones need not.
                    val flags = 0

                    // Timestamp must be recorded before the frame is handed over,
                    // otherwise a fast decode can complete before we enqueue it.
                    submitTimesNs.add(System.nanoTime())
                    codec.queueInputBuffer(
                        inputIndex,
                        0,
                        frame.data.size,
                        frame.timestamp,
                        flags
                    )
                } else {
                    Log.w(TAG, "No input buffer available")
                }
            } catch (e: IllegalStateException) {
                Log.e(TAG, "Decoder input error (codec released?)", e)
                codecFailed = true
                break
            } catch (e: Exception) {
                Log.e(TAG, "Decoder input error", e)
            }
        }
    }

    private fun outputLoop() {
        val bufferInfo = MediaCodec.BufferInfo()

        while (isRunning && !codecFailed) {
            val codec = this.codec ?: break

            try {
                val outputIndex = codec.dequeueOutputBuffer(bufferInfo, OUTPUT_TIMEOUT_US)
                when {
                    outputIndex >= 0 -> {
                        // Decode latency: pair this output with its input. Always
                        // measured — it is independent of when the frame is shown.
                        val submitNs = submitTimesNs.poll()
                        if (submitNs != null) {
                            val latUs = (System.nanoTime() - submitNs) / 1000
                            decLatSumUs += latUs
                            decLatCount++
                            if (latUs > decLatMaxUs) decLatMaxUs = latUs
                            val b = (latUs / 2000L).toInt().coerceIn(0, decLatBuckets.size - 1)
                            decLatBuckets[b]++
                        }
                        if (decLatCount >= 300) {
                            val hist = StringBuilder()
                            for (i in decLatBuckets.indices) {
                                if (decLatBuckets[i] == 0) continue
                                val lo = i * 2
                                val lbl = if (i == decLatBuckets.size - 1) "" + lo + "+" else "" + lo + "-" + (lo + 2)
                                hist.append(" ").append(lbl).append("ms:").append(decLatBuckets[i])
                            }
                            Log.i(TAG, "DECODE LATENCY (" + codecType.displayName + "): avg=" +
                                    String.format("%.2f", decLatSumUs / decLatCount / 1000.0) + "ms max=" +
                                    String.format("%.2f", decLatMaxUs / 1000.0) + "ms n=" + decLatCount + " |" + hist)
                            decLatSumUs = 0; decLatCount = 0; decLatMaxUs = 0
                            java.util.Arrays.fill(decLatBuckets, 0)
                        }

                        framesDecoded++

                        if (pacingActive) {
                            // Hand off to the Choreographer callback, which owns
                            // both the release and the cadence measurement. If the
                            // queue is full we are ahead of the display, so discard
                            // the oldest without rendering rather than stall the
                            // codec by holding its buffers.
                            if (!outputBufferQueue.offer(outputIndex)) {
                                outputBufferQueue.poll()?.let {
                                    try { codec.releaseOutputBuffer(it, false) } catch (e: Exception) {}
                                }
                                outputBufferQueue.offer(outputIndex)
                            }
                        } else {
                            codec.releaseOutputBuffer(outputIndex, true)
                            recordRender()
                        }
                    }
                    outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        val newFormat = codec.outputFormat
                        Log.i(TAG, "Output format changed: $newFormat")
                        Log.i(TAG, "Width: ${newFormat.getInteger(MediaFormat.KEY_WIDTH)}, Height: ${newFormat.getInteger(MediaFormat.KEY_HEIGHT)}")
                    }
                    outputIndex == MediaCodec.INFO_TRY_AGAIN_LATER -> {
                        // No output available yet
                    }
                }
            } catch (e: IllegalStateException) {
                Log.e(TAG, "Decoder output error (codec released?)", e)
                codecFailed = true
                break
            } catch (e: Exception) {
                Log.e(TAG, "Decoder error", e)
            }
        }
    }

    private fun startChoreographer() {
        // Use the real panel refresh, not the stream rate — they are not the
        // same number and the presentation timestamp has to be in the display's
        // terms.
        val hz = if (displayRefreshHz > 1f) displayRefreshHz else 120f
        vsyncPeriodNs = (1_000_000_000.0 / hz).toLong()

        val t = HandlerThread("${codecType.displayName}Choreographer")
        t.start()
        choreographerThread = t
        Handler(t.looper).post {
            pacingActive = true
            Log.i(TAG, "Frame pacing on: panel " + String.format("%.3f", hz) +
                    "Hz, vsync " + (vsyncPeriodNs / 1000) + "us")
            Choreographer.getInstance().postFrameCallback(object : Choreographer.FrameCallback {
                override fun doFrame(frameTimeNanos: Long) {
                    if (!isRunning || codecFailed) return

                    // Release exactly one buffer per vsync, timestamped for the
                    // NEXT vsync. Android asks for the buffer to be handed over
                    // ahead of its intended display time — passing the current
                    // vsync means "show this now", which is already late and
                    // lands the frame on the following refresh anyway, but
                    // without the compositor knowing that was the intent.
                    val idx = outputBufferQueue.poll()
                    if (idx != null) {
                        try {
                            codec?.releaseOutputBuffer(idx, frameTimeNanos + vsyncPeriodNs)
                            recordRender()
                        } catch (e: IllegalStateException) {
                            codecFailed = true
                            return
                        }
                    }
                    Choreographer.getInstance().postFrameCallback(this)
                }
            })
        }
    }

    private fun recordRender() {
        val nowNs = System.nanoTime()
        if (lastRenderNs != 0L) {
            val gapUs = (nowNs - lastRenderNs) / 1000
            renderGapSumUs += gapUs
            renderGapCount++
            if (gapUs > renderGapMaxUs) renderGapMaxUs = gapUs
            if (gapUs > targetGapUs * 3 / 2) renderLateFrames++
        }
        lastRenderNs = nowNs
        if (renderGapCount >= 300) {
            Log.i(TAG, "Render cadence (" + (if (pacingActive) "paced" else "immediate") + "): avg=" +
                    String.format("%.2f", renderGapSumUs / renderGapCount / 1000.0) + "ms max=" +
                    String.format("%.2f", renderGapMaxUs / 1000.0) + "ms late=" +
                    renderLateFrames + "/" + renderGapCount)
            renderGapSumUs = 0; renderGapCount = 0
            renderGapMaxUs = 0; renderLateFrames = 0
        }
    }

    fun stop() {
        isRunning = false
        pacingActive = false
        choreographerThread?.quitSafely()
        choreographerThread = null
        outputBufferQueue.clear()

        decoderThread?.interrupt()
        outputThread?.interrupt()

        try {
            decoderThread?.join(1000)
            outputThread?.join(1000)
        } catch (e: InterruptedException) {
            // Ignore
        }

        codec?.stop()
        codec?.release()
        codec = null

        frameQueue.clear()
        submitTimesNs.clear()
        Log.i(TAG, "${codecType.displayName} decoder stopped")
    }
}

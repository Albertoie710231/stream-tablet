package com.streamtablet.video

import android.media.MediaCodec
import android.media.MediaCodecList
import android.media.MediaFormat
import android.util.Log
import android.view.Surface
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * Video decoder that supports multiple codecs (AV1, HEVC, H.264)
 */
class VideoDecoder(
    private val surface: Surface,
    private val width: Int,
    private val height: Int,
    private val codecType: CodecType
) {
    companion object {
        private const val TAG = "VideoDecoder"
        private const val INPUT_TIMEOUT_US = 10000L   // 10ms for input
        private const val OUTPUT_TIMEOUT_US = 5000L   // 5ms for output (balance latency vs CPU)
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

            // Create format
            val format = MediaFormat.createVideoFormat(codecType.mimeType, width, height).apply {
                // Low latency mode
                setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
                setInteger(MediaFormat.KEY_PRIORITY, 0)  // Realtime priority
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

            Log.i(TAG, "${codecType.displayName} decoder started: ${width}x${height}")

        } catch (e: Exception) {
            Log.e(TAG, "Failed to start ${codecType.displayName} decoder", e)
            throw e
        }
    }

    private fun findDecoder(): String? {
        val codecList = MediaCodecList(MediaCodecList.ALL_CODECS)
        var softwareDecoder: String? = null

        Log.i(TAG, "Searching for ${codecType.displayName} decoders...")

        for (codecInfo in codecList.codecInfos) {
            if (codecInfo.isEncoder) continue
            for (type in codecInfo.supportedTypes) {
                if (type.equals(codecType.mimeType, ignoreCase = true)) {
                    val isHw = codecInfo.isHardwareAccelerated
                    val isSw = codecInfo.isSoftwareOnly
                    Log.i(TAG, "Found ${codecType.displayName} decoder: ${codecInfo.name} (hw=$isHw, sw=$isSw)")

                    // Prefer hardware decoder
                    if (isHw) {
                        Log.i(TAG, "Selected hardware decoder: ${codecInfo.name}")
                        return codecInfo.name
                    }
                    // Remember software decoder as fallback
                    if (softwareDecoder == null) {
                        softwareDecoder = codecInfo.name
                    }
                }
            }
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

                    var flags = 0
                    if (frame.isKeyframe) {
                        flags = flags or MediaCodec.BUFFER_FLAG_KEY_FRAME
                    }

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
                        // Release to surface for rendering
                        codec.releaseOutputBuffer(outputIndex, true)
                        framesDecoded++
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

    fun stop() {
        isRunning = false

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
        Log.i(TAG, "${codecType.displayName} decoder stopped")
    }
}

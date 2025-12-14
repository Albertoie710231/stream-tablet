package com.streamtablet.video

import android.media.MediaCodec
import android.media.MediaCodecList
import android.media.MediaFormat
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

class AV1Decoder(
    private val surface: Surface,
    private val width: Int,
    private val height: Int
) {
    companion object {
        private const val TAG = "AV1Decoder"
        private const val MIME_TYPE = MediaFormat.MIMETYPE_VIDEO_AV1
        private const val TIMEOUT_US = 10000L
    }

    private var codec: MediaCodec? = null
    @Volatile
    private var isRunning = false
    @Volatile
    private var codecFailed = false
    private val frameQueue = LinkedBlockingQueue<FrameData>(30)
    private var decoderThread: Thread? = null
    private var outputThread: Thread? = null

    data class FrameData(
        val data: ByteArray,
        val timestamp: Long,
        val isKeyframe: Boolean = false
    )

    fun start() {
        if (isRunning) return

        try {
            // Find AV1 decoder
            val decoderName = findAV1Decoder()
            if (decoderName == null) {
                Log.e(TAG, "No AV1 decoder found, falling back to H.264")
                // Could implement H.264 fallback here
                throw RuntimeException("AV1 decoder not available")
            }

            Log.i(TAG, "Using decoder: $decoderName")

            // Create format
            val format = MediaFormat.createVideoFormat(MIME_TYPE, width, height).apply {
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

            // Start decoder thread
            decoderThread = Thread({ decoderLoop() }, "AV1DecoderInput").apply { start() }
            outputThread = Thread({ outputLoop() }, "AV1DecoderOutput").apply { start() }

            Log.i(TAG, "Decoder started: ${width}x${height}")

        } catch (e: Exception) {
            Log.e(TAG, "Failed to start decoder", e)
            throw e
        }
    }

    private fun findAV1Decoder(): String? {
        val codecList = MediaCodecList(MediaCodecList.ALL_CODECS)
        for (codecInfo in codecList.codecInfos) {
            if (codecInfo.isEncoder) continue
            for (type in codecInfo.supportedTypes) {
                if (type.equals(MIME_TYPE, ignoreCase = true)) {
                    // Prefer hardware decoder
                    if (codecInfo.isHardwareAccelerated) {
                        return codecInfo.name
                    }
                }
            }
        }
        // Fall back to any decoder
        for (codecInfo in codecList.codecInfos) {
            if (codecInfo.isEncoder) continue
            for (type in codecInfo.supportedTypes) {
                if (type.equals(MIME_TYPE, ignoreCase = true)) {
                    return codecInfo.name
                }
            }
        }
        return null
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
            Log.w(TAG, "Frame dropped - queue full")
        }

        frameQueue.offer(FrameData(data, timestamp, isKeyframe))
    }

    private var inputFrameCount = 0

    private fun decoderLoop() {
        while (isRunning && !codecFailed) {
            try {
                val frame = frameQueue.poll(100, TimeUnit.MILLISECONDS) ?: continue
                val codec = this.codec ?: break

                val inputIndex = codec.dequeueInputBuffer(TIMEOUT_US)
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

                    inputFrameCount++
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

    private var outputFrameCount = 0

    private fun outputLoop() {
        val bufferInfo = MediaCodec.BufferInfo()

        while (isRunning && !codecFailed) {
            val codec = this.codec ?: break

            try {
                val outputIndex = codec.dequeueOutputBuffer(bufferInfo, TIMEOUT_US)
                when {
                    outputIndex >= 0 -> {
                        // Release to surface for rendering
                        codec.releaseOutputBuffer(outputIndex, true)
                        outputFrameCount++
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
        Log.i(TAG, "Decoder stopped")
    }
}

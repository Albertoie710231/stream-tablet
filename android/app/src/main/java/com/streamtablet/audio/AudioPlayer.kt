package com.streamtablet.audio

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaFormat
import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder

class AudioPlayer(
    private val sampleRate: Int = 48000,
    private val channels: Int = 2
) {
    companion object {
        private const val TAG = "AudioPlayer"
        private const val MIME_TYPE = MediaFormat.MIMETYPE_AUDIO_OPUS
        private const val INPUT_TIMEOUT_US = 10000L  // 10ms
        private const val OUTPUT_TIMEOUT_US = 0L     // Non-blocking
    }

    private var decoder: MediaCodec? = null
    private var audioTrack: AudioTrack? = null
    private val lock = Object()

    @Volatile
    private var isRunning = false

    fun start(): Boolean {
        if (isRunning) {
            Log.w(TAG, "AudioPlayer already running")
            return true
        }

        try {
            // Create Opus decoder
            val format = MediaFormat.createAudioFormat(MIME_TYPE, sampleRate, channels)

            // Create Opus identification header for MediaCodec
            val opusHeader = createOpusHeader(channels, sampleRate)
            format.setByteBuffer("csd-0", ByteBuffer.wrap(opusHeader))

            // CSD-1: Pre-skip in nanoseconds (312 samples at 48kHz = 6500000 ns)
            val preSkipNs = (312L * 1_000_000_000L) / sampleRate
            val csd1 = ByteBuffer.allocate(8).order(ByteOrder.nativeOrder())
            csd1.putLong(preSkipNs)
            csd1.flip()
            format.setByteBuffer("csd-1", csd1)

            // CSD-2: Seek pre-roll in nanoseconds (80ms = 80000000 ns)
            val preRollNs = 80_000_000L
            val csd2 = ByteBuffer.allocate(8).order(ByteOrder.nativeOrder())
            csd2.putLong(preRollNs)
            csd2.flip()
            format.setByteBuffer("csd-2", csd2)

            Log.i(TAG, "Creating Opus decoder with format: $format")

            val codec = MediaCodec.createDecoderByType(MIME_TYPE)
            Log.i(TAG, "MediaCodec created: ${codec.name}")

            codec.configure(format, null, null, 0)
            Log.i(TAG, "MediaCodec configured")

            codec.start()
            Log.i(TAG, "MediaCodec started")

            decoder = codec

            // Create AudioTrack with low latency
            val channelConfig = if (channels == 2)
                AudioFormat.CHANNEL_OUT_STEREO else AudioFormat.CHANNEL_OUT_MONO

            val minBuffer = AudioTrack.getMinBufferSize(
                sampleRate, channelConfig, AudioFormat.ENCODING_PCM_16BIT
            )

            val attributes = AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                .build()

            val audioFormat = AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(sampleRate)
                .setChannelMask(channelConfig)
                .build()

            audioTrack = AudioTrack.Builder()
                .setAudioAttributes(attributes)
                .setAudioFormat(audioFormat)
                .setBufferSizeInBytes(minBuffer * 2)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
                .build()

            audioTrack?.play()
            isRunning = true

            Log.i(TAG, "Audio player started: ${sampleRate}Hz, $channels channels, buffer=${minBuffer * 2}")
            return true
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start audio player", e)
            stop()
            return false
        }
    }

    private fun createOpusHeader(channels: Int, sampleRate: Int): ByteArray {
        // Minimal Opus identification header (19 bytes)
        // See https://wiki.xiph.org/OggOpus#ID_Header
        return byteArrayOf(
            'O'.code.toByte(), 'p'.code.toByte(), 'u'.code.toByte(), 's'.code.toByte(),
            'H'.code.toByte(), 'e'.code.toByte(), 'a'.code.toByte(), 'd'.code.toByte(),
            1,  // Version
            channels.toByte(),  // Channel count
            0, 0,  // Pre-skip (little endian)
            (sampleRate and 0xFF).toByte(),
            ((sampleRate shr 8) and 0xFF).toByte(),
            ((sampleRate shr 16) and 0xFF).toByte(),
            ((sampleRate shr 24) and 0xFF).toByte(),
            0, 0,  // Output gain (little endian)
            0      // Channel mapping family (0 = mono/stereo)
        )
    }

    // Debug counters
    private var inputQueueCount = 0
    private var inputFailCount = 0
    private var outputFrameCount = 0
    private var pcmBytesWritten = 0L
    private var lastLogTime = System.currentTimeMillis()

    fun decodeAndPlay(opusData: ByteArray, timestamp: Long) {
        if (!isRunning) {
            return
        }

        synchronized(lock) {
            if (!isRunning) {
                return
            }

            val codec = decoder
            val track = audioTrack

            if (codec == null || track == null) {
                return
            }

            try {
                // Queue input
                val inputIndex = codec.dequeueInputBuffer(INPUT_TIMEOUT_US)
                if (inputIndex >= 0) {
                    val inputBuffer = codec.getInputBuffer(inputIndex)
                    inputBuffer?.clear()
                    inputBuffer?.put(opusData)
                    codec.queueInputBuffer(inputIndex, 0, opusData.size, timestamp, 0)
                    inputQueueCount++
                } else {
                    inputFailCount++
                }

                // Drain all available output
                val bufferInfo = MediaCodec.BufferInfo()
                var outputIndex = codec.dequeueOutputBuffer(bufferInfo, OUTPUT_TIMEOUT_US)
                while (outputIndex != MediaCodec.INFO_TRY_AGAIN_LATER) {
                    when {
                        outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                            val format = codec.outputFormat
                            Log.i(TAG, "Output format changed: $format")
                        }
                        outputIndex >= 0 -> {
                            val outputBuffer = codec.getOutputBuffer(outputIndex)
                            if (outputBuffer != null && bufferInfo.size > 0) {
                                val pcmData = ByteArray(bufferInfo.size)
                                outputBuffer.get(pcmData)
                                track.write(pcmData, 0, pcmData.size)
                                outputFrameCount++
                                pcmBytesWritten += pcmData.size
                            }
                            codec.releaseOutputBuffer(outputIndex, false)
                        }
                    }
                    outputIndex = codec.dequeueOutputBuffer(bufferInfo, OUTPUT_TIMEOUT_US)
                }

                // Log every 5 seconds
                val now = System.currentTimeMillis()
                if (now - lastLogTime >= 5000) {
                    Log.i(TAG, "Audio decode: queued=$inputQueueCount, failed=$inputFailCount, decoded=$outputFrameCount, pcm=${pcmBytesWritten}B, trackState=${track.playState}")
                    inputQueueCount = 0
                    inputFailCount = 0
                    outputFrameCount = 0
                    pcmBytesWritten = 0
                    lastLogTime = now
                }
            } catch (e: IllegalStateException) {
                // Codec is in bad state, stop trying
                Log.e(TAG, "Codec in bad state, stopping", e)
                isRunning = false
            } catch (e: Exception) {
                Log.e(TAG, "Decode/play error", e)
            }
            Unit
        }
    }

    fun stop() {
        isRunning = false

        synchronized(lock) {
            try {
                audioTrack?.stop()
            } catch (e: Exception) {
                Log.w(TAG, "Error stopping AudioTrack", e)
            }
            audioTrack?.release()
            audioTrack = null

            try {
                decoder?.stop()
            } catch (e: Exception) {
                Log.w(TAG, "Error stopping decoder", e)
            }
            decoder?.release()
            decoder = null
        }

        Log.i(TAG, "Audio player stopped")
    }

    fun isPlaying(): Boolean = isRunning
}

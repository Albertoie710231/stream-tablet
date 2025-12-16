package com.streamtablet.audio

import android.os.Process
import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit

class AudioReceiver(private val port: Int) {
    companion object {
        private const val TAG = "AudioReceiver"
        private const val AUDIO_MAGIC: Short = 0x5341  // "SA"
        private const val HEADER_SIZE = 12
        private const val PACKET_QUEUE_SIZE = 100
        private const val RECEIVE_BUFFER_SIZE = 256 * 1024  // 256KB
    }

    data class AudioPacket(
        val data: ByteArray,
        val timestamp: Long,
        val sequence: Int
    ) {
        override fun equals(other: Any?): Boolean {
            if (this === other) return true
            if (javaClass != other?.javaClass) return false
            other as AudioPacket
            return data.contentEquals(other.data) && timestamp == other.timestamp && sequence == other.sequence
        }

        override fun hashCode(): Int {
            var result = data.contentHashCode()
            result = 31 * result + timestamp.hashCode()
            result = 31 * result + sequence
            return result
        }
    }

    private var socket: DatagramSocket? = null
    private var receiverThread: Thread? = null
    private val packetQueue = ArrayBlockingQueue<AudioPacket>(PACKET_QUEUE_SIZE)

    @Volatile
    private var isReceiving = false

    fun start() {
        if (isReceiving) {
            Log.w(TAG, "AudioReceiver already running")
            return
        }

        try {
            socket = DatagramSocket(port).apply {
                receiveBufferSize = RECEIVE_BUFFER_SIZE
                soTimeout = 1000  // 1 second timeout for clean shutdown
            }

            isReceiving = true
            receiverThread = Thread({
                Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
                receiveLoop()
            }, "AudioReceiver").apply { start() }

            Log.i(TAG, "Audio receiver started on port $port")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start audio receiver", e)
            stop()
        }
    }

    private fun receiveLoop() {
        val buffer = ByteArray(1500)  // MTU size
        val packet = DatagramPacket(buffer, buffer.size)

        var packetsReceived = 0
        var packetsParsed = 0
        var packetsDropped = 0
        var bytesReceived = 0L
        var lastLogTime = System.currentTimeMillis()

        while (isReceiving) {
            try {
                socket?.receive(packet)
                packetsReceived++
                bytesReceived += packet.length

                parseAudioPacket(packet.data, packet.length)?.let { audio ->
                    packetsParsed++
                    if (!packetQueue.offer(audio)) {
                        // Queue full - drop oldest packet
                        packetQueue.poll()
                        packetQueue.offer(audio)
                        packetsDropped++
                    }
                }

                // Log every 5 seconds
                val now = System.currentTimeMillis()
                if (now - lastLogTime >= 5000) {
                    Log.i(TAG, "UDP receive: recv=$packetsReceived, parsed=$packetsParsed, dropped=$packetsDropped, bytes=$bytesReceived, queue=${packetQueue.size}")
                    packetsReceived = 0
                    packetsParsed = 0
                    packetsDropped = 0
                    bytesReceived = 0
                    lastLogTime = now
                }
            } catch (e: java.net.SocketTimeoutException) {
                // Expected during shutdown, continue
                // Also log timeout to see if packets are arriving
                val now = System.currentTimeMillis()
                if (now - lastLogTime >= 5000) {
                    Log.w(TAG, "UDP timeout - no packets received in last 5s, total recv=$packetsReceived")
                    lastLogTime = now
                }
            } catch (e: Exception) {
                if (isReceiving) {
                    Log.e(TAG, "Receive error", e)
                }
            }
        }

        Log.i(TAG, "Audio receive loop ended")
    }

    private fun parseAudioPacket(data: ByteArray, length: Int): AudioPacket? {
        if (length < HEADER_SIZE) {
            return null
        }

        val buffer = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)

        // Parse header
        val magic = buffer.short
        if (magic != AUDIO_MAGIC) {
            return null
        }

        val sequence = buffer.short.toInt() and 0xFFFF
        val timestamp = buffer.int.toLong() and 0xFFFFFFFFL
        val payloadLen = buffer.short.toInt() and 0xFFFF
        buffer.short  // reserved

        if (payloadLen <= 0 || HEADER_SIZE + payloadLen > length) {
            return null
        }

        // Extract payload
        val payload = ByteArray(payloadLen)
        System.arraycopy(data, HEADER_SIZE, payload, 0, payloadLen)

        return AudioPacket(payload, timestamp, sequence)
    }

    fun getNextPacket(timeoutMs: Long): AudioPacket? {
        return try {
            packetQueue.poll(timeoutMs, TimeUnit.MILLISECONDS)
        } catch (e: InterruptedException) {
            null
        }
    }

    fun getQueueSize(): Int = packetQueue.size

    fun clearQueue() {
        packetQueue.clear()
    }

    fun stop() {
        isReceiving = false

        receiverThread?.let {
            it.interrupt()
            try {
                it.join(2000)
            } catch (e: InterruptedException) {
                Log.w(TAG, "Interrupted while waiting for receiver thread")
            }
        }
        receiverThread = null

        socket?.close()
        socket = null

        packetQueue.clear()
        Log.i(TAG, "Audio receiver stopped")
    }
}

package com.streamtablet.network

import android.os.Process
import android.util.Log
import java.io.DataInputStream
import java.io.DataOutputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.LinkedBlockingQueue

class ConnectionManager {
    companion object {
        private const val TAG = "ConnectionManager"
        private const val VIDEO_MAGIC: Short = 0x5354  // "ST"
        private const val PACKET_QUEUE_SIZE = 500  // Buffer ~500 packets
    }

    data class ServerConfig(
        val width: Int,
        val height: Int,
        val videoPort: Int,
        val inputPort: Int,
        val audioPort: Int = 0,
        val audioSampleRate: Int = 48000,
        val audioChannels: Int = 2,
        val audioFrameMs: Int = 10,
        val codecType: Int = 0  // 0=AV1, 1=HEVC, 2=H264
    ) {
        val audioEnabled: Boolean get() = audioPort != 0
    }

    data class VideoFrame(
        val data: ByteArray,
        val timestamp: Long,
        val isKeyframe: Boolean
    )

    private var controlSocket: Socket? = null
    private var controlIn: DataInputStream? = null
    private var controlOut: DataOutputStream? = null

    private var videoSocket: DatagramSocket? = null
    private var inputSocket: Socket? = null
    private var inputOut: DataOutputStream? = null

    private var serverAddress: String = ""
    private var serverPort: Int = 9500
    private var serverConfig: ServerConfig? = null

    private val inputQueue = LinkedBlockingQueue<InputEvent>(100)
    private var inputThread: Thread? = null

    // Raw packet queue - receives packets as fast as possible
    private data class RawPacket(val data: ByteArray, val length: Int)
    private val packetQueue = ArrayBlockingQueue<RawPacket>(PACKET_QUEUE_SIZE)
    private var receiverThread: Thread? = null
    @Volatile
    private var isReceiving = false
    private var droppedPackets = 0
    private var lastDropLog: Long = 0

    // Frame reassembly
    private var currentFrameNumber: Int = -1
    private var frameFragments = mutableMapOf<Int, ByteArray>()
    private var expectedFragments = 0
    private var frameIsKeyframe = false
    private var frameStartTime: Long = 0
    private var waitingForKeyframe = false
    private var lastKeyframeRequest: Long = 0

    // Statistics
    private var packetsReceived = 0
    private var framesCompleted = 0
    private var framesIncomplete = 0
    private var keyframeRequests = 0
    private var lastStatsLog: Long = 0

    // Callback for keyframe requests
    var onKeyframeNeeded: (() -> Unit)? = null

    // Callback for disconnection (server closed connection)
    var onDisconnected: (() -> Unit)? = null

    // Control socket monitoring
    private var controlMonitorThread: Thread? = null
    @Volatile
    private var isMonitoring = false

    fun connect(address: String, port: Int) {
        serverAddress = address
        serverPort = port

        Log.i(TAG, "Connecting to $address:$port")

        // Create video socket FIRST so we know our local port
        videoSocket = DatagramSocket()
        videoSocket?.soTimeout = 1000
        // Increase receive buffer for burst packet arrival (4MB to match server send buffer)
        videoSocket?.receiveBufferSize = 4 * 1024 * 1024
        val actualBuffer = videoSocket?.receiveBufferSize ?: 0
        Log.i(TAG, "Video socket created on local port ${videoSocket?.localPort}, requested 4MB buffer, actual=${actualBuffer/1024}KB")

        // Connect control channel
        controlSocket = Socket(address, port)
        controlIn = DataInputStream(controlSocket!!.getInputStream())
        controlOut = DataOutputStream(controlSocket!!.getOutputStream())

        // Send config request (now includes our video port)
        sendConfigRequest()

        // Receive server config
        val config = receiveConfig()
        serverConfig = config

        Log.i(TAG, "Server config: ${config.width}x${config.height}, video=${config.videoPort}, input=${config.inputPort}")

        // Send initial packet to server to punch through NAT
        val serverAddr = InetAddress.getByName(address)
        val initPacket = DatagramPacket(ByteArray(1), 1, serverAddr, config.videoPort)
        videoSocket?.send(initPacket)

        // Start high-priority receiver thread to pull packets from kernel ASAP
        startReceiverThread()

        // Start control socket monitor to detect server disconnection
        startControlMonitorThread()

        Log.i(TAG, "Connected successfully")
    }

    private fun startReceiverThread() {
        isReceiving = true
        receiverThread = Thread({
            // Set high priority to minimize packet loss
            Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)

            val socket = videoSocket ?: return@Thread
            val buffer = ByteArray(1500)
            val packet = DatagramPacket(buffer, buffer.size)

            Log.i(TAG, "Receiver thread started with high priority")

            while (isReceiving) {
                try {
                    socket.receive(packet)

                    // Copy data to queue (we reuse the buffer)
                    val dataCopy = ByteArray(packet.length)
                    System.arraycopy(packet.data, 0, dataCopy, 0, packet.length)

                    // Non-blocking offer - if queue is full, drop oldest packet
                    if (!packetQueue.offer(RawPacket(dataCopy, packet.length))) {
                        packetQueue.poll()  // Remove oldest
                        packetQueue.offer(RawPacket(dataCopy, packet.length))
                        droppedPackets++

                        // Log drops periodically
                        val now = System.currentTimeMillis()
                        if (now - lastDropLog > 1000) {
                            Log.w(TAG, "Packet queue full - dropped $droppedPackets packets")
                            lastDropLog = now
                            droppedPackets = 0
                        }
                    }
                } catch (e: java.net.SocketTimeoutException) {
                    // Normal timeout, continue
                } catch (e: Exception) {
                    if (isReceiving) {
                        Log.e(TAG, "Receiver thread error", e)
                    }
                    break
                }
            }

            Log.i(TAG, "Receiver thread stopped")
        }, "UDPReceiver").apply {
            start()
        }
    }

    private fun startControlMonitorThread() {
        isMonitoring = true
        controlMonitorThread = Thread({
            Log.i(TAG, "Control monitor thread started")

            val input = controlIn ?: return@Thread

            while (isMonitoring) {
                try {
                    // Try to read from control socket - this will detect server disconnect
                    // Set a read timeout so we can check isMonitoring periodically
                    controlSocket?.soTimeout = 1000

                    val lengthHigh = input.read()
                    if (lengthHigh == -1) {
                        // Server closed connection
                        Log.i(TAG, "Server closed connection (EOF)")
                        if (isMonitoring) {
                            isMonitoring = false
                            onDisconnected?.invoke()
                        }
                        break
                    }

                    val lengthLow = input.read()
                    if (lengthLow == -1) {
                        Log.i(TAG, "Server closed connection (EOF on length)")
                        if (isMonitoring) {
                            isMonitoring = false
                            onDisconnected?.invoke()
                        }
                        break
                    }

                    val length = (lengthHigh shl 8) or lengthLow
                    if (length > 0) {
                        val type = input.read()
                        if (type == -1) {
                            Log.i(TAG, "Server closed connection (EOF on type)")
                            if (isMonitoring) {
                                isMonitoring = false
                                onDisconnected?.invoke()
                            }
                            break
                        }

                        // Handle disconnect message from server
                        if (type == 0x08) {  // MSG_DISCONNECT
                            Log.i(TAG, "Received disconnect message from server")
                            if (isMonitoring) {
                                isMonitoring = false
                                onDisconnected?.invoke()
                            }
                            break
                        }

                        // Skip remaining data for other message types
                        if (length > 1) {
                            val remaining = ByteArray(length - 1)
                            input.readFully(remaining)
                        }
                    }
                } catch (e: java.net.SocketTimeoutException) {
                    // Normal timeout, continue monitoring
                } catch (e: java.io.EOFException) {
                    Log.i(TAG, "Server closed connection (EOFException)")
                    if (isMonitoring) {
                        isMonitoring = false
                        onDisconnected?.invoke()
                    }
                    break
                } catch (e: java.net.SocketException) {
                    // Socket closed, likely due to disconnect() call
                    if (isMonitoring) {
                        Log.i(TAG, "Control socket closed: ${e.message}")
                        isMonitoring = false
                        onDisconnected?.invoke()
                    }
                    break
                } catch (e: Exception) {
                    if (isMonitoring) {
                        Log.e(TAG, "Control monitor error", e)
                        isMonitoring = false
                        onDisconnected?.invoke()
                    }
                    break
                }
            }

            Log.i(TAG, "Control monitor thread stopped")
        }, "ControlMonitor").apply {
            start()
        }
    }

    fun connectInput() {
        val config = serverConfig ?: return

        try {
            inputSocket = Socket(serverAddress, config.inputPort)
            inputOut = DataOutputStream(inputSocket!!.getOutputStream())

            // Start input sending thread
            inputThread = Thread({ inputSendLoop() }, "InputSender").apply { start() }

            Log.i(TAG, "Input channel connected")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to connect input channel", e)
        }
    }

    private fun sendConfigRequest() {
        // Config request: [length:2][type:1][width:2][height:2][videoPort:2][inputPort:2]
        val buffer = ByteBuffer.allocate(11).order(ByteOrder.BIG_ENDIAN)
        buffer.putShort(9)  // length
        buffer.put(0x03)    // MSG_CONFIG_REQUEST

        // Request our tablet resolution (landscape)
        buffer.putShort(2800)  // width (landscape)
        buffer.putShort(1752)  // height (landscape)
        buffer.putShort(videoSocket?.localPort?.toShort() ?: 0)  // our video port
        buffer.putShort(0)     // input port (server will tell us)

        controlOut?.write(buffer.array())
        controlOut?.flush()
    }

    private fun receiveConfig(): ServerConfig {
        // Read message header
        val lengthHigh = controlIn!!.readByte().toInt() and 0xFF
        val lengthLow = controlIn!!.readByte().toInt() and 0xFF
        val length = (lengthHigh shl 8) or lengthLow

        val type = controlIn!!.readByte()
        if (type != 0x04.toByte()) {  // MSG_CONFIG_RESPONSE
            throw RuntimeException("Unexpected message type: $type")
        }

        val data = ByteArray(length - 1)
        controlIn!!.readFully(data)

        val buffer = ByteBuffer.wrap(data).order(ByteOrder.BIG_ENDIAN)
        val width = buffer.short.toInt() and 0xFFFF
        val height = buffer.short.toInt() and 0xFFFF
        val videoPort = buffer.short.toInt() and 0xFFFF
        val inputPort = buffer.short.toInt() and 0xFFFF

        // Parse extended audio config if present (14 bytes total)
        var audioPort = 0
        var audioSampleRate = 48000
        var audioChannels = 2
        var audioFrameMs = 10
        var codecType = 0  // Default to AV1

        if (data.size >= 14) {
            audioPort = buffer.short.toInt() and 0xFFFF
            audioSampleRate = buffer.short.toInt() and 0xFFFF
            audioChannels = buffer.get().toInt() and 0xFF
            audioFrameMs = buffer.get().toInt() and 0xFF
            Log.i(TAG, "Audio config: port=$audioPort, ${audioSampleRate}Hz, ${audioChannels}ch, ${audioFrameMs}ms")
        }

        // Parse codec type if present (15 bytes total)
        if (data.size >= 15) {
            codecType = buffer.get().toInt() and 0xFF
            val codecNames = arrayOf("AV1", "HEVC", "H.264")
            val codecName = if (codecType < codecNames.size) codecNames[codecType] else "unknown"
            Log.i(TAG, "Codec: $codecName (type=$codecType)")
        }

        return ServerConfig(width, height, videoPort, inputPort, audioPort, audioSampleRate, audioChannels, audioFrameMs, codecType)
    }

    fun getConfig(): ServerConfig {
        return serverConfig ?: throw RuntimeException("Not connected")
    }

    private var packetCount = 0

    fun receiveVideoFrame(): VideoFrame? {
        if (!isReceiving) return null

        try {
            // Get packet from queue (blocking with timeout)
            val rawPacket = packetQueue.poll(100, java.util.concurrent.TimeUnit.MILLISECONDS)
                ?: return null

            packetCount++

            return parseVideoPacket(rawPacket.data, rawPacket.length)
        } catch (e: InterruptedException) {
            return null
        } catch (e: Exception) {
            Log.e(TAG, "Error processing packet", e)
            return null
        }
    }

    private fun parseVideoPacket(data: ByteArray, length: Int): VideoFrame? {
        if (length < 16) return null  // 16-byte header

        packetsReceived++
        val buffer = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)

        val magic = buffer.short
        if (magic != VIDEO_MAGIC) {
            Log.w(TAG, "Invalid magic: $magic")
            return null
        }

        @Suppress("UNUSED_VARIABLE")
        val sequence = buffer.short.toInt() and 0xFFFF
        val frameNumber = buffer.short.toInt() and 0xFFFF
        val flags = buffer.get().toInt() and 0xFF
        buffer.get()  // reserved
        val fragmentIdx = buffer.short.toInt() and 0xFFFF  // Now 16-bit
        val fragmentCount = buffer.short.toInt() and 0xFFFF  // Now 16-bit, total count
        val payloadLen = buffer.short.toInt() and 0xFFFF
        buffer.short  // reserved2

        val isKeyframe = (flags and 0x01) != 0
        // isStart and isEnd reserved for future use (packet loss detection)
        @Suppress("UNUSED_VARIABLE")
        val isStart = (flags and 0x02) != 0
        @Suppress("UNUSED_VARIABLE")
        val isEnd = (flags and 0x04) != 0

        // Extract payload (starts after 16-byte header)
        val payload = ByteArray(payloadLen)
        System.arraycopy(data, 16, payload, 0, payloadLen)

        // Handle frame reassembly
        if (frameNumber != currentFrameNumber) {
            // Check if previous frame was incomplete (packet loss detected)
            if (currentFrameNumber >= 0 && frameFragments.size > 0 && frameFragments.size < expectedFragments) {
                framesIncomplete++
                // Stream is now corrupted until next keyframe
                if (!waitingForKeyframe) {
                    waitingForKeyframe = true
                    requestKeyframeThrottled()
                }
            }

            // New frame, reset
            currentFrameNumber = frameNumber
            frameFragments.clear()
            expectedFragments = fragmentCount
            frameIsKeyframe = isKeyframe
            frameStartTime = System.currentTimeMillis()

            // If this is a keyframe, we can stop waiting
            if (isKeyframe) {
                waitingForKeyframe = false
            }
        }

        // If waiting for keyframe and this isn't one, skip this frame's packets
        if (waitingForKeyframe && !frameIsKeyframe) {
            return null
        }

        frameFragments[fragmentIdx] = payload

        // Check if frame is complete
        if (frameFragments.size == expectedFragments) {
            // Verify all fragments are present (0 to expectedFragments-1)
            for (i in 0 until expectedFragments) {
                if (!frameFragments.containsKey(i)) {
                    Log.w(TAG, "Frame $frameNumber missing fragment $i")
                    frameFragments.clear()
                    if (!waitingForKeyframe) {
                        waitingForKeyframe = true
                        requestKeyframeThrottled()
                    }
                    return null
                }
            }

            // Reassemble frame
            val totalSize = frameFragments.values.sumOf { it.size }
            val frameData = ByteArray(totalSize)
            var offset = 0
            for (i in 0 until expectedFragments) {
                val fragment = frameFragments[i]!!
                System.arraycopy(fragment, 0, frameData, offset, fragment.size)
                offset += fragment.size
            }

            frameFragments.clear()
            framesCompleted++

            // Log stats every 5 seconds
            val now = System.currentTimeMillis()
            if (now - lastStatsLog >= 5000) {
                Log.i(TAG, "Network stats: packets=$packetsReceived, frames=$framesCompleted, incomplete=$framesIncomplete, keyframeReqs=$keyframeRequests")
                packetsReceived = 0
                framesCompleted = 0
                framesIncomplete = 0
                keyframeRequests = 0
                lastStatsLog = now
            }

            return VideoFrame(frameData, System.currentTimeMillis() * 1000, frameIsKeyframe)
        }

        return null
    }

    private fun requestKeyframeThrottled() {
        val now = System.currentTimeMillis()
        // Don't request more than once per 500ms to avoid cascade effect
        // (keyframes are large and can cause more packet loss)
        if (now - lastKeyframeRequest > 500) {
            lastKeyframeRequest = now
            keyframeRequests++
            onKeyframeNeeded?.invoke() ?: requestKeyframe()
        }
    }

    fun sendInput(event: InputEvent) {
        inputQueue.offer(event)
    }

    private fun inputSendLoop() {
        val buffer = ByteBuffer.allocate(28).order(ByteOrder.LITTLE_ENDIAN)

        while (inputSocket?.isConnected == true) {
            try {
                val event = inputQueue.poll(100, java.util.concurrent.TimeUnit.MILLISECONDS) ?: continue

                buffer.clear()
                buffer.put(event.type.value)
                buffer.put(event.pointerId)
                buffer.putFloat(event.x)
                buffer.putFloat(event.y)
                buffer.putFloat(event.pressure)
                buffer.putFloat(event.tiltX)
                buffer.putFloat(event.tiltY)
                buffer.putShort(event.buttons)
                buffer.putInt(event.timestamp)

                inputOut?.write(buffer.array())
                inputOut?.flush()
            } catch (e: Exception) {
                Log.e(TAG, "Error sending input", e)
                break
            }
        }
    }

    fun requestKeyframe() {
        try {
            // Send keyframe request
            val buffer = ByteBuffer.allocate(3).order(ByteOrder.BIG_ENDIAN)
            buffer.putShort(1)   // length
            buffer.put(0x05)     // MSG_KEYFRAME_REQUEST

            controlOut?.write(buffer.array())
            controlOut?.flush()
        } catch (e: Exception) {
            Log.e(TAG, "Error requesting keyframe", e)
        }
    }

    fun disconnect() {
        Log.i(TAG, "Disconnecting")

        // Stop monitoring first to prevent disconnect callback
        isMonitoring = false

        // Stop receiver thread
        isReceiving = false
        receiverThread?.interrupt()

        // Stop control monitor thread
        controlMonitorThread?.interrupt()

        inputThread?.interrupt()

        try {
            // Send disconnect message
            val buffer = ByteBuffer.allocate(3).order(ByteOrder.BIG_ENDIAN)
            buffer.putShort(1)
            buffer.put(0x08)  // MSG_DISCONNECT
            controlOut?.write(buffer.array())
            controlOut?.flush()
        } catch (e: Exception) {
            // Ignore
        }

        inputOut?.close()
        inputSocket?.close()
        videoSocket?.close()
        controlIn?.close()
        controlOut?.close()
        controlSocket?.close()

        // Wait for threads to finish
        try {
            receiverThread?.join(500)
            controlMonitorThread?.join(500)
            inputThread?.join(500)
        } catch (e: InterruptedException) {
            // Ignore
        }

        inputSocket = null
        videoSocket = null
        controlSocket = null
        serverConfig = null
        receiverThread = null
        controlMonitorThread = null
        inputThread = null

        // Reset frame reassembly state
        currentFrameNumber = -1
        frameFragments.clear()
        expectedFragments = 0
        waitingForKeyframe = false
        lastKeyframeRequest = 0
        packetQueue.clear()
    }
}

enum class InputEventType(val value: Byte) {
    TOUCH_DOWN(0x01),
    TOUCH_MOVE(0x02),
    TOUCH_UP(0x03),
    STYLUS_DOWN(0x04),
    STYLUS_MOVE(0x05),
    STYLUS_UP(0x06),
    STYLUS_HOVER(0x07),
    KEY_DOWN(0x08),
    KEY_UP(0x09),
    SCROLL(0x0A)
}

data class InputEvent(
    val type: InputEventType,
    val pointerId: Byte,
    val x: Float,
    val y: Float,
    val pressure: Float,
    val tiltX: Float,
    val tiltY: Float,
    val buttons: Short,
    val timestamp: Int
)

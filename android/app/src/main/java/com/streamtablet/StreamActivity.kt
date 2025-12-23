package com.streamtablet

import android.os.Bundle
import android.text.Editable
import android.text.TextWatcher
import android.view.Gravity
import android.view.KeyEvent
import android.view.View
import android.view.WindowManager
import android.view.inputmethod.InputMethodManager
import android.widget.FrameLayout
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.lifecycleScope
import com.streamtablet.audio.AudioPlayer
import com.streamtablet.audio.AudioReceiver
import com.streamtablet.calibration.CalibrationManager
import com.streamtablet.databinding.ActivityStreamBinding
import com.streamtablet.input.InputHandler
import com.streamtablet.input.FourFingerGestureDetector
import com.streamtablet.network.ConnectionManager
import com.streamtablet.video.VideoDecoder
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.yield

class StreamActivity : AppCompatActivity() {

    companion object {
        const val EXTRA_SERVER_ADDRESS = "server_address"
        const val EXTRA_PORT = "port"
        const val EXTRA_MAINTAIN_ASPECT_RATIO = "maintain_aspect_ratio"
    }

    private lateinit var binding: ActivityStreamBinding
    private lateinit var connectionManager: ConnectionManager
    private lateinit var calibrationManager: CalibrationManager
    private var decoder: VideoDecoder? = null
    private lateinit var inputHandler: InputHandler

    // Audio components
    private var audioReceiver: AudioReceiver? = null
    private var audioPlayer: AudioPlayer? = null
    private var audioJob: Job? = null

    private var serverAddress: String = ""
    private var port: Int = 9500
    private var maintainAspectRatio: Boolean = true

    @Volatile
    private var isConnected = false
    private var surfaceReady = false
    private var pendingSurface: android.view.SurfaceHolder? = null

    // FPS tracking
    private var fpsJob: Job? = null
    @Volatile
    private var framesReceived = 0
    @Volatile
    private var lastFramesDecoded = 0L

    // Keyboard and gesture support
    private lateinit var gestureDetector: FourFingerGestureDetector
    private var isKeyboardVisible = false
    private var currentKeyboardHeight = 0
    private var serverWidth = 0
    private var serverHeight = 0
    private var isProcessingTextChange = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Keep screen on
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        binding = ActivityStreamBinding.inflate(layoutInflater)
        setContentView(binding.root)

        // Hide system UI for fullscreen
        hideSystemUI()

        // Get connection parameters
        serverAddress = intent.getStringExtra(EXTRA_SERVER_ADDRESS) ?: ""
        port = intent.getIntExtra(EXTRA_PORT, 9500)
        maintainAspectRatio = intent.getBooleanExtra(EXTRA_MAINTAIN_ASPECT_RATIO, true)

        // Initialize components
        connectionManager = ConnectionManager()
        calibrationManager = CalibrationManager(this)
        inputHandler = InputHandler(connectionManager, calibrationManager)

        // Initialize gesture detector for keyboard toggle
        gestureDetector = FourFingerGestureDetector { toggleKeyboard() }

        // Set up keyboard insets listener to track keyboard visibility and resize video
        ViewCompat.setOnApplyWindowInsetsListener(binding.root) { _, insets ->
            val imeHeight = insets.getInsets(WindowInsetsCompat.Type.ime()).bottom
            isKeyboardVisible = imeHeight > 0
            resizeVideoForKeyboard(imeHeight)
            insets
        }

        // Set up keyboard input capture using TextWatcher (works with soft keyboard)
        binding.keyboardInput.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {
                // Only process if new characters were added (count > 0) and not during our clear
                if (isProcessingTextChange || count == 0) return
                s?.let {
                    // Send only the newly added characters
                    for (i in start until start + count) {
                        if (i < it.length) {
                            inputHandler.sendCharacter(it[i])
                        }
                    }
                }
            }
            override fun afterTextChanged(s: Editable?) {
                if (isProcessingTextChange) return
                s?.let {
                    if (it.isNotEmpty()) {
                        isProcessingTextChange = true
                        it.clear()
                        isProcessingTextChange = false
                    }
                }
            }
        })

        // Handle soft keyboard Enter/Done action
        binding.keyboardInput.setOnEditorActionListener { _, actionId, _ ->
            // Send Enter key for any editor action (Done, Go, Send, etc.)
            inputHandler.sendKeyEvent(KeyEvent.KEYCODE_ENTER, true)
            inputHandler.sendKeyEvent(KeyEvent.KEYCODE_ENTER, false)
            true
        }

        // Also handle hardware keyboard events (backspace, enter, etc.)
        binding.keyboardInput.setOnKeyListener { _, keyCode, event ->
            if (keyCode == KeyEvent.KEYCODE_DEL || keyCode == KeyEvent.KEYCODE_ENTER) {
                when (event.action) {
                    KeyEvent.ACTION_DOWN -> inputHandler.sendKeyEvent(keyCode, true)
                    KeyEvent.ACTION_UP -> inputHandler.sendKeyEvent(keyCode, false)
                }
                true
            } else {
                false  // Let TextWatcher handle regular characters
            }
        }

        // Set disconnect callback to exit stream screen when host disconnects
        connectionManager.onDisconnected = {
            android.util.Log.i("StreamActivity", "Host disconnected, returning to main screen")
            runOnUiThread {
                finish()
            }
        }

        // Set up touch and hover handling for stylus support
        // Check for 4-finger gesture first, then pass to input handler
        binding.videoSurface.setOnTouchListener { view, event ->
            val consumed = gestureDetector.onTouchEvent(event)

            // If gesture was triggered, cancel any touches that leaked through
            if (gestureDetector.wasTriggered()) {
                inputHandler.cancelAllTouches()
                gestureDetector.clearTriggered()
            }

            if (!consumed) {
                inputHandler.onTouch(view, event)
            }
            true
        }
        binding.videoSurface.setOnHoverListener { view, event ->
            inputHandler.onHover(view, event)
            true  // Always consume hover events
        }
        binding.videoSurface.setOnGenericMotionListener { view, event ->
            inputHandler.onGenericMotion(view, event)
        }

        // Request focus to receive hover events
        binding.videoSurface.requestFocus()

        // Register surface callback BEFORE starting connection
        binding.videoSurface.holder.addCallback(object : android.view.SurfaceHolder.Callback {
            override fun surfaceCreated(holder: android.view.SurfaceHolder) {
                surfaceReady = true
                pendingSurface = holder

                // If already connected, start decoder now
                if (isConnected) {
                    startDecoder(holder)
                }
            }

            override fun surfaceChanged(holder: android.view.SurfaceHolder, format: Int, width: Int, height: Int) {}

            override fun surfaceDestroyed(holder: android.view.SurfaceHolder) {
                surfaceReady = false
                pendingSurface = null
                decoder?.stop()
            }
        })

        // Start connection
        lifecycleScope.launch {
            connect()
        }
    }

    private fun hideSystemUI() {
        // Use modern WindowInsetsController API
        WindowCompat.setDecorFitsSystemWindows(window, false)
        val controller = WindowInsetsControllerCompat(window, binding.root)
        controller.hide(WindowInsetsCompat.Type.systemBars())
        controller.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
    }

    private suspend fun connect() {
        try {
            withContext(Dispatchers.IO) {
                // Connect to server
                connectionManager.connect(serverAddress, port)
            }

            isConnected = true

            // If surface is already ready, start decoder now
            val holder = pendingSurface
            if (surfaceReady && holder != null) {
                startDecoder(holder)
            }

        } catch (e: Exception) {
            android.util.Log.e("StreamActivity", "Connection failed", e)
            runOnUiThread {
                binding.loadingIndicator.visibility = View.GONE
                binding.errorText.visibility = View.VISIBLE
                binding.errorText.text = "Connection failed: ${e.message}"
            }
        }
    }

    private fun startDecoder(holder: android.view.SurfaceHolder) {
        try {
            val config = connectionManager.getConfig()

            // Store server dimensions for keyboard resize calculations
            serverWidth = config.width
            serverHeight = config.height

            // Adjust surface size for aspect ratio if needed
            if (maintainAspectRatio) {
                adjustSurfaceAspectRatio(config.width, config.height)
            }

            // Select decoder based on codec type from server
            val codecType = VideoDecoder.CodecType.fromId(config.codecType)
            android.util.Log.i("StreamActivity", "Creating ${codecType.displayName} decoder")

            val newDecoder = VideoDecoder(holder.surface, config.width, config.height, codecType)
            newDecoder.setKeyframeRequestCallback {
                connectionManager.requestKeyframe()
            }
            decoder = newDecoder
            newDecoder.start()

            // Request a keyframe to start fresh
            connectionManager.requestKeyframe()

            // Start receiving video
            lifecycleScope.launch(Dispatchers.IO) {
                receiveVideo()
            }

            // Connect input channel
            lifecycleScope.launch(Dispatchers.IO) {
                connectionManager.connectInput()
            }

            // Start audio if enabled
            if (config.audioEnabled) {
                startAudio(config)
            }

            // Hide loading indicator and start FPS counter
            runOnUiThread {
                binding.loadingIndicator.visibility = View.GONE
            }

            // Start FPS overlay update
            startFpsOverlay()

        } catch (e: Exception) {
            android.util.Log.e("StreamActivity", "Error starting decoder", e)
        }
    }

    private fun startFpsOverlay() {
        fpsJob = lifecycleScope.launch {
            var lastReceivedCount = 0
            var lastDecodedCount = 0L

            while (true) {
                delay(1000)  // Update every second

                val currentReceived = framesReceived
                val currentDecoded = decoder?.getFramesDecoded() ?: 0L

                val receivedFps = currentReceived - lastReceivedCount
                val decodedFps = (currentDecoded - lastDecodedCount).toInt()

                lastReceivedCount = currentReceived
                lastDecodedCount = currentDecoded

                runOnUiThread {
                    binding.fpsOverlay.text = "RX: ${receivedFps} fps\nDEC: ${decodedFps} fps"
                }
            }
        }
    }

    private fun startAudio(config: ConnectionManager.ServerConfig) {
        try {
            // Create and start audio player
            val player = AudioPlayer(config.audioSampleRate, config.audioChannels)
            if (!player.start()) {
                android.util.Log.e("StreamActivity", "Failed to start audio player")
                return
            }
            audioPlayer = player

            // Create and start audio receiver
            val receiver = AudioReceiver(config.audioPort)
            receiver.start()
            audioReceiver = receiver

            // Start audio receive loop
            audioJob = lifecycleScope.launch(Dispatchers.IO) {
                receiveAudio()
            }

            android.util.Log.i("StreamActivity", "Audio started: ${config.audioSampleRate}Hz, ${config.audioChannels}ch, port=${config.audioPort}")
        } catch (e: Exception) {
            android.util.Log.e("StreamActivity", "Error starting audio", e)
            stopAudio()
        }
    }

    private fun stopAudio() {
        audioJob?.cancel()
        audioJob = null
        audioReceiver?.stop()
        audioReceiver = null
        audioPlayer?.stop()
        audioPlayer = null
    }

    private suspend fun receiveAudio() {
        val receiver = audioReceiver ?: return
        val player = audioPlayer ?: return

        var packetCount = 0
        var lastLogTime = System.currentTimeMillis()

        while (player.isPlaying()) {
            try {
                val packet = receiver.getNextPacket(100)
                if (packet != null) {
                    player.decodeAndPlay(packet.data, packet.timestamp)
                    packetCount++

                    // Log every 5 seconds
                    val now = System.currentTimeMillis()
                    if (now - lastLogTime >= 5000) {
                        android.util.Log.i("StreamActivity", "Audio: received $packetCount packets, queue=${receiver.getQueueSize()}")
                        packetCount = 0
                        lastLogTime = now
                    }
                } else {
                    // Yield to prevent busy-waiting when no packet available
                    yield()
                }
            } catch (e: Exception) {
                android.util.Log.e("StreamActivity", "Audio receive error", e)
                break
            }
        }
    }

    private fun toggleKeyboard() {
        val imm = getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager
        if (isKeyboardVisible) {
            // Hide keyboard
            imm.hideSoftInputFromWindow(binding.keyboardInput.windowToken, 0)
        } else {
            // Show keyboard
            binding.keyboardInput.requestFocus()
            imm.showSoftInput(binding.keyboardInput, InputMethodManager.SHOW_IMPLICIT)
        }
        android.util.Log.i("StreamActivity", "Keyboard toggled: visible=$isKeyboardVisible")
    }

    private fun resizeVideoForKeyboard(keyboardHeight: Int) {
        currentKeyboardHeight = keyboardHeight
        runOnUiThread {
            // Re-adjust aspect ratio with keyboard-aware available space
            if (serverWidth > 0 && serverHeight > 0 && maintainAspectRatio) {
                adjustSurfaceAspectRatio(serverWidth, serverHeight)
            }
            android.util.Log.i("StreamActivity", "Resizing video for keyboard: keyboardHeight=$keyboardHeight")
        }
    }

    private fun adjustSurfaceAspectRatio(sourceWidth: Int, sourceHeight: Int) {
        runOnUiThread {
            val parent = binding.videoSurface.parent as? FrameLayout ?: return@runOnUiThread

            // Get screen dimensions, accounting for keyboard
            val screenWidth = parent.width
            val availableHeight = parent.height - currentKeyboardHeight

            if (screenWidth == 0 || availableHeight <= 0) {
                // Layout not ready, try again after layout
                parent.post { adjustSurfaceAspectRatio(sourceWidth, sourceHeight) }
                return@runOnUiThread
            }

            val sourceAspect = sourceWidth.toFloat() / sourceHeight.toFloat()
            val screenAspect = screenWidth.toFloat() / availableHeight.toFloat()

            val newWidth: Int
            val newHeight: Int

            if (sourceAspect > screenAspect) {
                // Source is wider - fit to width, add black bars top/bottom
                newWidth = screenWidth
                newHeight = (screenWidth / sourceAspect).toInt()
            } else {
                // Source is taller - fit to height, add black bars left/right
                newHeight = availableHeight
                newWidth = (availableHeight * sourceAspect).toInt()
            }

            val params = binding.videoSurface.layoutParams as FrameLayout.LayoutParams
            params.width = newWidth
            params.height = newHeight
            // Position video just above keyboard when visible
            if (currentKeyboardHeight > 0) {
                params.topMargin = 0
                params.bottomMargin = currentKeyboardHeight
                params.gravity = Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL
            } else {
                params.topMargin = 0
                params.bottomMargin = 0
                params.gravity = Gravity.CENTER
            }
            binding.videoSurface.layoutParams = params

            android.util.Log.i("StreamActivity", "Adjusted surface: ${sourceWidth}x${sourceHeight} -> ${newWidth}x${newHeight} (keyboard=$currentKeyboardHeight)")
        }
    }

    private suspend fun receiveVideo() {
        while (true) {
            try {
                val frame = connectionManager.receiveVideoFrame()
                if (frame != null) {
                    framesReceived++
                    decoder?.submitFrame(frame.data, frame.timestamp, frame.isKeyframe)
                } else {
                    // Yield to prevent busy-waiting when no frame available
                    yield()
                }
            } catch (e: Exception) {
                android.util.Log.e("StreamActivity", "Video receive error", e)
                break
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        fpsJob?.cancel()
        stopAudio()
        connectionManager.disconnect()
        decoder?.stop()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            hideSystemUI()
        }
    }
}

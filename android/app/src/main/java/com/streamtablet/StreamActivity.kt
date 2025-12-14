package com.streamtablet

import android.os.Bundle
import android.view.View
import android.view.WindowManager
import android.widget.FrameLayout
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.lifecycleScope
import com.streamtablet.calibration.CalibrationManager
import com.streamtablet.databinding.ActivityStreamBinding
import com.streamtablet.input.InputHandler
import com.streamtablet.network.ConnectionManager
import com.streamtablet.video.AV1Decoder
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class StreamActivity : AppCompatActivity() {

    companion object {
        const val EXTRA_SERVER_ADDRESS = "server_address"
        const val EXTRA_PORT = "port"
        const val EXTRA_MAINTAIN_ASPECT_RATIO = "maintain_aspect_ratio"
    }

    private lateinit var binding: ActivityStreamBinding
    private lateinit var connectionManager: ConnectionManager
    private lateinit var calibrationManager: CalibrationManager
    private var decoder: AV1Decoder? = null
    private lateinit var inputHandler: InputHandler

    private var serverAddress: String = ""
    private var port: Int = 9500
    private var maintainAspectRatio: Boolean = true

    @Volatile
    private var isConnected = false
    private var surfaceReady = false
    private var pendingSurface: android.view.SurfaceHolder? = null

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

        // Set up touch and hover handling for stylus support
        binding.videoSurface.setOnTouchListener { view, event ->
            inputHandler.onTouch(view, event)
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

            // Adjust surface size for aspect ratio if needed
            if (maintainAspectRatio) {
                adjustSurfaceAspectRatio(config.width, config.height)
            }

            val newDecoder = AV1Decoder(holder.surface, config.width, config.height)
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

            // Hide loading indicator
            runOnUiThread {
                binding.loadingIndicator.visibility = View.GONE
            }
        } catch (e: Exception) {
            android.util.Log.e("StreamActivity", "Error starting decoder", e)
        }
    }

    private fun adjustSurfaceAspectRatio(sourceWidth: Int, sourceHeight: Int) {
        runOnUiThread {
            val parent = binding.videoSurface.parent as? FrameLayout ?: return@runOnUiThread

            // Get screen dimensions
            val screenWidth = parent.width
            val screenHeight = parent.height

            if (screenWidth == 0 || screenHeight == 0) {
                // Layout not ready, try again after layout
                parent.post { adjustSurfaceAspectRatio(sourceWidth, sourceHeight) }
                return@runOnUiThread
            }

            val sourceAspect = sourceWidth.toFloat() / sourceHeight.toFloat()
            val screenAspect = screenWidth.toFloat() / screenHeight.toFloat()

            val newWidth: Int
            val newHeight: Int

            if (sourceAspect > screenAspect) {
                // Source is wider - fit to width, add black bars top/bottom
                newWidth = screenWidth
                newHeight = (screenWidth / sourceAspect).toInt()
            } else {
                // Source is taller - fit to height, add black bars left/right
                newHeight = screenHeight
                newWidth = (screenHeight * sourceAspect).toInt()
            }

            val params = binding.videoSurface.layoutParams as FrameLayout.LayoutParams
            params.width = newWidth
            params.height = newHeight
            binding.videoSurface.layoutParams = params

            android.util.Log.i("StreamActivity", "Adjusted surface: ${sourceWidth}x${sourceHeight} -> ${newWidth}x${newHeight}")
        }
    }

    private suspend fun receiveVideo() {
        while (true) {
            try {
                val frame = connectionManager.receiveVideoFrame()
                if (frame != null) {
                    decoder?.submitFrame(frame.data, frame.timestamp, frame.isKeyframe)
                }
            } catch (e: Exception) {
                android.util.Log.e("StreamActivity", "Video receive error", e)
                break
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
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

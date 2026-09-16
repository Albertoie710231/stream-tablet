package com.streamtablet

import android.content.Intent
import android.graphics.Color
import android.os.Bundle
import android.util.TypedValue
import android.view.Gravity
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import com.streamtablet.calibration.CalibrationActivity
import com.streamtablet.calibration.CalibrationManager
import com.streamtablet.databinding.ActivityMainBinding
import com.streamtablet.discovery.DiscoveredServer
import com.streamtablet.discovery.MdnsDiscovery

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var calibrationManager: CalibrationManager
    private lateinit var mdns: MdnsDiscovery
    private val discovered = linkedMapOf<String, DiscoveredServer>()

    private val calibrationLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        updateCalibrationStatus()
        if (result.resultCode == RESULT_OK) {
            Toast.makeText(this, "Calibration saved!", Toast.LENGTH_SHORT).show()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        calibrationManager = CalibrationManager(this)

        setupSpinners()

        binding.connectButton.setOnClickListener {
            connect()
        }

        binding.calibrateButton.setOnClickListener {
            startCalibration()
        }

        binding.calibrateButton.setOnLongClickListener {
            calibrationManager.clearCalibration()
            updateCalibrationStatus()
            Toast.makeText(this, "Calibration cleared", Toast.LENGTH_SHORT).show()
            true
        }

        // Show/hide CQP field based on quality mode
        binding.qualitySpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                // CQP is relevant for Auto (0) and High Quality (3)
                binding.cqpLayout.visibility = if (position == 0 || position == 3) View.VISIBLE else View.GONE
            }
            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }

        updateCalibrationStatus()

        mdns = MdnsDiscovery(this)
    }

    override fun onStart() {
        super.onStart()
        discovered.clear()
        refreshDiscoveredList()
        mdns.start(object : MdnsDiscovery.Listener {
            override fun onServerFound(server: DiscoveredServer) {
                runOnUiThread {
                    discovered[server.name] = server
                    refreshDiscoveredList()
                }
            }
            override fun onServerLost(name: String) {
                runOnUiThread {
                    discovered.remove(name)
                    refreshDiscoveredList()
                }
            }
        })
    }

    override fun onStop() {
        super.onStop()
        mdns.stop()
    }

    private fun refreshDiscoveredList() {
        val list = binding.discoveredList
        list.removeAllViews()
        binding.discoveryProgress.visibility =
            if (discovered.isEmpty()) View.VISIBLE else View.GONE

        if (discovered.isEmpty()) {
            val tv = TextView(this).apply {
                text = "Searching local network…"
                textSize = 13f
                alpha = 0.6f
                setPadding(0, dp(8), 0, dp(8))
            }
            list.addView(tv)
            return
        }

        for ((_, server) in discovered) {
            list.addView(buildServerRow(server))
        }
    }

    private fun buildServerRow(server: DiscoveredServer): View {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(12), dp(10), dp(12), dp(10))
            isClickable = true
            isFocusable = true
            val tv = TypedValue()
            context.theme.resolveAttribute(
                android.R.attr.selectableItemBackground, tv, true,
            )
            setBackgroundResource(tv.resourceId)
            setOnClickListener {
                binding.serverAddressEdit.setText(server.host)
                binding.portEdit.setText(server.port.toString())
                Toast.makeText(
                    this@MainActivity,
                    "Using ${server.name}",
                    Toast.LENGTH_SHORT,
                ).show()
            }
        }

        // Leading dot/indicator
        val dot = View(this).apply {
            layoutParams = LinearLayout.LayoutParams(dp(8), dp(8)).apply {
                marginEnd = dp(12)
            }
            setBackgroundColor(Color.parseColor("#4CAF50"))
        }
        row.addView(dot)

        val textCol = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f,
            )
        }
        textCol.addView(TextView(this).apply {
            text = server.name
            textSize = 15f
            setTextColor(Color.WHITE)
        })
        textCol.addView(TextView(this).apply {
            text = "${server.host}:${server.port}"
            textSize = 12f
            alpha = 0.6f
        })
        row.addView(textCol)

        val chevron = TextView(this).apply {
            text = "›"
            textSize = 22f
            alpha = 0.5f
            setPadding(dp(8), 0, 0, 0)
        }
        row.addView(chevron)

        return row
    }

    private fun dp(v: Int): Int =
        (v * resources.displayMetrics.density).toInt()

    private fun setupSpinners() {
        // Codec spinner
        ArrayAdapter.createFromResource(this, R.array.codec_options, android.R.layout.simple_spinner_item).also {
            it.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
            binding.codecSpinner.adapter = it
        }

        // Quality spinner
        ArrayAdapter.createFromResource(this, R.array.quality_options, android.R.layout.simple_spinner_item).also {
            it.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
            binding.qualitySpinner.adapter = it
        }

        // Pacing spinner
        ArrayAdapter.createFromResource(this, R.array.pacing_options, android.R.layout.simple_spinner_item).also {
            it.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
            binding.pacingSpinner.adapter = it
        }
    }

    private fun startCalibration() {
        val intent = Intent(this, CalibrationActivity::class.java)
        calibrationLauncher.launch(intent)
    }

    private fun updateCalibrationStatus() {
        binding.calibrationStatus.text = if (calibrationManager.isCalibrated) {
            getString(R.string.calibrated)
        } else {
            getString(R.string.not_calibrated)
        }
    }

    private fun connect() {
        val serverAddress = binding.serverAddressEdit.text.toString().trim()
        val port = binding.portEdit.text.toString().toIntOrNull() ?: 9500

        if (serverAddress.isEmpty()) {
            Toast.makeText(this, "Please enter server address", Toast.LENGTH_SHORT).show()
            return
        }

        binding.statusText.text = getString(R.string.connecting)

        val maintainAspectRatio = binding.aspectRatioSwitch.isChecked
        val codec = binding.codecSpinner.selectedItemPosition
        val fps = binding.fpsEdit.text.toString().toIntOrNull() ?: 60
        val quality = binding.qualitySpinner.selectedItemPosition
        val cqp = binding.cqpEdit.text.toString().toIntOrNull() ?: 24
        val bitrateText = binding.bitrateEdit.text.toString().trim()
        val bitrate = if (bitrateText.isEmpty() || bitrateText == "0") 0 else (bitrateText.toIntOrNull() ?: 0) * 1000  // UI shows kbps, protocol uses bps
        val pacing = binding.pacingSpinner.selectedItemPosition
        val audioEnabled = binding.audioSwitch.isChecked
        val audioExclusive = binding.audioExclusiveSwitch.isChecked
        val audioBitrate = binding.audioBitrateEdit.text.toString().toIntOrNull()?.times(1000) ?: 128000

        val intent = Intent(this, StreamActivity::class.java).apply {
            putExtra(StreamActivity.EXTRA_SERVER_ADDRESS, serverAddress)
            putExtra(StreamActivity.EXTRA_PORT, port)
            putExtra(StreamActivity.EXTRA_MAINTAIN_ASPECT_RATIO, maintainAspectRatio)
            putExtra(StreamActivity.EXTRA_CODEC, codec)
            putExtra(StreamActivity.EXTRA_FPS, fps)
            putExtra(StreamActivity.EXTRA_QUALITY, quality)
            putExtra(StreamActivity.EXTRA_CQP, cqp)
            putExtra(StreamActivity.EXTRA_BITRATE, bitrate)
            putExtra(StreamActivity.EXTRA_PACING, pacing)
            putExtra(StreamActivity.EXTRA_AUDIO_ENABLED, audioEnabled)
            putExtra(StreamActivity.EXTRA_AUDIO_EXCLUSIVE, audioExclusive)
            putExtra(StreamActivity.EXTRA_AUDIO_BITRATE, audioBitrate)
        }
        startActivity(intent)
    }

    override fun onResume() {
        super.onResume()
        binding.statusText.text = getString(R.string.disconnected)
    }
}

package com.streamtablet

import android.content.Intent
import android.os.Bundle
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import com.streamtablet.calibration.CalibrationActivity
import com.streamtablet.calibration.CalibrationManager
import com.streamtablet.databinding.ActivityMainBinding

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var calibrationManager: CalibrationManager

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

        binding.connectButton.setOnClickListener {
            connect()
        }

        binding.calibrateButton.setOnClickListener {
            startCalibration()
        }

        // Long press to clear calibration
        binding.calibrateButton.setOnLongClickListener {
            calibrationManager.clearCalibration()
            updateCalibrationStatus()
            Toast.makeText(this, "Calibration cleared", Toast.LENGTH_SHORT).show()
            true
        }

        updateCalibrationStatus()
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

        val intent = Intent(this, StreamActivity::class.java).apply {
            putExtra(StreamActivity.EXTRA_SERVER_ADDRESS, serverAddress)
            putExtra(StreamActivity.EXTRA_PORT, port)
            putExtra(StreamActivity.EXTRA_MAINTAIN_ASPECT_RATIO, maintainAspectRatio)
        }
        startActivity(intent)
    }

    override fun onResume() {
        super.onResume()
        binding.statusText.text = getString(R.string.disconnected)
    }
}

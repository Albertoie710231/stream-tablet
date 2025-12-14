package com.streamtablet

import android.content.Intent
import android.os.Bundle
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.streamtablet.databinding.ActivityMainBinding

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.connectButton.setOnClickListener {
            connect()
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

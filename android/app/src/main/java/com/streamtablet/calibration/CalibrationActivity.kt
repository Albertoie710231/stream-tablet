package com.streamtablet.calibration

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.os.Bundle
import android.view.MotionEvent
import android.view.View
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat

class CalibrationActivity : AppCompatActivity() {

    private lateinit var calibrationManager: CalibrationManager
    private lateinit var calibrationView: CalibrationView
    private lateinit var calibrationPoints: MutableList<CalibrationManager.CalibrationPoint>
    private var currentPointIndex = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        calibrationManager = CalibrationManager(this)
        calibrationPoints = calibrationManager.getCalibrationPoints().toMutableList()
        currentPointIndex = 0

        calibrationView = CalibrationView(this)
        setContentView(calibrationView)

        // Go fullscreen - must be after setContentView
        hideSystemUI()

        Toast.makeText(this, "Tap the center of each target with your stylus", Toast.LENGTH_LONG).show()
    }

    private fun hideSystemUI() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        val controller = WindowInsetsControllerCompat(window, calibrationView)
        controller.hide(WindowInsetsCompat.Type.systemBars())
        controller.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
    }

    private fun onPointCalibrated(x: Float, y: Float) {
        if (currentPointIndex >= calibrationPoints.size) return

        // Record the actual touch position
        calibrationPoints[currentPointIndex].actualX = x
        calibrationPoints[currentPointIndex].actualY = y
        calibrationPoints[currentPointIndex].isCalibrated = true

        currentPointIndex++
        calibrationView.invalidate()

        if (currentPointIndex >= calibrationPoints.size) {
            // All points calibrated, calculate coefficients
            if (calibrationManager.calculateCalibration(calibrationPoints)) {
                Toast.makeText(this, "Calibration complete!", Toast.LENGTH_SHORT).show()
                setResult(RESULT_OK)
                finish()
            } else {
                Toast.makeText(this, "Calibration failed, please try again", Toast.LENGTH_SHORT).show()
                resetCalibration()
            }
        }
    }

    private fun resetCalibration() {
        calibrationPoints = calibrationManager.getCalibrationPoints().toMutableList()
        currentPointIndex = 0
        calibrationView.invalidate()
    }

    override fun onBackPressed() {
        // Allow canceling calibration
        setResult(RESULT_CANCELED)
        super.onBackPressed()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            hideSystemUI()
        }
    }

    inner class CalibrationView(context: Context) : View(context) {

        private val targetPaint = Paint().apply {
            color = Color.RED
            style = Paint.Style.STROKE
            strokeWidth = 4f
            isAntiAlias = true
        }

        private val targetFillPaint = Paint().apply {
            color = Color.argb(50, 255, 0, 0)
            style = Paint.Style.FILL
            isAntiAlias = true
        }

        private val completedPaint = Paint().apply {
            color = Color.GREEN
            style = Paint.Style.FILL
            isAntiAlias = true
        }

        private val crosshairPaint = Paint().apply {
            color = Color.WHITE
            style = Paint.Style.STROKE
            strokeWidth = 2f
            isAntiAlias = true
        }

        private val textPaint = Paint().apply {
            color = Color.WHITE
            textSize = 48f
            textAlign = Paint.Align.CENTER
            isAntiAlias = true
        }

        private val bgPaint = Paint().apply {
            color = Color.BLACK
        }

        private val targetRadius = 40f
        private val crosshairLength = 60f

        override fun onDraw(canvas: Canvas) {
            super.onDraw(canvas)

            // Black background
            canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), bgPaint)

            // Draw all calibration points
            for ((index, point) in calibrationPoints.withIndex()) {
                val screenX = point.targetX * width
                val screenY = point.targetY * height

                if (point.isCalibrated) {
                    // Draw completed point (green dot)
                    canvas.drawCircle(screenX, screenY, targetRadius / 2, completedPaint)
                } else if (index == currentPointIndex) {
                    // Draw current target (red crosshair with circle)
                    canvas.drawCircle(screenX, screenY, targetRadius, targetFillPaint)
                    canvas.drawCircle(screenX, screenY, targetRadius, targetPaint)

                    // Crosshair
                    canvas.drawLine(screenX - crosshairLength, screenY,
                                   screenX + crosshairLength, screenY, crosshairPaint)
                    canvas.drawLine(screenX, screenY - crosshairLength,
                                   screenX, screenY + crosshairLength, crosshairPaint)

                    // Center dot
                    canvas.drawCircle(screenX, screenY, 4f, crosshairPaint)
                } else {
                    // Draw future point (dim circle)
                    val dimPaint = Paint(targetPaint).apply { alpha = 100 }
                    canvas.drawCircle(screenX, screenY, targetRadius / 2, dimPaint)
                }
            }

            // Draw instructions
            val instruction = when {
                currentPointIndex >= calibrationPoints.size -> "Calibration complete!"
                else -> "Point ${currentPointIndex + 1} of ${calibrationPoints.size} - Tap the target"
            }
            canvas.drawText(instruction, width / 2f, height - 100f, textPaint)

            // Draw progress
            val progress = "Progress: ${calibrationPoints.count { it.isCalibrated }} / ${calibrationPoints.size}"
            canvas.drawText(progress, width / 2f, 100f, textPaint)
        }

        override fun onTouchEvent(event: MotionEvent): Boolean {
            // Only accept stylus input for calibration
            if (event.getToolType(0) != MotionEvent.TOOL_TYPE_STYLUS) {
                if (event.actionMasked == MotionEvent.ACTION_DOWN) {
                    Toast.makeText(context, "Please use the stylus for calibration", Toast.LENGTH_SHORT).show()
                }
                return true
            }

            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    // Normalize coordinates
                    val normalizedX = event.x / width
                    val normalizedY = event.y / height

                    // Check if touch is near the current target
                    if (currentPointIndex < calibrationPoints.size) {
                        val target = calibrationPoints[currentPointIndex]
                        val dx = normalizedX - target.targetX
                        val dy = normalizedY - target.targetY
                        val distance = kotlin.math.sqrt(dx * dx + dy * dy)

                        // Allow some margin for error (20% of screen size)
                        if (distance < 0.2f) {
                            onPointCalibrated(normalizedX, normalizedY)
                        } else {
                            Toast.makeText(context, "Please tap closer to the target", Toast.LENGTH_SHORT).show()
                        }
                    }
                }
            }
            return true
        }
    }

    companion object {
        const val REQUEST_CODE = 1001
    }
}

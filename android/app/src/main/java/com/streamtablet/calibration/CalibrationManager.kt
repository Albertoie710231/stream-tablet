package com.streamtablet.calibration

import android.content.Context
import android.content.SharedPreferences
import android.util.Log
import kotlin.math.pow

private const val TAG = "CalibrationManager"

/**
 * Manages stylus calibration using polynomial correction.
 *
 * Uses a 2D polynomial transformation to correct non-linear distortion:
 * x' = a0 + a1*x + a2*y + a3*x*y + a4*x² + a5*y²
 * y' = b0 + b1*x + b2*y + b3*x*y + b4*x² + b5*y²
 */
class CalibrationManager(context: Context) {

    private val prefs: SharedPreferences = context.getSharedPreferences(
        PREFS_NAME, Context.MODE_PRIVATE
    )

    // Polynomial coefficients for X correction
    private var coeffX = DoubleArray(6) { if (it == 1) 1.0 else 0.0 }  // Default: identity

    // Polynomial coefficients for Y correction
    private var coeffY = DoubleArray(6) { if (it == 2) 1.0 else 0.0 }  // Default: identity

    var isCalibrated: Boolean = false
        private set

    init {
        loadCalibration()
    }

    /**
     * Calibration point data
     */
    data class CalibrationPoint(
        val targetX: Float,  // Expected position (0-1)
        val targetY: Float,
        var actualX: Float = 0f,  // Actual touch position (0-1)
        var actualY: Float = 0f,
        var isCalibrated: Boolean = false
    )

    /**
     * Get the 9 calibration target points
     */
    fun getCalibrationPoints(): List<CalibrationPoint> {
        val margin = 0.1f  // 10% margin from edges
        return listOf(
            // Top row
            CalibrationPoint(margin, margin),                    // Top-left
            CalibrationPoint(0.5f, margin),                      // Top-center
            CalibrationPoint(1f - margin, margin),               // Top-right
            // Middle row
            CalibrationPoint(margin, 0.5f),                      // Middle-left
            CalibrationPoint(0.5f, 0.5f),                        // Center
            CalibrationPoint(1f - margin, 0.5f),                 // Middle-right
            // Bottom row
            CalibrationPoint(margin, 1f - margin),               // Bottom-left
            CalibrationPoint(0.5f, 1f - margin),                 // Bottom-center
            CalibrationPoint(1f - margin, 1f - margin)           // Bottom-right
        )
    }

    /**
     * Calculate calibration coefficients from calibration points.
     * Uses least-squares polynomial fitting.
     */
    fun calculateCalibration(points: List<CalibrationPoint>): Boolean {
        if (points.size < 6 || points.any { !it.isCalibrated }) {
            return false
        }

        // Build matrices for least-squares fitting
        // We're solving: A * coefficients = targets
        // Where A is the Vandermonde-like matrix of polynomial terms

        val n = points.size

        // For each axis, solve the system
        coeffX = solvePolynomialFit(points, extractX = true)
        coeffY = solvePolynomialFit(points, extractX = false)

        Log.i(TAG, "Calibration calculated:")
        Log.i(TAG, "  coeffX = [${coeffX.joinToString(", ") { "%.6f".format(it) }}]")
        Log.i(TAG, "  coeffY = [${coeffY.joinToString(", ") { "%.6f".format(it) }}]")

        // Log calibration points for debugging
        points.forEach { p ->
            Log.d(TAG, "  Point: target(%.2f,%.2f) actual(%.2f,%.2f)".format(
                p.targetX, p.targetY, p.actualX, p.actualY))
        }

        isCalibrated = true
        saveCalibration()

        return true
    }

    /**
     * Solve polynomial fit using least squares
     */
    private fun solvePolynomialFit(points: List<CalibrationPoint>, extractX: Boolean): DoubleArray {
        val n = points.size
        val numCoeffs = 6

        // Build design matrix A where each row is [1, x, y, xy, x², y²]
        val A = Array(n) { DoubleArray(numCoeffs) }
        val b = DoubleArray(n)

        for (i in 0 until n) {
            val p = points[i]
            val x = p.actualX.toDouble()
            val y = p.actualY.toDouble()

            A[i][0] = 1.0
            A[i][1] = x
            A[i][2] = y
            A[i][3] = x * y
            A[i][4] = x * x
            A[i][5] = y * y

            b[i] = if (extractX) p.targetX.toDouble() else p.targetY.toDouble()
        }

        // Solve using normal equations: (A^T * A) * x = A^T * b
        val ATA = Array(numCoeffs) { DoubleArray(numCoeffs) }
        val ATb = DoubleArray(numCoeffs)

        // Compute A^T * A
        for (i in 0 until numCoeffs) {
            for (j in 0 until numCoeffs) {
                var sum = 0.0
                for (k in 0 until n) {
                    sum += A[k][i] * A[k][j]
                }
                ATA[i][j] = sum
            }
        }

        // Compute A^T * b
        for (i in 0 until numCoeffs) {
            var sum = 0.0
            for (k in 0 until n) {
                sum += A[k][i] * b[k]
            }
            ATb[i] = sum
        }

        // Solve using Gaussian elimination with partial pivoting
        return gaussianElimination(ATA, ATb)
    }

    /**
     * Gaussian elimination with partial pivoting
     */
    private fun gaussianElimination(A: Array<DoubleArray>, b: DoubleArray): DoubleArray {
        val n = b.size
        val augmented = Array(n) { i -> A[i].copyOf(n + 1).also { it[n] = b[i] } }

        // Forward elimination
        for (col in 0 until n) {
            // Find pivot
            var maxRow = col
            for (row in col + 1 until n) {
                if (kotlin.math.abs(augmented[row][col]) > kotlin.math.abs(augmented[maxRow][col])) {
                    maxRow = row
                }
            }

            // Swap rows
            val temp = augmented[col]
            augmented[col] = augmented[maxRow]
            augmented[maxRow] = temp

            // Eliminate
            if (kotlin.math.abs(augmented[col][col]) < 1e-10) continue

            for (row in col + 1 until n) {
                val factor = augmented[row][col] / augmented[col][col]
                for (j in col until n + 1) {
                    augmented[row][j] -= factor * augmented[col][j]
                }
            }
        }

        // Back substitution
        val result = DoubleArray(n)
        for (i in n - 1 downTo 0) {
            var sum = augmented[i][n]
            for (j in i + 1 until n) {
                sum -= augmented[i][j] * result[j]
            }
            result[i] = if (kotlin.math.abs(augmented[i][i]) > 1e-10) {
                sum / augmented[i][i]
            } else {
                0.0
            }
        }

        return result
    }

    /**
     * Apply calibration correction to raw coordinates.
     * Input and output are normalized 0-1 coordinates.
     */
    fun correct(rawX: Float, rawY: Float): Pair<Float, Float> {
        if (!isCalibrated) {
            return Pair(rawX, rawY)
        }

        val x = rawX.toDouble()
        val y = rawY.toDouble()

        // Apply polynomial correction
        val correctedX = coeffX[0] +
                        coeffX[1] * x +
                        coeffX[2] * y +
                        coeffX[3] * x * y +
                        coeffX[4] * x * x +
                        coeffX[5] * y * y

        val correctedY = coeffY[0] +
                        coeffY[1] * x +
                        coeffY[2] * y +
                        coeffY[3] * x * y +
                        coeffY[4] * x * x +
                        coeffY[5] * y * y

        return Pair(
            correctedX.toFloat().coerceIn(0f, 1f),
            correctedY.toFloat().coerceIn(0f, 1f)
        )
    }

    /**
     * Save calibration to SharedPreferences
     */
    private fun saveCalibration() {
        prefs.edit().apply {
            putBoolean(KEY_CALIBRATED, isCalibrated)
            for (i in 0 until 6) {
                putFloat("${KEY_COEFF_X}_$i", coeffX[i].toFloat())
                putFloat("${KEY_COEFF_Y}_$i", coeffY[i].toFloat())
            }
            apply()
        }
    }

    /**
     * Load calibration from SharedPreferences
     */
    private fun loadCalibration() {
        isCalibrated = prefs.getBoolean(KEY_CALIBRATED, false)
        if (isCalibrated) {
            for (i in 0 until 6) {
                coeffX[i] = prefs.getFloat("${KEY_COEFF_X}_$i",
                    if (i == 1) 1f else 0f).toDouble()
                coeffY[i] = prefs.getFloat("${KEY_COEFF_Y}_$i",
                    if (i == 2) 1f else 0f).toDouble()
            }
            Log.i(TAG, "Calibration loaded:")
            Log.i(TAG, "  coeffX = [${coeffX.joinToString(", ") { "%.6f".format(it) }}]")
            Log.i(TAG, "  coeffY = [${coeffY.joinToString(", ") { "%.6f".format(it) }}]")
        } else {
            Log.i(TAG, "No calibration data found")
        }
    }

    /**
     * Clear calibration data
     */
    fun clearCalibration() {
        isCalibrated = false
        coeffX = DoubleArray(6) { if (it == 1) 1.0 else 0.0 }
        coeffY = DoubleArray(6) { if (it == 2) 1.0 else 0.0 }
        prefs.edit().clear().apply()
    }

    companion object {
        private const val PREFS_NAME = "stylus_calibration"
        private const val KEY_CALIBRATED = "is_calibrated"
        private const val KEY_COEFF_X = "coeff_x"
        private const val KEY_COEFF_Y = "coeff_y"
    }
}

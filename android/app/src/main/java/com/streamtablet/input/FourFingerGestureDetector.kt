package com.streamtablet.input

import android.view.MotionEvent

/**
 * Detects 4-finger tap gestures.
 * Triggers the callback when 4 fingers touch and release within 500ms.
 */
class FourFingerGestureDetector(private val onTriggered: () -> Unit) {
    private var fourFingerStartTime = 0L
    private var wasFourFingers = false
    private var triggered = false

    /** Returns true if the gesture was triggered since last clearTriggered() */
    fun wasTriggered(): Boolean = triggered

    /** Clear the triggered state */
    fun clearTriggered() { triggered = false }

    /**
     * Process a touch event.
     * @return true if 4 fingers are currently touching (to consume the event)
     */
    fun onTouchEvent(event: MotionEvent): Boolean {
        val pointerCount = event.pointerCount

        when (event.actionMasked) {
            MotionEvent.ACTION_POINTER_DOWN -> {
                if (pointerCount == 4 && !wasFourFingers) {
                    wasFourFingers = true
                    fourFingerStartTime = System.currentTimeMillis()
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                if (wasFourFingers) {
                    val duration = System.currentTimeMillis() - fourFingerStartTime
                    // Trigger on quick tap (< 500ms)
                    if (duration < 500) {
                        triggered = true
                        onTriggered()
                    }
                    wasFourFingers = false
                }
            }
            MotionEvent.ACTION_POINTER_UP -> {
                // Reset if fingers are lifted (but not all at once)
                if (event.pointerCount <= 4) {
                    // Check if this was a quick tap before resetting
                    if (wasFourFingers && event.pointerCount == 4) {
                        val duration = System.currentTimeMillis() - fourFingerStartTime
                        if (duration < 500) {
                            triggered = true
                            onTriggered()
                        }
                    }
                    wasFourFingers = false
                }
            }
        }
        return pointerCount >= 4
    }

    fun reset() {
        wasFourFingers = false
        fourFingerStartTime = 0L
    }
}

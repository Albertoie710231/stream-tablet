package com.streamtablet.input

import android.util.Log
import android.view.MotionEvent
import android.view.View
import com.streamtablet.calibration.CalibrationManager
import com.streamtablet.network.ConnectionManager
import com.streamtablet.network.InputEvent
import com.streamtablet.network.InputEventType

private const val TAG = "InputHandler"

class InputHandler(
    private val connectionManager: ConnectionManager,
    private val calibrationManager: CalibrationManager? = null
) : View.OnTouchListener, View.OnHoverListener, View.OnGenericMotionListener {

    // Map Android pointer IDs to slots 0-9
    private val pointerIdToSlot = mutableMapOf<Int, Int>()
    private val usedSlots = BooleanArray(10)

    private fun getSlotForPointerId(pointerId: Int): Int {
        // Return existing slot if already mapped
        pointerIdToSlot[pointerId]?.let { return it }

        // Find first available slot
        for (slot in 0 until 10) {
            if (!usedSlots[slot]) {
                usedSlots[slot] = true
                pointerIdToSlot[pointerId] = slot
                return slot
            }
        }

        // Fallback: use pointerId modulo 10
        return pointerId % 10
    }

    private fun releaseSlot(pointerId: Int) {
        pointerIdToSlot[pointerId]?.let { slot ->
            usedSlots[slot] = false
            pointerIdToSlot.remove(pointerId)
        }
    }

    override fun onTouch(view: View, event: MotionEvent): Boolean {
        val pointerCount = event.pointerCount
        val action = event.actionMasked
        val actionIndex = event.actionIndex

        when (action) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                sendEvent(view, event, actionIndex, getDownEventType(event, actionIndex))
            }

            MotionEvent.ACTION_MOVE -> {
                // Send move events for all pointers
                for (i in 0 until pointerCount) {
                    sendEvent(view, event, i, getMoveEventType(event, i))
                }
            }

            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                sendEvent(view, event, actionIndex, getUpEventType(event, actionIndex))
                // Release slot on touch up
                val pointerId = event.getPointerId(actionIndex)
                if (!isStylus(event, actionIndex)) {
                    releaseSlot(pointerId)
                }
            }

            MotionEvent.ACTION_CANCEL -> {
                // Send up events for all pointers and release all slots
                for (i in 0 until pointerCount) {
                    sendEvent(view, event, i, getUpEventType(event, i))
                    val pointerId = event.getPointerId(i)
                    if (!isStylus(event, i)) {
                        releaseSlot(pointerId)
                    }
                }
            }
        }

        return true
    }

    // Handle stylus hover events (when stylus is near screen but not touching)
    override fun onHover(view: View, event: MotionEvent): Boolean {
        val toolType = event.getToolType(0)
        Log.d(TAG, "onHover: action=${event.actionMasked}, toolType=$toolType, x=${event.x}, y=${event.y}")

        // Only handle stylus hover events
        if (toolType != MotionEvent.TOOL_TYPE_STYLUS) {
            return false
        }

        when (event.actionMasked) {
            MotionEvent.ACTION_HOVER_ENTER -> {
                Log.d(TAG, "HOVER_ENTER")
                sendEvent(view, event, 0, InputEventType.STYLUS_HOVER)
            }
            MotionEvent.ACTION_HOVER_MOVE -> {
                Log.d(TAG, "HOVER_MOVE")
                sendEvent(view, event, 0, InputEventType.STYLUS_HOVER)
            }
            MotionEvent.ACTION_HOVER_EXIT -> {
                Log.d(TAG, "HOVER_EXIT")
                // Send stylus up when leaving hover range
                sendEvent(view, event, 0, InputEventType.STYLUS_UP)
            }
        }
        return true
    }

    // Handle generic motion events (backup for stylus events)
    override fun onGenericMotion(view: View, event: MotionEvent): Boolean {
        val toolType = event.getToolType(0)
        Log.d(TAG, "onGenericMotion: action=${event.actionMasked}, toolType=$toolType, x=${event.x}, y=${event.y}")

        // Handle stylus hover events that might come through generic motion
        if (toolType == MotionEvent.TOOL_TYPE_STYLUS) {
            when (event.actionMasked) {
                MotionEvent.ACTION_HOVER_ENTER,
                MotionEvent.ACTION_HOVER_MOVE -> {
                    Log.d(TAG, "GenericMotion HOVER")
                    sendEvent(view, event, 0, InputEventType.STYLUS_HOVER)
                    return true
                }
                MotionEvent.ACTION_HOVER_EXIT -> {
                    Log.d(TAG, "GenericMotion HOVER_EXIT")
                    sendEvent(view, event, 0, InputEventType.STYLUS_UP)
                    return true
                }
            }
        }
        return false
    }

    private fun sendEvent(view: View, event: MotionEvent, pointerIndex: Int, type: InputEventType) {
        val pointerId = event.getPointerId(pointerIndex)

        // Map pointer ID to slot (0-9) for touch events
        val slot = if (isStylus(event, pointerIndex)) {
            0  // Stylus always uses slot 0
        } else {
            getSlotForPointerId(pointerId)
        }

        // Normalize coordinates to 0-1
        var x = (event.getX(pointerIndex) / view.width).coerceIn(0f, 1f)
        var y = (event.getY(pointerIndex) / view.height).coerceIn(0f, 1f)

        // Apply calibration correction for stylus input
        if (isStylus(event, pointerIndex) && calibrationManager != null) {
            val corrected = calibrationManager.correct(x, y)
            x = corrected.first
            y = corrected.second
        }

        // Get pressure (0-1)
        val pressure = event.getPressure(pointerIndex).coerceIn(0f, 1f)

        // Get tilt (only available for stylus)
        val tiltX = if (isStylus(event, pointerIndex)) {
            event.getAxisValue(MotionEvent.AXIS_TILT, pointerIndex)
        } else 0f

        val orientation = if (isStylus(event, pointerIndex)) {
            event.getAxisValue(MotionEvent.AXIS_ORIENTATION, pointerIndex)
        } else 0f

        // Get button state
        val buttons = event.buttonState

        val inputEvent = InputEvent(
            type = type,
            pointerId = slot.toByte(),
            x = x,
            y = y,
            pressure = pressure,
            tiltX = tiltX,
            tiltY = orientation,
            buttons = buttons.toShort(),
            timestamp = event.eventTime.toInt()
        )

        connectionManager.sendInput(inputEvent)
    }

    private fun isStylus(event: MotionEvent, pointerIndex: Int): Boolean {
        return event.getToolType(pointerIndex) == MotionEvent.TOOL_TYPE_STYLUS
    }

    private fun getDownEventType(event: MotionEvent, pointerIndex: Int): InputEventType {
        return if (isStylus(event, pointerIndex)) {
            InputEventType.STYLUS_DOWN
        } else {
            InputEventType.TOUCH_DOWN
        }
    }

    private fun getMoveEventType(event: MotionEvent, pointerIndex: Int): InputEventType {
        return if (isStylus(event, pointerIndex)) {
            InputEventType.STYLUS_MOVE
        } else {
            InputEventType.TOUCH_MOVE
        }
    }

    private fun getUpEventType(event: MotionEvent, pointerIndex: Int): InputEventType {
        return if (isStylus(event, pointerIndex)) {
            InputEventType.STYLUS_UP
        } else {
            InputEventType.TOUCH_UP
        }
    }
}

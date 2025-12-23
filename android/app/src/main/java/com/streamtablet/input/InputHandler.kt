package com.streamtablet.input

import android.util.Log
import android.view.KeyEvent
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

    // Keyboard deduplication to prevent double letters
    private var lastSentChar: Char = '\u0000'
    private var lastSentTime: Long = 0
    private val DEDUP_WINDOW_MS = 100L

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

    /**
     * Cancel all active touches by sending TOUCH_UP for each.
     * Used when a gesture is detected to undo any touch events that leaked through.
     */
    fun cancelAllTouches() {
        for ((_, slot) in pointerIdToSlot) {
            val inputEvent = InputEvent(
                type = InputEventType.TOUCH_UP,
                pointerId = slot.toByte(),
                x = 0f,
                y = 0f,
                pressure = 0f,
                tiltX = 0f,
                tiltY = 0f,
                buttons = 0,
                timestamp = (System.currentTimeMillis() % Int.MAX_VALUE).toInt()
            )
            connectionManager.sendInput(inputEvent)
        }
        pointerIdToSlot.clear()
        usedSlots.fill(false)
        Log.d(TAG, "Cancelled all active touches")
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
            val oldX = x
            val oldY = y
            val corrected = calibrationManager.correct(x, y)
            x = corrected.first
            y = corrected.second
            if (calibrationManager.isCalibrated) {
                Log.d(TAG, "Calibration applied: (%.3f,%.3f) -> (%.3f,%.3f)".format(oldX, oldY, x, y))
            }
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

    /**
     * Send a character from the soft keyboard to the server.
     * Converts the character to key events (with shift if needed).
     */
    fun sendCharacter(char: Char) {
        val now = System.currentTimeMillis()

        // Deduplicate rapid identical characters (TextWatcher can fire multiple times)
        if (char == lastSentChar && (now - lastSentTime) < DEDUP_WINDOW_MS) {
            Log.d(TAG, "Ignoring duplicate character: '$char'")
            return
        }
        lastSentChar = char
        lastSentTime = now

        val (keyCode, needsShift) = charToLinuxKeyCode(char)
        if (keyCode == 0) {
            Log.d(TAG, "Unknown character: $char (${char.code})")
            return
        }

        // Press shift if needed
        if (needsShift) {
            sendLinuxKey(42, true)  // KEY_LEFTSHIFT down
        }

        // Press and release the key
        sendLinuxKey(keyCode, true)
        sendLinuxKey(keyCode, false)

        // Release shift if needed
        if (needsShift) {
            sendLinuxKey(42, false)  // KEY_LEFTSHIFT up
        }

        Log.d(TAG, "Sent character: '$char' keyCode=$keyCode shift=$needsShift")
    }

    private fun sendLinuxKey(linuxKeyCode: Int, pressed: Boolean) {
        val inputEvent = InputEvent(
            type = if (pressed) InputEventType.KEY_DOWN else InputEventType.KEY_UP,
            pointerId = 0,
            x = 0f,
            y = 0f,
            pressure = 0f,
            tiltX = 0f,
            tiltY = 0f,
            buttons = linuxKeyCode.toShort(),
            timestamp = (System.currentTimeMillis() % Int.MAX_VALUE).toInt()
        )
        connectionManager.sendInput(inputEvent)
    }

    /**
     * Map a character to Linux keycode and whether shift is needed.
     * Returns Pair(keyCode, needsShift)
     */
    private fun charToLinuxKeyCode(char: Char): Pair<Int, Boolean> {
        return when (char) {
            // Lowercase letters
            'a' -> Pair(30, false)
            'b' -> Pair(48, false)
            'c' -> Pair(46, false)
            'd' -> Pair(32, false)
            'e' -> Pair(18, false)
            'f' -> Pair(33, false)
            'g' -> Pair(34, false)
            'h' -> Pair(35, false)
            'i' -> Pair(23, false)
            'j' -> Pair(36, false)
            'k' -> Pair(37, false)
            'l' -> Pair(38, false)
            'm' -> Pair(50, false)
            'n' -> Pair(49, false)
            'o' -> Pair(24, false)
            'p' -> Pair(25, false)
            'q' -> Pair(16, false)
            'r' -> Pair(19, false)
            's' -> Pair(31, false)
            't' -> Pair(20, false)
            'u' -> Pair(22, false)
            'v' -> Pair(47, false)
            'w' -> Pair(17, false)
            'x' -> Pair(45, false)
            'y' -> Pair(21, false)
            'z' -> Pair(44, false)

            // Uppercase letters (same keys, with shift)
            'A' -> Pair(30, true)
            'B' -> Pair(48, true)
            'C' -> Pair(46, true)
            'D' -> Pair(32, true)
            'E' -> Pair(18, true)
            'F' -> Pair(33, true)
            'G' -> Pair(34, true)
            'H' -> Pair(35, true)
            'I' -> Pair(23, true)
            'J' -> Pair(36, true)
            'K' -> Pair(37, true)
            'L' -> Pair(38, true)
            'M' -> Pair(50, true)
            'N' -> Pair(49, true)
            'O' -> Pair(24, true)
            'P' -> Pair(25, true)
            'Q' -> Pair(16, true)
            'R' -> Pair(19, true)
            'S' -> Pair(31, true)
            'T' -> Pair(20, true)
            'U' -> Pair(22, true)
            'V' -> Pair(47, true)
            'W' -> Pair(17, true)
            'X' -> Pair(45, true)
            'Y' -> Pair(21, true)
            'Z' -> Pair(44, true)

            // Numbers
            '0' -> Pair(11, false)
            '1' -> Pair(2, false)
            '2' -> Pair(3, false)
            '3' -> Pair(4, false)
            '4' -> Pair(5, false)
            '5' -> Pair(6, false)
            '6' -> Pair(7, false)
            '7' -> Pair(8, false)
            '8' -> Pair(9, false)
            '9' -> Pair(10, false)

            // Shifted number symbols
            '!' -> Pair(2, true)   // Shift+1
            '@' -> Pair(3, true)   // Shift+2
            '#' -> Pair(4, true)   // Shift+3
            '$' -> Pair(5, true)   // Shift+4
            '%' -> Pair(6, true)   // Shift+5
            '^' -> Pair(7, true)   // Shift+6
            '&' -> Pair(8, true)   // Shift+7
            '*' -> Pair(9, true)   // Shift+8
            '(' -> Pair(10, true)  // Shift+9
            ')' -> Pair(11, true)  // Shift+0

            // Common punctuation
            ' ' -> Pair(57, false)  // Space
            '\n' -> Pair(28, false) // Enter
            '\t' -> Pair(15, false) // Tab
            ',' -> Pair(51, false)  // Comma
            '.' -> Pair(52, false)  // Period
            '/' -> Pair(53, false)  // Slash
            ';' -> Pair(39, false)  // Semicolon
            '\'' -> Pair(40, false) // Apostrophe
            '[' -> Pair(26, false)  // Left bracket
            ']' -> Pair(27, false)  // Right bracket
            '\\' -> Pair(43, false) // Backslash
            '-' -> Pair(12, false)  // Minus
            '=' -> Pair(13, false)  // Equals
            '`' -> Pair(41, false)  // Grave

            // Shifted punctuation
            '<' -> Pair(51, true)   // Shift+comma
            '>' -> Pair(52, true)   // Shift+period
            '?' -> Pair(53, true)   // Shift+slash
            ':' -> Pair(39, true)   // Shift+semicolon
            '"' -> Pair(40, true)   // Shift+apostrophe
            '{' -> Pair(26, true)   // Shift+left bracket
            '}' -> Pair(27, true)   // Shift+right bracket
            '|' -> Pair(43, true)   // Shift+backslash
            '_' -> Pair(12, true)   // Shift+minus
            '+' -> Pair(13, true)   // Shift+equals
            '~' -> Pair(41, true)   // Shift+grave

            else -> Pair(0, false)  // Unknown character
        }
    }

    /**
     * Send a keyboard key event to the server.
     * @param androidKeyCode The Android KeyEvent keycode
     * @param pressed true for key down, false for key up
     */
    fun sendKeyEvent(androidKeyCode: Int, pressed: Boolean) {
        val linuxKeyCode = androidToLinuxKeyCode(androidKeyCode)
        if (linuxKeyCode == 0) {
            Log.d(TAG, "Unknown key code: $androidKeyCode")
            return
        }

        val inputEvent = InputEvent(
            type = if (pressed) InputEventType.KEY_DOWN else InputEventType.KEY_UP,
            pointerId = 0,
            x = 0f,
            y = 0f,
            pressure = 0f,
            tiltX = 0f,
            tiltY = 0f,
            buttons = linuxKeyCode.toShort(),  // Store keycode in buttons field
            timestamp = (System.currentTimeMillis() % Int.MAX_VALUE).toInt()
        )

        connectionManager.sendInput(inputEvent)
        Log.d(TAG, "Sent key event: android=$androidKeyCode linux=$linuxKeyCode pressed=$pressed")
    }

    /**
     * Map Android KeyEvent codes to Linux input.h KEY_* codes.
     * See: https://github.com/torvalds/linux/blob/master/include/uapi/linux/input-event-codes.h
     */
    private fun androidToLinuxKeyCode(androidCode: Int): Int {
        return when (androidCode) {
            // Letters A-Z (Linux KEY_A=30 to KEY_Z=44, with gaps)
            KeyEvent.KEYCODE_A -> 30
            KeyEvent.KEYCODE_B -> 48
            KeyEvent.KEYCODE_C -> 46
            KeyEvent.KEYCODE_D -> 32
            KeyEvent.KEYCODE_E -> 18
            KeyEvent.KEYCODE_F -> 33
            KeyEvent.KEYCODE_G -> 34
            KeyEvent.KEYCODE_H -> 35
            KeyEvent.KEYCODE_I -> 23
            KeyEvent.KEYCODE_J -> 36
            KeyEvent.KEYCODE_K -> 37
            KeyEvent.KEYCODE_L -> 38
            KeyEvent.KEYCODE_M -> 50
            KeyEvent.KEYCODE_N -> 49
            KeyEvent.KEYCODE_O -> 24
            KeyEvent.KEYCODE_P -> 25
            KeyEvent.KEYCODE_Q -> 16
            KeyEvent.KEYCODE_R -> 19
            KeyEvent.KEYCODE_S -> 31
            KeyEvent.KEYCODE_T -> 20
            KeyEvent.KEYCODE_U -> 22
            KeyEvent.KEYCODE_V -> 47
            KeyEvent.KEYCODE_W -> 17
            KeyEvent.KEYCODE_X -> 45
            KeyEvent.KEYCODE_Y -> 21
            KeyEvent.KEYCODE_Z -> 44

            // Numbers 0-9 (Linux KEY_1=2 to KEY_0=11)
            KeyEvent.KEYCODE_0 -> 11
            KeyEvent.KEYCODE_1 -> 2
            KeyEvent.KEYCODE_2 -> 3
            KeyEvent.KEYCODE_3 -> 4
            KeyEvent.KEYCODE_4 -> 5
            KeyEvent.KEYCODE_5 -> 6
            KeyEvent.KEYCODE_6 -> 7
            KeyEvent.KEYCODE_7 -> 8
            KeyEvent.KEYCODE_8 -> 9
            KeyEvent.KEYCODE_9 -> 10

            // Special keys
            KeyEvent.KEYCODE_SPACE -> 57        // KEY_SPACE
            KeyEvent.KEYCODE_ENTER -> 28        // KEY_ENTER
            KeyEvent.KEYCODE_DEL -> 14          // KEY_BACKSPACE
            KeyEvent.KEYCODE_FORWARD_DEL -> 111 // KEY_DELETE
            KeyEvent.KEYCODE_TAB -> 15          // KEY_TAB
            KeyEvent.KEYCODE_ESCAPE -> 1        // KEY_ESC

            // Punctuation and symbols
            KeyEvent.KEYCODE_COMMA -> 51        // KEY_COMMA
            KeyEvent.KEYCODE_PERIOD -> 52       // KEY_DOT
            KeyEvent.KEYCODE_SLASH -> 53        // KEY_SLASH
            KeyEvent.KEYCODE_SEMICOLON -> 39    // KEY_SEMICOLON
            KeyEvent.KEYCODE_APOSTROPHE -> 40   // KEY_APOSTROPHE
            KeyEvent.KEYCODE_LEFT_BRACKET -> 26 // KEY_LEFTBRACE
            KeyEvent.KEYCODE_RIGHT_BRACKET -> 27 // KEY_RIGHTBRACE
            KeyEvent.KEYCODE_BACKSLASH -> 43    // KEY_BACKSLASH
            KeyEvent.KEYCODE_MINUS -> 12        // KEY_MINUS
            KeyEvent.KEYCODE_EQUALS -> 13       // KEY_EQUAL
            KeyEvent.KEYCODE_GRAVE -> 41        // KEY_GRAVE

            // Modifier keys
            KeyEvent.KEYCODE_SHIFT_LEFT -> 42   // KEY_LEFTSHIFT
            KeyEvent.KEYCODE_SHIFT_RIGHT -> 54  // KEY_RIGHTSHIFT
            KeyEvent.KEYCODE_CTRL_LEFT -> 29    // KEY_LEFTCTRL
            KeyEvent.KEYCODE_CTRL_RIGHT -> 97   // KEY_RIGHTCTRL
            KeyEvent.KEYCODE_ALT_LEFT -> 56     // KEY_LEFTALT
            KeyEvent.KEYCODE_ALT_RIGHT -> 100   // KEY_RIGHTALT
            KeyEvent.KEYCODE_META_LEFT -> 125   // KEY_LEFTMETA
            KeyEvent.KEYCODE_META_RIGHT -> 126  // KEY_RIGHTMETA
            KeyEvent.KEYCODE_CAPS_LOCK -> 58    // KEY_CAPSLOCK

            // Arrow keys
            KeyEvent.KEYCODE_DPAD_UP -> 103     // KEY_UP
            KeyEvent.KEYCODE_DPAD_DOWN -> 108   // KEY_DOWN
            KeyEvent.KEYCODE_DPAD_LEFT -> 105   // KEY_LEFT
            KeyEvent.KEYCODE_DPAD_RIGHT -> 106  // KEY_RIGHT

            // Navigation
            KeyEvent.KEYCODE_HOME -> 102        // KEY_HOME
            KeyEvent.KEYCODE_MOVE_END -> 107    // KEY_END
            KeyEvent.KEYCODE_PAGE_UP -> 104     // KEY_PAGEUP
            KeyEvent.KEYCODE_PAGE_DOWN -> 109   // KEY_PAGEDOWN
            KeyEvent.KEYCODE_INSERT -> 110      // KEY_INSERT

            // Function keys
            KeyEvent.KEYCODE_F1 -> 59
            KeyEvent.KEYCODE_F2 -> 60
            KeyEvent.KEYCODE_F3 -> 61
            KeyEvent.KEYCODE_F4 -> 62
            KeyEvent.KEYCODE_F5 -> 63
            KeyEvent.KEYCODE_F6 -> 64
            KeyEvent.KEYCODE_F7 -> 65
            KeyEvent.KEYCODE_F8 -> 66
            KeyEvent.KEYCODE_F9 -> 67
            KeyEvent.KEYCODE_F10 -> 68
            KeyEvent.KEYCODE_F11 -> 87
            KeyEvent.KEYCODE_F12 -> 88

            else -> 0  // Unknown key
        }
    }
}

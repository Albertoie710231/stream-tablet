#pragma once

namespace stream_tablet {

// Ensures the KWin virtual output matches the requested (width, height, fps).
// Finds the first output whose name contains "Virtual", adds a custom mode if
// needed, and switches to it. Returns true on success.
bool apply_virtual_output_mode(int width, int height, int fps);

// Persist / restore the last known tablet display mode so the virtual output
// can be configured at server startup, before the PipeWire capture connects.
bool load_saved_display_mode(int& width, int& height, int& fps);
void save_display_mode(int width, int height, int fps);

}  // namespace stream_tablet

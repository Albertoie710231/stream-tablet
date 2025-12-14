#pragma once

#include <cmath>
#include <algorithm>

namespace stream_tablet {

class CoordTransform {
public:
    enum class Mode {
        LETTERBOX,  // Maintain aspect ratio with black bars
        FILL,       // Crop to fill
        STRETCH     // Distort to fill
    };

    CoordTransform() = default;

    // Initialize with screen and tablet dimensions
    void init(int screen_width, int screen_height,
              int tablet_width, int tablet_height,
              Mode mode = Mode::LETTERBOX, bool rotate90 = false) {
        m_screen_width = screen_width;
        m_screen_height = screen_height;
        m_tablet_width = tablet_width;
        m_tablet_height = tablet_height;
        m_mode = mode;
        m_rotate90 = rotate90;

        calculate_transform();
    }

    // Transform normalized tablet coordinates (0-1) to screen coordinates
    void transform(float tx, float ty, int& sx, int& sy) const {
        // Apply rotation if tablet is portrait and screen is landscape
        if (m_rotate90) {
            float temp = tx;
            tx = ty;
            ty = 1.0f - temp;
        }

        // Apply inverse letterbox transform
        float screen_x = (tx - m_offset_x) / m_scale_x;
        float screen_y = (ty - m_offset_y) / m_scale_y;

        // Clamp to valid range
        screen_x = std::clamp(screen_x, 0.0f, 1.0f);
        screen_y = std::clamp(screen_y, 0.0f, 1.0f);

        // Convert to pixel coordinates
        sx = static_cast<int>(screen_x * m_screen_width);
        sy = static_cast<int>(screen_y * m_screen_height);
    }

    // Get the visible area on tablet for debugging
    void get_visible_area(float& x, float& y, float& w, float& h) const {
        x = m_offset_x;
        y = m_offset_y;
        w = m_scale_x;
        h = m_scale_y;
    }

private:
    void calculate_transform() {
        float screen_aspect = static_cast<float>(m_screen_width) / m_screen_height;
        float tablet_aspect = static_cast<float>(m_tablet_width) / m_tablet_height;

        if (m_rotate90) {
            // Swap tablet dimensions for aspect calculation
            tablet_aspect = static_cast<float>(m_tablet_height) / m_tablet_width;
        }

        switch (m_mode) {
            case Mode::LETTERBOX:
                if (tablet_aspect > screen_aspect) {
                    // Tablet is wider - letterbox on sides
                    m_scale_x = screen_aspect / tablet_aspect;
                    m_scale_y = 1.0f;
                    m_offset_x = (1.0f - m_scale_x) / 2.0f;
                    m_offset_y = 0.0f;
                } else {
                    // Tablet is taller - letterbox on top/bottom
                    m_scale_x = 1.0f;
                    m_scale_y = tablet_aspect / screen_aspect;
                    m_offset_x = 0.0f;
                    m_offset_y = (1.0f - m_scale_y) / 2.0f;
                }
                break;

            case Mode::FILL:
                if (tablet_aspect > screen_aspect) {
                    // Crop sides
                    m_scale_x = 1.0f;
                    m_scale_y = tablet_aspect / screen_aspect;
                    m_offset_x = 0.0f;
                    m_offset_y = (1.0f - m_scale_y) / 2.0f;
                } else {
                    // Crop top/bottom
                    m_scale_x = screen_aspect / tablet_aspect;
                    m_scale_y = 1.0f;
                    m_offset_x = (1.0f - m_scale_x) / 2.0f;
                    m_offset_y = 0.0f;
                }
                break;

            case Mode::STRETCH:
                m_scale_x = 1.0f;
                m_scale_y = 1.0f;
                m_offset_x = 0.0f;
                m_offset_y = 0.0f;
                break;
        }
    }

    int m_screen_width = 0;
    int m_screen_height = 0;
    int m_tablet_width = 0;
    int m_tablet_height = 0;
    Mode m_mode = Mode::LETTERBOX;
    bool m_rotate90 = false;

    float m_scale_x = 1.0f;
    float m_scale_y = 1.0f;
    float m_offset_x = 0.0f;
    float m_offset_y = 0.0f;
};

}  // namespace stream_tablet

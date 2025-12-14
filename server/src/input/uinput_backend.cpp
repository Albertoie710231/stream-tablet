#include "uinput_backend.hpp"
#include "../util/logger.hpp"
#include <linux/uinput.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cmath>

namespace stream_tablet {

// Weylus uses 65535 as the max value for absolute axes
#define ABS_MAXVAL 65535

UInputBackend::UInputBackend() = default;

UInputBackend::~UInputBackend() {
    shutdown();
}

bool UInputBackend::init(int screen_width, int screen_height) {
    m_screen_width = screen_width;
    m_screen_height = screen_height;

    // Create three separate devices like Weylus
    if (!init_stylus_device()) {
        return false;
    }

    if (!init_mouse_device()) {
        destroy_stylus_device();
        return false;
    }

    if (!init_touch_device()) {
        destroy_stylus_device();
        destroy_mouse_device();
        return false;
    }

    LOG_INFO("Created uinput devices: stylus + mouse + touch (Weylus-style)");
    return true;
}

bool UInputBackend::init_stylus_device() {
    m_stylus_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (m_stylus_fd < 0) {
        LOG_ERROR("Failed to open /dev/uinput for stylus device");
        return false;
    }

    // Enable synchronization
    ioctl(m_stylus_fd, UI_SET_EVBIT, EV_SYN);

    // Direct input (like tablet)
    ioctl(m_stylus_fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    // Stylus buttons
    ioctl(m_stylus_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(m_stylus_fd, UI_SET_KEYBIT, BTN_TOOL_PEN);
    ioctl(m_stylus_fd, UI_SET_KEYBIT, BTN_TOOL_RUBBER);
    ioctl(m_stylus_fd, UI_SET_KEYBIT, BTN_TOUCH);

    // Absolute axes
    ioctl(m_stylus_fd, UI_SET_EVBIT, EV_ABS);

    struct uinput_abs_setup abs_setup = {};

    abs_setup.code = ABS_X;
    abs_setup.absinfo.minimum = 0;
    abs_setup.absinfo.maximum = ABS_MAXVAL;
    abs_setup.absinfo.resolution = 12;
    ioctl(m_stylus_fd, UI_ABS_SETUP, &abs_setup);

    abs_setup.code = ABS_Y;
    abs_setup.absinfo.maximum = ABS_MAXVAL;
    ioctl(m_stylus_fd, UI_ABS_SETUP, &abs_setup);

    abs_setup.code = ABS_PRESSURE;
    abs_setup.absinfo.minimum = 0;
    abs_setup.absinfo.maximum = ABS_MAXVAL;
    abs_setup.absinfo.resolution = 12;
    ioctl(m_stylus_fd, UI_ABS_SETUP, &abs_setup);

    abs_setup.code = ABS_TILT_X;
    abs_setup.absinfo.minimum = -90;
    abs_setup.absinfo.maximum = 90;
    abs_setup.absinfo.resolution = 12;
    ioctl(m_stylus_fd, UI_ABS_SETUP, &abs_setup);

    abs_setup.code = ABS_TILT_Y;
    abs_setup.absinfo.minimum = -90;
    abs_setup.absinfo.maximum = 90;
    ioctl(m_stylus_fd, UI_ABS_SETUP, &abs_setup);

    // Device setup
    struct uinput_setup usetup = {};
    strcpy(usetup.name, "StreamTablet Stylus");
    usetup.id.bustype = BUS_VIRTUAL;
    usetup.id.vendor = 0x1701;
    usetup.id.product = 0x1701;
    usetup.id.version = 1;

    if (ioctl(m_stylus_fd, UI_DEV_SETUP, &usetup) < 0 ||
        ioctl(m_stylus_fd, UI_DEV_CREATE) < 0) {
        LOG_ERROR("Failed to create stylus device");
        close(m_stylus_fd);
        m_stylus_fd = -1;
        return false;
    }

    usleep(50000);
    return true;
}

bool UInputBackend::init_mouse_device() {
    m_mouse_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (m_mouse_fd < 0) {
        LOG_ERROR("Failed to open /dev/uinput for mouse device");
        return false;
    }

    // Enable synchronization
    ioctl(m_mouse_fd, UI_SET_EVBIT, EV_SYN);

    // Direct input
    ioctl(m_mouse_fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    // Mouse buttons
    ioctl(m_mouse_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(m_mouse_fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(m_mouse_fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(m_mouse_fd, UI_SET_KEYBIT, BTN_MIDDLE);

    // Absolute axes for positioning
    ioctl(m_mouse_fd, UI_SET_EVBIT, EV_ABS);

    struct uinput_abs_setup abs_setup = {};

    abs_setup.code = ABS_X;
    abs_setup.absinfo.minimum = 0;
    abs_setup.absinfo.maximum = ABS_MAXVAL;
    abs_setup.absinfo.resolution = 0;
    ioctl(m_mouse_fd, UI_ABS_SETUP, &abs_setup);

    abs_setup.code = ABS_Y;
    abs_setup.absinfo.maximum = ABS_MAXVAL;
    ioctl(m_mouse_fd, UI_ABS_SETUP, &abs_setup);

    // Device setup
    struct uinput_setup usetup = {};
    strcpy(usetup.name, "StreamTablet Mouse");
    usetup.id.bustype = BUS_VIRTUAL;
    usetup.id.vendor = 0x1701;
    usetup.id.product = 0x1702;
    usetup.id.version = 1;

    if (ioctl(m_mouse_fd, UI_DEV_SETUP, &usetup) < 0 ||
        ioctl(m_mouse_fd, UI_DEV_CREATE) < 0) {
        LOG_ERROR("Failed to create mouse device");
        close(m_mouse_fd);
        m_mouse_fd = -1;
        return false;
    }

    usleep(50000);
    return true;
}

bool UInputBackend::init_touch_device() {
    m_touch_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (m_touch_fd < 0) {
        LOG_ERROR("Failed to open /dev/uinput for touch device");
        return false;
    }

    // Enable synchronization
    ioctl(m_touch_fd, UI_SET_EVBIT, EV_SYN);

    // Direct input (touchscreen, not touchpad)
    ioctl(m_touch_fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    // Touch buttons (like Weylus)
    ioctl(m_touch_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(m_touch_fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(m_touch_fd, UI_SET_KEYBIT, BTN_TOOL_FINGER);
    ioctl(m_touch_fd, UI_SET_KEYBIT, BTN_TOOL_DOUBLETAP);
    ioctl(m_touch_fd, UI_SET_KEYBIT, BTN_TOOL_TRIPLETAP);
    ioctl(m_touch_fd, UI_SET_KEYBIT, BTN_TOOL_QUADTAP);
    ioctl(m_touch_fd, UI_SET_KEYBIT, BTN_TOOL_QUINTTAP);

    // Absolute axes
    ioctl(m_touch_fd, UI_SET_EVBIT, EV_ABS);

    struct uinput_abs_setup abs_setup = {};

    abs_setup.code = ABS_X;
    abs_setup.absinfo.minimum = 0;
    abs_setup.absinfo.maximum = ABS_MAXVAL;
    abs_setup.absinfo.resolution = 200;
    ioctl(m_touch_fd, UI_ABS_SETUP, &abs_setup);

    abs_setup.code = ABS_Y;
    abs_setup.absinfo.maximum = ABS_MAXVAL;
    ioctl(m_touch_fd, UI_ABS_SETUP, &abs_setup);

    // Multi-touch axes (5 slots like Weylus)
    abs_setup.code = ABS_MT_SLOT;
    abs_setup.absinfo.minimum = 0;
    abs_setup.absinfo.maximum = 4;
    abs_setup.absinfo.resolution = 0;
    ioctl(m_touch_fd, UI_ABS_SETUP, &abs_setup);

    abs_setup.code = ABS_MT_TRACKING_ID;
    abs_setup.absinfo.minimum = 0;
    abs_setup.absinfo.maximum = 4;
    ioctl(m_touch_fd, UI_ABS_SETUP, &abs_setup);

    abs_setup.code = ABS_MT_POSITION_X;
    abs_setup.absinfo.minimum = 0;
    abs_setup.absinfo.maximum = ABS_MAXVAL;
    abs_setup.absinfo.resolution = 200;
    ioctl(m_touch_fd, UI_ABS_SETUP, &abs_setup);

    abs_setup.code = ABS_MT_POSITION_Y;
    abs_setup.absinfo.maximum = ABS_MAXVAL;
    ioctl(m_touch_fd, UI_ABS_SETUP, &abs_setup);

    abs_setup.code = ABS_MT_PRESSURE;
    abs_setup.absinfo.minimum = 0;
    abs_setup.absinfo.maximum = ABS_MAXVAL;
    abs_setup.absinfo.resolution = 0;
    ioctl(m_touch_fd, UI_ABS_SETUP, &abs_setup);

    // Device setup
    struct uinput_setup usetup = {};
    strcpy(usetup.name, "StreamTablet Touch");
    usetup.id.bustype = BUS_VIRTUAL;
    usetup.id.vendor = 0x1701;
    usetup.id.product = 0x1703;
    usetup.id.version = 1;

    if (ioctl(m_touch_fd, UI_DEV_SETUP, &usetup) < 0 ||
        ioctl(m_touch_fd, UI_DEV_CREATE) < 0) {
        LOG_ERROR("Failed to create touch device");
        close(m_touch_fd);
        m_touch_fd = -1;
        return false;
    }

    usleep(50000);
    return true;
}

void UInputBackend::destroy_stylus_device() {
    if (m_stylus_fd >= 0) {
        ioctl(m_stylus_fd, UI_DEV_DESTROY);
        close(m_stylus_fd);
        m_stylus_fd = -1;
    }
}

void UInputBackend::destroy_mouse_device() {
    if (m_mouse_fd >= 0) {
        ioctl(m_mouse_fd, UI_DEV_DESTROY);
        close(m_mouse_fd);
        m_mouse_fd = -1;
    }
}

void UInputBackend::destroy_touch_device() {
    if (m_touch_fd >= 0) {
        ioctl(m_touch_fd, UI_DEV_DESTROY);
        close(m_touch_fd);
        m_touch_fd = -1;
    }
}

void UInputBackend::emit(int fd, int type, int code, int value) {
    struct input_event ev = {};
    ev.type = type;
    ev.code = code;
    ev.value = value;
    write(fd, &ev, sizeof(ev));
}

// Transform screen coordinates to 0-65535 range
int UInputBackend::transform_coord(int val, int max) {
    return static_cast<int>((static_cast<float>(val) / max) * ABS_MAXVAL);
}

void UInputBackend::send_stylus(int x, int y, float pressure, float tilt_x, float tilt_y,
                                 bool tip_down, bool button1, bool button2, bool eraser,
                                 bool in_range) {
    if (m_stylus_fd < 0) return;

    // Suppress unused parameter warnings - buttons reserved for future use
    (void)button1;
    (void)button2;

    int abs_x = transform_coord(x, m_screen_width);
    int abs_y = transform_coord(y, m_screen_height);
    int abs_pressure = static_cast<int>(pressure * ABS_MAXVAL);

    if (in_range) {
        // Activate pen tool if not already active (for hover support)
        if (!m_stylus_tool_active && !eraser) {
            emit(m_stylus_fd, EV_KEY, BTN_TOOL_PEN, 1);
            emit(m_stylus_fd, EV_KEY, BTN_TOOL_RUBBER, 0);
            m_stylus_tool_active = true;
        }

        if (eraser && m_stylus_tool_active) {
            emit(m_stylus_fd, EV_KEY, BTN_TOOL_PEN, 0);
            emit(m_stylus_fd, EV_KEY, BTN_TOOL_RUBBER, 1);
            m_stylus_tool_active = false;
        }

        // Handle touch state transitions
        if (tip_down && !m_stylus_touching) {
            // Pen touched down
            m_stylus_touching = true;
            emit(m_stylus_fd, EV_KEY, BTN_TOUCH, 1);
        } else if (!tip_down && m_stylus_touching) {
            // Pen lifted but still hovering
            m_stylus_touching = false;
            emit(m_stylus_fd, EV_KEY, BTN_TOUCH, 0);
        }

        // Always send position (for both hover and touch)
        emit(m_stylus_fd, EV_ABS, ABS_X, abs_x);
        emit(m_stylus_fd, EV_ABS, ABS_Y, abs_y);
        emit(m_stylus_fd, EV_ABS, ABS_PRESSURE, m_stylus_touching ? abs_pressure : 0);
        emit(m_stylus_fd, EV_ABS, ABS_TILT_X, static_cast<int>(tilt_x));
        emit(m_stylus_fd, EV_ABS, ABS_TILT_Y, static_cast<int>(tilt_y));
    } else {
        // Stylus leaving range - release everything
        if (m_stylus_touching) {
            emit(m_stylus_fd, EV_KEY, BTN_TOUCH, 0);
            m_stylus_touching = false;
        }
        if (m_stylus_tool_active) {
            emit(m_stylus_fd, EV_KEY, BTN_TOOL_PEN, 0);
            m_stylus_tool_active = false;
        }
        emit(m_stylus_fd, EV_KEY, BTN_TOOL_RUBBER, 0);
        emit(m_stylus_fd, EV_ABS, ABS_PRESSURE, 0);
    }

    emit(m_stylus_fd, EV_SYN, SYN_REPORT, 0);
}

void UInputBackend::send_touch(int x, int y, int slot, bool down, float pressure) {
    if (m_touch_fd < 0 || slot < 0 || slot >= 5) return;

    int abs_x = transform_coord(x, m_screen_width);
    int abs_y = transform_coord(y, m_screen_height);
    int abs_pressure = static_cast<int>(pressure * ABS_MAXVAL);

    // Select slot
    emit(m_touch_fd, EV_ABS, ABS_MT_SLOT, slot);

    if (down) {
        // Check if this is a new touch or move
        bool was_down = m_touch_slots[slot].active;

        if (!was_down) {
            // New touch - assign tracking ID
            m_touch_slots[slot].active = true;
            m_touch_slots[slot].tracking_id = slot;
            emit(m_touch_fd, EV_ABS, ABS_MT_TRACKING_ID, slot);

            // Send BTN_TOUCH and appropriate BTN_TOOL_*
            emit(m_touch_fd, EV_KEY, BTN_TOUCH, 1);

            // Update tool buttons based on active touch count
            int active_count = 0;
            for (int i = 0; i < 5; i++) {
                if (m_touch_slots[i].active) active_count++;
            }

            // Clear previous tool, set new one (like Weylus)
            switch (active_count - 1) {
                case 1: emit(m_touch_fd, EV_KEY, BTN_TOOL_FINGER, 0); break;
                case 2: emit(m_touch_fd, EV_KEY, BTN_TOOL_DOUBLETAP, 0); break;
                case 3: emit(m_touch_fd, EV_KEY, BTN_TOOL_TRIPLETAP, 0); break;
                case 4: emit(m_touch_fd, EV_KEY, BTN_TOOL_QUADTAP, 0); break;
                default: emit(m_touch_fd, EV_KEY, BTN_TOOL_QUINTTAP, 0); break;
            }
            switch (active_count) {
                case 1: emit(m_touch_fd, EV_KEY, BTN_TOOL_FINGER, 1); break;
                case 2: emit(m_touch_fd, EV_KEY, BTN_TOOL_DOUBLETAP, 1); break;
                case 3: emit(m_touch_fd, EV_KEY, BTN_TOOL_TRIPLETAP, 1); break;
                case 4: emit(m_touch_fd, EV_KEY, BTN_TOOL_QUADTAP, 1); break;
                default: emit(m_touch_fd, EV_KEY, BTN_TOOL_QUINTTAP, 1); break;
            }
        }

        // Send position
        emit(m_touch_fd, EV_ABS, ABS_MT_PRESSURE, abs_pressure);
        emit(m_touch_fd, EV_ABS, ABS_MT_POSITION_X, abs_x);
        emit(m_touch_fd, EV_ABS, ABS_MT_POSITION_Y, abs_y);
        emit(m_touch_fd, EV_ABS, ABS_X, abs_x);
        emit(m_touch_fd, EV_ABS, ABS_Y, abs_y);
    } else {
        // Touch up - release tracking ID
        emit(m_touch_fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
        emit(m_touch_fd, EV_KEY, BTN_TOUCH, 0);
        emit(m_touch_fd, EV_KEY, BTN_TOOL_FINGER, 0);

        // Clear appropriate BTN_TOOL based on previous count
        int active_count = 0;
        for (int i = 0; i < 5; i++) {
            if (m_touch_slots[i].active) active_count++;
        }
        switch (active_count) {
            case 2: emit(m_touch_fd, EV_KEY, BTN_TOOL_DOUBLETAP, 0); break;
            case 3: emit(m_touch_fd, EV_KEY, BTN_TOOL_TRIPLETAP, 0); break;
            case 4: emit(m_touch_fd, EV_KEY, BTN_TOOL_QUADTAP, 0); break;
            case 5: emit(m_touch_fd, EV_KEY, BTN_TOOL_QUINTTAP, 0); break;
        }

        m_touch_slots[slot].active = false;
        m_touch_slots[slot].tracking_id = -1;
    }

    emit(m_touch_fd, EV_SYN, SYN_REPORT, 0);
}

void UInputBackend::reset_all() {
    // Reset stylus
    if (m_stylus_fd >= 0) {
        emit(m_stylus_fd, EV_KEY, BTN_TOUCH, 0);
        emit(m_stylus_fd, EV_KEY, BTN_TOOL_PEN, 0);
        emit(m_stylus_fd, EV_KEY, BTN_TOOL_RUBBER, 0);
        emit(m_stylus_fd, EV_ABS, ABS_PRESSURE, 0);
        emit(m_stylus_fd, EV_SYN, SYN_REPORT, 0);
    }

    // Reset mouse
    if (m_mouse_fd >= 0) {
        emit(m_mouse_fd, EV_KEY, BTN_LEFT, 0);
        emit(m_mouse_fd, EV_KEY, BTN_RIGHT, 0);
        emit(m_mouse_fd, EV_KEY, BTN_MIDDLE, 0);
        emit(m_mouse_fd, EV_SYN, SYN_REPORT, 0);
    }

    // Reset touch
    if (m_touch_fd >= 0) {
        for (int slot = 0; slot < 5; slot++) {
            if (m_touch_slots[slot].active) {
                emit(m_touch_fd, EV_ABS, ABS_MT_SLOT, slot);
                emit(m_touch_fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
                m_touch_slots[slot].active = false;
                m_touch_slots[slot].tracking_id = -1;
            }
        }
        emit(m_touch_fd, EV_KEY, BTN_TOUCH, 0);
        emit(m_touch_fd, EV_KEY, BTN_TOOL_FINGER, 0);
        emit(m_touch_fd, EV_KEY, BTN_TOOL_DOUBLETAP, 0);
        emit(m_touch_fd, EV_KEY, BTN_TOOL_TRIPLETAP, 0);
        emit(m_touch_fd, EV_KEY, BTN_TOOL_QUADTAP, 0);
        emit(m_touch_fd, EV_KEY, BTN_TOOL_QUINTTAP, 0);
        emit(m_touch_fd, EV_SYN, SYN_REPORT, 0);
    }

    m_stylus_tool_active = false;
    m_stylus_touching = false;

    LOG_DEBUG("Reset all input state");
}

void UInputBackend::sync() {
    // Each device syncs itself after events
}

void UInputBackend::shutdown() {
    reset_all();
    destroy_touch_device();
    destroy_mouse_device();
    destroy_stylus_device();
    LOG_INFO("Destroyed uinput devices");
}

}  // namespace stream_tablet

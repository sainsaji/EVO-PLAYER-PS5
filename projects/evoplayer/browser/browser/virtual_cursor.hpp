// Prospero Browser - WPE WebKit Port for PlayStation 5
// DualSense Virtual Cursor Physics and State.
// SPDX-License-Identifier: BSD-2-Clause / GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <algorithm>

namespace prospero::browser {

enum class CursorShape {
    Arrow,
    Pointer,    // Hovering link / button
    Text,       // Over input field
    Grab,
};

class VirtualCursor {
public:
    VirtualCursor();

    void set_bounds(float min_x, float min_y, float max_x, float max_y);
    void set_position(float x, float y);

    // Update with left analog stick input [-1.0f .. 1.0f], delta time in seconds
    void update_stick(float stick_x, float stick_y, float dt);

    // Touchpad delta update
    void update_touchpad(float delta_x, float delta_y);

    // Button states
    void set_button_down(bool down);

    // Shape
    void set_shape(CursorShape shape) { shape_ = shape; }
    CursorShape shape() const { return shape_; }

    // Coordinates
    float x() const { return pos_x_; }
    float y() const { return pos_y_; }
    int32_t screen_x() const { return static_cast<int32_t>(pos_x_); }
    int32_t screen_y() const { return static_cast<int32_t>(pos_y_); }

    bool is_button_down() const { return button_down_; }
    bool is_moving() const { return is_moving_; }
    float speed_multiplier() const { return speed_multiplier_; }
    void set_speed_multiplier(float mul) { speed_multiplier_ = std::clamp(mul, 0.2f, 3.0f); }

    // Click pulse animation factor (0.0 .. 1.0)
    float click_pulse() const { return click_pulse_; }
    void update_animation(float dt);

private:
    float pos_x_{960.0f};
    float pos_y_{540.0f};
    float min_x_{0.0f};
    float min_y_{0.0f};
    float max_x_{1920.0f};
    float max_y_{1080.0f};

    float speed_multiplier_{1.0f};
    float base_speed_{900.0f}; // pixels per second at max deflection
    float deadzone_{0.12f};

    bool button_down_{false};
    bool is_moving_{false};
    CursorShape shape_{CursorShape::Arrow};

    float click_pulse_{0.0f};
};

} // namespace prospero::browser

// Prospero Browser - WPE WebKit Port for PlayStation 5
// DualSense Virtual Cursor Physics and State implementation.
// SPDX-License-Identifier: BSD-2-Clause / GPL-3.0-or-later

#include "virtual_cursor.hpp"
#include <cmath>

namespace prospero::browser {

VirtualCursor::VirtualCursor() = default;

void VirtualCursor::set_bounds(float min_x, float min_y, float max_x, float max_y) {
    min_x_ = min_x;
    min_y_ = min_y;
    max_x_ = max_x;
    max_y_ = max_y;
    pos_x_ = std::clamp(pos_x_, min_x_, max_x_);
    pos_y_ = std::clamp(pos_y_, min_y_, max_y_);
}

void VirtualCursor::set_position(float x, float y) {
    pos_x_ = std::clamp(x, min_x_, max_x_);
    pos_y_ = std::clamp(y, min_y_, max_y_);
}

void VirtualCursor::update_stick(float stick_x, float stick_y, float dt) {
    float length = std::sqrt(stick_x * stick_x + stick_y * stick_y);
    if (length < deadzone_) {
        is_moving_ = false;
        return;
    }

    // Radial deadzone remapping
    float normalized_mag = (length - deadzone_) / (1.0f - deadzone_);
    normalized_mag = std::clamp(normalized_mag, 0.0f, 1.0f);

    // Exponential acceleration curve for sub-pixel precision near center
    float curved_speed = normalized_mag * normalized_mag;
    float dir_x = stick_x / length;
    float dir_y = stick_y / length;

    float move_dist = curved_speed * base_speed_ * speed_multiplier_ * dt;
    pos_x_ += dir_x * move_dist;
    pos_y_ += dir_y * move_dist;

    pos_x_ = std::clamp(pos_x_, min_x_, max_x_);
    pos_y_ = std::clamp(pos_y_, min_y_, max_y_);

    is_moving_ = true;
}

void VirtualCursor::update_touchpad(float delta_x, float delta_y) {
    pos_x_ += delta_x * speed_multiplier_;
    pos_y_ += delta_y * speed_multiplier_;
    pos_x_ = std::clamp(pos_x_, min_x_, max_x_);
    pos_y_ = std::clamp(pos_y_, min_y_, max_y_);
}

void VirtualCursor::set_button_down(bool down) {
    if (down && !button_down_) {
        // Trigger click pulse animation
        click_pulse_ = 1.0f;
    }
    button_down_ = down;
}

void VirtualCursor::update_animation(float dt) {
    if (click_pulse_ > 0.0f) {
        click_pulse_ = std::max(0.0f, click_pulse_ - dt * 6.0f);
    }
}

} // namespace prospero::browser

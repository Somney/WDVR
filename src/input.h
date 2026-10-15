// Stage 4: Vive wands as an Xbox controller.
// Watch Dogs reads the gamepad through XInput; we hook XInputGetState and merge
// the wands into controller #0 (a real gamepad keeps working alongside).
#pragma once
#include "vr.h"

namespace wdvr::input {

// Call every frame: installs the hook once XInput is loaded, and publishes the
// latest wand state for the game's input thread.
void update(const VrState& vr, bool enabled);

struct Status {
    bool hooked = false;
    char dll[32] = "";
    unsigned game_reads = 0;      // how often the game asked for controller input
    bool real_pad = false;        // a physical controller is also connected
    // last values sent to the game (for the menu)
    unsigned short buttons = 0;
    unsigned char lt = 0, rt = 0;
    short lx = 0, ly = 0, rx = 0, ry = 0;
};
Status status();

// Tuning
struct Options {
    float deadzone = 0.15f;       // trackpad centre ignored
    bool  move_needs_touch = true;
    float turn_speed = 1.0f;      // right trackpad horizontal -> right stick scale
};
Options& options();
// Replace the right stick sent to the game (-1..1): the mod steers the game's camera.
void set_right_stick_override(bool on, float x, float y);
// What the player is doing with the right stick / right trackpad, left-right (-1..1), even while overridden.
float user_right_x();
// Press controller buttons (XINPUT_GAMEPAD_* bits) for 'ms' milliseconds, on top of everything else.
void inject_buttons(unsigned short buttons, unsigned ms);
// The controller button (bit number 0-15) the game most recently saw go down within 'within_ms', or -1.
int last_button_pressed(unsigned long long within_ms, unsigned long long* when);

} // namespace wdvr::input

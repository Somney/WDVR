// Finding and controlling Watch Dogs' render camera, Aiden's head and his bones.
// Camera layout and code locations come from Otis_Inf's Watch_Dogs camera
// (InjectableGenericCameraSystem, BSD-2-Clause, (c) Frans Bouma).
#pragma once
#include <cstdint>

namespace wdvr::camera {

struct Status {
    bool      engine_found = false;     // Disrupt_b64.dll located
    uintptr_t engine_base = 0;
    uint32_t  engine_size = 0;
    uint32_t  engine_stamp = 0;         // PE TimeDateStamp: identifies the game build
    bool      hooks_ok = false;         // all code patches installed
    char      message[192] = "Waiting for the game to start drawing";
};

const Status& status();

// Starts the background search + hook install (call once the game is rendering).
void begin_setup();

// Live camera data (both copies the game keeps). nullptr until found.
uintptr_t struct_a();
uintptr_t struct_b();

// Head look: the headset turns the game camera (the game's own rotation writes are blocked).
void set_head_look(bool on);
bool head_look();
// Called every frame with the headset angles in degrees.
void update_head_look(float head_yaw, float head_pitch, float head_roll);
// Point the next frame exactly where the game itself aims (its own camera direction) - the scope picture.
void look_along_game_aim();

// Conventions (found on real hardware).
struct Tuning {
    float yaw_offset = 0.0f;   // degrees
    bool  invert_yaw = false, invert_pitch = true, invert_roll = false;   // pitch confirmed inverted on real hardware
    bool  use_roll = true;
    bool  follow_game = true;  // "straight ahead" = where the game's camera faces
};
Tuning& tuning();
// Field of view to force while head look is on (radians, 0 = leave the game's).
void set_fov_override(float radians);
// How often the camera's FOV fields held something other than what we last wrote (someone else changed them);
// seen = the last such values (+24 current, +28 target, radians).
unsigned fov_changed(float seen[2]);
// Rotation lag: write each request's rotation one request late (the game applies struct writes a frame sooner than
// the position). set_request = the request number the next look write belongs to.
void set_rot_lag(bool on);
void set_request(unsigned idx);
void recenter(float head_yaw);

// Stereo: available once the position write is hooked.
bool stereo_available();
unsigned long long position_writes();
// Offset (world units) added to the camera position for the next frame drawn.
void set_eye_offset(bool on, const float off[3]);
// Camera's right-pointing axis in world space (from the rotation we last wrote).
void camera_right(float out[3]);
// Where the view looks (world space, unit), from the rotation we last wrote.
void view_forward(float out[3]);
// The rotation we last wrote: rows = forward, up, right (world space).
void last_look(float m[3][3]);

// Following the game's own camera direction (captured while we block its writes).
bool  game_rot_valid();
float game_yaw_deg();
float game_pitch_deg();       // same convention update_head_look uses
bool  game_forward(float out[3]);
// World-space forward/right/up of the current "straight ahead" (yaw only).
void  base_frame(float fwd[3], float right[3], float up[3]);
// Keep "straight ahead" at a fixed world yaw instead of following the game's camera
// (on foot: the head steers Aiden, so the view must not follow the camera it steers).
void  set_base_yaw_override(bool on, float yaw_deg);

// First-person anchor: camera position = Aiden's head + off (world units).
// Returns false (and stays off) if the address isn't valid memory.
// Where the first-person anchor last put the camera (written by the game's camera update).
bool  anchored_camera_pos(float out[3]);
bool  set_anchor(uintptr_t player_addr, const float off[3]);
// Where the game itself wanted the camera this frame.
bool  game_camera_pos(float out[3]);
// The field of view the game wants (radians, horizontal) - changes when it zooms (scopes, hacked cameras).
float game_fov();

// Aiden auto-find: the game's head-position writer is hooked; these are the character
// objects whose head was written most recently (newest first). Head = object + kHeadOffset.
constexpr uintptr_t kHeadOffset = 0x8AC;
bool      head_hook_available();
size_t    recent_head_objects(uintptr_t* out, size_t max);
unsigned  head_writes();
uintptr_t rig_for(uintptr_t obj);    // the character's rig object (r12 at the head writer)

// Bone overrides: the game's per-bone store is hooked; for the chosen bone list, bones mapped
// to a slot get our transform (quaternion x,y,z,w + position x,y,z,0, model space).
bool     bone_hooks_available();
void     set_bone_override_target(uintptr_t bones);
void     set_bone_override_enabled(bool on);
void     set_bone_slot(int bone, int slot);      // slot -1 = release
float*   bone_slot_data(int slot);
const float* bone_slot_raw(int slot);            // the game's value from its last store
unsigned bone_raw_seq();

} // namespace wdvr::camera

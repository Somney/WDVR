// SteamVR (OpenVR) connection. Implemented in C against OpenVR's plain-C
// function tables, which work with any compiler.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int    connected;      // device is plugged in / tracked by SteamVR
    int    valid;          // pose is valid this frame
    float  pos[3];         // metres, SteamVR standing space
    float  yaw, pitch, roll; // degrees
    float  m[3][4];        // raw device-to-world matrix
    int    is_controller;
    int    role;           // 1 = left hand, 2 = right hand, 0 = unknown
    uint64_t buttons;      // pressed-button bitmask (legacy input)
    uint64_t touched;      // touched-button bitmask (trackpad touch = bit 32)
    float  trackpad[2];    // -1..1
    float  trigger;        // 0..1
    char   model[64];
} VrDevice;

#define VR_MAX_DEVICES 16

typedef struct {
    int      ok;
    char     status[160];
    VrDevice hmd;
    VrDevice left, right;
    VrDevice all[VR_MAX_DEVICES];
} VrState;

// Loads openvr_api.dll from the mod folder and connects as a background app
// (SteamVR must already be running). Returns 1 on success.
int  vr_init(const char* mod_dir);
// Re-connects as a Scene app: we own the headset display and must submit frames.
int  vr_init_scene(const char* mod_dir);
int  vr_is_scene(void);

typedef struct { float u0, v0, u1, v1; } VrBounds;
// Blocks until SteamVR wants the next frame (paces the game to the headset). 0 = OK.
int  vr_wait_poses(void);
// Sends one D3D11 texture to both eyes, cropped by 'left'/'right'. 'pose' = the
// head pose the image was rendered with (or NULL). Returns 0 = OK.
int  vr_submit(void* d3d11_texture, const float pose[3][4], const VrBounds* left, const VrBounds* right);
// Sends a texture to one eye (0 = left, 1 = right).
int  vr_submit_eye(int eye, void* d3d11_texture, const float pose[3][4], const VrBounds* bounds);
// Floating flat screen (menus/map): placed 'distance' m in front of the head pose
// 'hmd_m' (yaw only), 'width' m wide. Update its picture each frame with vr_screen_update.
int  vr_screen_show(void* d3d11_texture, const float hmd_m[3][4], float distance, float width);
void vr_screen_update(void* d3d11_texture);
void vr_screen_hide(void);
// The game's HUD (with transparency) as a panel locked 'distance' m in front of the head.
int  vr_hud_show(void* d3d11_texture, float distance, float width, float down);
void vr_hud_hide(void);
// A piece of the HUD held on top of a wand: hand 0 = left (phone), 1 = right (weapon wheel).
int  vr_hand_show(int hand, void* d3d11_texture, float width, float tilt_deg, float up, float back);
void vr_hand_hide(int hand);
// The rifle scope picture on the right wand, facing back toward your eye.
int  vr_scope_show(void* d3d11_texture, float width, float up, float back, float tilt_deg);
void vr_scope_hide(void);
// The red aim dot: placed at 'pos' (standing space, metres), facing 'head', 'width' metres wide.
int  vr_dot_show(void* d3d11_texture, const float pos[3], const float head[3], float width);
void vr_dot_hide(void);
// Per-eye field of view as tangents (OpenVR GetProjectionRaw). eye 0 = left.
void vr_eye_tangents(int eye, float* l, float* r, float* t, float* b);
void vr_shutdown(void);
void vr_poll(VrState* out);   // call once per frame

#ifdef __cplusplus
}
#endif

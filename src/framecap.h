// HUD split: the game draws its finished picture into one full-screen texture, then
// its HUD on top, then copies that to the screen. We send the HUD draws into our own
// see-through texture, so the headset gets a clean 3D picture and the HUD can be shown
// on floating panels; the middle part (markers that sit on the world) is put back.
#pragma once
#include <d3d11.h>

namespace wdvr::framecap {

// Call at the very start of Present / right before handing the frame to the real Present.
void on_present_begin(ID3D11Resource* backbuffer);
void on_present_end();

void hud_enable(ID3D11DeviceContext* ctx, bool on);   // draw hooks are installed the first time
bool hud_active();
ID3D11Texture2D* hud_texture();
ID3D11ShaderResourceView* hud_srv();
const char* hud_status();
// The part of the screen (0..1) where the HUD stays in the 3D picture; everything outside goes on the panel.
void hud_world_area(bool on, float x0, float y0, float x1, float y1);
// An area that is never put back into the world (the phone / weapon wheel - they go on the hands).
void hud_exclude_area(bool on, float x0, float y0, float x1, float y1);
// A second area that is never put back into the world (the game's crosshair at the screen centre).
void hud_exclude_area2(bool on, float x0, float y0, float x1, float y1);

// The game's main scene depth buffer (full resolution, most draws per frame), or nullptr.
ID3D11Resource* scene_depth();

// Frame ID. Call frame_tag when writing the camera for request 'idx' with field of view 'fov' (radians, already tagged).
// frame_shown (at Present): 1 = the frame being presented was drawn for request *idx, 0 = not known for this frame,
// -1 = frame ID isn't working (yet) - fall back to guessing.
void hooks_install(ID3D11DeviceContext* ctx);
void frame_tag(unsigned idx, float fov, float aspect);
int  frame_shown(unsigned* idx);
bool frame_id_locked();
const char* frame_id_status();
// True while frames carry no tag (lost / searching / missing for a moment).
bool frame_id_missing();
// Rotation check: record each request's look matrix (9 floats); rot_report = which request the drawn rotation came from.
void rot_tag(unsigned idx, const float m[9]);
void rot_report(char* out, size_t n);
// The horizontal field of view (degrees) the game actually drew the last frame with, read at the spot that held the tag.
bool probe_fov(float* deg, char* where, size_t n);

} // namespace wdvr::framecap

// WDVR - Watch Dogs VR mod (Ultimate ASI Loader plugin).
//
// Every frame (hooked Present):
//   * the finished game frame goes to the Vive (alternate-eye 3D, head-tracked camera);
//   * on foot the camera sits in Aiden's head; the game's own camera (which decides where he
//     walks, faces and shoots) is steered to where you look - or, while aiming, to where the
//     right wand points - and his body turns with your head;
//   * his arms follow the wands (2-bone IK written into the game's own bone evaluation);
//   * the HUD is split off: world markers stay in the 3D picture, the rest floats on a panel,
//     the phone sits on the left wand, the weapon wheel on the right, menus/maps on a big screen.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cfloat>
#include <string>
#include <vector>
#include <algorithm>

#include "MinHook.h"
#include "framecap.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "log.h"
#include "vr.h"
#include "camera.h"
#include "input.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#define WDVR_VERSION "0.16.14"

namespace wdvr {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static HMODULE              g_self;
static char                 g_mod_dir[MAX_PATH];
static PresentFn            g_orig_present;
static ResizeBuffersFn      g_orig_resize;
static IDXGISwapChain*      g_swapchain;
static ID3D11Device*        g_device;
static ID3D11DeviceContext* g_context;
static ID3D11RenderTargetView* g_rtv;
static HWND                 g_hwnd;
static WNDPROC              g_orig_wndproc;
static bool                 g_imgui_ready, g_menu_open;
static ULONGLONG            g_start_tick;
static UINT                 g_width, g_height;
static VrState              g_vr;

static void save_config();

// ---------------------------------------------------------------- small helpers
static bool safe_read(uintptr_t addr, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, out, n, &got) && got == n;
}
static uintptr_t rd64(uintptr_t a) { uintptr_t v = 0; safe_read(a, &v, 8); return v; }
static float wrap180(float a) { while (a > 180) a -= 360; while (a < -180) a += 360; return a; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static DWORD WINAPI beep_thread(LPVOID p) { uintptr_t v = (uintptr_t)p; Beep((DWORD)(v >> 16), (DWORD)(v & 0xFFFF)); return 0; }
static void beep(int freq, int ms) {   // audible cues (you can't see the monitor in the headset)
    if (HANDLE t = CreateThread(nullptr, 0, beep_thread, (LPVOID)(((uintptr_t)freq << 16) | (uintptr_t)ms), 0, nullptr)) CloseHandle(t);
}
static bool key_pressed(int vk) {      // edge-triggered, only while the game window has focus
    static bool down[256];
    bool d = (GetAsyncKeyState(vk) & 0x8000) != 0 && GetForegroundWindow() == g_hwnd;
    bool hit = d && !down[vk & 255];
    down[vk & 255] = d;
    return hit;
}

// Vectors and quaternions (x, y, z, w).
static float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const float* a, const float* b, float* o) {
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
static float norm3(float* v) { float l = sqrtf(dot3(v, v)); if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; } return l; }
static void quat_mul(const float* a, const float* b, float* o) {
    float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}
static void qconj(const float* q, float* o) { o[0] = -q[0]; o[1] = -q[1]; o[2] = -q[2]; o[3] = q[3]; }
static void qrot(const float* q, const float* v, float* o) {        // rotate v by unit quaternion q
    float tx = 2 * (q[1] * v[2] - q[2] * v[1]), ty = 2 * (q[2] * v[0] - q[0] * v[2]), tz = 2 * (q[0] * v[1] - q[1] * v[0]);
    float x = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    float y = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    float z = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
    o[0] = x; o[1] = y; o[2] = z;
}
static void qunrot(const float* q, const float* v, float* o) { float c[4]; qconj(q, c); qrot(c, v, o); }
// Rotation taking the local pair (a, s) onto the target pair (A, S): a -> A exactly, s -> S as close as possible.
static void frame_quat(const float* a, const float* s, const float* A, const float* S, float* q) {
    auto basis = [](const float* x, const float* y, float m[3][3]) {
        float e0[3] = { x[0], x[1], x[2] }; norm3(e0);
        float k = dot3(y, e0);
        float e1[3] = { y[0] - e0[0] * k, y[1] - e0[1] * k, y[2] - e0[2] * k };
        if (norm3(e1) < 1e-4f) {                                        // y parallel to x: any perpendicular
            float t[3] = { fabsf(e0[2]) < 0.9f ? 0.0f : 1.0f, 0, fabsf(e0[2]) < 0.9f ? 1.0f : 0.0f };
            cross3(t, e0, e1); norm3(e1);
        }
        float e2[3]; cross3(e0, e1, e2);
        for (int i = 0; i < 3; i++) { m[i][0] = e0[i]; m[i][1] = e1[i]; m[i][2] = e2[i]; }   // columns
    };
    float L[3][3], W[3][3], R[3][3];
    basis(a, s, L); basis(A, S, W);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) R[i][j] = W[i][0] * L[j][0] + W[i][1] * L[j][1] + W[i][2] * L[j][2];
    float tr = R[0][0] + R[1][1] + R[2][2];
    if (tr > 0) { float k = sqrtf(tr + 1) * 2; q[3] = 0.25f * k; q[0] = (R[2][1] - R[1][2]) / k; q[1] = (R[0][2] - R[2][0]) / k; q[2] = (R[1][0] - R[0][1]) / k; }
    else if (R[0][0] > R[1][1] && R[0][0] > R[2][2]) { float k = sqrtf(1 + R[0][0] - R[1][1] - R[2][2]) * 2; q[3] = (R[2][1] - R[1][2]) / k; q[0] = 0.25f * k; q[1] = (R[0][1] + R[1][0]) / k; q[2] = (R[0][2] + R[2][0]) / k; }
    else if (R[1][1] > R[2][2]) { float k = sqrtf(1 + R[1][1] - R[0][0] - R[2][2]) * 2; q[3] = (R[0][2] - R[2][0]) / k; q[0] = (R[0][1] + R[1][0]) / k; q[1] = 0.25f * k; q[2] = (R[1][2] + R[2][1]) / k; }
    else { float k = sqrtf(1 + R[2][2] - R[0][0] - R[1][1]) * 2; q[3] = (R[1][0] - R[0][1]) / k; q[0] = (R[0][2] + R[2][0]) / k; q[1] = (R[1][2] + R[2][1]) / k; q[2] = 0.25f * k; }
}

// ---------------------------------------------------------------- state
// Headset display
static bool             g_hmd_display;
static char             g_hmd_msg[160] = "Off";
static ID3D11Texture2D* g_eye_tex;            // copy of the finished frame
static float            g_pose_ring[8][3][4]; // head poses we wrote into the camera, newest last
static int              g_eye_ring[8];        // which eye each pose was for (-1 = mono)
static unsigned         g_pose_count;
static bool             g_have_pose;
static int              g_frame_delay = 2;    // frames between writing a pose and that image reaching Present
// The image reaching Present was drawn with the camera we wrote 2 frames earlier (0.14.0: a frame drawn from the game's
// own camera showed up in the left eye only -> labels were one frame off; that is also why "swap eyes" used to be needed).
static bool             g_stereo = true, g_swap_eyes = false;
static float            g_eye_sep = 0.064f;
static ID3D11Texture2D* g_eye_img[2];         // latest image for each eye
static float            g_eye_pose[2][3][4];
static bool             g_eye_valid[2];
static int              g_monitor_mode = 1;   // 0 = raw game (flickers in 3D), 1 = left eye, 2 = left eye as the headset sees it
static VrBounds         g_bounds[2];
static float            g_fov_written;        // radians (horizontal)
static float            g_fov_scale = 1.0f;
static int              g_submit_err, g_wait_err, g_err_logs;
// Head tracking origin (set by recenter)
static float            g_head_origin[3], g_head_origin_yaw;
static bool             g_have_origin, g_need_recenter;
// First-person: anchored to Aiden's head (found through the hooked head writer)
static uintptr_t        g_aiden_obj, g_aiden_addr;   // character object, its head position (object + 0x8AC)
static ULONGLONG        g_aiden_seen;
static float            g_fp_off[3] = { 0.12f, 0.0f, 0.05f };        // on foot: forward, right, up from the head
static float            g_fp_off_car[3] = { 0.14f, -0.06f, -0.14f }; // in a vehicle
static bool             g_fp_lean;            // allow a small (15 cm) lean
static float            g_fp_lead = 0.0f;     // seconds of motion added ahead (speed lag fix) - 0.16.3: measured 0 lag, off by default
static float            g_aiden_vel[2];
static bool             g_in_vehicle, g_remote_view, g_remote_force;
// Driving: alternate-eye 3D shows each eye a different moment (the car moves ~30 cm between them at speed, and a frame
// arriving a frame early/late makes an eye jump) -> by default every frame goes to both eyes in a vehicle: smooth, flat.
static bool             g_car_3d = true;
static bool             g_car_frame = true;   // in a vehicle the view turns with the car itself, not the game's chase camera
static float            g_car_axis = 999; static bool g_car_axis_ok;   // learned: car forward = body axis + this (saved)
static bool             g_auto_cockpit = true; // pick the game's first-person car camera by itself (0.16.9)
static int              g_cam_button = 3;     // change-camera button: 1..16 controller bit + 1 (3 = D-pad left), 1000 + vk key
static const char* cam_button_name(int b, char* buf, size_t n);
static bool             g_use_cockpit = true;  // the game's own first-person car camera: use its position (0.16.8)
static bool             g_cockpit;            // ... and it is active right now
static bool             g_cockpit_held;       // ... but pulled away from the driver's eyes (slow motion): view held there
static float            g_cockpit_rel[3];     // its usual spot relative to Aiden's head (world axes, this frame)
static bool             g_steady_seat = true; // in a vehicle the eye is held at a fixed seat point (ignores Aiden's driving animation)
static float            g_car_off; static bool g_car_off_ok;   // car body axis -> "forward" (learned while driving)
static float            g_anchor_off_last[3]; // the anchor offset written last (lag measurement)
static float            g_cam_dist, g_cam_angle;   // game camera vs Aiden's head (view detection)
static float            g_eye_world[3]; static bool g_have_eye;   // where your eyes are in the game (last frame)
static float            g_aim_dist = 20.0f;   // the wand's aim line and the game's aim line cross this far away (fallback)
// Automatic aim distance: while aiming, a few frames are drawn from the game's own aim camera; the depth under its
// crosshair says how far the bullet would go. Turning depth values into metres needs the camera's near plane, which
// is learned once from the ground below the crosshair and saved for good.
static bool             g_aim_auto = true;
static float            g_depth_near;          // learned, saved
static int              g_depth_model;         // 0 not learned, 1 standard depth (z = n/(1-d)), 2 reversed (z = n/d)
static float            g_aim_live;            // measured distance to what the game's aim hits (0 = none)
static ULONGLONG        g_aim_live_t;
static float            g_aim_hit[3];
static int              g_depth_samples;
static bool             g_aiming;             // LT / left trigger
// 0.16.12: aim line of a wand held like a pistol grip. The Vive wand's own forward axis comes out of the ring at the
// tip, which points up when the wrist is relaxed; VR shooters tilt the barrel down toward the underside (trigger
// side) so you aim with a natural wrist. aim_tilt = how far down (degrees) from the tip axis.
static float            g_aim_tilt = 40.0f;
static void wand_aim_dir(const VrDevice& d, float out[3]) {   // tracking space, unit
    float a = g_aim_tilt * 0.0174532925f, c = cosf(a), sn = sinf(a);
    for (int i = 0; i < 3; i++) out[i] = -d.m[i][2] * c - d.m[i][1] * sn;
}
static bool             g_wand_input = true;
// Menus / maps: the game on a big floating screen
static bool             g_screen_mode, g_screen_auto = true, g_screen_auto_entered;
static float            g_screen_dist = 2.0f, g_screen_width = 3.0f;
static ID3D11Texture2D* g_black_tex;
static bool             g_menu_full;          // the HUD covers most of the screen (pause menu, map)
static char             g_vr_stall[200];
// Zoom: rifle scopes get their own picture on the right wand; hacked cameras zoom the whole view.
static bool             g_scope_on = true;
static float            g_scope_size = 0.14f, g_scope_up = 0.06f, g_scope_back = 0.12f, g_scope_fov_max = 35.0f;
static bool             g_scoped, g_scope_shown;
static int              g_next_type = -1;     // what the next game frame is: -1 mono, 0/1 left/right eye, 2 the game's own view
static int              g_shown_type = -1;    // what the frame being presented now was (-9 = not sure: next to a type change)
static unsigned         g_shown_idx;          // its request index
static float            g_req_fov;            // field of view of the current request (with its tag)
static bool             g_rot_lag = true;     // same for the rotation (0.16.4: it is also written straight into the struct)
static int              g_phone_cycle = 16;   // phone open: 3 game-view frames in every N (wrist picture), the rest headset
static bool             g_fov_lag = true;     // write each request's FOV one request late (the game applies FOV a frame later)
static bool             g_shown_mid_g;        // the frame shown now is a game-view frame with game-view requests on both sides
static bool             g_phone_live;         // phone open and the headset keeps running (game-view frames mixed in)
// Each frame request: the camera it was drawn with (for reading its depth buffer) and the wand ray at that moment.
struct FrameCam { bool ok; float pos[3], m[3][3], fov, wand_o[3], wand_d[3], ground; bool gok; float gpos[3], gfwd[3]; };
static FrameCam         g_frame_cam[8];
static void scope_frame();
static float            g_aspect = 16.0f / 9.0f;  // back buffer width / height
static unsigned         g_fid_hist[5], g_fid_unknown;   // frames drawn 0..4 requests late / not identified (log)
// Red dot: where the game's aim line (= where bullets go) meets the world, shown in the headset while aiming.
static bool             g_red_dot = true;
static float            g_dot_size = 0.6f;            // degrees
static float            g_dot_w[3]; static ULONGLONG g_dot_t;   // world point, when measured

// ---------------------------------------------------------------- Aiden
// Among the characters whose head the game just updated, Aiden is ~2.5 m in front of the game
// camera (third person) or at it. Once found, keep that object while the game keeps updating it.
static void auto_track_aiden() {
    if (!camera::head_hook_available() || !camera::struct_a()) return;
    uintptr_t objs[64];
    size_t n = camera::recent_head_objects(objs, 64);
    ULONGLONG now = GetTickCount64();
    for (size_t i = 0; i < n; i++) if (objs[i] == g_aiden_obj) { g_aiden_seen = now; break; }
    if (g_aiden_obj && now - g_aiden_seen < 500) { g_aiden_addr = g_aiden_obj + camera::kHeadOffset; return; }
    float cam[3];
    if (!safe_read(camera::struct_a() + 0x04, cam, sizeof cam)) return;
    float f[3], r[3], u[3];
    camera::base_frame(f, r, u);
    float ahead[3] = { cam[0] + f[0] * 2.5f, cam[1] + f[1] * 2.5f, cam[2] - 0.3f };
    uintptr_t best = 0; float best_score = 3.0f;
    for (size_t i = 0; i < n; i++) {
        float h[3];
        if (!safe_read(objs[i] + camera::kHeadOffset, h, sizeof h) || !std::isfinite(h[0]) || !std::isfinite(h[1]) || !std::isfinite(h[2])) continue;
        if (h[2] < cam[2] - 2.5f || h[2] > cam[2] + 1.0f) continue;
        auto dist = [&](const float* p) { float dx = h[0] - p[0], dy = h[1] - p[1], dz = h[2] - p[2]; return sqrtf(dx * dx + dy * dy + dz * dz); };
        float score = fminf(dist(cam), dist(ahead));
        if (score < best_score) { best_score = score; best = objs[i]; }
    }
    if (best) {
        if (best != g_aiden_obj) log("Aiden found (object %p, %.2f m from the expected spot, %zu characters seen)", (void*)best, best_score, n);
        g_aiden_obj = best; g_aiden_seen = now; g_aiden_addr = best + camera::kHeadOffset;
    } else if (g_aiden_obj) {
        log("Lost Aiden's object - searching");
        g_aiden_obj = 0; g_aiden_addr = 0;
    }
}

static void do_recenter() {
    camera::recenter(g_vr.hmd.yaw);
    memcpy(g_head_origin, g_vr.hmd.pos, sizeof g_head_origin);
    g_head_origin_yaw = g_vr.hmd.yaw;
    g_have_origin = true;
    log("Recentered (head yaw %.0f)", g_head_origin_yaw);
}

// Tracking space (OpenVR: x right, y up, -z forward) -> game world, in the frame f/r/u that
// "straight ahead" (the recentered head direction) maps to. Note camera::base_frame's r points to
// the view's LEFT (turning the head right raises the yaw, which turns the view toward -r) - so
// your right goes to -r. (Up to 0.10.3 this was mirrored; the T-pose calibration hid it.)
static void tracking_to_world(const float* f, const float* r, const float* u, const float* d, float* out) {
    float y = g_head_origin_yaw * 0.0174532925f;
    float fa = d[0] * sinf(y) - d[2] * cosf(y);
    float ra = d[0] * cosf(y) + d[2] * sinf(y);
    for (int i = 0; i < 3; i++) out[i] = f[i] * fa - r[i] * ra + u[i] * d[1];
}

// ---------------------------------------------------------------- headset display
static void compute_view_geometry() {
    float l[2], r[2], t[2], b[2], tanV = 0;
    for (int e = 0; e < 2; e++) {
        vr_eye_tangents(e, &l[e], &r[e], &t[e], &b[e]);
        tanV = fmaxf(tanV, fmaxf(fabsf(t[e]), fabsf(b[e])));
    }
    float aspect = g_height ? (float)g_width / (float)g_height : 16.0f / 9.0f;
    float W = tanV * aspect;                     // horizontal tangent covered by the full game image
    for (int e = 0; e < 2; e++) {
        g_bounds[e].u0 = (l[e] + W) / (2 * W);
        g_bounds[e].u1 = (r[e] + W) / (2 * W);
        g_bounds[e].v0 = (t[e] + tanV) / (2 * tanV);
        g_bounds[e].v1 = (b[e] + tanV) / (2 * tanV);
    }
    g_fov_written = 2.0f * atanf(W * g_fov_scale);   // the game stores a horizontal FOV
    camera::set_fov_override(g_fov_written);
    log("Headset view: left eye u %.3f-%.3f v %.3f-%.3f, game FOV %.1f deg (scale %.2f)",
        g_bounds[0].u0, g_bounds[0].u1, g_bounds[0].v0, g_bounds[0].v1, g_fov_written * 57.2958f, g_fov_scale);
}

static void release_eye_images() {
    for (int e = 0; e < 2; e++) { if (g_eye_img[e]) g_eye_img[e]->Release(); g_eye_img[e] = nullptr; g_eye_valid[e] = false; }
}

static void set_hmd_display(bool on) {
    if (on == g_hmd_display) return;
    if (on) {
        if (!camera::struct_a()) { snprintf(g_hmd_msg, sizeof g_hmd_msg, "Load into gameplay first (camera not found yet)"); return; }
        if (!vr_init_scene(g_mod_dir)) { VrState s; vr_poll(&s); snprintf(g_hmd_msg, sizeof g_hmd_msg, "%s", s.status); return; }
        camera::set_head_look(true);
        compute_view_geometry();
        g_have_pose = false; g_pose_count = 0; g_err_logs = 0;
        g_need_recenter = true;            // first valid head pose becomes "straight ahead"
        g_hmd_display = true;
        snprintf(g_hmd_msg, sizeof g_hmd_msg, "On - put the headset on");
        log("Headset display ON");
    } else {
        if (g_screen_mode) { vr_screen_hide(); g_screen_mode = false; }
        g_hmd_display = false;
        camera::set_head_look(false);
        camera::set_fov_override(0);
        vr_shutdown();
        vr_init(g_mod_dir);                      // back to tracking-only
        if (g_eye_tex) { g_eye_tex->Release(); g_eye_tex = nullptr; }
        release_eye_images();
        { float z[3] = { 0, 0, 0 }; camera::set_eye_offset(false, z); }
        snprintf(g_hmd_msg, sizeof g_hmd_msg, "Off");
        save_config();
        log("Headset display OFF");
    }
}

// Floating screen: the game image with alpha forced to 1 (the game's alpha channel holds
// random values; the headset view ignores alpha, overlays don't).
static ID3D11Texture2D *g_scr_src, *g_scr_out;
static ID3D11ShaderResourceView* g_scr_srv;
static ID3D11RenderTargetView* g_scr_rtv;
static ID3D11VertexShader* g_scr_vs;
static ID3D11PixelShader* g_scr_ps;
static ID3D11SamplerState* g_scr_smp;

static void release_screen_blit() {
    for (IUnknown** p : { (IUnknown**)&g_scr_src, (IUnknown**)&g_scr_out, (IUnknown**)&g_scr_srv, (IUnknown**)&g_scr_rtv })
        if (*p) { (*p)->Release(); *p = nullptr; }
}

static ID3D11Texture2D* screen_blit(ID3D11Texture2D* src_game) {
    D3D11_TEXTURE2D_DESC sd; src_game->GetDesc(&sd);
    if (!g_scr_vs) {
        static const char* hlsl =
            "struct VO { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };"
            "VO vs(uint id : SV_VertexID) { VO o; float2 uv = float2((id << 1) & 2, id & 2); o.uv = uv;"
            "  o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }"
            "Texture2D t : register(t0); SamplerState s : register(s0);"
            "float4 ps(VO i) : SV_TARGET { return float4(t.Sample(s, i.uv).rgb, 1); }";
        ID3DBlob *vb = nullptr, *pb = nullptr;
        if (FAILED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vb, nullptr)) ||
            FAILED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, &pb, nullptr))) {
            log("Screen: shader compile failed"); if (vb) vb->Release(); if (pb) pb->Release(); return nullptr;
        }
        g_device->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g_scr_vs);
        g_device->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_scr_ps);
        vb->Release(); pb->Release();
        D3D11_SAMPLER_DESC smp = {};
        smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        g_device->CreateSamplerState(&smp, &g_scr_smp);
    }
    if (g_scr_src) { D3D11_TEXTURE2D_DESC d; g_scr_src->GetDesc(&d); if (d.Width != sd.Width || d.Height != sd.Height) release_screen_blit(); }
    if (!g_scr_src) {
        D3D11_TEXTURE2D_DESC d = sd;   // same 8-bit family as the game image, typeless so we can read the raw bytes
        d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = 0;
        d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0; d.MipLevels = d.ArraySize = 1; d.SampleDesc = { 1, 0 };
        if (FAILED(g_device->CreateTexture2D(&d, nullptr, &g_scr_src))) return nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC v = {}; v.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = 1;
        g_device->CreateShaderResourceView(g_scr_src, &v, &g_scr_srv);
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        if (FAILED(g_device->CreateTexture2D(&d, nullptr, &g_scr_out))) { d.MiscFlags = 0; g_device->CreateTexture2D(&d, nullptr, &g_scr_out); }
        if (g_scr_out) g_device->CreateRenderTargetView(g_scr_out, nullptr, &g_scr_rtv);
    }
    if (!g_scr_srv || !g_scr_rtv) return nullptr;
    if (sd.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && sd.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        sd.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS) return nullptr;
    g_context->CopyResource(g_scr_src, src_game);

    ID3D11RenderTargetView* old_rtv = nullptr; ID3D11DepthStencilView* old_dsv = nullptr;
    g_context->OMGetRenderTargets(1, &old_rtv, &old_dsv);
    UINT nvp = 1; D3D11_VIEWPORT old_vp; g_context->RSGetViewports(&nvp, &old_vp);
    ID3D11VertexShader* old_vs = nullptr; ID3D11PixelShader* old_ps = nullptr;
    g_context->VSGetShader(&old_vs, nullptr, nullptr); g_context->PSGetShader(&old_ps, nullptr, nullptr);
    ID3D11ShaderResourceView* old_srv = nullptr; g_context->PSGetShaderResources(0, 1, &old_srv);
    ID3D11SamplerState* old_smp = nullptr; g_context->PSGetSamplers(0, 1, &old_smp);
    ID3D11InputLayout* old_il = nullptr; g_context->IAGetInputLayout(&old_il);
    D3D11_PRIMITIVE_TOPOLOGY old_top; g_context->IAGetPrimitiveTopology(&old_top);
    ID3D11BlendState* old_bs = nullptr; float old_bf[4]; UINT old_mask; g_context->OMGetBlendState(&old_bs, old_bf, &old_mask);
    ID3D11RasterizerState* old_rs = nullptr; g_context->RSGetState(&old_rs);
    ID3D11DepthStencilState* old_ds = nullptr; UINT old_ref; g_context->OMGetDepthStencilState(&old_ds, &old_ref);

    D3D11_VIEWPORT vp = { 0, 0, (float)sd.Width, (float)sd.Height, 0, 1 };
    g_context->OMSetRenderTargets(1, &g_scr_rtv, nullptr);
    g_context->RSSetViewports(1, &vp);
    g_context->VSSetShader(g_scr_vs, nullptr, 0); g_context->PSSetShader(g_scr_ps, nullptr, 0);
    g_context->PSSetShaderResources(0, 1, &g_scr_srv); g_context->PSSetSamplers(0, 1, &g_scr_smp);
    g_context->IASetInputLayout(nullptr); g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF); g_context->RSSetState(nullptr); g_context->OMSetDepthStencilState(nullptr, 0);
    g_context->Draw(3, 0);

    ID3D11ShaderResourceView* none = nullptr; g_context->PSSetShaderResources(0, 1, &none);
    g_context->OMSetRenderTargets(1, &old_rtv, old_dsv);
    g_context->RSSetViewports(1, &old_vp);
    g_context->VSSetShader(old_vs, nullptr, 0); g_context->PSSetShader(old_ps, nullptr, 0);
    g_context->PSSetShaderResources(0, 1, &old_srv); g_context->PSSetSamplers(0, 1, &old_smp);
    g_context->IASetInputLayout(old_il); g_context->IASetPrimitiveTopology(old_top);
    g_context->OMSetBlendState(old_bs, old_bf, old_mask); g_context->RSSetState(old_rs); g_context->OMSetDepthStencilState(old_ds, old_ref);
    for (IUnknown* u : { (IUnknown*)old_rtv, (IUnknown*)old_dsv, (IUnknown*)old_vs, (IUnknown*)old_ps, (IUnknown*)old_srv,
                         (IUnknown*)old_smp, (IUnknown*)old_il, (IUnknown*)old_bs, (IUnknown*)old_rs, (IUnknown*)old_ds })
        if (u) u->Release();
    return g_scr_out;
}

static void set_screen_mode(bool on) {
    if (on == g_screen_mode || !g_hmd_display) return;
    g_screen_mode = on;
    if (on) {   // let the game draw its menu/map normally: release the camera and the VR field of view
        camera::set_head_look(false);
        camera::set_fov_override(0);
        float z[3] = { 0, 0, 0 };
        camera::set_eye_offset(false, z);
        if (g_eye_tex) { ID3D11Texture2D* t = screen_blit(g_eye_tex); vr_screen_show(t ? t : g_eye_tex, g_vr.hmd.m, g_screen_dist, g_screen_width); }
        log("Screen mode ON (menu / map on the big screen)");
    } else {
        vr_screen_hide();
        camera::set_head_look(true);
        compute_view_geometry();             // restores the VR field of view
        log("Screen mode OFF");
    }
}

static void hmd_display_frame(IDXGISwapChain* sc) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) return;
    D3D11_TEXTURE2D_DESC bd; back->GetDesc(&bd);
    if (bd.Height) g_aspect = (float)bd.Width / bd.Height;
    if (g_eye_tex) {
        D3D11_TEXTURE2D_DESC ed; g_eye_tex->GetDesc(&ed);
        if (ed.Width != bd.Width || ed.Height != bd.Height || ed.Format != bd.Format) {
            g_eye_tex->Release(); g_eye_tex = nullptr;
            release_eye_images();
        }
    }
    if (!g_eye_tex) {
        D3D11_TEXTURE2D_DESC d = bd;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags = 0;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED; d.SampleDesc = { 1, 0 }; d.MipLevels = 1; d.ArraySize = 1;
        HRESULT hr = g_device->CreateTexture2D(&d, nullptr, &g_eye_tex);
        if (FAILED(hr)) { d.MiscFlags = 0; hr = g_device->CreateTexture2D(&d, nullptr, &g_eye_tex); }
        log("Headset texture %ux%u format %d: 0x%08lx", d.Width, d.Height, (int)d.Format, (unsigned long)hr);
        if (FAILED(hr)) { back->Release(); return; }
        compute_view_geometry();                 // resolution may have changed
    }
    if (bd.SampleDesc.Count > 1) g_context->ResolveSubresource(g_eye_tex, 0, back, 0, bd.Format);
    else g_context->CopyResource(g_eye_tex, back);

    static bool waited_once = false;             // SteamVR wants a pose request before the first image
    if (!g_have_pose) waited_once = false;
    if (g_screen_mode) {                         // eyes see black; the game is on the floating screen
        if (!g_black_tex) {
            D3D11_TEXTURE2D_DESC d = {};
            d.Width = d.Height = 16; d.MipLevels = d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            static uint32_t px[16 * 16];
            for (auto& p : px) p = 0xFF000000;
            D3D11_SUBRESOURCE_DATA init = { px, 16 * 4, 0 };
            g_device->CreateTexture2D(&d, &init, &g_black_tex);
        }
        { ID3D11Texture2D* t = screen_blit(g_eye_tex); vr_screen_update(t ? t : g_eye_tex); }
        if (waited_once && g_black_tex) { VrBounds full = { 0, 0, 1, 1 }; g_submit_err = vr_submit(g_black_tex, nullptr, &full, &full); }
    } else if (waited_once) {
        // Pair this image with the head pose (and eye) it was drawn with. Frame ID says exactly which request it was;
        // without it, guess 'delay' frames ago and don't trust frames next to a change of type.
        const float (*pose)[4] = nullptr;
        int eye = -1;
        bool edge = false;
        if (g_have_pose && g_pose_count) {
            unsigned fid = 0, idx;
            int fs = framecap::frame_shown(&fid);
            bool exact = fs == 1 && fid < g_pose_count && g_pose_count - fid <= 6;
            if (exact) { idx = fid; if (g_pose_count - fid < 5) g_fid_hist[g_pose_count - fid]++; }
            else {
                unsigned d = (g_pose_count >= (unsigned)g_frame_delay) ? (unsigned)g_frame_delay : 1u;
                idx = g_pose_count - d;
                if (fs >= 0) g_fid_unknown++;
            }
            pose = g_pose_ring[idx & 7]; eye = g_eye_ring[idx & 7];
            g_shown_idx = idx;
            auto changed = [&](int other) { return other != eye && !(other >= 0 && other <= 1 && eye >= 0 && eye <= 1); };
            if (fs == 0 && !exact) {
                // frame ID works but couldn't name this frame: only a run of plain mono frames is safe to use
                edge = !(eye == -1 && (idx < 1 || g_eye_ring[(idx - 1) & 7] == -1) && (idx + 1 >= g_pose_count || g_eye_ring[(idx + 1) & 7] == -1));
            } else if (!exact) {
                // (an eye frame next to an eye frame of the other side is the normal stereo rhythm, not a change)
                if (idx >= 1 && changed(g_eye_ring[(idx - 1) & 7])) edge = true;
                if (idx + 1 < g_pose_count && changed(g_eye_ring[(idx + 1) & 7])) edge = true;
            }
            static ULONGLONG rep_t = 0;                   // every 20 s: how late frames arrive
            if (!rep_t) rep_t = GetTickCount64();
            if (GetTickCount64() - rep_t > 20000) {
                unsigned tot = g_fid_unknown; for (unsigned h : g_fid_hist) tot += h;
                if (tot) {
                    int mode = 1; for (int i = 1; i < 5; i++) if (g_fid_hist[i] > g_fid_hist[mode]) mode = i;
                    char rr[160]; framecap::rot_report(rr, sizeof rr);
                    log("Frames: drawn 1 / 2 / 3 requests late %.0f%% / %.0f%% / %.0f%%, not identified %.0f%% (frame ID %s); %s (rotation lag %s)",
                        100.0 * g_fid_hist[1] / tot, 100.0 * g_fid_hist[2] / tot, 100.0 * g_fid_hist[3] / tot, 100.0 * g_fid_unknown / tot,
                        framecap::frame_id_status(), rr, g_rot_lag ? "on" : "off");
                    if (framecap::frame_id_locked() && g_fid_hist[mode] > tot / 2) g_frame_delay = mode;   // used for HUD timing
                }
                memset(g_fid_hist, 0, sizeof g_fid_hist); g_fid_unknown = 0; rep_t = GetTickCount64();
            }
        }
        g_shown_type = edge ? -9 : eye;
        // Game-view frame with game-view requests on both sides: whatever part of the game reads its camera a frame
        // early or late (its phone UI does), it still got the game view - only these feed the phone panel.
        g_shown_mid_g = eye == 2 && !edge && g_shown_idx >= 1 && g_eye_ring[(g_shown_idx - 1) & 7] == 2 &&
                        (g_shown_idx + 1 >= g_pose_count || g_eye_ring[(g_shown_idx + 1) & 7] == 2);
        if (eye == 2 || edge) {                    // the game's own view (scope / phone); eyes keep their last images
            if (eye == 2 && !edge && g_scoped) scope_frame();
            int e0 = 0, e1 = 0;
            if (g_eye_valid[0]) e0 = vr_submit_eye(0, g_eye_img[0], g_eye_pose[0], &g_bounds[0]);
            if (g_eye_valid[1]) e1 = vr_submit_eye(1, g_eye_img[1], g_eye_pose[1], &g_bounds[1]);
            g_submit_err = e0 ? e0 : e1;
        }
        else if (eye < 0) {                               // mono (driving): this frame to both eyes
            g_submit_err = vr_submit(g_eye_tex, pose, &g_bounds[0], &g_bounds[1]);
            for (int e = 0; e < 2; e++) {                 // it is both eyes' latest image now (phone / switch frames reuse it)
                if (!g_eye_img[e]) { D3D11_TEXTURE2D_DESC d; g_eye_tex->GetDesc(&d); g_device->CreateTexture2D(&d, nullptr, &g_eye_img[e]); }
                if (!g_eye_img[e]) { g_eye_valid[e] = false; continue; }
                g_context->CopyResource(g_eye_img[e], g_eye_tex);
                if (pose) memcpy(g_eye_pose[e], pose, sizeof g_eye_pose[e]);
                g_eye_valid[e] = pose != nullptr;
            }
        }
        else {      // stereo: keep this frame as that eye's image; send both eyes their latest
            if (!g_eye_img[eye]) { D3D11_TEXTURE2D_DESC d; g_eye_tex->GetDesc(&d); g_device->CreateTexture2D(&d, nullptr, &g_eye_img[eye]); }
            if (g_eye_img[eye]) {
                g_context->CopyResource(g_eye_img[eye], g_eye_tex);
                memcpy(g_eye_pose[eye], pose, sizeof g_eye_pose[eye]);
                g_eye_valid[eye] = true;
            }
            int e0 = 0, e1 = 0;
            if (g_eye_valid[0]) e0 = vr_submit_eye(0, g_eye_img[0], g_eye_pose[0], &g_bounds[0]);
            if (g_eye_valid[1]) e1 = vr_submit_eye(1, g_eye_img[1], g_eye_pose[1], &g_bounds[1]);
            g_submit_err = e0 ? e0 : e1;
        }
    }
    // Steady monitor: the left eye's latest image instead of the alternating frames.
    if (!g_screen_mode && g_monitor_mode && g_eye_valid[0] && g_eye_img[0] && bd.SampleDesc.Count == 1) {
        if (g_monitor_mode == 1) g_context->CopyResource(back, g_eye_img[0]);
        else {
            const VrBounds& b = g_bounds[0];
            UINT x0 = (UINT)(b.u0 * bd.Width), x1 = (UINT)(b.u1 * bd.Width), y0 = (UINT)(b.v0 * bd.Height), y1 = (UINT)(b.v1 * bd.Height);
            if (x1 > x0 && y1 > y0 && x1 <= bd.Width && y1 <= bd.Height) {
                if (g_rtv) { const float black[4] = { 0, 0, 0, 1 }; g_context->ClearRenderTargetView(g_rtv, black); }
                D3D11_BOX box = { x0, y0, 0, x1, y1, 1 };
                g_context->CopySubresourceRegion(back, 0, (bd.Width - (x1 - x0)) / 2, y0, 0, g_eye_img[0], 0, &box);
            }
        }
    }
    back->Release();
    g_wait_err = vr_wait_poses();
    if (!g_wait_err) waited_once = true;
    if ((g_submit_err || g_wait_err) && g_err_logs < 10) { g_err_logs++; log("Headset: submit error %d, wait error %d", g_submit_err, g_wait_err); }
}

// ---------------------------------------------------------------- Aiden turns with your head
// On foot, "straight ahead" for the view is a fixed world yaw (g_view_yaw), turned only by the
// right stick / right trackpad. The game's own camera - which decides where Aiden walks, faces
// and shoots - is steered with the right stick to where you look, or while aiming to where the
// right wand points. So looking around turns Aiden, and the gun fires along the wand.
static bool  g_head_steers = true;
static float g_turn_rate = 140.0f;       // smooth turn, degrees per second at full stick
static bool  g_snap_turn;                // snap turn instead of smooth
static float g_snap_deg = 45.0f;
static float g_steer_range = 10.0f;      // degrees of error for full stick
static float g_steer_dz = 0.24f;         // the game's stick dead zone
static float g_steer_sign[2] = { 1, 1 }; // which way the stick turns the game's camera (learned, then saved)
static bool  g_steer_locked[2];
static bool  g_steer_on;
static float g_view_yaw;
static float g_steer_err[2];
static int   g_menu_kind;                // phone / weapon wheel open (they use the stick themselves)
static bool  g_hands_ui = true;           // phone on the left wrist, weapon wheel on the right hand

static void steer_update(float dt) {
    bool want = g_head_steers && g_hmd_display && camera::head_look() && camera::game_rot_valid() && g_aiden_obj &&
                g_vr.hmd.valid && !g_in_vehicle && !g_remote_view && !g_screen_mode;
    if (!want) {
        if (g_steer_on) {
            g_steer_on = false;
            input::set_right_stick_override(false, 0, 0);
            camera::set_base_yaw_override(false, 0);
            log("Head steering: paused");
        }
        return;
    }
    if (!g_steer_on) {                          // start where the view already is: no jump
        g_steer_on = true;
        g_view_yaw = camera::game_yaw_deg();
        log("Head steering: on (signs %+.0f %+.0f%s)", g_steer_sign[0], g_steer_sign[1], g_steer_locked[0] && g_steer_locked[1] ? ", saved" : ", learning");
    }
    static float last_game_yaw = 0; static bool had_menu = false;
    if (g_menu_kind) {                          // phone / weapon wheel: the stick is theirs; the view turns with the game camera
        input::set_right_stick_override(false, 0, 0);
        float gy = camera::game_yaw_deg();
        if (had_menu) g_view_yaw = wrap180(g_view_yaw + wrap180(gy - last_game_yaw));
        last_game_yaw = gy; had_menu = true;
        camera::set_base_yaw_override(true, g_view_yaw);
        return;
    }
    had_menu = false;
    // Right pad / stick turns you: the view and (through the steering + body turn) Aiden together.
    float rx = input::user_right_x();
    if (g_snap_turn) {
        static bool armed = true;
        if (armed && fabsf(rx) > 0.7f) { g_view_yaw = wrap180(g_view_yaw + (rx > 0 ? 1 : -1) * g_steer_sign[0] * g_snap_deg); armed = false; }
        else if (fabsf(rx) < 0.3f) armed = true;
    } else g_view_yaw = wrap180(g_view_yaw + rx * g_steer_sign[0] * g_turn_rate * dt);
    camera::set_base_yaw_override(true, g_view_yaw);

    const auto& t = camera::tuning();
    const VrDevice& src = (g_aiming && g_vr.right.valid) ? g_vr.right : g_vr.hmd;
    float ty = g_view_yaw + t.yaw_offset + (t.invert_yaw ? -src.yaw : src.yaw);
    float tp = t.invert_pitch ? -src.pitch : src.pitch;
    float gc[3];
    if (g_aiming && g_vr.right.valid && g_have_eye && camera::game_camera_pos(gc)) {
        // The game shoots along ITS camera's centre line, and its camera sits over Aiden's shoulder - not in
        // your hand. Aim that line at the point g_aim_dist down the wand's line, so the two cross there.
        float f[3], r[3], u[3], d[3], wp[3], wd[3];
        camera::base_frame(f, r, u);
        float dp[3] = { g_vr.right.pos[0] - g_vr.hmd.pos[0], g_vr.right.pos[1] - g_vr.hmd.pos[1], g_vr.right.pos[2] - g_vr.hmd.pos[2] };
        float fw[3]; wand_aim_dir(g_vr.right, fw);
        tracking_to_world(f, r, u, dp, wp); tracking_to_world(f, r, u, fw, wd); norm3(wd);
        float dist = g_aim_dist;
        if (g_aim_auto && g_aim_live > 0 && GetTickCount64() - g_aim_live_t < 600) {
            // where the wand line passes the thing the game's aim hits (fixed point: converges onto that surface)
            float h[3] = { g_aim_hit[0] - g_eye_world[0] - wp[0], g_aim_hit[1] - g_eye_world[1] - wp[1], g_aim_hit[2] - g_eye_world[2] - wp[2] };
            float t = dot3(h, wd);
            if (t > 1.0f) { g_aim_live += (t - g_aim_live) * 0.35f; dist = g_aim_live; }
        }
        for (int i = 0; i < 3; i++) d[i] = g_eye_world[i] + wp[i] + wd[i] * dist - gc[i];
        if (norm3(d) > 0.5f) { ty = atan2f(-d[1], d[0]) * 57.2958f; tp = -asinf(clampf(d[2], -1, 1)) * 57.2958f; }
    }
    float ang[2] = { camera::game_yaw_deg(), camera::game_pitch_deg() };
    g_steer_err[0] = wrap180(ty - ang[0]); g_steer_err[1] = tp - ang[1];

    static float last_cmd[2], last_ang[2], corr[2];
    static int corr_n[2], agree[2];
    static bool have_last;
    static ULONGLONG check_t, bad_since;
    ULONGLONG now = GetTickCount64();
    // Learn which way the stick turns the game's camera from how the camera actually moves while we push.
    if (have_last)
        for (int i = 0; i < 2; i++) {
            float d = i == 0 ? wrap180(ang[0] - last_ang[0]) : ang[1] - last_ang[1];
            if (fabsf(last_cmd[i]) > 0.4f && fabsf(d) < 20) { corr[i] += last_cmd[i] * d; corr_n[i]++; }
        }
    memcpy(last_ang, ang, sizeof ang); have_last = true;
    if (now - check_t > 300) {
        for (int i = 0; i < 2; i++) {
            if (corr_n[i] >= 6 && !g_steer_locked[i] && fabsf(corr[i]) > 0.5f) {
                float s = corr[i] > 0 ? 1.0f : -1.0f;      // positive stick raised the angle -> push positive when the error is positive
                if (s != g_steer_sign[i]) { g_steer_sign[i] = s; agree[i] = 0; log("Head steering: %s direction learned (%+.0f)", i ? "up/down" : "left/right", s); }
                else if (++agree[i] >= 3) { g_steer_locked[i] = true; log("Head steering: %s direction confirmed", i ? "up/down" : "left/right"); save_config(); }
            }
            corr[i] = 0; corr_n[i] = 0;
        }
        check_t = now;
    }
    // Safety: a far-off error that won't come down means a wrong direction - learn again.
    if (fabsf(g_steer_err[0]) > 60) { if (!bad_since) bad_since = now; else if (now - bad_since > 2000) {
        g_steer_locked[0] = g_steer_locked[1] = false; agree[0] = agree[1] = 0; bad_since = 0; log("Head steering: not converging - relearning directions (error %.0f deg, game yaw %.0f, view yaw %.0f, aim %d)", g_steer_err[0], ang[0], g_view_yaw, (int)g_aiming); } }
    else bad_since = 0;
    float cmd[2];
    for (int i = 0; i < 2; i++) {
        float a = fabsf(g_steer_err[i]);
        float m = a < 0.6f ? 0.0f : g_steer_dz + (1 - g_steer_dz) * fminf(1.0f, a / g_steer_range);
        cmd[i] = (g_steer_err[i] > 0 ? m : -m) * g_steer_sign[i];
    }
    memcpy(last_cmd, cmd, sizeof cmd);
    input::set_right_stick_override(true, cmd[0], cmd[1]);
}

// ---------------------------------------------------------------- Aiden's body: turn + arms
// Aiden's live pose (from the game's bone code): pose = [rig->[0x10] + 0x110] + 0x1B0, bone count
// at pose+0x28, bones = [pose+0xB8], 32 bytes each: quaternion (x,y,z,w), position (x,y,z,0), model
// space (z up). Our transforms go in through the hooked bone store; the game builds the children
// from them. Bones: pelvis 1, left arm 6/9/10 (finger 19), right arm 48/51/52 (finger 61), head 90.
static bool  g_body_turn = true;          // his whole body faces where you look
static float g_body_deadzone = 0.0f;      // degrees you can look away before the body follows
static bool  g_arms_on = true;
static bool  g_hands_follow = true;       // hands turn with the wands (off = straight wrists)
static float g_hand_roll[2] = { 0, 0 };   // degrees, twist of each hand about the wand
static float g_arms_grip = 0.06f;         // wand grip -> wrist distance (back along the wand)
static float g_arms_pole_out = 0.5f;      // how much the elbows point outward
static int   g_arms_conv;                 // quaternion convention: 0 = as stored, 1 = stored conjugated
static char  g_body_msg[160] = "off";

// T-pose calibration: a turn about the vertical, and the offset that puts your shoulder line on Aiden's - kept in his
// body frame (forward, left, up), so it stays right when his body turns.
struct ArmCal { bool ok = false; float yaw = 0, off[3] = {}; };
static ArmCal g_arm_cal;
static int    g_cal_state;               // 0 idle, 1 counting down, 2 sampling
static ULONGLONG g_cal_t0;
static float  g_cal_acc[2][6]; static int g_cal_n;
static char   g_cal_result[160]; static ULONGLONG g_cal_result_t;
static const int kCalCountdownMs = 8000;

struct ArmDef { int clav, upper, fore, hand, finger; float side; int slot0, clav_slot; };
static const ArmDef kArms[2] = { { 5, 6, 9, 10, 19, +1.0f, 0, 15 }, { 47, 48, 51, 52, 61, -1.0f, 3, 16 } };   // left, right
static const int kChest = 4;              // top spine bone: the clavicles are held fixed relative to it
// Twist/helper bones the animation moves on its own (they pulled the coat): same turn as their segment.
struct Helper { int bone, seg; };
static const Helper kHelpers[2][4] = { { { 7, 0 }, { 8, 0 }, { 29, 1 }, { 30, 1 } }, { { 49, 0 }, { 50, 0 }, { 72, 1 }, { 73, 1 } } };
static const int kHelperSlot0 = 6, kPelvis = 1, kPelvisSlot = 14, kCoatSlot0 = 17, kCoatMax = 40, kSlots = 57;
// Hand-in-pocket: the animation also moves the coat / pocket bones around the hips. A detector rebuilds where the
// animation wants each hand (from the game's own values captured by the bone hook); when that is in a pocket, the
// hip-area bones it disturbed are held at their relaxed pose (relative to the pelvis) instead.
struct CoatBone { int bone; float rel_q[4], rel_p[3]; };
static CoatBone g_coat[kCoatMax]; static int g_coat_n;
static float g_pocket_pt[2][3];            // pocket points in the pelvis' own frame (left, right)
static bool  g_pocket[2], g_pocket_fix = true;
static ULONGLONG g_pocket_until;
static float g_last_q[kSlots][4], g_prev_q[kSlots][4];   // rotation we wrote this / the previous frame (true convention)
static float g_arm_reach = 1.0f;          // scales how far your hands are from your chest (1 = real distance)
static float g_arm_stretch = 1.10f;       // how much Aiden's arms may stretch to reach the wands

// Learned once per skeleton from the game's own pose: each arm bone's limb direction and the
// elbow-hinge direction in the bone's own frame, segment lengths, helpers' rest offsets, and
// which way the pelvis faces in its own frame.
struct ArmRest { float axis[3][3], hinge[3][3], len[2], helper_t[4], helper_rel[4][4];
                 float clav_rel[4], clav_pos[3], shoulder[3]; };   // clavicle vs chest, shoulder in the clavicle's frame
static ArmRest  g_rest[2];
static float    g_pelvis_fwd[3];
static bool     g_rest_ok;
static uintptr_t g_rest_bones, g_body_bones;
static int      g_learn_frames; static float g_learn_bend[2], g_learn_minbend[2];
static bool     g_mapped[kSlots];
static float    g_body_delta, g_anim_yaw; static bool g_anim_yaw_ok;

static void tq(const float* stored, float* q) { if (g_arms_conv) qconj(stored, q); else memcpy(q, stored, 16); }   // true rotation

static void map_slot(int bone, int slot, bool on) {
    if (g_mapped[slot] == on) return;
    camera::set_bone_slot(bone, on ? slot : -1);
    g_mapped[slot] = on;
}
static int slot_bone(int slot) {
    if (slot >= kCoatSlot0) return slot - kCoatSlot0 < g_coat_n ? g_coat[slot - kCoatSlot0].bone : 0;
    if (slot == kPelvisSlot) return kPelvis;
    if (slot == kArms[0].clav_slot) return kArms[0].clav;
    if (slot == kArms[1].clav_slot) return kArms[1].clav;
    if (slot >= kHelperSlot0) return kHelpers[(slot - kHelperSlot0) / 4][(slot - kHelperSlot0) % 4].bone;
    const ArmDef& A = kArms[slot / 3];
    return slot % 3 == 0 ? A.upper : slot % 3 == 1 ? A.fore : A.hand;
}
static void body_release(const char* why) {
    camera::set_bone_override_enabled(false);
    for (int s = 0; s < kSlots; s++) map_slot(slot_bone(s), s, false);
    if (g_body_bones) log("Body: released (%s)", why);
    g_body_bones = 0; g_body_delta = 0; g_anim_yaw_ok = false;
    snprintf(g_body_msg, sizeof g_body_msg, "%s", why);
}

struct PoseRef { uintptr_t bones = 0; uint32_t count = 0; };
static PoseRef find_pose() {
    PoseRef r;
    uintptr_t rig = camera::rig_for(g_aiden_obj);
    if (!rig) return r;
    uintptr_t p = rd64(rd64(rig + 0x10) + 0x110);
    if (!p) return r;
    p += 0x1B0;
    safe_read(p + 0x28, &r.count, 4);
    r.bones = rd64(p + 0x98 + 0x20);
    if (r.count > 1024 || !r.bones) r.count = 0;
    return r;
}

// Model -> world matrix of Aiden (entity+0x20, entity = rig->[8]->[0x10]->[8]). Rows: x, y, z axes, origin.
static bool aiden_matrix(float m[4][3]) {
    uintptr_t rig = camera::rig_for(g_aiden_obj);
    if (!rig) return false;
    uintptr_t ent = rd64(rd64(rd64(rig + 8) + 0x10) + 8);
    float f[16];
    if (!ent || !safe_read(ent + 0x20, f, sizeof f)) return false;
    float h[3] = { 0, 0, 0 }; if (g_aiden_addr) safe_read(g_aiden_addr, h, 12);
    auto near_head = [&](float x, float y, float z) { return fabsf(x - h[0]) < 4 && fabsf(y - h[1]) < 4 && fabsf(z - h[2]) < 4; };
    if (near_head(f[12], f[13], f[14])) {          // 4x4, translation in row 4
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) m[r][c] = f[r * 4 + c];
        m[3][0] = f[12]; m[3][1] = f[13]; m[3][2] = f[14];
    } else if (near_head(f[3], f[7], f[11])) {     // translation in column 4: columns are the axes
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) m[r][c] = f[c * 4 + r];
        m[3][0] = f[3]; m[3][1] = f[7]; m[3][2] = f[11];
    } else return false;
    return true;
}
static void world_to_model(float m[4][3], const float* w, float* o, bool is_dir) {
    float d[3] = { w[0], w[1], w[2] };
    if (!is_dir) for (int i = 0; i < 3; i++) d[i] -= m[3][i];
    for (int a = 0; a < 3; a++) { float l2 = dot3(m[a], m[a]); o[a] = l2 > 1e-6f ? dot3(d, m[a]) / l2 : 0; }
}
// Body frame from the shoulders (model space): lateral = toward his left, forward = lateral x up.
static void body_frame(const float* sl, const float* sr, float* lat, float* fwd) {
    lat[0] = sl[0] - sr[0]; lat[1] = sl[1] - sr[1]; lat[2] = 0; norm3(lat);
    fwd[0] = lat[1]; fwd[1] = -lat[0]; fwd[2] = 0;
}

static bool read_bone(int b, float* out) { return safe_read(g_body_bones + (uintptr_t)b * 32, out, 32); }

// One frame of learning (overrides off). Picks the frame where the elbows are most bent.
static void learn_rest_frame() {
    float sl[8], sr[8], pel[8];
    if (!read_bone(kArms[0].upper, sl) || !read_bone(kArms[1].upper, sr) || !read_bone(kPelvis, pel)) return;
    float lat[3], bfwd[3];
    body_frame(sl + 4, sr + 4, lat, bfwd);
    if (g_learn_frames == 0) {
        float qp[4]; tq(pel, qp);
        qunrot(qp, bfwd, g_pelvis_fwd);
    }
    {   // hip-area bones, from the frame where both arms hang most freely
        static float best = 1e9f;
        if (g_learn_frames == 0) best = 1e9f;
        float bu0[8], bf0[8], bh0[8], bu1[8], bf1[8], bh1[8];
        if (read_bone(6, bu0) && read_bone(9, bf0) && read_bone(10, bh0) && read_bone(48, bu1) && read_bone(51, bf1) && read_bone(52, bh1)) {
            auto bend = [](const float* u, const float* f, const float* h) {
                float a[3] = { f[4] - u[4], f[5] - u[5], f[6] - u[6] }, b[3] = { h[4] - f[4], h[5] - f[5], h[6] - f[6] }, n[3];
                norm3(a); norm3(b); cross3(a, b, n); return sqrtf(dot3(n, n)); };
            float sum = bend(bu0, bf0, bh0) + bend(bu1, bf1, bh1);
            static std::vector<float> all;
            uint32_t count = 0; safe_read(rd64(rd64(camera::rig_for(g_aiden_obj) + 0x10) + 0x110) + 0x1B0 + 0x28, &count, 4);
            if (sum < best && count >= 320 && count <= 1024) {
                all.assign(count * 8, 0);
                if (safe_read(g_body_bones, all.data(), count * 32)) {
                    best = sum;
                    float qp[4], cq[4]; tq(pel, qp); qconj(qp, cq);
                    float pts[2][3];
                    for (int sd = 0; sd < 2; sd++) {
                        float side = sd ? -0.20f : 0.20f, w[3] = { pel[4] + lat[0] * side, pel[5] + lat[1] * side, pel[6] - 0.05f }, d[3];
                        memcpy(pts[sd], w, 12);
                        for (int i = 0; i < 3; i++) d[i] = w[i] - pel[4 + i];
                        qunrot(qp, d, g_pocket_pt[sd]);
                    }
                    std::vector<std::pair<float, int>> cand;
                    for (int b = 0; b < (int)count; b++) {
                        if (b <= 90 || (b >= 89 && b <= 249) || b == 269 || b == 270 || b == 271 || b == 272 || (b >= 314 && b <= 317)) continue;
                        const float* e = &all[b * 8];
                        float dist = 1e9f;
                        for (int sd = 0; sd < 2; sd++) { float d[3] = { e[4] - pts[sd][0], e[5] - pts[sd][1], e[6] - pts[sd][2] }; dist = fminf(dist, sqrtf(dot3(d, d))); }
                        if (dist < 0.35f) cand.push_back({ dist, b });
                    }
                    std::sort(cand.begin(), cand.end());
                    g_coat_n = 0;
                    for (auto& c : cand) {
                        if (g_coat_n >= kCoatMax) break;
                        const float* e = &all[c.second * 8];
                        CoatBone& cb = g_coat[g_coat_n++];
                        cb.bone = c.second;
                        float qb[4], d[3] = { e[4] - pel[4], e[5] - pel[5], e[6] - pel[6] };
                        tq(e, qb); quat_mul(cq, qb, cb.rel_q); qunrot(qp, d, cb.rel_p);
                    }
                }
            }
        }
    }
    for (int a = 0; a < 2; a++) {
        const ArmDef& A = kArms[a];
        float bu[8], bf[8], bh[8], fin[8];
        if (!read_bone(A.upper, bu) || !read_bone(A.fore, bf) || !read_bone(A.hand, bh) || !read_bone(A.finger, fin)) return;
        float du[3] = { bf[4] - bu[4], bf[5] - bu[5], bf[6] - bu[6] }, df[3] = { bh[4] - bf[4], bh[5] - bf[5], bh[6] - bf[6] };
        float dh[3] = { fin[4] - bh[4], fin[5] - bh[5], fin[6] - bh[6] };
        float l1 = norm3(du), l2 = norm3(df); norm3(dh);
        float n[3]; cross3(du, df, n);
        float bend = norm3(n);
        ArmRest& R = g_rest[a];
        if (g_learn_frames == 0 || bend < g_learn_minbend[a]) {   // clavicle: from the frame with the arm hanging most freely
            float ch[8], cl[8];
            if (!read_bone(kChest, ch) || !read_bone(A.clav, cl)) return;
            float qc[4], qcl[4], cc[4], d1[3] = { cl[4] - ch[4], cl[5] - ch[5], cl[6] - ch[6] }, d2[3] = { bu[4] - cl[4], bu[5] - cl[5], bu[6] - cl[6] };
            tq(ch, qc); tq(cl, qcl); qconj(qc, cc);
            quat_mul(cc, qcl, R.clav_rel);
            qunrot(qc, d1, R.clav_pos); qunrot(qcl, d2, R.shoulder);
            g_learn_minbend[a] = bend;
        }
        if (g_learn_frames > 0 && bend <= g_learn_bend[a]) continue;
        if (bend < 0.1f) {                       // nearly straight arm: hinge = body right, kept perpendicular to the upper arm
            float right[3] = { -lat[0], -lat[1], 0 }, k = dot3(right, du);
            for (int i = 0; i < 3; i++) n[i] = right[i] - du[i] * k;
            norm3(n);
        }
        g_learn_bend[a] = bend;
        float qu[4], qf[4], qh[4]; tq(bu, qu); tq(bf, qf); tq(bh, qh);
        qunrot(qu, du, R.axis[0]); qunrot(qf, df, R.axis[1]); qunrot(qh, dh, R.axis[2]);
        qunrot(qu, n, R.hinge[0]); qunrot(qf, n, R.hinge[1]); qunrot(qh, n, R.hinge[2]);
        R.len[0] = (l1 > 0.1f && l1 < 0.5f) ? l1 : 0.28f; R.len[1] = (l2 > 0.1f && l2 < 0.5f) ? l2 : 0.28f;
        for (int h = 0; h < 4; h++) {
            const Helper& H = kHelpers[a][h];
            float hb[8];
            if (!read_bone(H.bone, hb)) continue;
            const float* m = H.seg ? bf : bu; const float* e = H.seg ? bh : bf;
            float seg[3] = { e[4] - m[4], e[5] - m[5], e[6] - m[6] }, rel[3] = { hb[4] - m[4], hb[5] - m[5], hb[6] - m[6] };
            float l2s = dot3(seg, seg);
            R.helper_t[h] = clampf(l2s > 1e-6f ? dot3(rel, seg) / l2s : 0, 0, 1);
            float qm[4], qhb[4], cm[4]; tq(m, qm); tq(hb, qhb); qconj(qm, cm);
            quat_mul(cm, qhb, R.helper_rel[h]);    // helper = segment * rel
        }
    }
    g_learn_frames++;
}

static void cal_result(const char* msg) {
    snprintf(g_cal_result, sizeof g_cal_result, "%s", msg); g_cal_result_t = GetTickCount64(); log("Arms: %s", msg);
    beep(strncmp(msg, "T-pose saved", 12) == 0 ? 1400 : 300, strncmp(msg, "T-pose saved", 12) == 0 ? 350 : 600);
}
// T-pose: arms straight out to the sides at shoulder height, wands pointing forward.
static void arms_start_calibration() {
    if (!g_hmd_display) set_hmd_display(true);      // the measurement is made in the headset view
    if (!g_hmd_display) { cal_result("T-pose: couldn't turn VR on - is SteamVR running?"); return; }
    g_arms_on = true;
    g_cal_state = 1; g_cal_t0 = GetTickCount64();
    memset(g_cal_acc, 0, sizeof g_cal_acc); g_cal_n = 0;
    log("Arms: T-pose calibration started (%d s countdown)", kCalCountdownMs / 1000);
}
static void arms_finish_calibration(const float* sl, const float* sr) {
    g_cal_state = 0;
    float Lm[3], Rm[3], Fm[3];
    for (int i = 0; i < 3; i++) {
        Lm[i] = g_cal_acc[0][i] / g_cal_n; Rm[i] = g_cal_acc[1][i] / g_cal_n;
        Fm[i] = (g_cal_acc[0][3 + i] + g_cal_acc[1][3 + i]) / (2.0f * g_cal_n);
    }
    float lat[3], bfwd[3];
    body_frame(sl, sr, lat, bfwd);
    float sm[2] = { Lm[0] - Rm[0], Lm[1] - Rm[1] };
    float span_m = sqrtf(sm[0] * sm[0] + sm[1] * sm[1] + (Lm[2] - Rm[2]) * (Lm[2] - Rm[2]));
    if (span_m < 1.0f) {
        char m[160]; snprintf(m, sizeof m, "T-pose failed: wands only %.2f m apart - arms straight out to the sides, then try again", span_m);
        cal_result(m); return;
    }
    ArmCal c;
    c.yaw = atan2f(lat[1], lat[0]) - atan2f(sm[1], sm[0]);   // your left-right line onto his
    float mid_m[3], mid_e[3], d[3];
    for (int i = 0; i < 3; i++) { mid_m[i] = (Lm[i] + Rm[i]) / 2; mid_e[i] = (sl[i] + sr[i]) / 2; d[i] = mid_e[i] - mid_m[i]; }
    c.off[0] = d[0] * bfwd[0] + d[1] * bfwd[1]; c.off[1] = d[0] * lat[0] + d[1] * lat[1]; c.off[2] = d[2];
    float fx = (cosf(c.yaw) * Fm[0] - sinf(c.yaw) * Fm[1]) * bfwd[0] + (sinf(c.yaw) * Fm[0] + cosf(c.yaw) * Fm[1]) * bfwd[1];
    c.ok = true;
    g_arm_cal = c;
    save_config();
    cal_result(fx > 0.3f ? "T-pose saved - your arms are now Aiden's arms" : "T-pose saved - but the wands didn't point forward: redo it facing ahead");
    log("Arms: calibrated - wands L %.2f %.2f %.2f R %.2f %.2f %.2f, shoulders L %.2f %.2f %.2f R %.2f %.2f %.2f, turn %.0f, "
        "offset fwd %.2f left %.2f up %.2f, wands point %.2f forward", Lm[0], Lm[1], Lm[2], Rm[0], Rm[1], Rm[2], sl[0], sl[1], sl[2],
        sr[0], sr[1], sr[2], c.yaw * 57.3f, c.off[0], c.off[1], c.off[2], fx);
}
// Calibration applied to a wand position / direction (model space). mid = shoulder middle now, lat / fwd = body frame now.
static void cal_fix(float* p, bool is_dir, const float* mid, const float* lat, const float* fwd) {
    float c = cosf(g_arm_cal.yaw), sn = sinf(g_arm_cal.yaw);
    if (is_dir) { float x = c * p[0] - sn * p[1], y = sn * p[0] + c * p[1]; p[0] = x; p[1] = y; return; }
    float q[3];
    for (int i = 0; i < 2; i++) q[i] = p[i] + fwd[i] * g_arm_cal.off[0] + lat[i] * g_arm_cal.off[1] - mid[i];
    q[2] = p[2] + g_arm_cal.off[2] - mid[2];
    float x = c * q[0] - sn * q[1], y = sn * q[0] + c * q[1];
    p[0] = mid[0] + x * g_arm_reach; p[1] = mid[1] + y * g_arm_reach; p[2] = mid[2] + q[2] * g_arm_reach;
}

// f/r/u: the world frame "straight ahead" maps to; eye: where the eyes are (world); dt: seconds.
static void body_update(const float* f, const float* r, const float* u, const float* eye, float dt) {
    bool want = (g_arms_on || g_body_turn) && g_hmd_display && camera::head_look() && g_aiden_obj && !g_in_vehicle &&
                !g_remote_view && !g_screen_mode && camera::bone_hooks_available() && g_have_origin;
    if (!want) { if (g_body_bones) body_release("paused"); return; }
    if (!g_body_bones) {
        PoseRef pr = find_pose();
        if (pr.count < 91) { snprintf(g_body_msg, sizeof g_body_msg, "Aiden's skeleton not found"); return; }
        g_body_bones = pr.bones;
        camera::set_bone_override_enabled(false);
        camera::set_bone_override_target(pr.bones);
        if (g_rest_bones != pr.bones) { g_rest_ok = false; g_learn_frames = 0; g_learn_bend[0] = g_learn_bend[1] = 0; }
        log("Body: skeleton %p (%u bones)", (void*)pr.bones, pr.count);
    }
    if (!g_rest_ok) {                                   // learn from the game's own pose first (~1/3 s)
        camera::set_bone_override_enabled(false);
        static int wait = 0;
        if (g_learn_frames == 0 && wait == 0) for (int sl = 0; sl < kSlots; sl++) map_slot(slot_bone(sl), sl, false);
        if (g_learn_frames == 0 && wait < 4) { wait++; return; }    // let the game draw a few frames without our bones first
        wait = 0;
        learn_rest_frame();
        snprintf(g_body_msg, sizeof g_body_msg, "learning Aiden's skeleton...");
        if (g_learn_frames >= 90) {
            g_rest_ok = true; g_rest_bones = g_body_bones;
            log("Body: skeleton learned (elbow bend %.2f / %.2f, arms %.2f+%.2f m)", g_learn_bend[0], g_learn_bend[1], g_rest[0].len[0], g_rest[0].len[1]);
        }
        return;
    }
    float M[4][3];
    if (!aiden_matrix(M)) { camera::set_bone_override_enabled(false); snprintf(g_body_msg, sizeof g_body_msg, "Aiden's position matrix not found"); return; }
    bool active[kSlots] = {};

    // ---- body turn: rotate the pelvis (and with it everything) about the vertical so he faces the view
    float pel_now_q[4] = { 0, 0, 0, 1 }, pel_now_p[3] = {}; bool pel_ok = false;
    {
        float pel[8] = {};
        const float* raw = camera::bone_slot_raw(kPelvisSlot);
        bool have = g_mapped[kPelvisSlot] && raw && (raw[0] || raw[1] || raw[2] || raw[3]);
        if (have) memcpy(pel, raw, 32); else have = read_bone(kPelvis, pel);   // unmapped: memory holds the game's value
        float vf[3], hm[3], qp[4], af[3];
        camera::view_forward(vf); vf[2] = 0;
        world_to_model(M, vf, hm, true);
        tq(pel, qp); qrot(qp, g_pelvis_fwd, af);
        if (have && g_body_turn && (hm[0] || hm[1]) && (af[0] || af[1])) {
            float head_yaw = atan2f(hm[1], hm[0]) * 57.2958f, anim = atan2f(af[1], af[0]) * 57.2958f;
            if (!g_anim_yaw_ok) { g_anim_yaw = anim; g_anim_yaw_ok = true; }
            g_anim_yaw = wrap180(g_anim_yaw + wrap180(anim - g_anim_yaw) * fminf(1.0f, dt / 0.05f));   // light smoothing only
            float e = wrap180(head_yaw - g_anim_yaw - g_body_delta);
            float ex = fabsf(e) > g_body_deadzone ? e - (e > 0 ? g_body_deadzone : -g_body_deadzone) : 0;
            float step = clampf(ex * fminf(1.0f, dt / 0.08f), -360 * dt, 360 * dt);
            g_body_delta = wrap180(g_body_delta + step);
        } else g_body_delta = 0;
        if (!std::isfinite(g_body_delta)) g_body_delta = 0;
        if (have) {
            float h = g_body_delta * 0.0087266463f, rz[4] = { 0, 0, sinf(h), cosf(h) };
            quat_mul(rz, qp, pel_now_q); memcpy(pel_now_p, pel + 4, 12); pel_ok = true;
        }
        if (have && fabsf(g_body_delta) > 0.3f) {
            float h = g_body_delta * 0.0087266463f, rz[4] = { 0, 0, sinf(h), cosf(h) }, q[4], o[8];
            quat_mul(rz, qp, q);
            tq(q, o); memcpy(o + 4, pel + 4, 16);       // tq is its own inverse: back to the stored convention
            memcpy(camera::bone_slot_data(kPelvisSlot), o, 32);
            active[kPelvisSlot] = true;
        }
    }

    // ---- arms
    memcpy(g_prev_q, g_last_q, sizeof g_prev_q);        // what the game's latest bone values were built from
    // Shoulders: the clavicles are fixed to the chest (as learned), so the arm roots never follow the animation.
    float sh[2][3] = {}, clav_q[2][4], clav_p[2][3], ch[8];
    bool sh_ok = read_bone(kChest, ch);
    if (sh_ok) {
        float qc[4]; tq(ch, qc);
        for (int a = 0; a < 2; a++) {
            const ArmRest& R = g_rest[a];
            float off[3];
            quat_mul(qc, R.clav_rel, clav_q[a]);
            qrot(qc, R.clav_pos, off); for (int i = 0; i < 3; i++) clav_p[a][i] = ch[4 + i] + off[i];
            qrot(clav_q[a], R.shoulder, off); for (int i = 0; i < 3; i++) sh[a][i] = clav_p[a][i] + off[i];
        }
    }
    float lat[3] = { 0, 1, 0 }, bfwd[3] = { 1, 0, 0 };
    if (sh_ok && (sh[0][0] != sh[1][0] || sh[0][1] != sh[1][1])) body_frame(sh[0], sh[1], lat, bfwd);
    for (int a = 0; a < 2 && sh_ok; a++) {
        const ArmDef& A = kArms[a];
        const ArmRest& R = g_rest[a];
        const VrDevice& w = a ? g_vr.right : g_vr.left;
        // Phone open: the headset holds its last picture anyway, and the game draws the phone screen from where Aiden's
        // hand holds the phone - so let him hold it his way and the wrist panel stays steady.
        if (!g_arms_on || g_cal_state == 1 || !w.valid || (g_menu_kind == 1 && g_hands_ui)) continue;
        // wand -> game world -> model space
        float dpos[3] = { w.pos[0] - g_vr.hmd.pos[0], w.pos[1] - g_vr.hmd.pos[1], w.pos[2] - g_vr.hmd.pos[2] };
        float fwd_t[3] = { -w.m[0][2], -w.m[1][2], -w.m[2][2] }, right_t[3] = { w.m[0][0], w.m[1][0], w.m[2][0] };
        float up_t[3] = { w.m[0][1], w.m[1][1], w.m[2][1] }, wu[3], U[3];
        float wp[3], wf[3], wr[3], T[3], F[3], Rt[3];
        tracking_to_world(f, r, u, dpos, wp); for (int i = 0; i < 3; i++) wp[i] += eye[i];
        tracking_to_world(f, r, u, fwd_t, wf); tracking_to_world(f, r, u, right_t, wr); tracking_to_world(f, r, u, up_t, wu);
        world_to_model(M, wp, T, false); world_to_model(M, wf, F, true); world_to_model(M, wr, Rt, true); world_to_model(M, wu, U, true);
        if (g_cal_state == 2) {                         // T-pose: collect samples
            norm3(F);
            for (int i = 0; i < 3; i++) { g_cal_acc[a][i] += T[i]; g_cal_acc[a][3 + i] += F[i]; }
            if (a == 1) g_cal_n++;
            continue;
        }
        if (!g_arm_cal.ok) continue;                    // not calibrated: leave the arms to the game
        float mid[3] = { (sh[0][0] + sh[1][0]) / 2, (sh[0][1] + sh[1][1]) / 2, (sh[0][2] + sh[1][2]) / 2 };
        cal_fix(T, false, mid, lat, bfwd); cal_fix(F, true, mid, lat, bfwd); cal_fix(Rt, true, mid, lat, bfwd); cal_fix(U, true, mid, lat, bfwd);
        norm3(F); norm3(Rt);
        for (int i = 0; i < 3; i++) T[i] -= F[i] * g_arms_grip;           // the wrist sits behind the grip
        // two-bone IK from the shoulder
        const float* S = sh[a];
        float L1 = R.len[0], L2 = R.len[1];
        float D[3] = { T[0] - S[0], T[1] - S[1], T[2] - S[2] };
        float d = sqrtf(dot3(D, D));
        if (d < 1e-4f) continue;
        float dir[3] = { D[0] / d, D[1] / d, D[2] / d };
        if (d > (L1 + L2) * 0.998f) {                  // out of reach: stretch the arm a little rather than stop short
            float k = clampf(d / ((L1 + L2) * 0.998f), 1.0f, g_arm_stretch);
            L1 *= k; L2 *= k;
        }
        d = clampf(d, fabsf(L1 - L2) + 0.02f, L1 + L2 - 0.002f);
        float ax = (L1 * L1 - L2 * L2 + d * d) / (2 * d), hh = sqrtf(fmaxf(L1 * L1 - ax * ax, 0.0f));
        float pole[3];                                  // elbows down, a bit out and back
        for (int i = 0; i < 3; i++) pole[i] = -0.2f * bfwd[i] + A.side * g_arms_pole_out * lat[i];
        pole[2] -= 1.0f;
        float pd = dot3(pole, dir);
        float pp[3] = { pole[0] - dir[0] * pd, pole[1] - dir[1] * pd, pole[2] - dir[2] * pd }; norm3(pp);
        float E[3], H[3], du[3], df[3], n[3];
        for (int i = 0; i < 3; i++) { E[i] = S[i] + dir[i] * ax + pp[i] * hh; H[i] = S[i] + dir[i] * d; }
        for (int i = 0; i < 3; i++) { du[i] = E[i] - S[i]; df[i] = H[i] - E[i]; }
        cross3(pp, dir, n); norm3(n);                   // elbow hinge (same as du x df)
        // whole-bone orientations from (limb direction, hinge) - nothing left from the animation
        float qu[4], qf[4], qh[4];
        frame_quat(R.axis[0], R.hinge[0], du, n, qu);
        frame_quat(R.axis[1], R.hinge[1], df, n, qf);
        if (g_hands_follow) {
            float roll = g_hand_roll[a] * 0.0174532925f, c[3], s[3], Fh[3];
            // the hand (and the gun in it) points along the tilted aim line, not the wand's tip axis
            norm3(U);                                  // wand "up" (trackpad side) in model space
            float ta = g_aim_tilt * 0.0174532925f;
            for (int i = 0; i < 3; i++) Fh[i] = F[i] * cosf(ta) - U[i] * sinf(ta);
            norm3(Fh);
            cross3(Fh, Rt, c);
            for (int i = 0; i < 3; i++) s[i] = Rt[i] * cosf(roll) + c[i] * sinf(roll);
            frame_quat(R.axis[2], R.hinge[2], Fh, s, qh);
        } else frame_quat(R.axis[2], R.hinge[2], df, n, qh);
        bool finite = true;
        for (int i = 0; i < 4; i++) finite &= std::isfinite(qu[i]) && std::isfinite(qf[i]) && std::isfinite(qh[i]);
        for (int i = 0; i < 3; i++) finite &= std::isfinite(E[i]) && std::isfinite(H[i]);
        if (!finite) continue;                          // never hand the game garbage
        const float* qs[3] = { qu, qf, qh }; const float* ps[3] = { S, E, H };
        for (int k = 0; k < 3; k++) {
            float o[8]; tq(qs[k], o); memcpy(o + 4, ps[k], 12); o[7] = 0;
            memcpy(camera::bone_slot_data(A.slot0 + k), o, 32);
            memcpy(g_last_q[A.slot0 + k], qs[k], 16);
            active[A.slot0 + k] = true;
        }
        {
            float o[8]; tq(clav_q[a], o); memcpy(o + 4, clav_p[a], 12); o[7] = 0;
            memcpy(camera::bone_slot_data(A.clav_slot), o, 32);
            memcpy(g_last_q[A.clav_slot], clav_q[a], 16);
            active[A.clav_slot] = true;
        }
        for (int h = 0; h < 4; h++) {                   // twist/helper bones: same turn as their segment, same place along it
            const Helper& Hp = kHelpers[a][h];
            float q[4], o[8];
            quat_mul(Hp.seg ? qf : qu, R.helper_rel[h], q);
            tq(q, o);
            const float* p0 = Hp.seg ? E : S; const float* p1 = Hp.seg ? H : E;
            for (int i = 0; i < 3; i++) o[4 + i] = p0[i] + (p1[i] - p0[i]) * R.helper_t[h];
            o[7] = 0;
            int slot = kHelperSlot0 + a * 4 + h;
            memcpy(camera::bone_slot_data(slot), o, 32);
            active[slot] = true;
        }
    }
    // ---- pocket detector: where would the ANIMATION put each hand?
    if (pel_ok && sh_ok) {
        for (int a = 0; a < 2; a++) {
            const ArmDef& A = kArms[a];
            const ArmRest& R = g_rest[a];
            float hand[3]; bool ok = false;
            const float* rc = camera::bone_slot_raw(A.clav_slot); const float* ru = camera::bone_slot_raw(A.slot0);
            const float* rf = camera::bone_slot_raw(A.slot0 + 1);
            if (g_mapped[A.clav_slot] && g_mapped[A.slot0] && g_mapped[A.slot0 + 1] && rc && ru && rf) {
                // raw child = (our parent) * (animation's local turn) -> rebuild the animation's own chain
                float qc[4], qu[4], qf[4], c[4], lu[4], lf[4], au[4], af[4], v[3], S0[3], E0[3];
                tq(rc, qc); tq(ru, qu); tq(rf, qf);
                qconj(g_prev_q[A.clav_slot], c); quat_mul(c, qu, lu);
                qconj(g_prev_q[A.slot0], c); quat_mul(c, qf, lf);
                quat_mul(qc, lu, au); quat_mul(au, lf, af);
                qrot(qc, R.shoulder, v); for (int i = 0; i < 3; i++) S0[i] = rc[4 + i] + v[i];
                qrot(au, R.axis[0], v); for (int i = 0; i < 3; i++) E0[i] = S0[i] + v[i] * R.len[0];
                qrot(af, R.axis[1], v); for (int i = 0; i < 3; i++) hand[i] = E0[i] + v[i] * R.len[1];
                ok = true;
            } else { float b[8]; if (read_bone(A.hand, b)) { memcpy(hand, b + 4, 12); ok = true; } }
            bool in = false;
            if (ok) {
                float pt[3]; qrot(pel_now_q, g_pocket_pt[a], pt);
                float d[3] = { hand[0] - pel_now_p[0] - pt[0], hand[1] - pel_now_p[1] - pt[1], hand[2] - pel_now_p[2] - pt[2] };
                in = sqrtf(dot3(d, d)) < (g_pocket[a] ? 0.28f : 0.20f);   // hysteresis
            }
            if (in != g_pocket[a]) { g_pocket[a] = in; log("Pocket detector: %s hand %s", a ? "right" : "left", in ? "IN the pocket (animation)" : "out"); }
        }
        static ULONGLONG in_since = 0;
        if (g_pocket[0] || g_pocket[1]) { if (!in_since) in_since = GetTickCount64(); }
        else in_since = 0;
        if (in_since && GetTickCount64() - in_since > 150) g_pocket_until = GetTickCount64() + 1500;   // steady, not a brush past
        // 0.16.3: the hold fades in / out over ~0.25 s instead of switching (the coat snapped between the held and the
        // animated pose when the detector toggled - the flicker), and stays 1.5 s after the hand was last seen in.
        static float w = 0;
        float want = (g_pocket_fix && g_arms_on && GetTickCount64() < g_pocket_until) ? 1.0f : 0.0f;
        w += clampf(want - w, -dt * 4.0f, dt * 4.0f);
        if (w > 0.001f) {   // hold the hip-area bones it disturbs
            for (int k = 0; k < g_coat_n; k++) {
                const CoatBone& cb = g_coat[k];
                float q[4], v[3], o[8], p[3];
                quat_mul(pel_now_q, cb.rel_q, q); qrot(pel_now_q, cb.rel_p, v);
                for (int i = 0; i < 3; i++) p[i] = pel_now_p[i] + v[i];
                const float* raw = camera::bone_slot_raw(kCoatSlot0 + k);
                float rq[4];
                bool raw_ok = g_mapped[kCoatSlot0 + k] && raw && w < 0.999f;
                if (raw_ok) { tq(raw, rq); float l = dot3(rq, rq) + rq[3] * rq[3]; raw_ok = l > 0.8f && l < 1.2f && std::isfinite(raw[4]); }
                if (raw_ok) {   // blend: animation (raw) -> held, by w
                    if (dot3(rq, q) + rq[3] * q[3] < 0) for (float& x : rq) x = -x;
                    float b[4]; for (int i = 0; i < 4; i++) b[i] = rq[i] + (q[i] - rq[i]) * w;
                    float l = sqrtf(dot3(b, b) + b[3] * b[3]); if (l > 1e-6f) for (float& x : b) x /= l; else memcpy(b, q, 16);
                    memcpy(q, b, 16);
                    for (int i = 0; i < 3; i++) p[i] = raw[4 + i] + (p[i] - raw[4 + i]) * w;
                }
                tq(q, o); for (int i = 0; i < 3; i++) o[4 + i] = p[i]; o[7] = 0;
                memcpy(camera::bone_slot_data(kCoatSlot0 + k), o, 32);
                active[kCoatSlot0 + k] = true;
            }
        }
    }
    if (g_cal_state == 2 && GetTickCount64() - g_cal_t0 > 700 && g_cal_n > 5 && sh_ok) arms_finish_calibration(sh[0], sh[1]);
    // The pelvis stays mapped while turning (its raw value is how we see the animation's own facing).
    bool any = false;
    for (int s = 0; s < kSlots; s++) { if (s >= kCoatSlot0 + g_coat_n) break; map_slot(slot_bone(s), s, active[s]); any |= active[s]; }
    camera::set_bone_override_enabled(any);
    bool arms = active[0] || active[3];
    if (g_cal_state) snprintf(g_body_msg, sizeof g_body_msg, "T-pose calibration...");
    else snprintf(g_body_msg, sizeof g_body_msg, "body turned %+.0f deg, arms %s", g_body_delta,
                  !g_arms_on ? "off" : !g_arm_cal.ok ? "not calibrated - do the T-pose (Ctrl+F11)" : arms ? "following the wands" : "waiting for the wands");
}

// ---------------------------------------------------------------- aim distance from the game's depth
// While aiming, the depth buffer of a normal headset frame is read back. The wand's line is walked through that
// picture (projected with the camera that frame was drawn with) until it goes behind what's drawn there: that's what
// you are pointing at. The game's own aim is then steered through that point, so bullets land where the wand points
// at any distance. Depth values -> metres needs the camera's near plane: learned once from the ground, saved.
static ID3D11Texture2D* g_depth_stage[2]; static bool g_depth_full[2]; static int g_depth_i;
static FrameCam g_depth_cam[2];
static std::vector<float> g_nstd, g_nrev;

static float read_depth(const D3D11_MAPPED_SUBRESOURCE& m, DXGI_FORMAT f, UINT x, UINT y) {
    const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
    switch (f) {
        case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
            { uint32_t v; memcpy(&v, row + x * 4, 4); return (v & 0xFFFFFF) / 16777215.0f; }
        case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT:
            { float v; memcpy(&v, row + x * 4, 4); return v; }
        case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            { float v; memcpy(&v, row + x * 8, 4); return v; }
        case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM:
            { uint16_t v; memcpy(&v, row + x * 2, 2); return v / 65535.0f; }
        default: return -1;
    }
}
static float median(std::vector<float> v) { std::sort(v.begin(), v.end()); return v.empty() ? 0 : v[v.size() / 2]; }
static float depth_to_z(float d) {      // view-space distance along the camera's forward
    if (g_depth_model == 1) return d >= 0.999999f ? 1e9f : g_depth_near / (1 - d);
    if (g_depth_model == 2) return d <= 1e-7f ? 1e9f : g_depth_near / d;
    return -1;
}

// Called every Present. 'eye_frame' = the frame just shown is a trusted headset (eye) frame.
static void aim_depth_frame(bool eye_frame) {
    ID3D11Resource* dep = framecap::scene_depth();
    if (!dep || !g_aim_auto) return;
    ID3D11Texture2D* dt = nullptr; dep->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&dt);
    if (!dt) return;
    D3D11_TEXTURE2D_DESC dd; dt->GetDesc(&dd); dt->Release();
    int j = g_depth_i ^ 1;
    D3D11_MAPPED_SUBRESOURCE m;
    if (g_depth_stage[j] && g_depth_full[j] && SUCCEEDED(g_context->Map(g_depth_stage[j], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
        const FrameCam& c = g_depth_cam[j];
        float tanH = tanf(c.fov / 2), tanV = tanH * dd.Height / dd.Width;
        auto sample = [&](float xn, float yn) {           // xn, yn in -1..1 (yn up)
            UINT px = (UINT)clampf((xn * 0.5f + 0.5f) * dd.Width, 0, (float)dd.Width - 1);
            UINT py = (UINT)clampf((0.5f - yn * 0.5f) * dd.Height, 0, (float)dd.Height - 1);
            return read_depth(m, dd.Format, px, py);
        };
        if (g_depth_model == 0) {                         // learn the near plane from the ground beside / below you
            float h = c.pos[2] - c.ground;
            for (float xn : { -0.45f, 0.45f }) for (float yn : { -0.45f, -0.6f, -0.75f }) {
                float ray_z = c.m[0][2] + xn * tanH * c.m[2][2] + yn * tanV * c.m[1][2];   // forward + right + up parts
                float d = sample(xn, yn);
                if (d <= 0 || d >= 1 || ray_z > -0.25f || h < 0.8f || h > 2.6f) continue;
                float z = h / -ray_z;                     // ray scaled so its forward part is 1 -> this IS the view depth
                g_nstd.push_back(z * (1 - d)); g_nrev.push_back(z * d);
            }
            g_depth_samples = (int)g_nstd.size();
            if (g_nstd.size() >= 120) {
                auto spread = [](std::vector<float> v, float med) { for (float& x : v) x = fabsf(x - med); return median(v) / fmaxf(med, 1e-6f); };
                float ms = median(g_nstd), mr = median(g_nrev), ss = spread(g_nstd, ms), sr = spread(g_nrev, mr);
                log("Aim: depth calibration - standard n %.4f (spread %.2f), reversed n %.4f (spread %.2f)", ms, ss, mr, sr);
                int model = ss <= sr ? 1 : 2; float n = model == 1 ? ms : mr, sp = model == 1 ? ss : sr;
                if (sp < 0.2f && n > 0.001f && n < 5) {
                    g_depth_model = model; g_depth_near = n;
                    log("Aim: depth calibrated (%s, near %.4f) - saved", model == 1 ? "standard" : "reversed", n);
                    save_config();
                } else log("Aim: depth calibration not consistent yet - collecting again");
                g_nstd.clear(); g_nrev.clear();
            }
        } else if (g_aiming) {
            // Walk a line through the picture until it goes behind what's drawn there. Returns the distance along it.
            auto march = [&](const float* o, const float* dir, bool& found) {
                auto behind = [&](float t, bool& onscreen) {
                    float P[3], v[3];
                    for (int i = 0; i < 3; i++) { P[i] = o[i] + dir[i] * t; v[i] = P[i] - c.pos[i]; }
                    float z = dot3(v, c.m[0]);
                    onscreen = false;
                    if (z < 0.3f) return false;
                    float xn = dot3(v, c.m[2]) / z / tanH, yn = dot3(v, c.m[1]) / z / tanV;
                    if (fabsf(xn) > 0.98f || fabsf(yn) > 0.98f) return false;
                    onscreen = true;
                    float zs = depth_to_z(sample(xn, yn));
                    return zs > 1.0f && z >= zs;              // (closer than 1 m = Aiden's own arms / gun: look past)
                };
                float prev_t = 0, hit_t = 300; bool seen = false;
                found = false;
                for (float t = 1.0f; t < 300; t *= 1.07f) {
                    bool on;
                    if (behind(t, on)) {
                        float lo = prev_t > 0 ? prev_t : t * 0.9f, hi = t;
                        for (int k = 0; k < 6; k++) { float mid = (lo + hi) / 2; bool o2; if (behind(mid, o2)) hi = mid; else lo = mid; }
                        hit_t = hi; found = true; break;
                    }
                    if (on) seen = true;
                    else if (seen || t > 8) break;            // left the picture (or never came into it)
                    prev_t = t;
                }
                return hit_t;
            };
            bool found;
            float hit_t = march(c.wand_o, c.wand_d, found);   // what the wand points at
            for (int i = 0; i < 3; i++) g_aim_hit[i] = c.wand_o[i] + c.wand_d[i] * hit_t;
            if (g_aim_live <= 0) g_aim_live = hit_t;
            g_aim_live_t = GetTickCount64();
            float gd = -1; bool gfound = false;
            if (c.gok) {                                       // what the game's aim line hits = where bullets go
                float dir[3] = { c.gfwd[0], c.gfwd[1], c.gfwd[2] };
                if (norm3(dir) > 0.5f) {
                    gd = march(c.gpos, dir, gfound);
                    float t = gfound ? gd : 150.0f, w[3];
                    for (int i = 0; i < 3; i++) w[i] = c.gpos[i] + dir[i] * t;
                    bool fresh = GetTickCount64() - g_dot_t > 400;
                    for (int i = 0; i < 3; i++) g_dot_w[i] = fresh ? w[i] : g_dot_w[i] + (w[i] - g_dot_w[i]) * 0.6f;
                    g_dot_t = GetTickCount64();
                }
            }
            static ULONGLONG last_log = 0;
            if (GetTickCount64() - last_log > 1000) {
                last_log = GetTickCount64();
                float miss = 0;
                if (gfound && found) { float d[3]; for (int i = 0; i < 3; i++) d[i] = g_dot_w[i] - g_aim_hit[i]; miss = sqrtf(dot3(d, d)); }
                float ad[3]; wand_aim_dir(g_vr.right, ad);
                const float (*wm)[4] = g_vr.right.m;
                log("Aim: wand points at %.1f m%s, bullets hit %s%.1f m (%.2f m apart); grip check: tip %+.0f deg, aim line %+.0f deg, "
                    "head %+.0f deg (up = +), trackpad facing up %.2f (positive = axes as assumed, grip angle %.0f)",
                    hit_t, found ? "" : " (nothing hit - far)", gfound ? "" : "far / ", gd, miss,
                    asinf(clampf(-wm[1][2], -1, 1)) * 57.3f, asinf(clampf(ad[1], -1, 1)) * 57.3f,
                    asinf(clampf(-g_vr.hmd.m[1][2], -1, 1)) * 57.3f, wm[1][1], g_aim_tilt);
            }
        }
        g_context->Unmap(g_depth_stage[j], 0);
        g_depth_full[j] = false;
    }
    // Take a new copy every 3rd trusted eye frame while aiming (or while still learning).
    static unsigned n = 0;
    if (!eye_frame || !(g_aiming || g_depth_model == 0) || (++n % 2) != 0) return;
    const FrameCam& fc = g_frame_cam[g_shown_idx & 7];
    if (!fc.ok || fc.fov < 0.05f || fc.fov > 3) return;
    int i = g_depth_i;
    D3D11_TEXTURE2D_DESC sd = dd; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    if (g_depth_stage[i]) { D3D11_TEXTURE2D_DESC od; g_depth_stage[i]->GetDesc(&od); if (od.Width != sd.Width || od.Height != sd.Height || od.Format != sd.Format) { g_depth_stage[i]->Release(); g_depth_stage[i] = nullptr; } }
    if (!g_depth_stage[i] && FAILED(g_device->CreateTexture2D(&sd, nullptr, &g_depth_stage[i]))) { g_depth_stage[i] = nullptr; return; }
    g_depth_cam[i] = fc;
    g_context->CopyResource(g_depth_stage[i], dep);
    g_depth_full[i] = true;
    g_depth_i = i ^ 1;
}

// The red dot: the point the game's aim line hits (from the depth above), shown at that spot in the headset while
// aiming - that's where bullets land. World -> room: the inverse of tracking_to_world, from where your eyes are now.
static ID3D11Texture2D* g_dot_tex; static bool g_dot_shown;
static void world_to_tracking(const float* f, const float* r, const float* u, const float* w, float* d) {
    float fa = dot3(w, f), ra = -dot3(w, r);
    float y = g_head_origin_yaw * 0.0174532925f;
    d[0] = fa * sinf(y) + ra * cosf(y);
    d[1] = dot3(w, u);
    d[2] = -fa * cosf(y) + ra * sinf(y);
}
static void dot_frame() {
    ULONGLONG now = GetTickCount64();
    bool show = g_red_dot && g_aim_auto && g_depth_model && g_aiming && !g_scoped && g_hmd_display && !g_screen_mode &&
                !g_remote_view && !g_in_vehicle && g_have_eye && g_vr.hmd.valid && g_dot_t && now - g_dot_t < 400 && vr_is_scene();
    if (!show) { if (g_dot_shown) { vr_dot_hide(); g_dot_shown = false; } return; }
    if (!g_dot_tex) {
        static uint32_t px[64 * 64];
        for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
            float d = sqrtf((x - 31.5f) * (x - 31.5f) + (y - 31.5f) * (y - 31.5f));
            float a_in = clampf(10.5f - d, 0, 1), a_out = clampf(14.0f - d, 0, 1);   // red dot, thin dark ring
            uint8_t r = (uint8_t)(255 * a_in), g = (uint8_t)(30 * a_in), b = (uint8_t)(30 * a_in);
            uint8_t a = (uint8_t)(255 * fmaxf(a_in, a_out * 0.75f));
            px[y * 64 + x] = (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24;
        }
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = d.Height = 64; d.MipLevels = d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = { px, 64 * 4, 0 };
        if (FAILED(g_device->CreateTexture2D(&d, &init, &g_dot_tex))) { g_dot_tex = nullptr; return; }
    }
    float f[3], r[3], u[3], w[3], t[3];
    camera::base_frame(f, r, u);
    for (int i = 0; i < 3; i++) w[i] = g_dot_w[i] - g_eye_world[i];
    world_to_tracking(f, r, u, w, t);
    float dist = sqrtf(dot3(t, t));
    if (dist < 0.5f) { if (g_dot_shown) { vr_dot_hide(); g_dot_shown = false; } return; }
    float pos[3] = { g_vr.hmd.pos[0] + t[0], g_vr.hmd.pos[1] + t[1], g_vr.hmd.pos[2] + t[2] };
    float width = 2 * dist * tanf(g_dot_size * 0.5f * 0.0174533f) * 64 / 21;   // the dot is 21 px of the 64
    int e = vr_dot_show(g_dot_tex, pos, g_vr.hmd.pos, width);
    static int last_e = -100;
    if (!g_dot_shown && e != last_e) log("Red dot shown (%d)", e);
    last_e = e;
    g_dot_shown = e == 0;
}

// ---------------------------------------------------------------- HUD: panel, phone, wheel, menus
static bool  g_hud_panel = true;
static float g_hud_dist = 1.2f, g_hud_width = 1.5f, g_hud_down = 0.05f;
static bool  g_hide_reticle = true;       // leave the game's crosshair (screen centre) out of the headset picture
static float g_reticle_box = 0.025f;      // 0.16.14 (was 0.06, cut into other HUD): half-height of that box (fraction of the screen height; width matches in pixels)
static bool  g_hud_world = true;          // keep the middle of the HUD in the 3D picture (markers line up with the world)
static float g_hud_world_size = 0.8f;
static float g_hud_rect[4];               // the middle area actually used (0..1 screen)
// The game draws both on the right half, tilted in 3D and swaying, and hides the rest of the HUD
// while they're open - so the cut-out areas are generous.
static float g_phone_rect[4] = { 0.40f, 0.04f, 0.94f, 0.97f };
static float g_wheel_rect[4] = { 0.40f, 0.10f, 0.97f, 0.82f };
static const float kMenuProbe[4] = { 0.44f, 0.08f, 0.95f, 0.92f };   // where an open phone/wheel shows up
static float g_phone_width = 0.42f, g_phone_tilt = -65.0f, g_phone_up = 0.03f, g_phone_back = 0.12f;   // docked on the left wrist
static float g_wheel_width = 0.56f, g_wheel_tilt = -50.0f, g_wheel_up = 0.08f, g_wheel_back = 0.03f;
static float g_hud_cover, g_probe_cover, g_probe_h, g_left_cover;
static ID3D11Texture2D *g_hud_vr, *g_hud_zero, *g_hand_vr, *g_phone_vr, *g_probe_stage[2];
static bool  g_probe_full[2]; static int g_probe_i;
static int   g_hand_shown = -1;
static bool  g_hud_shown;
static bool  g_phone_fresh; static int g_phone_wait;   // phone panel has a picture from a game-view frame
static const float* menu_rect() { return g_menu_kind == 2 ? g_wheel_rect : g_phone_rect; }

static bool ensure_tex(ID3D11Texture2D*& t, const D3D11_TEXTURE2D_DESC& want, const void* init_px = nullptr) {
    if (t) { D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); if (d.Width == want.Width && d.Height == want.Height) return true; t->Release(); t = nullptr; }
    D3D11_SUBRESOURCE_DATA init = { init_px, want.Width * 4, 0 };
    return SUCCEEDED(g_device->CreateTexture2D(&want, init_px ? &init : nullptr, &t)) && t;
}

// Scope frame: the middle of the zoomed picture (with the game's scope overlay) on the right wand.
static ID3D11Texture2D* g_scope_tex;
static void scope_frame() {
    ID3D11Texture2D* opaque = screen_blit(g_eye_tex);
    if (!opaque) return;
    D3D11_TEXTURE2D_DESC d; opaque->GetDesc(&d);
    UINT side = d.Height * 9 / 10;
    D3D11_TEXTURE2D_DESC sd = d; sd.Width = sd.Height = side; sd.BindFlags = D3D11_BIND_SHADER_RESOURCE; sd.MiscFlags = 0;
    if (!ensure_tex(g_scope_tex, sd)) return;
    D3D11_BOX b = { (d.Width - side) / 2, (d.Height - side) / 2, 0, (d.Width + side) / 2, (d.Height + side) / 2, 1 };
    g_context->CopySubresourceRegion(g_scope_tex, 0, 0, 0, 0, opaque, 0, &b);
    int e = vr_scope_show(g_scope_tex, g_scope_size, g_scope_up, g_scope_back, g_aim_tilt);
    if (!g_scope_shown) log("Scope picture on the right wand (%d)", e);
    g_scope_shown = e == 0;
}

// Every 6 frames: read the HUD layer back (a copy made last time, so we never wait on the GPU) and
// work out whether a full-screen menu, the phone or the weapon wheel is open.
static void hud_probe(ID3D11Texture2D* src, const D3D11_TEXTURE2D_DESC& sd) {
    // While the phone is open with the headset live, only game-view frames show the HUD as the game lays it out (eye
    // frames draw the phone for your head): copy from those only. Read back the copy made last time (no GPU wait).
    static unsigned fr = 0, last_copy = 0;
    ++fr;
    bool usable = g_phone_live && g_menu_kind == 1 ? g_shown_mid_g : (g_shown_type != 2 || !g_phone_live);
    bool copy_now = usable && fr - last_copy >= 6;
    if (!copy_now && !(g_probe_full[g_probe_i ^ 1])) return;
    int i = g_probe_i, j = i ^ 1;
    D3D11_TEXTURE2D_DESC d = sd; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    if (!g_probe_stage[i]) g_probe_full[i] = false;
    if (!ensure_tex(g_probe_stage[i], d)) return;
    D3D11_MAPPED_SUBRESOURCE m;
    if (g_probe_stage[j] && g_probe_full[j] && SUCCEEDED(g_context->Map(g_probe_stage[j], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
        D3D11_TEXTURE2D_DESC jd; g_probe_stage[j]->GetDesc(&jd);
        UINT px0 = (UINT)(kMenuProbe[0] * jd.Width), px1 = (UINT)(kMenuProbe[2] * jd.Width);
        UINT py0 = (UINT)(kMenuProbe[1] * jd.Height), py1 = (UINT)(kMenuProbe[3] * jd.Height);
        unsigned cov = 0, tot = 0, pcov = 0, ptot = 0, lcov = 0, ltot = 0; int y0 = -1, y1 = -1;
        UINT lx1 = (UINT)(0.30f * jd.Width), ly0 = (UINT)(0.55f * jd.Height);   // minimap corner
        for (UINT y = 0; y < jd.Height; y += 4) {
            const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
            bool in_y = y >= py0 && y < py1;
            unsigned rc = 0;
            for (UINT x = 0; x < jd.Width; x += 4) {
                bool on = row[x * 4 + 3] > 40;
                tot++; cov += on;
                if (in_y && x >= px0 && x < px1) { ptot++; pcov += on; rc += on; }
                if (x < lx1 && y >= ly0) { ltot++; lcov += on; }
            }
            if (rc >= 6) { if (y0 < 0) y0 = (int)y; y1 = (int)y; }
        }
        g_context->Unmap(g_probe_stage[j], 0);
        g_probe_full[j] = false;
        g_hud_cover = tot ? (float)cov / tot : 0;
        g_probe_cover = ptot ? (float)pcov / ptot : 0;
        g_probe_h = y0 >= 0 ? (float)(y1 - y0) / jd.Height : 0;
        g_left_cover = ltot ? (float)lcov / ltot : 0;
        // Full-screen menu (pause menu, map): most of the screen is HUD.
        static int full_n = 0;
        bool full = g_menu_full ? g_hud_cover > 0.20f : g_hud_cover > 0.40f;
        if (full != g_menu_full) { if (++full_n >= 2) { g_menu_full = full; full_n = 0; log("Full-screen menu %s (HUD covers %.0f%% of the screen)", full ? "open" : "closed", g_hud_cover * 100); } }
        else full_n = 0;
        // Phone (tall) or weapon wheel: lots of HUD on the right half.
        int kind = g_menu_kind;
        if (g_menu_full || g_screen_mode) kind = 0;
        // The game hides the rest of the HUD (minimap) while the phone / wheel is open - other big pop-ups don't.
        else if (!kind && g_probe_cover > 0.08f && g_left_cover < 0.01f) kind = g_probe_h > 0.42f ? 1 : 2;
        else if (kind && (g_probe_cover < 0.03f || g_left_cover > 0.03f)) kind = 0;
        if (kind != g_menu_kind) {
            log("%s (right half %.0f%% covered, %.2f tall, minimap corner %.1f%%)", kind == 1 ? "Phone opened" : kind == 2 ? "Weapon wheel opened" :
                g_menu_kind == 1 ? "Phone closed" : "Weapon wheel closed", g_probe_cover * 100, g_probe_h, g_left_cover * 100);
            g_menu_kind = kind;
        }
    }
    if (!copy_now) return;
    last_copy = fr;
    g_context->CopyResource(g_probe_stage[i], src);
    g_probe_full[i] = true;
    g_probe_i = j;
}

static void hide_hands() { if (g_hand_shown >= 0) { vr_hand_hide(g_hand_shown); g_hand_shown = -1; } }

// The HUD composite runs while the game renders the frame that reaches the NEXT Present; with a delay of 2 that frame
// was requested (camera written) at the previous Present, not this one.
static int type_presented_next() {
    int idx = (int)g_pose_count + 1 - g_frame_delay;
    if (!g_hmd_display || idx >= (int)g_pose_count || idx < 0) return g_next_type;
    return g_eye_ring[idx & 7];
}

static void hud_update() {
    bool split = g_hud_panel && g_hmd_display;
    int next = type_presented_next();
    {   // middle area = what the headset shows, shrunk a bit (lens edges are blurry)
        float u0 = fminf(g_bounds[0].u0, g_bounds[1].u0), u1 = fmaxf(g_bounds[0].u1, g_bounds[1].u1);
        float v0 = fminf(g_bounds[0].v0, g_bounds[1].v0), v1 = fmaxf(g_bounds[0].v1, g_bounds[1].v1);
        if (!(u1 > u0) || !(v1 > v0)) { u0 = 0.25f; u1 = 0.75f; v0 = 0; v1 = 1; }
        float k = (1 - g_hud_world_size) * 0.5f;
        g_hud_rect[0] = u0 + (u1 - u0) * k; g_hud_rect[2] = u1 - (u1 - u0) * k;
        g_hud_rect[1] = v0 + (v1 - v0) * k; g_hud_rect[3] = v1 - (v1 - v0) * k;
    }
    // Menus / maps go on the big screen: the whole HUD goes back into the picture, nothing is cut out.
    if (g_screen_mode) framecap::hud_world_area(true, 0, 0, 1, 1);
    else if (g_scoped) framecap::hud_world_area(next == 2, 0, 0, 1, 1);   // scope overlay only in the scope picture
    else if (g_menu_kind == 1 && g_hands_ui) framecap::hud_world_area(false, 0, 0, 1, 1);   // phone: only on the wrist
    else if (next == 2) framecap::hud_world_area(false, 0, 0, 1, 1);
    else framecap::hud_world_area(g_hud_world, g_hud_rect[0], g_hud_rect[1], g_hud_rect[2], g_hud_rect[3]);
    { const float* r = menu_rect(); framecap::hud_exclude_area(!g_screen_mode && g_hands_ui && g_menu_kind, r[0], r[1], r[2], r[3]); }
    // The game's crosshair sits at the centre of the screen = stuck to your view in the headset. The red dot / wand
    // replace it, so that box is left out of the headset picture (kept in the scope picture and on the big screen).
    framecap::hud_exclude_area2(g_hide_reticle && !g_screen_mode && next != 2,
                                0.5f - g_reticle_box * 0.5625f, 0.5f - g_reticle_box, 0.5f + g_reticle_box * 0.5625f, 0.5f + g_reticle_box);
    framecap::hud_enable(g_context, split);
    ID3D11Texture2D* src = framecap::hud_texture();
    if (!split || !framecap::hud_active() || !src) {
        g_menu_kind = 0; g_menu_full = false;
        hide_hands();
        if (g_hud_shown) { vr_hud_hide(); g_hud_shown = false; }
        return;
    }
    D3D11_TEXTURE2D_DESC sd; src->GetDesc(&sd);
    hud_probe(src, sd);
    if (!g_hands_ui) g_menu_kind = 0;
    if (g_screen_mode || g_scoped || !vr_is_scene()) {
        hide_hands();
        if (g_hud_shown) { vr_hud_hide(); g_hud_shown = false; }
        return;
    }
    D3D11_TEXTURE2D_DESC d = sd; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0; d.MiscFlags = 0;
    if (!ensure_tex(g_hud_vr, d)) return;
    if (!g_hud_zero) { std::vector<uint32_t> z((size_t)sd.Width * sd.Height, 0); ensure_tex(g_hud_zero, d, z.data()); }
    else { D3D11_TEXTURE2D_DESC zd; g_hud_zero->GetDesc(&zd); if (zd.Width != sd.Width || zd.Height != sd.Height) { g_hud_zero->Release(); g_hud_zero = nullptr; return; } }
    // A game-view frame (phone panel source) has the HUD laid out for the game's own camera: the panel keeps its last picture.
    bool panel_frame = g_shown_type != 2;
    if (panel_frame) g_context->CopyResource(g_hud_vr, src);
    auto blank = [&](const float* r) {
        D3D11_BOX b = { (UINT)(r[0] * sd.Width), (UINT)(r[1] * sd.Height), 0, (UINT)(r[2] * sd.Width), (UINT)(r[3] * sd.Height), 1 };
        if (panel_frame && g_hud_zero && b.right > b.left && b.bottom > b.top && b.right <= sd.Width && b.bottom <= sd.Height)
            g_context->CopySubresourceRegion(g_hud_vr, 0, b.left, b.top, 0, g_hud_zero, 0, &b);
    };
    if (g_hud_world) blank(g_hud_rect);                  // the middle stays in the world
    int hand = g_menu_kind == 1 ? 0 : g_menu_kind == 2 ? 1 : -1;
    if (hand >= 0) {                              // phone -> left hand, weapon wheel -> right hand
        const float* r = menu_rect();
        D3D11_BOX b = { (UINT)(r[0] * sd.Width), (UINT)(r[1] * sd.Height), 0, (UINT)(r[2] * sd.Width), (UINT)(r[3] * sd.Height), 1 };
        D3D11_TEXTURE2D_DESC hd = d; hd.Width = b.right - b.left; hd.Height = b.bottom - b.top;
        ID3D11Texture2D*& tex = hand == 0 ? g_phone_vr : g_hand_vr;
        if (hand != 0) { g_phone_fresh = false; g_phone_wait = 0; }
        bool made = !tex;
        if (b.right > b.left && b.bottom > b.top && b.right <= sd.Width && b.bottom <= sd.Height && ensure_tex(tex, hd)) {
            if (made && hand == 0) g_phone_fresh = false;
            bool fallback = hand == 0 && !g_phone_live && !g_phone_fresh && ++g_phone_wait > 30;   // no game-view frames coming: plain copy
            // The phone is copied only from the game's own-view frames: there it is drawn as in the normal game,
            // not squashed by the headset's wide view or tilted by your head.
            if (hand == 1 || g_shown_mid_g || fallback) { g_context->CopySubresourceRegion(tex, 0, 0, 0, 0, src, 0, &b); if (hand == 0) g_phone_fresh = true; }
            if (g_hand_shown >= 0 && g_hand_shown != hand) { vr_hand_hide(g_hand_shown); g_hand_shown = -1; }
            int e = hand == 0 ? (g_phone_fresh ? vr_hand_show(0, tex, g_phone_width, g_phone_tilt, g_phone_up, g_phone_back) : -4)
                              : vr_hand_show(1, tex, g_wheel_width, g_wheel_tilt, g_wheel_up, g_wheel_back);
            static int last_e = 1;
            if (e != last_e && e != -4) { log("%s on the %s hand: %s (%d)", hand ? "Weapon wheel" : "Phone", hand ? "right" : "left", e == 0 ? "shown" : e == -3 ? "wand not found" : "error", e); last_e = e; }
            g_hand_shown = e == 0 ? hand : -1;
            blank(r);
        }
    } else { hide_hands(); g_phone_fresh = false; g_phone_wait = 0; }
    if (hand == 0) {      // phone open: the game hides the rest of its HUD anyway, and our eye frames would show the phone
        if (g_hud_shown) { vr_hud_hide(); g_hud_shown = false; }   // drawn for your head (huge, warped) - wrist only
        return;
    }
    int e = vr_hud_show(g_hud_vr, g_hud_dist, g_hud_width, g_hud_down);
    if (!g_hud_shown) log("HUD panel shown in the headset (%d)", e);
    g_hud_shown = true;
}

// ---------------------------------------------------------------- picture snapshot (F10)
// Saves what the game drew and the HUD layer, so the layout can be studied.
static bool g_snap_req; static int g_snap_n;
static char g_snap_msg[160] = "";
static bool save_texture_bmp(ID3D11Texture2D* src, const char* path) {
    if (!src) return false;
    D3D11_TEXTURE2D_DESC d; src->GetDesc(&d);
    if (d.SampleDesc.Count > 1) return false;
    D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    sd.MipLevels = 1; sd.ArraySize = 1;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &st)) || !st) return false;
    g_context->CopyResource(st, src);
    D3D11_MAPPED_SUBRESOURCE m;
    bool ok = false;
    if (SUCCEEDED(g_context->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
        if (FILE* f = fopen(path, "wb")) {
            uint32_t w = d.Width, h = d.Height, img = w * h * 4;
            uint8_t hdr[54] = { 'B', 'M' };
            auto put32 = [&](int o, uint32_t v) { memcpy(hdr + o, &v, 4); };
            put32(2, 54 + img); put32(10, 54); put32(14, 40); put32(18, w); put32(22, (uint32_t)-(int32_t)h);
            hdr[26] = 1; hdr[28] = 32; put32(34, img);
            fwrite(hdr, 1, 54, f);
            std::vector<uint8_t> row(w * 4);
            for (uint32_t y = 0; y < h; y++) {
                memcpy(row.data(), (const uint8_t*)m.pData + (size_t)y * m.RowPitch, w * 4);
                if (rgba) for (uint32_t x = 0; x < w; x++) std::swap(row[x * 4], row[x * 4 + 2]);
                fwrite(row.data(), 1, w * 4, f);
            }
            fclose(f);
            ok = true;
        }
        g_context->Unmap(st, 0);
    }
    st->Release();
    return ok;
}
static void take_snapshot(IDXGISwapChain* sc) {
    g_snap_req = false;
    char p1[MAX_PATH], p2[MAX_PATH];
    int n = ++g_snap_n;
    snprintf(p1, sizeof p1, "%s\\snap%d_frame.bmp", g_mod_dir, n);
    snprintf(p2, sizeof p2, "%s\\snap%d_hud.bmp", g_mod_dir, n);
    ID3D11Texture2D* back = nullptr;
    bool a = false, b = false;
    if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) && back) { a = save_texture_bmp(back, p1); back->Release(); }
    if (framecap::hud_texture()) b = save_texture_bmp(framecap::hud_texture(), p2);
    snprintf(g_snap_msg, sizeof g_snap_msg, "Snapshot %d saved (picture %s, HUD %s; frame shown: type %d request %u of %u, monitor %d)",
             n, a ? "ok" : "failed", b ? "ok" : "none", g_shown_type, g_shown_idx, g_pose_count, g_monitor_mode);
    log("%s - HUD covers %.0f%%, right half %.0f%% (%.2f tall), menu %d, phone/wheel %d, screen mode %d", g_snap_msg,
        g_hud_cover * 100, g_probe_cover * 100, g_probe_h, g_menu_full, g_menu_kind, g_screen_mode);
}

// ---------------------------------------------------------------- settings (WDVR.ini next to WDVR.asi)
struct Setting { const char* key; char type; void* p; };   // type: f float, b bool, i int
static const std::vector<Setting>& settings() {
    static std::vector<Setting> s = {
        { "fov_scale", 'f', &g_fov_scale }, { "frame_delay2", 'i', &g_frame_delay }, { "monitor_mode", 'i', &g_monitor_mode },
        { "stereo", 'b', &g_stereo }, { "swap_eyes2", 'b', &g_swap_eyes }, { "eye_distance", 'f', &g_eye_sep },
        { "invert_pitch", 'b', &camera::tuning().invert_pitch },
        { "fp_forward", 'f', &g_fp_off[0] }, { "fp_right", 'f', &g_fp_off[1] }, { "fp_eye_height", 'f', &g_fp_off[2] },
        { "car_forward", 'f', &g_fp_off_car[0] }, { "car_right", 'f', &g_fp_off_car[1] }, { "car_eye_height", 'f', &g_fp_off_car[2] },
        { "fp_lean", 'b', &g_fp_lean }, { "car_3d2", 'b', &g_car_3d }, { "car_frame2", 'b', &g_car_frame }, { "use_cockpit", 'b', &g_use_cockpit }, { "auto_cockpit", 'b', &g_auto_cockpit }, { "cam_button2", 'i', &g_cam_button }, { "car_axis", 'f', &g_car_axis }, { "steady_seat", 'b', &g_steady_seat }, { "fp_lead2", 'f', &g_fp_lead }, { "wand_input", 'b', &g_wand_input },
        { "trackpad_deadzone", 'f', &input::options().deadzone }, { "trackpad_turn", 'f', &input::options().turn_speed },
        { "screen_auto", 'b', &g_screen_auto }, { "screen_dist", 'f', &g_screen_dist }, { "screen_width", 'f', &g_screen_width },
        { "head_steers", 'b', &g_head_steers }, { "snap_turn", 'b', &g_snap_turn }, { "snap_deg", 'f', &g_snap_deg }, { "turn_rate", 'f', &g_turn_rate }, { "steer_range", 'f', &g_steer_range },
        { "steer_deadzone", 'f', &g_steer_dz }, { "steer_sign_x", 'f', &g_steer_sign[0] }, { "steer_sign_y", 'f', &g_steer_sign[1] },
        { "steer_locked_x", 'b', &g_steer_locked[0] }, { "steer_locked_y", 'b', &g_steer_locked[1] },
        { "body_turn", 'b', &g_body_turn }, { "body_deadzone", 'f', &g_body_deadzone },
        { "arms_on", 'b', &g_arms_on }, { "hands_follow", 'b', &g_hands_follow }, { "aim_tilt", 'f', &g_aim_tilt }, { "hand_roll_l", 'f', &g_hand_roll[0] },
        { "hand_roll_r", 'f', &g_hand_roll[1] }, { "arms_grip", 'f', &g_arms_grip }, { "arms_elbows", 'f', &g_arms_pole_out }, { "arm_reach", 'f', &g_arm_reach }, { "pocket_fix", 'b', &g_pocket_fix }, { "arm_stretch2", 'f', &g_arm_stretch },
        { "arms_conv", 'i', &g_arms_conv },
        { "arms_cal_v4", 'b', &g_arm_cal.ok }, { "arms_cal_yaw", 'f', &g_arm_cal.yaw }, { "arms_cal_fwd", 'f', &g_arm_cal.off[0] },
        { "arms_cal_left", 'f', &g_arm_cal.off[1] }, { "arms_cal_up", 'f', &g_arm_cal.off[2] },
        { "aim_dist", 'f', &g_aim_dist }, { "aim_auto", 'b', &g_aim_auto }, { "red_dot", 'b', &g_red_dot }, { "fov_lag", 'b', &g_fov_lag }, { "rot_lag", 'b', &g_rot_lag }, { "phone_cycle", 'i', &g_phone_cycle }, { "red_dot_size", 'f', &g_dot_size }, { "depth_near3", 'f', &g_depth_near }, { "depth_model3", 'i', &g_depth_model }, { "scope_on", 'b', &g_scope_on }, { "scope_size", 'f', &g_scope_size }, { "scope_up", 'f', &g_scope_up },
        { "scope_back", 'f', &g_scope_back }, { "scope_fov_max", 'f', &g_scope_fov_max },
        { "hud_panel", 'b', &g_hud_panel }, { "hud_dist", 'f', &g_hud_dist }, { "hud_width", 'f', &g_hud_width }, { "hud_down", 'f', &g_hud_down },
        { "hud_world", 'b', &g_hud_world }, { "hide_reticle", 'b', &g_hide_reticle }, { "reticle_box2", 'f', &g_reticle_box }, { "hud_world_size", 'f', &g_hud_world_size }, { "hands_ui", 'b', &g_hands_ui },
        { "phone_size6", 'f', &g_phone_width }, { "wrist_tilt", 'f', &g_phone_tilt }, { "wrist_up", 'f', &g_phone_up }, { "wrist_back", 'f', &g_phone_back },
        { "wheel_size2", 'f', &g_wheel_width }, { "wheel_tilt", 'f', &g_wheel_tilt }, { "wheel_up", 'f', &g_wheel_up }, { "wheel_back", 'f', &g_wheel_back },
    };
    return s;
}
static std::string config_text() {
    std::string t = "; WDVR settings - saved automatically\n";
    char line[96];
    for (const Setting& s : settings()) {
        if (s.type == 'f') snprintf(line, sizeof line, "%s=%.4f\n", s.key, *(float*)s.p);
        else if (s.type == 'b') snprintf(line, sizeof line, "%s=%d\n", s.key, *(bool*)s.p ? 1 : 0);
        else snprintf(line, sizeof line, "%s=%d\n", s.key, *(int*)s.p);
        t += line;
    }
    return t;
}
static std::string g_saved_text;
static void config_path(char* out, size_t n) { snprintf(out, n, "%s\\WDVR.ini", g_mod_dir); }
static void save_config() {
    char path[MAX_PATH]; config_path(path, sizeof path);
    std::string t = config_text();
    if (FILE* f = fopen(path, "w")) { fputs(t.c_str(), f); fclose(f); g_saved_text = t; log("Settings saved"); }
}
static void load_config() {
    char path[MAX_PATH]; config_path(path, sizeof path);
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[160];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char key[64]; float v;
        if (line[0] == ';' || sscanf(line, "%63[^=]=%f", key, &v) != 2) continue;
        for (const Setting& s : settings()) {
            if (strcmp(s.key, key)) continue;
            if (s.type == 'f') *(float*)s.p = v; else if (s.type == 'b') *(bool*)s.p = v != 0; else *(int*)s.p = (int)v;
            n++;
        }
    }
    fclose(f);
    g_saved_text = config_text();
    g_car_axis_ok = fabsf(g_car_axis) <= 180.0f;
    log("Settings loaded (%d values)%s", n, g_arm_cal.ok ? "" : " - arms not calibrated (do the T-pose)");
}

// ---------------------------------------------------------------- report + menu
static std::string build_report() {
    char buf[4096]; int n = 0;
    auto add = [&](const char* fmt, auto... a) {
        if (n >= (int)sizeof buf - 1) return;
        int w = snprintf(buf + n, sizeof buf - n, fmt, a...);
        if (w > 0) n = (n + w < (int)sizeof buf) ? n + w : (int)sizeof buf - 1;
    };
    add("WDVR %s | %ux%u | %s\n", WDVR_VERSION, g_width, g_height, camera::status().message);
    add("SteamVR: %s | HMD %d/%d L %d/%d R %d/%d (connected/tracking)\n", g_vr.status, g_vr.hmd.connected, g_vr.hmd.valid,
        g_vr.left.connected, g_vr.left.valid, g_vr.right.connected, g_vr.right.valid);
    add("Headset: %s | submit %d wait %d | FOV %.1f scale %.2f | %s\n", g_hmd_msg, g_submit_err, g_wait_err, g_fov_written * 57.2958f, g_fov_scale, g_vr_stall);
    add("Aiden %p | view: %s%s%s | cam %.1f m %.0f deg | aiming %d\n", (void*)g_aiden_obj, g_in_vehicle ? "vehicle" : "on foot",
        g_remote_view ? ", remote camera" : "", g_screen_mode ? ", screen mode" : "", g_cam_dist, g_cam_angle, g_aiming);
    add("Aim: auto %d, depth %s, model %d near %.4f, samples %d, live %.1f m\n", g_aim_auto, framecap::scene_depth() ? "found" : "none",
        g_depth_model, g_depth_near, g_depth_samples, g_aim_live);
    add("Frame ID: %s | frame delay %d | shown type %d idx %u (requests %u)\n", framecap::frame_id_status(), g_frame_delay, g_shown_type, g_shown_idx, g_pose_count);
    add("Steering %s: err %.0f/%.0f, signs %+.0f%s %+.0f%s, view yaw %.0f, game yaw %.0f pitch %.0f\n", g_steer_on ? "on" : "off",
        g_steer_err[0], g_steer_err[1], g_steer_sign[0], g_steer_locked[0] ? "!" : "?", g_steer_sign[1], g_steer_locked[1] ? "!" : "?",
        g_view_yaw, camera::game_yaw_deg(), camera::game_pitch_deg());
    add("Body: %s | cal %d turn %.0f off %.2f %.2f %.2f | conv %d | bone writes %u | pocket %d %d, coat bones %d\n", g_body_msg,
        g_arm_cal.ok, g_arm_cal.yaw * 57.3f, g_arm_cal.off[0], g_arm_cal.off[1], g_arm_cal.off[2], g_arms_conv, camera::bone_raw_seq(),
        g_pocket[0], g_pocket[1], g_coat_n);
    add("HUD: %s | covers %.0f%% | right half %.0f%% (%.2f tall) | menu %d phone/wheel %d\n", framecap::hud_status(), g_hud_cover * 100,
        g_probe_cover * 100, g_probe_h, g_menu_full, g_menu_kind);
    auto is = input::status();
    add("Input: %s %s, %u reads, real pad %d\n", is.hooked ? "hooked" : "not hooked", is.dll, is.game_reads, is.real_pad);
    return buf;
}

static void slider(const char* label, float* v, float lo, float hi, const char* fmt) {
    ImGui::SetNextItemWidth(220); ImGui::SliderFloat(label, v, lo, hi, fmt);
}

static void draw_menu() {
    ImGui::SetNextWindowSize(ImVec2(560, 680), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("WDVR - Watch Dogs VR  (INSERT to close)", &g_menu_open)) { ImGui::End(); return; }
    ImGui::Text("Version %s   %.0f fps", WDVR_VERSION, ImGui::GetIO().Framerate);
    ImGui::SameLine();
    if (ImGui::Button("Copy report for Claude")) ImGui::SetClipboardText(build_report().c_str());
    if (g_vr_stall[0]) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", g_vr_stall);

    if (ImGui::CollapsingHeader("Headset", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("SteamVR: %s", g_vr.status);
        if (!g_vr.ok && ImGui::Button("Connect to SteamVR")) { vr_init(g_mod_dir); VrState s; vr_poll(&s); log("SteamVR (retry): %s", s.status); }
        bool on = g_hmd_display;
        if (ImGui::Checkbox("VR on (game in the headset)", &on)) set_hmd_display(on);
        ImGui::SameLine(); ImGui::TextDisabled("%s", g_hmd_msg);
        if (ImGui::Button("Recenter (numpad 5)")) do_recenter();
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("World scale", &g_fov_scale, 0.6f, 1.6f, "%.2f")) compute_view_geometry();
        ImGui::SetNextItemWidth(220); ImGui::SliderInt("Frame delay (if one eye flickers while aiming, try 1 or 3)", &g_frame_delay, 1, 4);
        ImGui::Checkbox("3D", &g_stereo); ImGui::SameLine(); ImGui::Checkbox("Swap eyes", &g_swap_eyes);
        slider("Eye distance", &g_eye_sep, 0.0f, 0.3f, "%.3f m");
        ImGui::Text("Monitor:"); ImGui::SameLine();
        ImGui::RadioButton("game", &g_monitor_mode, 0); ImGui::SameLine();
        ImGui::RadioButton("left eye", &g_monitor_mode, 1); ImGui::SameLine();
        ImGui::RadioButton("headset crop", &g_monitor_mode, 2);
    }
    if (ImGui::CollapsingHeader("Aiden: view, turning, aiming", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("%s - %s%s", g_aiden_obj ? "Aiden found" : "looking for Aiden...", g_in_vehicle ? "in a vehicle" : "on foot",
                    g_remote_view ? " (showing a hacked camera)" : "");
        ImGui::Checkbox("Aiden turns and walks where you look; aims where the right wand points", &g_head_steers);
        if (g_head_steers) {
            ImGui::TextDisabled("  %s | off by %.0f / %.0f deg | stick directions %s", g_steer_on ? (g_aiming ? "aiming with the wand" : "following your head") : "paused",
                                g_steer_err[0], g_steer_err[1], g_steer_locked[0] && g_steer_locked[1] ? "learned" : "learning...");
            ImGui::Checkbox("Snap turn (right pad / stick)", &g_snap_turn);
            if (g_snap_turn) slider("Snap angle", &g_snap_deg, 15, 90, "%.0f deg");
            else slider("Smooth turn speed (right pad / stick)", &g_turn_rate, 30, 360, "%.0f deg/s");
            slider("Steering: degrees for full stick", &g_steer_range, 3, 40, "%.0f");
            slider("Steering: game stick dead zone", &g_steer_dz, 0, 0.5f, "%.2f");
            if (ImGui::Button("Relearn stick directions")) { g_steer_locked[0] = g_steer_locked[1] = false; }
        }
        ImGui::Checkbox("Aiden's body turns with your head", &g_body_turn);
        if (g_body_turn) slider("Look-away before the body follows", &g_body_deadzone, 0, 60, "%.0f deg");
        ImGui::Checkbox("Show the hacked camera's view (numpad -)", &g_remote_force);
        for (int prof = 0; prof < 2; prof++) {
            float* o = prof ? g_fp_off_car : g_fp_off;
            ImGui::PushID(prof);
            ImGui::SeparatorText(prof ? "Eyes in a vehicle" : "Eyes on foot");
            if (prof) {
                ImGui::Checkbox("3D while driving (off = flat but smoother at speed)", &g_car_3d);
                ImGui::Checkbox("View turns with the car (not the game's swaying chase camera)", &g_car_frame);
                ImGui::Checkbox("Steady seat (ignore Aiden's leaning / bouncing while driving)", &g_steady_seat);
                ImGui::Checkbox("Use the game's first-person car camera when it is selected", &g_use_cockpit);
                ImGui::SameLine(); ImGui::TextDisabled(g_cockpit ? "(active)" : "(not active)");
                ImGui::Checkbox("Select the first-person car camera automatically", &g_auto_cockpit);
                { char b[48]; ImGui::SameLine(); ImGui::TextDisabled("(presses %s)", cam_button_name(g_cam_button, b, sizeof b)); }
            }
            slider("Height (numpad 9/3)", &o[2], -1.0f, 1.0f, "%.2f m");
            slider("Forward (numpad 8/2)", &o[0], -1.0f, 1.0f, "%.2f m");
            slider("Sideways (numpad 6/4)", &o[1], -1.0f, 1.0f, "%.2f m");
            ImGui::PopID();
        }
        ImGui::Checkbox("Aim: measure the distance to what you aim at (depth)", &g_aim_auto);
        ImGui::SameLine();
        if (!framecap::scene_depth()) ImGui::TextDisabled("(no depth buffer found yet)");
        else if (!g_depth_model) ImGui::TextDisabled("(learning: stand on flat ground and look slightly down, %d/120)", g_depth_samples);
        else ImGui::TextDisabled("(calibrated, wand points at %.0f m)", g_aim_live);
        if (g_depth_model && ImGui::SmallButton("Redo aim depth calibration")) { g_depth_model = 0; g_depth_near = 0; }
        slider("Aim: fallback crossing distance", &g_aim_dist, 3, 100, "%.0f m");
        ImGui::Checkbox("Red dot: where your bullets land (while aiming)", &g_red_dot);
        if (g_red_dot) slider("Red dot size", &g_dot_size, 0.2f, 2.0f, "%.1f deg");
        ImGui::TextDisabled("Frame ID: %s", framecap::frame_id_status());
        ImGui::Checkbox("Timing fix: field of view one frame late", &g_fov_lag);
        ImGui::Checkbox("Timing fix: rotation one frame late (phone / scope edges)", &g_rot_lag);
        ImGui::Checkbox("Rifle scope picture on the right wand", &g_scope_on);
        ImGui::SameLine(); ImGui::TextDisabled(g_scoped ? "(scoped now)" : "(game FOV %.0f)", camera::game_fov() * 57.2958f);
        if (g_scope_on) {
            slider("Scope size", &g_scope_size, 0.05f, 0.4f, "%.2f m");
            slider("Scope height above the wand", &g_scope_up, -0.1f, 0.3f, "%.2f m");
            slider("Scope back toward you", &g_scope_back, -0.1f, 0.4f, "%.2f m");
            slider("Scope when the game zooms below", &g_scope_fov_max, 10, 60, "%.0f deg");
        }
        ImGui::Checkbox("Small lean (max 15 cm, numpad 0)", &g_fp_lean);
        slider("Speed lag fix (numpad * and /)", &g_fp_lead, 0.0f, 0.1f, "%.3f s");
    }
    if (ImGui::CollapsingHeader("Arms", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Aiden's arms follow my wands", &g_arms_on);
        ImGui::SameLine(); ImGui::TextDisabled("%s", g_body_msg);
        if (ImGui::Button("Calibrate: T-pose (Ctrl+F11)")) arms_start_calibration();
        ImGui::SameLine(); ImGui::TextDisabled(g_arm_cal.ok ? "calibrated (turn %.0f)" : "not calibrated", g_arm_cal.yaw * 57.3f);
        ImGui::Checkbox("Hands turn with the wands (off = straight wrists)", &g_hands_follow);
        slider("Gun grip angle (aim line tilted down from the wand tip)", &g_aim_tilt, -70.0f, 70.0f, "%.0f deg");
        slider("Left hand twist", &g_hand_roll[0], -180, 180, "%.0f deg");
        slider("Right hand twist", &g_hand_roll[1], -180, 180, "%.0f deg");
        slider("Wrist behind the wand grip", &g_arms_grip, -0.15f, 0.25f, "%.2f m");
        slider("Reach (hand distance from chest)", &g_arm_reach, 0.7f, 1.4f, "%.2f");
        slider("Arm stretch allowed", &g_arm_stretch, 1.0f, 1.6f, "%.2f");
        ImGui::Checkbox("Cancel the hand-in-pocket animation (holds the coat / pocket bones)", &g_pocket_fix);
        ImGui::SameLine(); ImGui::TextDisabled("(%s%s, %d bones)", g_pocket[0] ? "L in pocket " : "", g_pocket[1] ? "R in pocket" : "", g_coat_n);
        if (ImGui::Button("Relearn Aiden's arm pose (stand idle, arms down)")) { g_rest_ok = false; g_learn_frames = 0; g_learn_bend[0] = g_learn_bend[1] = 0; g_rest_bones = 0; }
        slider("Elbows out", &g_arms_pole_out, 0.0f, 1.5f, "%.2f");
        { bool c = g_arms_conv != 0; if (ImGui::Checkbox("Other rotation convention (if arms look scrambled)", &c)) { g_arms_conv = c; g_rest_ok = false; g_learn_frames = 0; g_learn_bend[0] = g_learn_bend[1] = 0; } }
    }
    if (ImGui::CollapsingHeader("HUD, phone, menus", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("HUD on a floating panel", &g_hud_panel);
        ImGui::SameLine(); ImGui::TextDisabled("(%s)", framecap::hud_status());
        slider("Panel distance", &g_hud_dist, 0.5f, 3.0f, "%.2f m");
        slider("Panel width", &g_hud_width, 0.5f, 3.0f, "%.2f m");
        slider("Panel lower by", &g_hud_down, -0.5f, 0.5f, "%.2f m");
        ImGui::Checkbox("Markers / prompts stay in the world", &g_hud_world);
        ImGui::Checkbox("Hide the game's crosshair in the headset", &g_hide_reticle);
        if (g_hide_reticle) slider("Crosshair box size", &g_reticle_box, 0.01f, 0.15f, "%.3f");
        slider("Size of that middle area", &g_hud_world_size, 0.3f, 1.0f, "%.2f");
        ImGui::Checkbox("Phone on my left hand, weapon wheel on my right", &g_hands_ui);
        ImGui::SameLine(); ImGui::TextDisabled(g_menu_kind == 1 ? "(phone open)" : g_menu_kind == 2 ? "(wheel open)" : "");
        if (g_hands_ui && ImGui::TreeNode("Phone / wheel placement")) {
            slider("Phone size", &g_phone_width, 0.1f, 1.2f, "%.2f m");
            slider("Phone tilt (wrist)", &g_phone_tilt, -90, 30, "%.0f deg");
            slider("Phone height above the wrist", &g_phone_up, -0.1f, 0.3f, "%.2f m");
            slider("Phone along the arm (toward you)", &g_phone_back, -0.2f, 0.35f, "%.2f m");
            slider("Wheel size", &g_wheel_width, 0.1f, 1.2f, "%.2f m");
            slider("Wheel tilt", &g_wheel_tilt, -90, 30, "%.0f deg");
            slider("Wheel height above the wand", &g_wheel_up, -0.1f, 0.3f, "%.2f m");
            slider("Wheel forward/back", &g_wheel_back, -0.2f, 0.2f, "%.2f m");
            ImGui::TreePop();
        }
        ImGui::Checkbox("Menus and maps on a big screen automatically", &g_screen_auto);
        ImGui::SameLine(); ImGui::TextDisabled(g_screen_mode ? "(on now)" : "(numpad +)");
        slider("Big screen width", &g_screen_width, 1.0f, 6.0f, "%.1f m");
        slider("Big screen distance", &g_screen_dist, 1.0f, 5.0f, "%.1f m");
    }
    if (ImGui::CollapsingHeader("Controllers")) {
        auto is = input::status();
        ImGui::Checkbox("Vive wands act as an Xbox controller", &g_wand_input);
        ImGui::TextDisabled("Game input: %s%s, %u reads%s", is.hooked ? "hooked " : "waiting for XInput", is.dll, is.game_reads, is.real_pad ? ", real pad connected" : "");
        ImGui::TextDisabled("Left pad: move (touch), click centre L3 / edges D-pad.  Right pad: turn (touch), click centre R3 /\n"
                            "edges Y B A X.  Triggers LT aim / RT shoot.  Grips LB / RB.  Menu buttons Back / Start.");
        slider("Trackpad dead zone", &input::options().deadzone, 0.0f, 0.5f, "%.2f");
        slider("Trackpad turn strength", &input::options().turn_speed, 0.2f, 2.0f, "%.1f");
    }
    if (ImGui::CollapsingHeader("For Claude")) {
        if (ImGui::Button("Save a snapshot picture (F10)")) g_snap_req = true;
        if (g_snap_msg[0]) { ImGui::SameLine(); ImGui::TextDisabled("%s", g_snap_msg); }
        ImGui::TextWrapped("%s", build_report().c_str());
    }
    ImGui::End();
}

// ---------------------------------------------------------------- input, rendering setup
static LRESULT CALLBACK hooked_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_imgui_ready && g_menu_open) {
        ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp);
        if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || msg == WM_INPUT)
            return msg == WM_INPUT ? DefWindowProcW(h, msg, wp, lp) : 0;   // keep mouse/keyboard away from the game
    }
    return CallWindowProcW(g_orig_wndproc, h, msg, wp, lp);
}

static void create_rtv() {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) return;
    D3D11_TEXTURE2D_DESC td; back->GetDesc(&td);
    g_width = td.Width; g_height = td.Height;
    HRESULT hr = g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    if (FAILED(hr)) {                       // typeless back buffers need an explicit format
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        switch (td.Format) {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS: rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
            case DXGI_FORMAT_B8G8R8A8_TYPELESS: rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
            case DXGI_FORMAT_R10G10B10A2_TYPELESS: rd.Format = DXGI_FORMAT_R10G10B10A2_UNORM; break;
            default: rd.Format = td.Format;
        }
        hr = g_device->CreateRenderTargetView(back, &rd, &g_rtv);
    }
    log("Back buffer %ux%u format %d, RTV %s", td.Width, td.Height, (int)td.Format, SUCCEEDED(hr) ? "ok" : "FAILED");
    back->Release();
}

static bool init_imgui(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(sc->GetDesc(&desc))) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(desc.OutputWindow, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(desc.OutputWindow)) return false;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_device))) return false;
    g_device->GetImmediateContext(&g_context);
    g_swapchain = sc;
    g_hwnd = desc.OutputWindow;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    g_orig_wndproc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)hooked_wndproc);
    create_rtv();
    g_imgui_ready = true;
    g_start_tick = GetTickCount64();
    log("ImGui ready on window %p", (void*)g_hwnd);
    return true;
}

// If the game replaces its swapchain (or device) after startup, move our overlay onto the new one.
static bool rebind_swapchain(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(sc->GetDesc(&desc))) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(desc.OutputWindow, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(desc.OutputWindow)) return false;
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev))) return false;
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (dev != g_device) {
        ImGui_ImplDX11_Shutdown();
        if (g_context) g_context->Release();
        if (g_device) g_device->Release();
        g_device = dev;
        g_device->GetImmediateContext(&g_context);
        ImGui_ImplDX11_Init(g_device, g_context);
        log("Game switched to a new D3D11 device - overlay moved");
    } else dev->Release();
    if (desc.OutputWindow != g_hwnd) {
        if (g_orig_wndproc) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_orig_wndproc);
        ImGui_ImplWin32_Shutdown();
        g_hwnd = desc.OutputWindow;
        ImGui_ImplWin32_Init(g_hwnd);
        g_orig_wndproc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)hooked_wndproc);
        log("Game switched to a new window %p - overlay moved", (void*)g_hwnd);
    }
    g_swapchain = sc;
    log("Overlay now on swapchain %p", (void*)sc);
    return true;
}

// ---------------------------------------------------------------- per-frame parts of Present
// Where the game's camera is vs Aiden's head: <4.5 m on foot, ~5-12 m in a vehicle,
// far away or looking elsewhere = a hacked camera / cutscene (show the game's camera).
static void detect_view() {
    float gc[3], h[3];
    static ULONGLONG veh_since = 0, rem_since = 0;
    if (!g_aiden_addr || !camera::game_camera_pos(gc) || !safe_read(g_aiden_addr, h, sizeof h)) return;
    float dx = h[0] - gc[0], dy = h[1] - gc[1], dz = h[2] - gc[2];
    g_cam_dist = sqrtf(dx * dx + dy * dy + dz * dz);
    float fwd[3];
    if (g_cam_dist > 0.01f && camera::game_forward(fwd)) g_cam_angle = acosf(clampf((fwd[0] * dx + fwd[1] * dy + fwd[2] * dz) / g_cam_dist, -1, 1)) * 57.2958f;
    ULONGLONG now = GetTickCount64();
    bool rem = g_remote_force || (g_remote_view ? !((g_cam_dist < 4.5f || (g_in_vehicle && g_cam_dist < 12.0f)) && g_cam_angle < 25.0f)
                                                : (g_cam_dist > 15.0f || (g_cam_dist > 3.5f && g_cam_angle > 35.0f)));
    if (rem != g_remote_view) {
        if (!rem_since) rem_since = now;
        if (now - rem_since > (rem ? 150u : 600u)) {
            g_remote_view = rem; rem_since = 0;
            log("View: %s (game camera %.1f m from Aiden, %.0f deg off-axis)", rem ? "hacked camera / cutscene" : "back to Aiden", g_cam_dist, g_cam_angle);
        }
    } else rem_since = 0;
    // Vehicle: the chase camera sits far behind Aiden. 0.16.5: the game's close / hood car camera (switched by a button
    // press in the 0.16.4 test) sits only ~2 m away, IN FRONT of Aiden and looking away from him -> also a vehicle.
    // (On foot the game camera is always behind him, looking past him: angle ~15 deg.)
    bool hood = g_cam_dist < 4.8f && g_cam_angle > 110.0f;   // close / hood / first-person car camera: in front of Aiden, looking away
    // 0.16.7: once in a vehicle, stay there until the game camera is back in its on-foot spot (over the shoulder,
    // 1.2-3.5 m, looking past Aiden). While driving the game's camera sometimes came within 0.2 m of Aiden (crash /
    // look-back / camera swing) and the view dropped to on-foot mode for 4-12 s in the middle of a drive.
    bool foot_pose = g_cam_dist > 1.2f && g_cam_dist < 3.5f && g_cam_angle < 35.0f;
    bool veh = g_in_vehicle ? !foot_pose : (g_cam_dist > 4.8f || hood);
    if (!g_remote_view && veh != g_in_vehicle) {
        if (!veh_since) veh_since = now;
        if (now - veh_since > (veh ? 300u : 500u)) { g_in_vehicle = veh; veh_since = 0; log("View: %s (game camera %.1f m away, %.0f deg%s)", veh ? "in a vehicle" : "on foot", g_cam_dist, g_cam_angle, hood ? ", close car camera" : ""); }
    } else veh_since = 0;
}

// In a vehicle "straight ahead" follows the car itself: the yaw of Aiden's body matrix (he sits rigidly in the seat).
// The game's chase camera swings on springs, lags in turns and shakes at speed - following it made the whole view
// sway and judder. The car-axis -> forward offset starts from the chase camera and settles while driving straight.
static void car_frame_update(float dt) {
    static bool on = false;
    static float last_ax = 0; static bool have_ax = false; static float rate = 0, straight_t = 0;
    float M[4][3];
    bool want = g_car_frame && g_in_vehicle && !g_remote_view && g_aiden_obj && camera::game_rot_valid() && aiden_matrix(M);
    if (!want) {
        if (on) { camera::set_base_yaw_override(false, 0); on = false; g_car_off_ok = false; have_ax = false; log("Car view: following the game camera"); }
        return;
    }
    float ax = atan2f(-M[0][1], M[0][0]) * 57.2958f;          // yaw of the body's x axis (game yaw convention)
    if (!std::isfinite(ax) || (fabsf(M[0][0]) + fabsf(M[0][1])) < 0.3f) return;
    if (have_ax && dt > 0.001f) rate += (fabsf(wrap180(ax - last_ax)) / dt - rate) * 0.1f;   // how fast the car turns (deg/s, smoothed)
    last_ax = ax; have_ax = true;
    // Start: the axis offset learned in earlier drives (saved), else where the chase camera looks.
    if (!g_car_off_ok) { g_car_off = g_car_axis_ok ? g_car_axis : wrap180(camera::game_yaw_deg() - ax); g_car_off_ok = true; }
    // 0.16.7: forward = the direction the car actually travels (driving straight, > 22 km/h), not the game's chase
    // camera - the chase camera swings with speed and lags in turns, and the view used to drift with it.
    float sp = sqrtf(g_aiden_vel[0] * g_aiden_vel[0] + g_aiden_vel[1] * g_aiden_vel[1]);
    if (sp > 6 && rate < 4) {
        float vy = atan2f(-g_aiden_vel[1], g_aiden_vel[0]) * 57.2958f;
        float d = wrap180(vy - ax - g_car_off);
        if (fabsf(d) < 45) {                                      // (not reversing / sliding sideways)
            g_car_off = wrap180(g_car_off + d * fminf(1.0f, dt / (g_car_axis_ok ? 3.0f : 0.5f)));
            straight_t += dt;
            static ULONGLONG saved_t = 0;
            if (straight_t > 2 && fabsf(d) < 2 && (!g_car_axis_ok || (fabsf(wrap180(g_car_axis - g_car_off)) > 1 && GetTickCount64() - saved_t > 30000))) {
                g_car_axis = g_car_off; g_car_axis_ok = true; straight_t = 0; saved_t = GetTickCount64(); save_config();
                log("Car view: car forward learned from the driving direction (body axis %+.1f deg)", g_car_axis);
            }
        } else straight_t = 0;
    } else straight_t = 0;
    camera::set_base_yaw_override(true, wrap180(ax + g_car_off));
    if (!on) { on = true; log("Car view: turns with the car (body axis %+.0f deg, %s)", g_car_off, g_car_axis_ok ? "learned" : "from the chase camera until you drive straight"); }
}

// Head look, first-person anchor, body, 3D.
static void view_frame(float dt) {
    if (!camera::head_look() || !g_vr.hmd.valid || g_screen_mode) return;
    {   // field of view: the headset's, zoomed like the game when it zooms a hacked camera; the scope frame uses the game's own
        float fov = g_fov_written, gf = camera::game_fov();
        static float remote_fov0; static bool was_remote;
        if (g_remote_view && gf > 0.05f && gf < 3.0f) {
            if (!was_remote || gf > remote_fov0) remote_fov0 = gf;
            fov = 2 * atanf(tanf(g_fov_written / 2) * tanf(gf / 2) / tanf(remote_fov0 / 2));
        }
        was_remote = g_remote_view;
        if (g_next_type == 2 && gf > 0.01f) fov = gf;
        // Frame ID tag: a few parts in 100000 per request (invisible) - lets us tell which request each frame was drawn for.
        fov *= 1.0f + 5e-5f * (float)((g_pose_count & 7) + 1);
        g_req_fov = fov;
        // The game uses the field of view one frame later than the position / direction (measured 0.16.0: FOV arrives
        // 1 request after it's written, position 2). So write each request's FOV one request late: then a frame's
        // position, direction and FOV all come from the same request (otherwise a game-view frame next to your eye
        // frames mixes them: third person with the headset's wide FOV in an eye, a zoomed view on the phone).
        static float prev_fov; static unsigned prev_idx; static ULONGLONG prev_t;
        ULONGLONG nowt = GetTickCount64();
        bool have_prev = g_fov_lag && prev_t && nowt - prev_t < 300 && prev_idx + 1 == g_pose_count;
        float wfov = have_prev ? prev_fov : fov; unsigned widx = have_prev ? prev_idx : g_pose_count;
        camera::set_fov_override(wfov);
        framecap::frame_tag(widx, wfov, g_aspect);
        prev_fov = fov; prev_idx = g_pose_count; prev_t = nowt;
        // Diagnosis (0.16.3): while frames carry no tag, say what FOV the game really drew with and whether something
        // other than the blocked FOV function changed the camera's FOV fields (0.16.1 / 0.16.2 lost the tag for ~5 s
        // after weapon switches + aiming). Once a second, at most 40 lines per session.
        static ULONGLONG chk_t = 0; static int chk_n = 0; static unsigned chg0 = 0;
        float seen[2]; unsigned chg = camera::fov_changed(seen);
        if (framecap::frame_id_missing() && nowt - chk_t > 1000 && chk_n < 40) {
            float drawn; char where[64] = "";
            bool pv = framecap::probe_fov(&drawn, where, sizeof where);
            log("Frame ID check: game drew FOV %s%.2f deg%s%s%s (we ask %.2f, game wants %.2f); FOV fields changed by the game %u times since last check (last %.2f / %.2f deg); aim %d, phone/wheel %d, scope frame %d, steering error %.0f deg, camera structs %p %p",
                pv ? "" : "? ", pv ? drawn : 0.0f, pv ? " (" : "", where, pv ? ")" : "", fov * 57.2958f, camera::game_fov() * 57.2958f,
                chg - chg0, seen[0] * 57.2958f, seen[1] * 57.2958f, (int)g_aiming, g_menu_kind, (int)(g_next_type == 2),
                g_steer_err[0], (void*)camera::struct_a(), (void*)camera::struct_b());
            chk_t = nowt; chk_n++; chg0 = chg;
        } else if (!framecap::frame_id_missing()) chg0 = chg;
    }
    camera::set_rot_lag(g_rot_lag);
    camera::set_request(g_pose_count);
    if (g_next_type == 2) camera::look_along_game_aim();   // the scope shows exactly where the gun fires (and the game's reticle centred)
    else camera::update_head_look(g_vr.hmd.yaw, g_vr.hmd.pitch, g_vr.hmd.roll);
    { float lm[3][3]; camera::last_look(lm); framecap::rot_tag(g_pose_count, &lm[0][0]); }
    unsigned slot = g_pose_count & 7;
    memcpy(g_pose_ring[slot], g_vr.hmd.m, sizeof g_pose_ring[0]);   // a later frame is drawn with this pose
    int eye = -1;
    float off[3] = { 0, 0, 0 };
    bool any = false, fp = false;
    if (g_hmd_display && camera::stereo_available()) {
        if (g_need_recenter) { do_recenter(); g_need_recenter = false; }
        detect_view();
        car_frame_update(dt);
        float f[3], r[3], u[3];
        camera::base_frame(f, r, u);
        fp = g_aiden_addr && !g_remote_view;
        if (fp) {
            // Eye offset: along where the car points in a vehicle, along where you look on foot (his head faces there).
            float vf[3] = { f[0], f[1], 0 }, vr[3] = { r[0], r[1], 0 };
            if (!g_in_vehicle && g_body_turn && g_body_bones && g_rest_ok) {
                float v[3]; camera::view_forward(v); v[2] = 0;
                if (norm3(v) > 0.1f) { vf[0] = v[0]; vf[1] = v[1]; vr[0] = -v[1]; vr[1] = v[0]; }
            }
            {   // 0.16.8: the game's own first-person car camera (cockpit view: dashboard, hands on the wheel) sits at
                // the driver's eyes, fixed to the car. When it is active, the headset uses ITS position (plus your
                // eye separation / lean) instead of Aiden's animated head - the game's view, made 3D.
                // (0.16.11: stays on until the game camera is > 2.5 m away - slow motion pulls it back up to ~1.9 m)
                bool ck = g_use_cockpit && g_in_vehicle && (g_cockpit ? g_cam_dist < 2.5f : g_cam_dist < 0.8f);
                if (ck != g_cockpit) { g_cockpit = ck; log("Car: %s (game camera %.2f m from Aiden's head)", ck ? "using the game's first-person car camera" : "first-person car camera off", g_cam_dist); }
                // 0.16.11: slow motion (focus) and some other moments pull the game's first-person camera back from the
                // driver's eyes (0.13 m -> 0.6-1.9 m from the head). Its normal spot is learned in the car's own frame
                // (relative to Aiden's head) during the first 1.5 s; while the game camera is away from that spot the
                // view stays there instead (anchored to the head at the game's exact moment, like on foot).
                static bool have = false; static float rel[3]; static ULONGLONG t0 = 0; static bool was = false;
                if (g_cockpit && !was) { have = false; t0 = GetTickCount64(); g_cockpit_held = false; }
                was = g_cockpit;
                float M[4][3], hp[3], gc[3];
                if (g_cockpit && aiden_matrix(M) && safe_read(g_aiden_addr, hp, sizeof hp) && camera::game_camera_pos(gc)) {
                    // measured in the car's own frame (Aiden's body matrix: rigid in the seat), not from his animated head
                    float d[3] = { gc[0] - M[3][0], gc[1] - M[3][1], gc[2] - M[3][2] }, loc[3], l2[3];
                    float dh[3] = { gc[0] - hp[0], gc[1] - hp[1], gc[2] - hp[2] };
                    bool okm = true;
                    for (int i = 0; i < 3; i++) { l2[i] = dot3(M[i], M[i]); if (l2[i] < 0.25f || l2[i] > 4) okm = false; else loc[i] = dot3(d, M[i]) / l2[i]; }
                    if (okm) {
                        float exp_w[3];
                        for (int i = 0; i < 3; i++) exp_w[i] = rel[0] * M[0][i] + rel[1] * M[1][i] + rel[2] * M[2][i];
                        float e[3] = { d[0] - exp_w[0], d[1] - exp_w[1], d[2] - exp_w[2] }, err = sqrtf(dot3(e, e));
                        bool held = have && (g_cockpit_held ? err > 0.08f : err > 0.15f);
                        if (held != g_cockpit_held) {
                            g_cockpit_held = held;
                            static ULONGLONG lt = 0; static int n = 0;
                            if (GetTickCount64() - lt > 5000 || n < 6) { lt = GetTickCount64(); n++;
                                log("Car: game's first-person camera %s (%.2f m off its usual spot)", held ? "pulled away (slow motion?) - view held at the driver's eyes" : "back - following it again", err); }
                        }
                        if (!have && sqrtf(dot3(dh, dh)) < 0.4f) { memcpy(rel, loc, sizeof rel); have = true; }
                        else if (have && !g_cockpit_held) {
                            float k = GetTickCount64() - t0 < 1500 ? 0.05f : 0.002f;
                            for (int i = 0; i < 3; i++) rel[i] += (loc[i] - rel[i]) * k;
                        }
                        // the usual spot as an offset from the head (the anchor follows the head at the game's moment)
                        for (int i = 0; i < 3; i++) g_cockpit_rel[i] = M[3][i] + exp_w[i] - hp[i];
                        if (dot3(g_cockpit_rel, g_cockpit_rel) > 1.0f) g_cockpit_held = false;   // implausible: follow the game
                    }
                } else if (!g_cockpit) g_cockpit_held = false;
            }
            const float* pv = g_in_vehicle ? g_fp_off_car : g_fp_off;
            if (g_cockpit && g_cockpit_held) memcpy(off, g_cockpit_rel, sizeof off);   // the cockpit camera's usual spot (from the head)
            else if (!g_cockpit) for (int i = 0; i < 3; i++) off[i] = vf[i] * pv[0] + vr[i] * pv[1] + u[i] * pv[2];
            static float last[3]; static LARGE_INTEGER last_t = {}; static uintptr_t last_a = 0;
            static double qpf = 0; if (qpf == 0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); qpf = (double)f.QuadPart; }
            float p[3]; LARGE_INTEGER now; QueryPerformanceCounter(&now);   // precise clock (GetTickCount64 steps by 16 ms)
            if (safe_read(g_aiden_addr, p, sizeof p)) {    // Aiden's speed (lag fix)
                float t = (float)((now.QuadPart - last_t.QuadPart) / qpf);
                if (last_a == g_aiden_addr && t > 0.004f && t < 0.2f) {
                    float vx = (p[0] - last[0]) / t, vy = (p[1] - last[1]) / t;
                    if (fabsf(vx) < 120 && fabsf(vy) < 120) { g_aiden_vel[0] += 0.2f * (vx - g_aiden_vel[0]); g_aiden_vel[1] += 0.2f * (vy - g_aiden_vel[1]); }
                }
                if (g_in_vehicle) {                        // driving report every 4 s (speed, frame times, position steps)
                    static int n = 0, same = 0, ln = 0; static float tmin = 1, tmax = 0, lsum = 0, lmin = 1, lmax = -1; static ULONGLONG rep = 0;
                    float sp = sqrtf(g_aiden_vel[0] * g_aiden_vel[0] + g_aiden_vel[1] * g_aiden_vel[1]);
                    if (t > 0 && t < 0.2f) { n++; tmin = fminf(tmin, t); tmax = fmaxf(tmax, t); if (sp > 2 && p[0] == last[0] && p[1] == last[1]) same++; }
                    float cw[3];   // how far behind Aiden's head the camera was placed, in seconds of travel
                    if (sp > 5 && camera::anchored_camera_pos(cw)) {
                        float d[2] = { p[0] - (cw[0] - g_anchor_off_last[0]), p[1] - (cw[1] - g_anchor_off_last[1]) };
                        float lt = (d[0] * g_aiden_vel[0] + d[1] * g_aiden_vel[1]) / (sp * sp);
                        if (std::isfinite(lt) && fabsf(lt) < 0.2f) { ln++; lsum += lt; lmin = fminf(lmin, lt); lmax = fmaxf(lmax, lt); }
                    }
                    ULONGLONG tk = GetTickCount64();
                    if (!rep) rep = tk;
                    if (tk - rep > 4000 && n) {
                        log("Driving: %.0f km/h, frames %.1f-%.1f ms, position not updated in %d of %d frames, %s; camera behind head %.1f ms (%.1f..%.1f, %d samples)",
                            sp * 3.6f, tmin * 1000, tmax * 1000, same, n, g_next_type < 0 ? "flat" : "3D",
                            ln ? lsum / ln * 1000 : 0.0f, ln ? lmin * 1000 : 0.0f, ln ? lmax * 1000 : 0.0f, ln);
                        n = same = ln = 0; tmin = 1; tmax = 0; lsum = 0; lmin = 1; lmax = -1; rep = tk;
                    }
                }
                memcpy(last, p, sizeof last); last_t = now; last_a = g_aiden_addr;
            }
            if (!g_cockpit) { off[0] += g_aiden_vel[0] * g_fp_lead; off[1] += g_aiden_vel[1] * g_fp_lead; }
            if (g_fp_lean && g_have_origin) {               // real head movement, clamped to 15 cm
                float d[3] = { g_vr.hmd.pos[0] - g_head_origin[0], g_vr.hmd.pos[1] - g_head_origin[1], g_vr.hmd.pos[2] - g_head_origin[2] }, w[3];
                tracking_to_world(f, r, u, d, w);
                float len = sqrtf(dot3(w, w));
                if (len > 0.15f) for (float& x : w) x *= 0.15f / len;
                for (int i = 0; i < 3; i++) off[i] += w[i];
            }
            {   // 0.16.7 steady seat: in a vehicle the game animates Aiden's head (leaning into turns, bracing, bumps -
                // stronger the faster you go). The eye is held at a fixed point of the seat instead: the head's place
                // in the car's own frame is learned in the first 2.5 s, then the anchor gets (seat point - head) added.
                // (Both read now; the anchor itself still follows the head at the game's exact moment.)
                static bool was = false, have = false; static ULONGLONG t0 = 0; static float seat[3];
                float M[4][3], hp[3];
                bool active = g_in_vehicle && !g_cockpit && g_steady_seat && aiden_matrix(M) && safe_read(g_aiden_addr, hp, sizeof hp);
                if (g_in_vehicle && !was) { have = false; t0 = GetTickCount64(); }
                was = g_in_vehicle;
                if (active) {
                    float d[3] = { hp[0] - M[3][0], hp[1] - M[3][1], hp[2] - M[3][2] }, loc[3];
                    bool okm = true;
                    for (int i = 0; i < 3; i++) { float l2 = dot3(M[i], M[i]); if (l2 < 0.25f || l2 > 4) okm = false; else loc[i] = dot3(d, M[i]) / l2; }
                    if (okm) {
                        if (!have) { memcpy(seat, loc, sizeof seat); have = true; log("Steady seat: learning where Aiden's head sits in the car"); }
                        else if (GetTickCount64() - t0 < 2500) for (int i = 0; i < 3; i++) seat[i] += (loc[i] - seat[i]) * 0.05f;
                        float c[3];
                        for (int i = 0; i < 3; i++) c[i] = M[3][i] + seat[0] * M[0][i] + seat[1] * M[1][i] + seat[2] * M[2][i] - hp[i];
                        if (dot3(c, c) < 0.16f) for (int i = 0; i < 3; i++) off[i] += c[i];
                    }
                }
            }
            float headp[3], eyep[3];
            if ((g_cockpit && !g_cockpit_held) ? camera::game_camera_pos(headp) : safe_read(g_aiden_addr, headp, sizeof headp)) {
                for (int i = 0; i < 3; i++) eyep[i] = headp[i] + off[i];
                memcpy(g_eye_world, eyep, sizeof eyep); g_have_eye = true;
                body_update(f, r, u, eyep, dt);
            }
        } else if (g_body_bones) body_release("paused");
        any = true;
        if (g_next_type == 2) eye = 2;                     // scope frame: no eye offset
        else if (g_next_type >= 0) {
            eye = g_next_type;                             // alternate: left, right, left...
            float right[3]; camera::camera_right(right);
            float half = 0.5f * g_eye_sep * ((eye == 0) != g_swap_eyes ? -1.0f : 1.0f);
            for (int i = 0; i < 3; i++) off[i] += right[i] * half;
        }
    }
    if (g_next_type == 2) {                               // scope: the game's own camera position and aim, untouched
        float zero[3] = { 0, 0, 0 };
        camera::set_eye_offset(true, zero);
    } else if (any && fp && g_cockpit && !g_cockpit_held) {
        camera::set_eye_offset(true, off);                 // the game's cockpit camera position + eye offset
    } else if (any && fp) {
        float zero[3] = { 0, 0, 0 };
        memcpy(g_anchor_off_last, off, sizeof off);
        if (!camera::set_anchor(g_aiden_addr, off)) {      // (no set_eye_offset first: it would clear the anchor for a moment)
            log("Aiden's position is no longer valid (area loaded?) - searching again");
            g_aiden_addr = 0; g_aiden_obj = 0;
            camera::set_eye_offset(false, zero);
        }
    } else camera::set_eye_offset(any, off);
    g_eye_ring[slot] = eye;
    {   // the camera this request will be drawn with, and the wand's line right now (aim depth)
        FrameCam& c = g_frame_cam[slot];
        float M[4][3];
        c.ok = false;
        if (any && fp && eye >= 0 && eye <= 1 && g_have_eye && aiden_matrix(M)) {
            float headp[3];
            if ((g_cockpit && !g_cockpit_held) ? camera::game_camera_pos(headp) : safe_read(g_aiden_addr, headp, sizeof headp)) {
                for (int i = 0; i < 3; i++) c.pos[i] = headp[i] + off[i];
                camera::last_look(c.m);
                c.fov = g_req_fov; c.ground = M[3][2];
                c.gok = camera::game_camera_pos(c.gpos) && camera::game_forward(c.gfwd);
                float f[3], r[3], u[3], dp[3] = { g_vr.right.pos[0] - g_vr.hmd.pos[0], g_vr.right.pos[1] - g_vr.hmd.pos[1], g_vr.right.pos[2] - g_vr.hmd.pos[2] };
                float fw[3], wp[3]; wand_aim_dir(g_vr.right, fw);
                camera::base_frame(f, r, u);
                tracking_to_world(f, r, u, dp, wp); tracking_to_world(f, r, u, fw, c.wand_d); norm3(c.wand_d);
                for (int i = 0; i < 3; i++) c.wand_o[i] = g_eye_world[i] + wp[i];
                c.ok = g_vr.right.valid;
            }
        }
    }
    g_pose_count++;
    g_have_pose = true;
}


// ---------------------------------------------------------------- car camera: pick the game's first-person view
// 0.16.9: the game remembers its car camera only per game session (user: switched to first person by hand every time).
// 0.16.10: the change-camera button is D-pad left (left trackpad, left edge click) - fixed. (0.16.9 learned it from the
// player's presses and later picked Y = get out of the car, which then looped in/out.) After you get in, the mod presses
// it until the first-person camera is active (at most 4 presses per car; it stops if you switch away from first person
// yourself, and switches itself off if you leave the car right after one of its presses).
static ULONGLONG g_key_edge[256];
static void track_keys() {
    static bool down[256];
    bool fg = GetForegroundWindow() == g_hwnd;
    ULONGLONG now = GetTickCount64();
    for (int vk = 0x08; vk < 0xFF; vk++) {
        if ((vk >= 0x10 && vk <= 0x12) || (vk >= 0x5B && vk <= 0x87) || (vk >= 0xA0 && vk <= 0xA5) || vk == VK_INSERT) continue;
        bool d = fg && (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (d && !down[vk]) g_key_edge[vk] = now;
        down[vk] = d;
    }
}
static int car_cam_mode() {           // -1 not in a car, 0 chase camera, 1 hood camera, 2 first-person camera
    if (!g_in_vehicle || g_remote_view) return -1;
    if (g_cam_dist < 0.8f) return 2;
    if (g_cam_dist < 4.8f && g_cam_angle > 110.0f) return 1;
    return 0;
}
static const char* cam_button_name(int b, char* buf, size_t n) {
    static const char* pad[16] = { "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Start", "Back", "L3", "R3",
                                   "LB", "RB", "?", "?", "A", "B", "X", "Y" };
    if (b >= 1 && b <= 16) snprintf(buf, n, "controller %s", pad[b - 1]);
    else if (b >= 1000) { char k[32] = "?"; UINT sc = MapVirtualKeyA(b - 1000, MAPVK_VK_TO_VSC); GetKeyNameTextA((LONG)sc << 16, k, sizeof k); snprintf(buf, n, "key %s", k); }
    else snprintf(buf, n, "not learned yet");
    return buf;
}
static void press_cam_button() {
    if (g_cam_button >= 1 && g_cam_button <= 16) input::inject_buttons((unsigned short)(1u << (g_cam_button - 1)), 150);
    else if (g_cam_button >= 1000 && GetForegroundWindow() == g_hwnd) {
        INPUT in = {}; in.type = INPUT_KEYBOARD;
        in.ki.wScan = (WORD)MapVirtualKeyA(g_cam_button - 1000, MAPVK_VK_TO_VSC); in.ki.dwFlags = KEYEVENTF_SCANCODE;
        SendInput(1, &in, sizeof in);
    }
}
static void release_cam_key() {
    if (g_cam_button < 1000) return;
    INPUT in = {}; in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD)MapVirtualKeyA(g_cam_button - 1000, MAPVK_VK_TO_VSC); in.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof in);
}
static void car_cam_frame() {
    track_keys();
    static int last = -1, pend = 0, pend_n = 0, presses = 0;
    static ULONGLONG enter_t = 0, mode_t = 0, press_t = 0, release_at = 0;
    static bool user_away = false, done_logged = false;
    ULONGLONG now = GetTickCount64();
    if (release_at && now >= release_at) { release_cam_key(); release_at = 0; }
    int m = (g_hmd_display && camera::head_look()) ? car_cam_mode() : -1;
    if (m != last) {
        if (last < 0 && m >= 0) { enter_t = now; presses = 0; user_away = false; done_logged = false; }
        static bool disabled = false;
        if (last >= 0 && m < 0 && press_t && now - press_t < 3000 && !disabled) {
            disabled = true; g_auto_cockpit = false;
            log("Car camera: you left the car right after an automatic press - automatic selection switched off (wrong button?)");
        }
        bool ours = press_t && now - press_t < 1500;
        if (last >= 0 && m >= 0 && !ours && now - enter_t > 1500) {
            if (last == 2) user_away = true;     // you left first person yourself: leave it that way for this car
        }
        last = m; mode_t = now;
    }
    if (m == 2 && presses && !done_logged) { done_logged = true; log("Car camera: first-person view selected automatically (%d press%s)", presses, presses == 1 ? "" : "es"); }
    if (!g_auto_cockpit || !g_use_cockpit || !g_cam_button || m < 0 || m == 2 || user_away) return;
    if (now - enter_t < 1500 || now - mode_t < 800 || now - press_t < 1000 || presses >= 4) return;
    press_cam_button(); press_t = now; presses++;
    if (g_cam_button >= 1000) release_at = now + 120;
    if (presses == 4) log("Car camera: 4 presses did not reach the first-person view - stopped for this car");
}

static void keys_frame(float dt) {
    if (key_pressed(VK_INSERT)) { g_menu_open = !g_menu_open; log("Menu %s", g_menu_open ? "opened" : "closed"); }
    if (key_pressed(VK_F10)) g_snap_req = true;
    if (key_pressed(VK_F11) && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) arms_start_calibration();
    if (!g_hmd_display || GetForegroundWindow() != g_hwnd) return;
    // Numpad (you can't see the menu in the headset): eye position, recenter, lean, screen, hacked-camera view, lag fix.
    auto held = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    float speed = (held(VK_SHIFT) ? 0.8f : 0.2f) * dt;
    float* pv = g_in_vehicle ? g_fp_off_car : g_fp_off;
    if (held(VK_NUMPAD8)) pv[0] += speed;
    if (held(VK_NUMPAD2)) pv[0] -= speed;
    if (held(VK_NUMPAD6)) pv[1] += speed;
    if (held(VK_NUMPAD4)) pv[1] -= speed;
    if (held(VK_NUMPAD9)) pv[2] += speed;
    if (held(VK_NUMPAD3)) pv[2] -= speed;
    if (key_pressed(VK_NUMPAD5)) do_recenter();
    if (key_pressed(VK_NUMPAD0)) { g_fp_lean = !g_fp_lean; log("Lean %s", g_fp_lean ? "on" : "off"); }
    if (key_pressed(VK_SUBTRACT)) { g_remote_force = !g_remote_force; log("Show the game's camera (manual) %s", g_remote_force ? "ON" : "OFF"); }
    if (key_pressed(VK_ADD)) { g_screen_auto_entered = false; set_screen_mode(!g_screen_mode); }
    bool km = key_pressed(VK_MULTIPLY), kd = key_pressed(VK_DIVIDE);
    if (km || kd) { g_fp_lead = clampf(g_fp_lead + (km ? 0.005f : -0.005f), 0, 0.1f); log("Speed lag fix %.3f s", g_fp_lead); }
}

// T-pose countdown/sampling timing (runs every frame, VR or not).
static void calibration_frame() {
    if (!g_cal_state) return;
    ULONGLONG el = GetTickCount64() - g_cal_t0;
    static int last_sec = -1;
    if (g_cal_state == 1) { int sec = (int)(el / 1000); if (sec != last_sec) { last_sec = sec; beep(sec >= kCalCountdownMs / 1000 - 3 ? 900 : 600, 90); } }
    else last_sec = -1;
    if (g_cal_state == 1 && el > (ULONGLONG)kCalCountdownMs) {
        g_cal_state = 2; g_cal_t0 = GetTickCount64(); g_cal_n = 0; memset(g_cal_acc, 0, sizeof g_cal_acc);
        log("Arms: sampling the T-pose"); beep(1100, 250);
    } else if (g_cal_state == 2 && el > 4000) {
        g_cal_state = 0;
        cal_result(!g_vr.left.valid || !g_vr.right.valid ? "T-pose failed: both wands must be on and tracked" :
                   !g_aiden_obj ? "T-pose failed: Aiden not found yet" : !g_rest_ok ? "T-pose failed: Aiden's skeleton isn't ready - try again" :
                   "T-pose failed: no measurement - is the headset tracking?");
    }
}

// Menus and maps -> big screen: when the game stops moving its camera (paused) or its HUD covers
// most of the screen (pause menu, map). Back to VR once neither holds for 0.4 s.
static void screen_auto_frame() {
    if (!g_hmd_display || !g_screen_auto) return;
    static unsigned long long last_writes = 0;
    static ULONGLONG still_since = 0, calm_since = 0;
    unsigned long long w = camera::position_writes();
    ULONGLONG now = GetTickCount64();
    if (w == last_writes) { if (!still_since) still_since = now; } else still_since = 0;
    last_writes = w;
    bool want = (still_since && now - still_since > 250) || g_menu_full;
    if (!g_screen_mode && want) { set_screen_mode(true); g_screen_auto_entered = true; }
    if (g_screen_mode && g_screen_auto_entered) {
        if (want) calm_since = 0;
        else if (!calm_since) calm_since = now;
        else if (now - calm_since > 400) { set_screen_mode(false); calm_since = 0; }
    }
}

static void stall_frame() {   // VR is on but the view isn't following the headset: say why
    static ULONGLONG since = 0, last_log = 0;
    bool running = camera::head_look() && g_vr.hmd.valid && !g_screen_mode;
    ULONGLONG now = GetTickCount64();
    if (!g_hmd_display || running) { since = 0; g_vr_stall[0] = 0; return; }
    if (!since) since = now;
    if (now - since < 2000) return;
    if (g_screen_mode) { g_vr_stall[0] = 0; return; }     // menus on the big screen: expected
    snprintf(g_vr_stall, sizeof g_vr_stall, "VR view paused: %s", !camera::head_look() ? "head look is off" :
             !g_vr.hmd.connected ? "SteamVR doesn't see the headset" : "headset not tracking - check SteamVR (base stations on, headset awake)");
    if (now - last_log > 5000) { last_log = now; log("%s", g_vr_stall); }
}

static void big_text(const char* t, float y, float size, ImU32 col, ImU32 bg) {
    auto* dl = ImGui::GetForegroundDrawList();
    ImFont* f = ImGui::GetFont();
    ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, t);
    ImVec2 p((ImGui::GetIO().DisplaySize.x - ts.x) * 0.5f, y);
    dl->AddRectFilled(ImVec2(p.x - 16, p.y - 10), ImVec2(p.x + ts.x + 16, p.y + ts.y + 10), bg, 8);
    dl->AddText(f, size, p, col, t);
}

static void overlay_frame() {
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    if (GetTickCount64() - g_start_tick < 45000 && !g_menu_open)
        ImGui::GetForegroundDrawList()->AddText(ImVec2(20, 20), IM_COL32(80, 255, 120, 255), "WDVR loaded - press INSERT for the VR menu");
    if (g_hud_panel && g_hmd_display && !g_screen_mode && !g_scoped && framecap::hud_active() && framecap::hud_srv()) {
        // The monitor shows the left eye: put the HUD that isn't in the picture any more back on top.
        auto* dl = ImGui::GetBackgroundDrawList();
        ImTextureID id = (ImTextureID)framecap::hud_srv();
        auto part = [&](float a, float b, float c, float d) {
            if (c > a && d > b) dl->AddImage(id, ImVec2(a * ds.x, b * ds.y), ImVec2(c * ds.x, d * ds.y), ImVec2(a, b), ImVec2(c, d));
        };
        if (!g_hud_world) part(0, 0, 1, 1);
        else {
            float x0 = g_hud_rect[0], y0 = g_hud_rect[1], x1 = g_hud_rect[2], y1 = g_hud_rect[3];
            part(0, 0, 1, y0); part(0, y1, 1, 1); part(0, y0, x0, y1); part(x1, y0, 1, y1);
            if (g_menu_kind) { const float* r = menu_rect(); part(fmaxf(r[0], x0), fmaxf(r[1], y0), fminf(r[2], x1), fminf(r[3], y1)); }
        }
    }
    ImGui::GetForegroundDrawList()->AddText(ImVec2(8, ds.y - 22), IM_COL32(80, 255, 120, 200), "WDVR " WDVR_VERSION " - INSERT = menu");
    if (g_hmd_display && !g_aiden_obj) ImGui::GetForegroundDrawList()->AddText(ImVec2(8, ds.y - 40), IM_COL32(255, 200, 60, 230), "Looking for Aiden...");
    if (g_cal_state) {
        char ct[96];
        if (g_cal_state == 1) snprintf(ct, sizeof ct, "T-POSE: arms out to the sides, wands forward, look ahead... %d", kCalCountdownMs / 1000 - (int)((GetTickCount64() - g_cal_t0) / 1000));
        else snprintf(ct, sizeof ct, "T-POSE: hold still...");
        big_text(ct, ds.y * 0.45f, 40, IM_COL32(120, 255, 160, 255), IM_COL32(0, 0, 0, 200));
    } else if (g_cal_result[0] && GetTickCount64() - g_cal_result_t < 5000)
        big_text(g_cal_result, ds.y * 0.45f, 34, strncmp(g_cal_result, "T-pose saved", 12) == 0 ? IM_COL32(120, 255, 160, 255) : IM_COL32(255, 120, 120, 255), IM_COL32(0, 0, 0, 200));
    if (g_vr_stall[0]) big_text(g_vr_stall, 60, 26, IM_COL32(255, 255, 255, 255), IM_COL32(120, 0, 0, 200));
}

static void autosave_frame() {   // save settings a couple of seconds after they stop changing
    static unsigned fr = 0; static std::string pending; static ULONGLONG changed_at = 0;
    if (++fr % 30) return;
    std::string t = config_text();
    if (t == g_saved_text) { pending.clear(); return; }
    if (t != pending) { pending = t; changed_at = GetTickCount64(); return; }
    if (GetTickCount64() - changed_at > 2000) { save_config(); pending.clear(); }
}

// ---------------------------------------------------------------- Present
static unsigned long long g_frames_other;
static ULONGLONG g_last_ours;

static HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!g_imgui_ready && !g_swapchain) init_imgui(sc);
    if (flags & DXGI_PRESENT_TEST) return g_orig_present(sc, sync, flags);
    if (g_imgui_ready && sc != g_swapchain) {      // another visible swapchain keeps presenting while ours went quiet: follow it
        if (++g_frames_other % 30 == 1) log("Present on a different swapchain %p (ours %p)", (void*)sc, (void*)g_swapchain);
        if (GetTickCount64() - g_last_ours > 1000 && rebind_swapchain(sc)) g_last_ours = GetTickCount64();
        if (sc != g_swapchain) return g_orig_present(sc, sync, flags);
    }
    if (!g_imgui_ready) return g_orig_present(sc, sync, flags);
    {
        ID3D11Resource* bb = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Resource), (void**)&bb)) && bb) { framecap::on_present_begin(bb); bb->Release(); }
    }
    g_last_ours = GetTickCount64();
    static ULONGLONG last_t = GetTickCount64();
    float dt = clampf((GetTickCount64() - last_t) / 1000.0f, 0.0f, 0.1f);
    last_t = GetTickCount64();

    camera::begin_setup();
    auto_track_aiden();
    static bool vr_tried = false;
    if (!vr_tried) { vr_tried = true; vr_init(g_mod_dir); VrState s; vr_poll(&s); log("SteamVR: %s", s.status); }

    if (g_hmd_display) hmd_display_frame(sc);       // finished frame -> headset, then wait for the next pose
    if (g_snap_req) take_snapshot(sc);
    if (g_hmd_display || g_menu_open) vr_poll(&g_vr);
    input::update(g_vr, g_wand_input && g_hmd_display);
    {
        auto is = input::status();
        bool lt = is.lt > 100 || (g_vr.left.connected && g_vr.left.trigger > 0.4f);
        if (lt != g_aiming) log("Aim %s", lt ? "start" : "end");
        g_aiming = lt;
    }
    {   // what the next game frame is for: left / right eye, or (aiming through a scope) every third one the scope
        float gf = camera::game_fov() * 57.2958f;
        bool scoped = g_scope_on && g_hmd_display && g_aiming && !g_remote_view && !g_screen_mode && camera::stereo_available() &&
                      gf > 1 && gf < g_scope_fov_max;
        if (scoped != g_scoped) { g_scoped = scoped; log("Scope %s (game FOV %.0f deg)", scoped ? "up" : "down", gf); }
        // While scoped or while the phone is open every frame is the game's own view (its camera, aim and zoom): the
        // scope picture / phone panel come from those, the headset keeps showing the last 3D images meanwhile.
        // (Mixing game-view frames between headset frames went wrong: the game doesn't hand frames back in a fixed order.)
        // Phone: with frame ID working, 3 of every 8 frames are the game's view (the middle one feeds the wrist panel)
        // and the headset keeps running live; without it the whole stream is game view and the headset holds still.
        bool phone = g_menu_kind == 1 && g_hands_ui && g_hmd_display && camera::stereo_available() && !g_screen_mode;
        // Live always: with frame ID the labels are exact; without it the frame order is guessed (2 requests late,
        // measured) and frames next to a change aren't trusted. Eyes go flat (one image to both) while the phone is
        // open: every frame then refreshes both eyes, so the 3 game-view frames are a short pause, not a long one per eye.
        g_phone_live = phone;
        g_phone_cycle = g_phone_cycle < 8 ? 8 : g_phone_cycle > 32 ? 32 : g_phone_cycle;
        bool game_view = g_scoped || (phone && g_pose_count % (unsigned)g_phone_cycle >= (unsigned)g_phone_cycle - 3);   // 3 of every 16 (0.16.5, was 12)
        bool stereo = g_stereo && camera::stereo_available() && !(g_in_vehicle && !g_remote_view && !g_car_3d) && !phone;
        static int eye_flip = 0;
        if (game_view) g_next_type = 2;
        else if (stereo) { g_next_type = eye_flip; eye_flip ^= 1; }
        else g_next_type = -1;
        if (!g_scoped && g_scope_shown) { vr_scope_hide(); g_scope_shown = false; }
    }
    if (g_hmd_display) framecap::hooks_install(g_context);
    aim_depth_frame(g_hmd_display && (g_shown_type == 0 || g_shown_type == 1));
    dot_frame();
    hud_update();
    keys_frame(dt);
    car_cam_frame();
    calibration_frame();
    screen_auto_frame();
    stall_frame();
    if (g_body_bones && (!g_hmd_display || g_screen_mode || !camera::head_look())) body_release("paused");
    steer_update(dt);
    view_frame(dt);
    autosave_frame();

    if (!g_rtv) create_rtv();
    if (g_rtv) {
        ImGui::GetIO().MouseDrawCursor = g_menu_open;
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        overlay_frame();
        if (g_menu_open) draw_menu();
        ImGui::Render();
        ID3D11RenderTargetView* old_rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* old_dsv = nullptr;
        g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, old_rtv, &old_dsv);
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, old_rtv, old_dsv);
        for (auto* r : old_rtv) if (r) r->Release();
        if (old_dsv) old_dsv->Release();
    }
    framecap::on_present_end();
    return g_orig_present(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE hooked_resize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    if (sc == g_swapchain && g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    HRESULT hr = g_orig_resize(sc, n, w, h, f, fl);
    log("ResizeBuffers %ux%u -> 0x%08lx", w, h, (unsigned long)hr);
    return hr;   // RTV is recreated on the next Present
}

// ---------------------------------------------------------------- setup
// A throwaway swapchain, just to learn where Present() lives.
static bool find_swapchain_functions(void** present, void** resize) {
    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc = DefWindowProcW; wc.hInstance = g_self; wc.lpszClassName = L"WDVR_dummy";
    RegisterClassExW(&wc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, g_self, nullptr);
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = wnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = E_FAIL;
    auto create = (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)GetProcAddress(LoadLibraryA("d3d11.dll"), "D3D11CreateDeviceAndSwapChain");
    if (!create) { log("d3d11.dll not available"); DestroyWindow(wnd); return false; }
    for (D3D_DRIVER_TYPE type : { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP }) {
        hr = create(nullptr, type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
        if (SUCCEEDED(hr)) break;
    }
    if (SUCCEEDED(hr)) {
        void** vtbl = *(void***)sc;
        *present = vtbl[8];     // IDXGISwapChain::Present
        *resize  = vtbl[13];    // IDXGISwapChain::ResizeBuffers
        sc->Release(); ctx->Release(); dev->Release();
    }
    DestroyWindow(wnd);
    UnregisterClassW(wc.lpszClassName, g_self);
    log("Dummy swapchain: 0x%08lx", (unsigned long)hr);
    return SUCCEEDED(hr);
}

static DWORD WINAPI init_thread(LPVOID) {
    Sleep(1500);   // let the game finish starting up
    void *present = nullptr, *resize = nullptr;
    if (!find_swapchain_functions(&present, &resize)) { log("Could not create dummy swapchain - giving up"); return 0; }
    if (MH_Initialize() != MH_OK) { log("MinHook init failed"); return 0; }
    MH_STATUS a = MH_CreateHook(present, (void*)&hooked_present, (void**)&g_orig_present);
    MH_STATUS b = MH_CreateHook(resize, (void*)&hooked_resize, (void**)&g_orig_resize);
    MH_STATUS c = MH_EnableHook(MH_ALL_HOOKS);
    log("Hooks: Present=%d ResizeBuffers=%d enable=%d (0 = OK)", a, b, c);
    return 0;
}

// Crash recorder: on a fatal-looking exception, log where it happened (module + offset).
static LONG CALLBACK crash_logger(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO)
        return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG count = 0;
    if (InterlockedIncrement(&count) > 4) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
    HMODULE mod = nullptr; char name[MAX_PATH] = "unknown (generated code, or no module)";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)rip, &mod) && mod)
        GetModuleFileNameA(mod, name, sizeof name);
    uintptr_t fault = ep->ExceptionRecord->NumberParameters >= 2 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
    log("CRASH? exception 0x%08lX at %p (%s +0x%llX), touching %p, thread %lu", (unsigned long)code, (void*)rip,
        strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name, (unsigned long long)(mod ? rip - (uintptr_t)mod : 0), (void*)fault, GetCurrentThreadId());
    return EXCEPTION_CONTINUE_SEARCH;
}

void start(HMODULE self) {
    g_self = self;
    g_start_tick = GetTickCount64();
    GetModuleFileNameA(self, g_mod_dir, MAX_PATH);
    if (char* s = strrchr(g_mod_dir, '\\')) *s = 0;
    log("WDVR %s, folder %s", WDVR_VERSION, g_mod_dir);
    load_config();
    AddVectoredExceptionHandler(0, crash_logger);
    if (HANDLE t = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr)) CloseHandle(t);
}

} // namespace wdvr

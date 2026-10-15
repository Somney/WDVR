// SteamVR connection via OpenVR's C function tables.
// openvr_api.dll is loaded at runtime from the mod folder, so the game still
// starts normally if the file is missing or SteamVR isn't installed.
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "openvr_capi.h"
#include "vr.h"

typedef uint32_t (__stdcall *PFN_InitInternal2)(EVRInitError*, EVRApplicationType, const char*);
typedef void     (__stdcall *PFN_ShutdownInternal)(void);
typedef intptr_t (__stdcall *PFN_GetGenericInterface)(const char*, EVRInitError*);
typedef int      (__stdcall *PFN_IsRuntimeInstalled)(void);
typedef const char* (__stdcall *PFN_GetEnumAsSymbol)(EVRInitError);

static HMODULE g_dll;
static PFN_ShutdownInternal g_shutdown;
static struct VR_IVRSystem_FnTable* g_sys;
static struct VR_IVRCompositor_FnTable* g_comp;
static struct VR_IVROverlay_FnTable* g_ovl;
static VROverlayHandle_t g_screen;
static VROverlayHandle_t g_hud;
static PFN_GetGenericInterface g_get;
static int g_scene;                       /* 1 = we own the headset display */
static TrackedDevicePose_t g_render_poses[VR_MAX_DEVICES];
static int g_have_render_poses;
static char g_status[160] = "Not started";

static void set_status(const char* s) { strncpy(g_status, s, sizeof g_status - 1); }

static int vr_init_type(const char* mod_dir, int scene) {
    if (g_sys) return 1;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\openvr_api.dll", mod_dir);
    if (!g_dll) g_dll = LoadLibraryA(path);
    if (!g_dll) { set_status("openvr_api.dll not found next to dxgi.dll"); return 0; }

    PFN_InitInternal2 init = (PFN_InitInternal2)GetProcAddress(g_dll, "VR_InitInternal2");
    PFN_GetGenericInterface get = (PFN_GetGenericInterface)GetProcAddress(g_dll, "VR_GetGenericInterface");
    PFN_IsRuntimeInstalled installed = (PFN_IsRuntimeInstalled)GetProcAddress(g_dll, "VR_IsRuntimeInstalled");
    PFN_GetEnumAsSymbol err_name = (PFN_GetEnumAsSymbol)GetProcAddress(g_dll, "VR_GetVRInitErrorAsSymbol");
    g_shutdown = (PFN_ShutdownInternal)GetProcAddress(g_dll, "VR_ShutdownInternal");
    if (!init || !get) { set_status("openvr_api.dll is too old or damaged"); return 0; }
    g_get = get;
    if (installed && !installed()) { set_status("SteamVR is not installed"); return 0; }

    // Background app: reads tracking without taking over the headset display.
    // Later stages switch to a Scene app when we start rendering to the Vive.
    EVRInitError err = EVRInitError_VRInitError_None;
    init(&err, scene ? EVRApplicationType_VRApplication_Scene : EVRApplicationType_VRApplication_Background, NULL);
    if (err != EVRInitError_VRInitError_None) {
        char buf[160];
        snprintf(buf, sizeof buf, "SteamVR connection failed: %s (is SteamVR running?)",
                 err_name ? err_name(err) : "unknown error");
        set_status(buf);
        return 0;
    }

    char iface[64];
    snprintf(iface, sizeof iface, "FnTable:%s", IVRSystem_Version);
    g_sys = (struct VR_IVRSystem_FnTable*)get(iface, &err);
    if (!g_sys || err != EVRInitError_VRInitError_None) {
        set_status("Connected, but SteamVR doesn't offer the expected IVRSystem version - update SteamVR");
        g_sys = NULL;
        if (g_shutdown) g_shutdown();
        return 0;
    }
    g_scene = scene;
    g_comp = NULL;
    g_have_render_poses = 0;
    if (scene) {
        snprintf(iface, sizeof iface, "FnTable:%s", IVRCompositor_Version);
        g_comp = (struct VR_IVRCompositor_FnTable*)get(iface, &err);
        if (!g_comp || err != EVRInitError_VRInitError_None) {
            g_comp = NULL;
            set_status("Connected, but SteamVR's compositor isn't available - update SteamVR");
            if (g_shutdown) g_shutdown();
            g_sys = NULL;
            return 0;
        }
        snprintf(iface, sizeof iface, "FnTable:%s", IVROverlay_Version);
        g_ovl = (struct VR_IVROverlay_FnTable*)get(iface, &err);
        if (err != EVRInitError_VRInitError_None) g_ovl = NULL;
        g_screen = 0;
        set_status("Connected to SteamVR - game is shown in the headset");
    } else {
        set_status("Connected to SteamVR");
    }
    return 1;
}

int vr_init(const char* mod_dir) { return vr_init_type(mod_dir, 0); }

int vr_init_scene(const char* mod_dir) {
    if (g_sys && g_scene) return 1;
    if (g_sys) vr_shutdown();
    return vr_init_type(mod_dir, 1);
}

int vr_is_scene(void) { return g_sys && g_scene && g_comp; }

int vr_wait_poses(void) {
    if (!vr_is_scene()) return -1;
    EVRCompositorError e = g_comp->WaitGetPoses(g_render_poses, VR_MAX_DEVICES, NULL, 0);
    g_have_render_poses = (e == EVRCompositorError_VRCompositorError_None);
    return (int)e;
}

int vr_submit(void* d3d11_texture, const float pose[3][4], const VrBounds* left, const VrBounds* right) {
    if (!vr_is_scene()) return -1;
    VRTextureWithPose_t t;
    memset(&t, 0, sizeof t);
    t.handle = d3d11_texture;
    t.eType = ETextureType_TextureType_DirectX;
    t.eColorSpace = EColorSpace_ColorSpace_Auto;
    EVRSubmitFlags flags = EVRSubmitFlags_Submit_Default;
    if (pose) { memcpy(t.mDeviceToAbsoluteTracking.m, pose, sizeof(float) * 12); flags = EVRSubmitFlags_Submit_TextureWithPose; }
    VRTextureBounds_t bl = { left->u0, left->v0, left->u1, left->v1 };
    VRTextureBounds_t br = { right->u0, right->v0, right->u1, right->v1 };
    EVRCompositorError e1 = g_comp->Submit(EVREye_Eye_Left, (struct Texture_t*)&t, &bl, flags);
    EVRCompositorError e2 = g_comp->Submit(EVREye_Eye_Right, (struct Texture_t*)&t, &br, flags);
    return e1 ? (int)e1 : (int)e2;
}

int vr_submit_eye(int eye, void* d3d11_texture, const float pose[3][4], const VrBounds* b) {
    if (!vr_is_scene() || !d3d11_texture) return -1;
    VRTextureWithPose_t t;
    memset(&t, 0, sizeof t);
    t.handle = d3d11_texture;
    t.eType = ETextureType_TextureType_DirectX;
    t.eColorSpace = EColorSpace_ColorSpace_Auto;
    EVRSubmitFlags flags = EVRSubmitFlags_Submit_Default;
    if (pose) { memcpy(t.mDeviceToAbsoluteTracking.m, pose, sizeof(float) * 12); flags = EVRSubmitFlags_Submit_TextureWithPose; }
    VRTextureBounds_t vb = { b->u0, b->v0, b->u1, b->v1 };
    return (int)g_comp->Submit(eye ? EVREye_Eye_Right : EVREye_Eye_Left, (struct Texture_t*)&t, &vb, flags);
}

// A flat "TV screen" floating in the world (menus, map).
int vr_screen_show(void* d3d11_texture, const float hmd_m[3][4], float distance, float width) {
    if (!vr_is_scene() || !g_ovl || !d3d11_texture) return -1;
    if (!g_screen) {
        if (g_ovl->CreateOverlay("wdvr.screen", "WDVR screen", &g_screen) != EVROverlayError_VROverlayError_None) { g_screen = 0; return -2; }
    }
    // Place it 'distance' metres in front of the head, level, facing the player.
    float fx = -hmd_m[0][2], fz = -hmd_m[2][2];
    float len = sqrtf(fx * fx + fz * fz); if (len < 1e-3f) { fx = 0; fz = -1; len = 1; }
    fx /= len; fz /= len;
    float c = -fz, sn = fx;                 /* yaw y: forward = (sin y, 0, -cos y) */
    HmdMatrix34_t t;
    t.m[0][0] = c;  t.m[0][1] = 0; t.m[0][2] = -sn;
    t.m[1][0] = 0;  t.m[1][1] = 1; t.m[1][2] = 0;
    t.m[2][0] = sn; t.m[2][1] = 0; t.m[2][2] = c;
    t.m[0][3] = hmd_m[0][3] + fx * distance;
    t.m[1][3] = hmd_m[1][3];
    t.m[2][3] = hmd_m[2][3] + fz * distance;
    g_ovl->SetOverlayWidthInMeters(g_screen, width);
    g_ovl->SetOverlayTransformAbsolute(g_screen, ETrackingUniverseOrigin_TrackingUniverseStanding, &t);
    Texture_t tex = { d3d11_texture, ETextureType_TextureType_DirectX, EColorSpace_ColorSpace_Gamma };
    g_ovl->SetOverlayTexture(g_screen, &tex);
    return (int)g_ovl->ShowOverlay(g_screen);
}

void vr_screen_update(void* d3d11_texture) {
    if (!g_ovl || !g_screen || !d3d11_texture) return;
    Texture_t tex = { d3d11_texture, ETextureType_TextureType_DirectX, EColorSpace_ColorSpace_Gamma };
    g_ovl->SetOverlayTexture(g_screen, &tex);
}

void vr_screen_hide(void) { if (g_ovl && g_screen) g_ovl->HideOverlay(g_screen); }

// The game's HUD as a see-through panel locked in front of the head.
int vr_hud_show(void* d3d11_texture, float distance, float width, float down) {
    if (!vr_is_scene() || !g_ovl || !d3d11_texture) return -1;
    if (!g_hud) {
        if (g_ovl->CreateOverlay("wdvr.hud", "WDVR HUD", &g_hud) != EVROverlayError_VROverlayError_None) { g_hud = 0; return -2; }
        g_ovl->SetOverlaySortOrder(g_hud, 10);
    }
    HmdMatrix34_t t;
    memset(&t, 0, sizeof t);
    t.m[0][0] = 1; t.m[1][1] = 1; t.m[2][2] = 1;
    t.m[1][3] = -down; t.m[2][3] = -distance;
    g_ovl->SetOverlayWidthInMeters(g_hud, width);
    g_ovl->SetOverlayTransformTrackedDeviceRelative(g_hud, k_unTrackedDeviceIndex_Hmd, &t);
    Texture_t tex = { d3d11_texture, ETextureType_TextureType_DirectX, EColorSpace_ColorSpace_Gamma };
    g_ovl->SetOverlayTexture(g_hud, &tex);
    return (int)g_ovl->ShowOverlay(g_hud);
}
void vr_hud_hide(void) { if (g_ovl && g_hud) g_ovl->HideOverlay(g_hud); }

static TrackedDeviceIndex_t hand_device(int hand) {   /* 0 left, 1 right; no role assigned: a controller that isn't the other hand */
    ETrackedControllerRole want = hand ? ETrackedControllerRole_TrackedControllerRole_RightHand : ETrackedControllerRole_TrackedControllerRole_LeftHand;
    ETrackedControllerRole other = hand ? ETrackedControllerRole_TrackedControllerRole_LeftHand : ETrackedControllerRole_TrackedControllerRole_RightHand;
    TrackedDeviceIndex_t idx = g_sys->GetTrackedDeviceIndexForControllerRole(want);
    if (idx == k_unTrackedDeviceIndexInvalid) {
        TrackedDeviceIndex_t o = g_sys->GetTrackedDeviceIndexForControllerRole(other);
        for (TrackedDeviceIndex_t i = 1; i < k_unMaxTrackedDeviceCount; i++)
            if (i != o && g_sys->GetTrackedDeviceClass(i) == ETrackedDeviceClass_TrackedDeviceClass_Controller &&
                g_sys->IsTrackedDeviceConnected(i)) { idx = i; break; }
    }
    return idx;
}

/* Overlays held on the wands: 0 = phone (left), 1 = weapon wheel (right), 2 = rifle scope (right). */
static VROverlayHandle_t g_hand_ovl[3];
static int hand_overlay(int slot, int hand, void* tex, float width, const HmdMatrix34_t* t) {
    if (!vr_is_scene() || !g_ovl || !g_sys || !tex) return -1;
    static const char* keys[3] = { "wdvr.phone", "wdvr.wheel", "wdvr.scope" };
    static const char* names[3] = { "WDVR phone", "WDVR weapon wheel", "WDVR scope" };
    TrackedDeviceIndex_t idx = hand_device(hand);
    VROverlayHandle_t* h = &g_hand_ovl[slot];
    if (idx == k_unTrackedDeviceIndexInvalid) { if (*h) g_ovl->HideOverlay(*h); return -3; }
    if (!*h) {
        if (g_ovl->CreateOverlay(keys[slot], names[slot], h) != EVROverlayError_VROverlayError_None) { *h = 0; return -2; }
        g_ovl->SetOverlaySortOrder(*h, 20);
    }
    g_ovl->SetOverlayWidthInMeters(*h, width);
    g_ovl->SetOverlayTransformTrackedDeviceRelative(*h, idx, t);
    Texture_t tx = { tex, ETextureType_TextureType_DirectX, EColorSpace_ColorSpace_Gamma };
    g_ovl->SetOverlayTexture(*h, &tx);
    return (int)g_ovl->ShowOverlay(*h);
}
// A piece of the HUD held on top of a wand (0 = left: phone, 1 = right: weapon wheel), tilted toward your face.
int vr_hand_show(int hand, void* d3d11_texture, float width, float tilt_deg, float up, float back) {
    if (hand < 0 || hand > 1) return -1;
    float a = tilt_deg * 0.0174532925f, c = cosf(a), sn = sinf(a);
    HmdMatrix34_t t;
    memset(&t, 0, sizeof t);
    t.m[0][0] = 1;                                  /* rotation about X */
    t.m[1][1] = c;  t.m[1][2] = -sn;
    t.m[2][1] = sn; t.m[2][2] = c;
    t.m[1][3] = up; t.m[2][3] = back;
    return hand_overlay(hand, hand, d3d11_texture, width, &t);
}
void vr_hand_hide(int hand) { if (g_ovl && hand >= 0 && hand < 3 && g_hand_ovl[hand]) g_ovl->HideOverlay(g_hand_ovl[hand]); }
// The scope picture on the right wand, facing back along the wand (toward your eye).
int vr_scope_show(void* d3d11_texture, float width, float up, float back, float tilt_deg) {
    /* 0.16.12: square to the tilted aim line (pistol grip): the picture faces back along it, 'up'/'back' along it too */
    float a = tilt_deg * 0.0174532925f, c = cosf(a), sn = sinf(a);
    HmdMatrix34_t t;
    memset(&t, 0, sizeof t);
    t.m[0][0] = 1;
    t.m[1][1] = c;   t.m[1][2] = sn;
    t.m[2][1] = -sn; t.m[2][2] = c;
    t.m[1][3] = up * c + back * sn; t.m[2][3] = -up * sn + back * c;
    return hand_overlay(2, 1, d3d11_texture, width, &t);
}
void vr_scope_hide(void) { vr_hand_hide(2); }

/* The red dot: a small picture at a point in the room (standing space), turned to face the head. */
static VROverlayHandle_t g_dot;
int vr_dot_show(void* d3d11_texture, const float pos[3], const float head[3], float width) {
    if (!vr_is_scene() || !g_ovl || !d3d11_texture) return -1;
    if (!g_dot) {
        if (g_ovl->CreateOverlay("wdvr.dot", "WDVR red dot", &g_dot) != EVROverlayError_VROverlayError_None) { g_dot = 0; return -2; }
        g_ovl->SetOverlaySortOrder(g_dot, 30);
    }
    float z[3] = { head[0] - pos[0], head[1] - pos[1], head[2] - pos[2] };
    float l = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (l < 1e-3f) return -1;
    z[0] /= l; z[1] /= l; z[2] /= l;
    float x[3] = { z[2], 0, -z[0] };               /* up (0,1,0) x z */
    float lx = sqrtf(x[0] * x[0] + x[2] * x[2]);
    if (lx < 1e-3f) { x[0] = 1; x[1] = 0; x[2] = 0; lx = 1; }
    x[0] /= lx; x[2] /= lx;
    float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
    HmdMatrix34_t t;
    for (int i = 0; i < 3; i++) { t.m[i][0] = x[i]; t.m[i][1] = y[i]; t.m[i][2] = z[i]; t.m[i][3] = pos[i]; }
    g_ovl->SetOverlayWidthInMeters(g_dot, width);
    g_ovl->SetOverlayTransformAbsolute(g_dot, ETrackingUniverseOrigin_TrackingUniverseStanding, &t);
    Texture_t tex = { d3d11_texture, ETextureType_TextureType_DirectX, EColorSpace_ColorSpace_Gamma };
    g_ovl->SetOverlayTexture(g_dot, &tex);
    int e = (int)g_ovl->ShowOverlay(g_dot);
    if (e == 10 || e == 11) g_dot = 0;              /* unknown / invalid handle: make a new one next time */
    return e;
}
void vr_dot_hide(void) { if (g_ovl && g_dot) g_ovl->HideOverlay(g_dot); }

void vr_eye_tangents(int eye, float* l, float* r, float* t, float* b) {
    *l = -1.2f; *r = 1.2f; *t = -1.3f; *b = 1.3f;   /* sane fallback */
    if (g_sys) g_sys->GetProjectionRaw(eye ? EVREye_Eye_Right : EVREye_Eye_Left, l, r, t, b);
}

void vr_shutdown(void) {
    if (g_sys && g_shutdown) g_shutdown();
    g_sys = NULL;
    g_comp = NULL;
    g_ovl = NULL;
    g_screen = 0;
    g_hud = 0;
    g_hand_ovl[0] = g_hand_ovl[1] = g_hand_ovl[2] = 0;
    g_dot = 0;      /* 0.16.5: was kept after a headset off/on -> 'invalid handle' (11) every frame, no red dot */
    g_scene = 0;
    g_have_render_poses = 0;
    set_status("Disconnected");
}

static void fill_pose(VrDevice* d, const TrackedDevicePose_t* p) {
    d->connected = p->bDeviceIsConnected;
    d->valid = p->bPoseIsValid;
    if (!d->valid) return;
    const float (*m)[4] = p->mDeviceToAbsoluteTracking.m;
    memcpy(d->m, m, sizeof d->m);
    d->pos[0] = m[0][3]; d->pos[1] = m[1][3]; d->pos[2] = m[2][3];
    // OpenVR: -Z is forward, +Y is up.
    float fx = -m[0][2], fy = -m[1][2], fz = -m[2][2];
    const float rad = 57.2957795f;
    d->yaw   = atan2f(fx, -fz) * rad;
    d->pitch = asinf(fy < -1 ? -1 : fy > 1 ? 1 : fy) * rad;
    d->roll  = atan2f(m[1][0], m[1][1]) * rad;
}

void vr_poll(VrState* out) {
    memset(out, 0, sizeof *out);
    strncpy(out->status, g_status, sizeof out->status - 1);
    if (!g_sys) return;
    out->ok = 1;

    TrackedDevicePose_t poses[VR_MAX_DEVICES];
    if (g_scene && g_have_render_poses)
        memcpy(poses, g_render_poses, sizeof poses);   /* poses SteamVR wants this frame rendered with */
    else
        g_sys->GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin_TrackingUniverseStanding,
                                               0.0f, poses, VR_MAX_DEVICES);

    for (uint32_t i = 0; i < VR_MAX_DEVICES; i++) {
        VrDevice* d = &out->all[i];
        fill_pose(d, &poses[i]);
        if (!d->connected) continue;

        // Device names and types rarely change: ask SteamVR about once a second, not every frame.
        static char     model_cache[VR_MAX_DEVICES][64];
        static int      class_cache[VR_MAX_DEVICES];
        static unsigned refresh;
        if (!model_cache[i][0] || (refresh % 90) == 0) {
            ETrackedPropertyError perr;
            g_sys->GetStringTrackedDeviceProperty(i, ETrackedDeviceProperty_Prop_ModelNumber_String,
                                                  model_cache[i], sizeof model_cache[i], &perr);
            class_cache[i] = (int)g_sys->GetTrackedDeviceClass(i);
        }
        if (i == VR_MAX_DEVICES - 1) refresh++;
        memcpy(d->model, model_cache[i], sizeof d->model);
        ETrackedDeviceClass cls = (ETrackedDeviceClass)class_cache[i];

        if (cls == ETrackedDeviceClass_TrackedDeviceClass_Controller) {
            d->is_controller = 1;
            d->role = (int)g_sys->GetControllerRoleForTrackedDeviceIndex(i);
            VRControllerState_t st;
            if (g_sys->GetControllerState(i, &st, sizeof st)) {
                d->buttons = st.ulButtonPressed;
                d->touched = st.ulButtonTouched;
                d->trackpad[0] = st.rAxis[0].x;
                d->trackpad[1] = st.rAxis[0].y;
                d->trigger = st.rAxis[1].x;
            }
            if (d->role == ETrackedControllerRole_TrackedControllerRole_LeftHand) out->left = *d;
            else if (d->role == ETrackedControllerRole_TrackedControllerRole_RightHand) out->right = *d;
            else if (!out->left.connected) out->left = *d;       // unassigned wand: fill a slot
            else if (!out->right.connected) out->right = *d;
        } else if (cls == ETrackedDeviceClass_TrackedDeviceClass_HMD) {
            out->hmd = *d;
        }
    }
}

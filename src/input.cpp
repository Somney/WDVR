// Stage 4: Vive wands as an Xbox controller. See input.h.
//
// Wand mapping (Xbox names):
//   Left trackpad (touch)     -> left stick (move)        Right trackpad (touch) -> right stick (turn camera)
//   Left trackpad click       -> centre: L3, edges: D-pad Right trackpad click   -> centre: R3, edges: Y/B/A/X
//   Left trigger              -> LT (aim)                 Right trigger          -> RT (shoot)
//   Left grip                 -> LB                       Right grip             -> RB
//   Left menu button          -> Back                     Right menu button      -> Start
#include "input.h"
#include <windows.h>
#include <xinput.h>
#include <cmath>
#include <cstring>
#include "MinHook.h"
#include "log.h"

namespace wdvr::input {

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using GetCapsFn  = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);

static GetStateFn g_orig_get_state;
static GetCapsFn  g_orig_get_caps;
static Status     g_status;
static Options    g_options;
static SRWLOCK    g_lock = SRWLOCK_INIT;
static XINPUT_GAMEPAD g_wand_pad;      // latest wand state, as a gamepad
static bool       g_wands_active;
static DWORD      g_packet;
static volatile bool  g_rs_ovr;
static volatile SHORT g_rs_x, g_rs_y;
static volatile SHORT g_real_rx;       // a real gamepad's right stick X (before any override)

Status status() { return g_status; }

// Button presses the game saw (rising edges, per bit) and injected presses (0.16.9: car camera auto-select).
static volatile ULONGLONG g_edge_t[16];
static WORD g_prev_buttons;
static volatile WORD g_inject; static volatile ULONGLONG g_inject_until;
void inject_buttons(unsigned short b, unsigned ms) { g_inject_until = GetTickCount64() + ms; g_inject = b; }
int last_button_pressed(unsigned long long within_ms, unsigned long long* when) {
    ULONGLONG now = GetTickCount64(), best = 0; int bit = -1;
    for (int i = 0; i < 16; i++) { ULONGLONG t = g_edge_t[i]; if (t && now - t <= within_ms && t > best) { best = t; bit = i; } }
    if (when) *when = best;
    return bit;
}
static void track_edges(WORD b) {
    WORD rise = b & ~g_prev_buttons;
    if (rise) { ULONGLONG now = GetTickCount64(); for (int i = 0; i < 16; i++) if (rise & (1u << i)) g_edge_t[i] = now; }
    g_prev_buttons = b;
}
Options& options() { return g_options; }

static SHORT axis(float v) {
    if (v > 1) v = 1; if (v < -1) v = -1;
    return (SHORT)(v * 32767.0f);
}

void set_right_stick_override(bool on, float x, float y) { g_rs_x = axis(x); g_rs_y = axis(y); g_rs_ovr = on; }

float user_right_x() {
    float real = g_real_rx / 32767.0f;
    if (fabsf(real) < 0.24f) real = 0;                 // XInput's usual right-stick dead zone
    else real = (real - (real > 0 ? 0.24f : -0.24f)) / 0.76f;
    AcquireSRWLockShared(&g_lock);
    float wand = g_wands_active ? g_wand_pad.sThumbRX / 32767.0f : 0.0f;
    ReleaseSRWLockShared(&g_lock);
    float v = real + wand;
    return v > 1 ? 1 : v < -1 ? -1 : v;
}

static void pad_axes(const VrDevice& d, float& x, float& y) {
    x = d.trackpad[0]; y = d.trackpad[1];
    float len = sqrtf(x * x + y * y);
    float dz = g_options.deadzone;
    if (len < dz) { x = y = 0; return; }
    float k = (len - dz) / (1 - dz) / len;   // rescale so movement starts smoothly after the dead zone
    x *= k; y *= k;
}

// Which edge of the trackpad is being clicked: 0 none/centre, 1 up, 2 right, 3 down, 4 left.
static int pad_edge(const VrDevice& d) {
    float x = d.trackpad[0], y = d.trackpad[1];
    if (x * x + y * y < 0.35f * 0.35f) return 0;
    if (fabsf(y) >= fabsf(x)) return y > 0 ? 1 : 3;
    return x > 0 ? 2 : 4;
}

static XINPUT_GAMEPAD wands_to_pad(const VrState& vr) {
    XINPUT_GAMEPAD p = {};
    const VrDevice& L = vr.left;
    const VrDevice& R = vr.right;
    const uint64_t kMenu = 1ull << 1, kGrip = 1ull << 2, kPad = 1ull << 32;

    if (L.connected) {
        float x, y; pad_axes(L, x, y);
        bool touching = (L.touched & kPad) || !g_options.move_needs_touch;
        bool clicking = (L.buttons & kPad) != 0;
        if (touching && !clicking) { p.sThumbLX = axis(x); p.sThumbLY = axis(y); }
        if (clicking) {
            switch (pad_edge(L)) {
                case 0: p.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB; break;
                case 1: p.wButtons |= XINPUT_GAMEPAD_DPAD_UP; break;
                case 2: p.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT; break;
                case 3: p.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN; break;
                case 4: p.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT; break;
            }
        }
        p.bLeftTrigger = (BYTE)(L.trigger * 255.0f);
        if (L.buttons & kGrip) p.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        if (L.buttons & kMenu) p.wButtons |= XINPUT_GAMEPAD_BACK;
    }
    if (R.connected) {
        float x, y; pad_axes(R, x, y);
        bool touching = (R.touched & kPad) != 0;
        bool clicking = (R.buttons & kPad) != 0;
        if (touching && !clicking) { p.sThumbRX = axis(x * g_options.turn_speed); p.sThumbRY = axis(y); }
        if (clicking) {
            switch (pad_edge(R)) {
                case 0: p.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB; break;
                case 1: p.wButtons |= XINPUT_GAMEPAD_Y; break;
                case 2: p.wButtons |= XINPUT_GAMEPAD_B; break;
                case 3: p.wButtons |= XINPUT_GAMEPAD_A; break;
                case 4: p.wButtons |= XINPUT_GAMEPAD_X; break;
            }
        }
        p.bRightTrigger = (BYTE)(R.trigger * 255.0f);
        if (R.buttons & kGrip) p.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        if (R.buttons & kMenu) p.wButtons |= XINPUT_GAMEPAD_START;
    }
    return p;
}

static SHORT add_axis(SHORT a, SHORT b) {
    int v = (int)a + (int)b;
    return (SHORT)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

// The game calls this (usually every frame) to read controller #0..3.
static DWORD hooked_get_state_inner(DWORD idx, XINPUT_STATE* st);
static DWORD WINAPI hooked_get_state(DWORD idx, XINPUT_STATE* st) {
    DWORD r = hooked_get_state_inner(idx, st);
    if (idx != 0 || !st) return r;
    if (g_inject && GetTickCount64() < g_inject_until) {          // a press the mod makes (it is not "the player's")
        if (r != ERROR_SUCCESS) { memset(st, 0, sizeof *st); r = ERROR_SUCCESS; }
        st->Gamepad.wButtons |= g_inject;
        st->dwPacketNumber += ++g_packet;
        g_prev_buttons |= g_inject;                               // (not recorded as a player press)
        return r;
    }
    if (g_inject) { g_inject = 0; if (r == ERROR_SUCCESS) st->dwPacketNumber += ++g_packet; }
    if (r == ERROR_SUCCESS) track_edges(st->Gamepad.wButtons); else g_prev_buttons = 0;
    return r;
}
static DWORD hooked_get_state_inner(DWORD idx, XINPUT_STATE* st) {
    DWORD r = g_orig_get_state(idx, st);
    if (idx != 0) return r;
    g_status.game_reads++;
    g_status.real_pad = (r == ERROR_SUCCESS);
    g_real_rx = (r == ERROR_SUCCESS && st) ? st->Gamepad.sThumbRX : 0;
    AcquireSRWLockShared(&g_lock);
    bool active = g_wands_active;
    XINPUT_GAMEPAD w = g_wand_pad;
    ReleaseSRWLockShared(&g_lock);
    if (g_rs_ovr && st) {      // view steering: the mod drives the right stick
        if (r != ERROR_SUCCESS) { memset(st, 0, sizeof *st); r = ERROR_SUCCESS; }
        st->Gamepad.sThumbRX = g_rs_x; st->Gamepad.sThumbRY = g_rs_y;
        if (!active) { st->dwPacketNumber += ++g_packet; return r; }
        w.sThumbRX = 0; w.sThumbRY = 0;
    }
    if (!active || !st) return r;
    if (r != ERROR_SUCCESS) { memset(st, 0, sizeof *st); r = ERROR_SUCCESS; }   // wands act as a connected pad
    XINPUT_GAMEPAD& g = st->Gamepad;
    g.wButtons |= w.wButtons;
    if (w.bLeftTrigger > g.bLeftTrigger) g.bLeftTrigger = w.bLeftTrigger;
    if (w.bRightTrigger > g.bRightTrigger) g.bRightTrigger = w.bRightTrigger;
    g.sThumbLX = add_axis(g.sThumbLX, w.sThumbLX); g.sThumbLY = add_axis(g.sThumbLY, w.sThumbLY);
    if (g_rs_ovr) { g.sThumbRX = g_rs_x; g.sThumbRY = g_rs_y; }
    else { g.sThumbRX = add_axis(g.sThumbRX, w.sThumbRX); g.sThumbRY = add_axis(g.sThumbRY, w.sThumbRY); }
    st->dwPacketNumber += ++g_packet;      // tell the game the state changed
    g_status.buttons = g.wButtons; g_status.lt = g.bLeftTrigger; g_status.rt = g.bRightTrigger;
    g_status.lx = g.sThumbLX; g_status.ly = g.sThumbLY; g_status.rx = g.sThumbRX; g_status.ry = g.sThumbRY;
    return r;
}

static DWORD WINAPI hooked_get_caps(DWORD idx, DWORD flags, XINPUT_CAPABILITIES* caps) {
    DWORD r = g_orig_get_caps(idx, flags, caps);
    if (idx == 0 && r != ERROR_SUCCESS && g_wands_active && caps) {
        memset(caps, 0, sizeof *caps);
        caps->Type = XINPUT_DEVTYPE_GAMEPAD;
        caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        caps->Gamepad.wButtons = 0xF3FF;
        caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 255;
        caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = (SHORT)0xFFC0;
        r = ERROR_SUCCESS;
    }
    return r;
}

static void try_hook() {
    static ULONGLONG last_try = 0;
    if (g_status.hooked || GetTickCount64() - last_try < 2000) return;
    last_try = GetTickCount64();
    const char* names[] = { "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll", "xinput1_2.dll", "xinput1_1.dll" };
    for (const char* n : names) {
        HMODULE m = GetModuleHandleA(n);
        if (!m) continue;
        void* gs = (void*)GetProcAddress(m, "XInputGetState");
        void* gc = (void*)GetProcAddress(m, "XInputGetCapabilities");
        if (!gs) continue;
        bool ok = MH_CreateHook(gs, (void*)&hooked_get_state, (void**)&g_orig_get_state) == MH_OK &&
                  MH_EnableHook(gs) == MH_OK;
        if (ok && gc && MH_CreateHook(gc, (void*)&hooked_get_caps, (void**)&g_orig_get_caps) == MH_OK)
            MH_EnableHook(gc);
        log("Input: hooked %s (%s)", n, ok ? "ok" : "FAILED");
        if (ok) { g_status.hooked = true; strncpy(g_status.dll, n, sizeof g_status.dll - 1); }
        return;
    }
}

void update(const VrState& vr, bool enabled) {
    try_hook();
    XINPUT_GAMEPAD p = {};
    bool active = enabled && vr.ok && (vr.left.connected || vr.right.connected);
    if (active) p = wands_to_pad(vr);
    AcquireSRWLockExclusive(&g_lock);
    g_wand_pad = p;
    g_wands_active = active;
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace wdvr::input

// Stage 2: finding and controlling Watch Dogs' render camera.
//
// Credits: camera layout, code sites and instruction bytes are from Otis_Inf's
// Watch_Dogs camera (https://github.com/FransBouma/InjectableGenericCameraSystem,
// Cameras/Watch_Dogs), BSD-2-Clause, Copyright (c) 2017 Frans Bouma.
//
// How it works:
//   * Disrupt_b64.dll is packed (VMProtect), so we only search it after the game
//     is already rendering, when its code is unpacked in memory.
//   * Each code site is located by its exact bytes. We first check the address
//     Otis_Inf's tool used (game v1.06.329); if the bytes there don't match we
//     search the whole module and require exactly ONE match, otherwise we give up
//     without patching anything (fail-closed).
//   * Site "capture": the game reads camera coordinates here; we record the
//     camera struct addresses (the game keeps two copies and flips between them).
//   * Sites "write pos" and "write rot": the game writes the camera here every
//     frame. Our stub skips those writes for the camera structs while head-look
//     is on, so our values stick.
#include "camera.h"
#include <windows.h>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <cstdio>
#include <vector>
#include <cstddef>
#include "log.h"

namespace wdvr::camera {

// Offsets inside a camera struct (struct = captured rax + 8).
constexpr int kCoords = 0x04;   // 3 floats
constexpr int kFov    = 0x24;   // float, radians
constexpr int kLook   = 0x44;   // 3x3 rotation matrix, 9 floats, row-major

static Status g_status;
static Tuning g_tuning;
const Status& status() { return g_status; }
Tuning& tuning() { return g_tuning; }

// Shared data the machine-code stubs read/write. Lives in executable memory we
// allocate, so the stubs can reach it with RIP-relative addressing.
struct alignas(16) StubData {
    uintptr_t slot_a;        // camera struct address #1
    uintptr_t slot_b;        // camera struct address #2
    uint8_t   block;         // 1 = skip the game's camera writes
    uint8_t   block_fov;     // 1 = skip the game's field-of-view write
    uint8_t   stereo;        // 1 = position path active (eye offset or first-person anchor)
    uint8_t   pad;
    float     eye_off[3];    // world-space offset for the eye being drawn next
    uint64_t  pos_hits;      // how often the game wrote the camera position
    float     game_rot[9];   // the rotation the game WANTED to write (captured while we block it)
    uint32_t  pad2;
    uint64_t  rot_hits;      // how often that capture ran
    uintptr_t player_addr;   // if set: camera position = [player_addr] + anchor_off (first-person)
    float     anchor_off[3];
    uint32_t  head_ring_idx; // next slot in the head-writer ring
    float     game_pos[3];   // where the GAME wanted the camera (before our first-person anchor)
    uint32_t  arm_raw_seq;   // counts game bone writes we saw (raw copies refreshed)
    uintptr_t arm_base;      // Aiden's model-space bone array: bone overrides apply only to this one
    uint8_t   arm_on;        // 1 = write our bone transforms over the game's
    uint8_t   pad5[3];
    float     game_fov;      // the FOV the game last tried to set (radians) - zoom (scopes, hacked cameras)
    float     cam_written[3];// the camera position our anchor last wrote (driving lag measurement)
};
static_assert(offsetof(StubData, eye_off) == 20 && offsetof(StubData, game_rot) == 40 && offsetof(StubData, rot_hits) == 80 &&
              offsetof(StubData, player_addr) == 88 && offsetof(StubData, anchor_off) == 96 && offsetof(StubData, game_pos) == 112 &&
              offsetof(StubData, arm_base) == 128 && offsetof(StubData, arm_on) == 136 &&
              offsetof(StubData, game_fov) == 140 && offsetof(StubData, cam_written) == 144, "layout");
constexpr size_t kDataArea = 256;   // bytes reserved for StubData before the stub code
static_assert(sizeof(StubData) <= kDataArea, "StubData grew into the stub code area");
static StubData* g_data;
static constexpr size_t kRingSlots = 256;
static uintptr_t* g_head_ring;    // objects the game just wrote a head position for (ring buffer)
static uintptr_t* g_rig_ring;     // r12 at the head writer, same slots as g_head_ring (the character's rig object)
// Bone overrides (Aiden's arms): map[bone] = slot+1 (0 = leave alone), 32 bytes per slot (quaternion, position).
static constexpr int kArmBones = 512, kArmSlots = 64;
static uint8_t* g_arm_map;        // kArmBones bytes
static float*   g_arm_data;       // our transforms, kArmSlots * 8 floats
static float*   g_arm_raw;        // the game's own transform for that bone, captured right before we overwrite it
static bool      g_pos_hooked;
static bool      g_fov_hooked;
static bool      g_head_hooked;
static uint8_t*  g_code;       // next free byte for stub code
static uint8_t*  g_code_end;

uintptr_t struct_a() { return g_data ? g_data->slot_a : 0; }
uintptr_t struct_b() { return g_data ? g_data->slot_b : 0; }

static void set_msg(const char* fmt, ...) {
    va_list a; va_start(a, fmt);
    vsnprintf(g_status.message, sizeof g_status.message, fmt, a);
    va_end(a);
    log("Camera: %s", g_status.message);
}

// ------------------------------------------------------------ pattern search
struct Pattern { std::vector<int> bytes; };   // -1 = wildcard

static Pattern parse(const char* s) {
    Pattern p;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (*s == '?') { p.bytes.push_back(-1); while (*s == '?') s++; }
        else { p.bytes.push_back((int)strtoul(s, nullptr, 16)); s += 2; }
    }
    return p;
}

static bool readable(DWORD prot) {
    if (prot & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                   PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY);
}

static bool matches_at(const uint8_t* p, const Pattern& pat) {
    for (size_t i = 0; i < pat.bytes.size(); i++)
        if (pat.bytes[i] >= 0 && p[i] != (uint8_t)pat.bytes[i]) return false;
    return true;
}

static bool range_readable(uintptr_t addr, size_t len) {
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t end = addr + len;
    while (addr < end) {
        if (!VirtualQuery((void*)addr, &mbi, sizeof mbi)) return false;
        if (mbi.State != MEM_COMMIT || !readable(mbi.Protect)) return false;
        addr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return true;
}

static std::vector<uintptr_t> scan(const Pattern& pat, int max_hits) {
    std::vector<uintptr_t> hits;
    uintptr_t addr = g_status.engine_base, end = addr + g_status.engine_size;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < end && VirtualQuery((void*)addr, &mbi, sizeof mbi)) {
        uintptr_t rbase = (uintptr_t)mbi.BaseAddress, rend = rbase + mbi.RegionSize;
        if (rend > end) rend = end;
        if (mbi.State == MEM_COMMIT && readable(mbi.Protect) && rend - rbase >= pat.bytes.size()) {
            const uint8_t* p = (const uint8_t*)rbase;
            size_t n = (rend - rbase) - pat.bytes.size();
            for (size_t i = 0; i <= n; i++) {
                if (matches_at(p + i, pat)) {
                    hits.push_back(rbase + i);
                    if ((int)hits.size() >= max_hits) return hits;
                }
            }
        }
        addr = rend;
    }
    return hits;
}

// Locate one code site: known address first, else a unique match anywhere.
static uintptr_t locate(const char* name, const char* pattern, int site_offset, uint32_t hint_rva) {
    Pattern pat = parse(pattern);
    uintptr_t hint = g_status.engine_base + hint_rva - site_offset;
    if (hint_rva && range_readable(hint, pat.bytes.size()) && matches_at((const uint8_t*)hint, pat)) {
        log("Camera: site '%s' found at known address (+0x%X)", name, hint_rva);
        return hint + site_offset;
    }
    auto hits = scan(pat, 9);
    if (hits.size() > 1) {
        char list[512] = ""; int n = 0;
        for (size_t i = 0; i < hits.size() && i < 8; i++)
            n += snprintf(list + n, sizeof list - n, " +0x%llX", (unsigned long long)(hits[i] + site_offset - g_status.engine_base));
        log("Camera: site '%s' candidates:%s%s", name, list, hits.size() > 8 ? " ..." : "");
    }
    if (hits.size() == 1) {
        log("Camera: site '%s' found by search at +0x%llX", name,
            (unsigned long long)(hits[0] + site_offset - g_status.engine_base));
        return hits[0] + site_offset;
    }
    log("Camera: site '%s' %s", name, hits.empty() ? "NOT found" : "matched more than once - refusing");
    return 0;
}

// ------------------------------------------------------------ code patching
// Disrupt_b64.dll's pages refuse ordinary writes; WriteProcessMemory with a
// real process handle works (documented by the wd-rawinput project).
static bool write_code(uintptr_t addr, const void* bytes, size_t len) {
    HANDLE h = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
                           PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
    if (!h) return false;
    SIZE_T done = 0;
    BOOL ok = WriteProcessMemory(h, (void*)addr, bytes, len, &done) && done == len;
    if (!ok) {
        DWORD old;
        if (VirtualProtectEx(h, (void*)addr, len, PAGE_EXECUTE_READWRITE, &old)) {
            ok = WriteProcessMemory(h, (void*)addr, bytes, len, &done) && done == len;
            VirtualProtectEx(h, (void*)addr, len, old, &old);
        }
    }
    FlushInstructionCache(h, (void*)addr, len);
    CloseHandle(h);
    return ok;
}

// Tiny x64 machine-code emitter for our stubs.
struct Emit {
    uint8_t* p;
    void b(std::initializer_list<uint8_t> v) { for (uint8_t x : v) *p++ = x; }
    void bytes(const void* src, size_t n) { memcpy(p, src, n); p += n; }
    // RIP-relative disp32 to 'target', where the instruction ends 'tail' bytes after the disp.
    void rel(const void* target, int tail = 0) {
        int32_t d = (int32_t)((intptr_t)target - (intptr_t)(p + 4 + tail));
        memcpy(p, &d, 4); p += 4;
    }
    void jmp_abs(uintptr_t to) { b({ 0xFF, 0x25, 0, 0, 0, 0 }); memcpy(p, &to, 8); p += 8; }
    uint8_t* jcc8(uint8_t op) { b({ op, 0 }); return p - 1; }      // returns patch slot
    void land(uint8_t* slot) { *slot = (uint8_t)(p - (slot + 1)); }
    uint8_t* jcc32(uint8_t op) { b({ 0x0F, op, 0, 0, 0, 0 }); return p - 4; }  // near Jcc
    uint8_t* jmp32() { b({ 0xE9, 0, 0, 0, 0 }); return p - 4; }               // near JMP
    void land32(uint8_t* slot) { int32_t d = (int32_t)(p - (slot + 4)); memcpy(slot, &d, 4); }
};

static bool alloc_stub_memory() {
    uint8_t* mem = (uint8_t*)VirtualAlloc(nullptr, 0x8000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return false;
    g_head_ring = (uintptr_t*)(mem + 0x2000);       // data only, after the code region
    memset(g_head_ring, 0, kRingSlots * sizeof(uintptr_t));
    g_rig_ring = (uintptr_t*)(mem + 0x3000);
    memset(g_rig_ring, 0, kRingSlots * sizeof(uintptr_t));
    g_arm_map = mem + 0x4000;                       // 512 bytes
    g_arm_data = (float*)(mem + 0x4200);            // 64 * 32 bytes
    g_arm_raw = (float*)(mem + 0x4A00);             // 64 * 32 bytes
    memset(mem + 0x4000, 0, 0x1200);
    g_data = (StubData*)mem;
    memset(g_data, 0, sizeof *g_data);
    g_code = mem + kDataArea;   // stubs start after the data area - never overlap it
    g_code_end = mem + 0x2000;
    return true;
}

// Replace 'len' bytes at 'site' with a jump to 'stub' (padded with NOPs).
static bool redirect(uintptr_t site, size_t len, uint8_t* stub) {
    uint8_t patch[128];
    if (len < 14 || len > sizeof patch) return false;
    patch[0] = 0xFF; patch[1] = 0x25; memset(patch + 2, 0, 4);
    uintptr_t to = (uintptr_t)stub; memcpy(patch + 6, &to, 8);
    memset(patch + 14, 0x90, len - 14);
    return write_code(site, patch, len);
}

// Stub for the capture site: remember camera struct (rax + 8), then run the
// original instructions and jump back.
static uint8_t* build_capture_stub(uintptr_t site, size_t len) {
    uint8_t* start = g_code;
    Emit e{ g_code };
    e.b({ 0x9C });                                   // pushfq
    e.b({ 0x51 });                                   // push rcx
    e.b({ 0x48, 0x8D, 0x48, 0x08 });                 // lea rcx,[rax+8]
    e.b({ 0x48, 0x3B, 0x0D }); e.rel(&g_data->slot_a); // cmp rcx,[slot_a]
    uint8_t* j = e.jcc8(0x74);                       // je done
    e.b({ 0x52 });                                   // push rdx
    e.b({ 0x48, 0x8B, 0x15 }); e.rel(&g_data->slot_a); // mov rdx,[slot_a]
    e.b({ 0x48, 0x89, 0x15 }); e.rel(&g_data->slot_b); // mov [slot_b],rdx
    e.b({ 0x48, 0x89, 0x0D }); e.rel(&g_data->slot_a); // mov [slot_a],rcx
    e.b({ 0x5A });                                   // pop rdx
    e.land(j);
    e.b({ 0x59 });                                   // pop rcx
    e.b({ 0x9D });                                   // popfq
    e.bytes((void*)site, len);                       // original instructions
    e.jmp_abs(site + len);
    g_code = e.p;
    return start;
}

// Stub for the FOV write (Otis_Inf's approach): the function ends with
//   mov [rcx+3C],r8d / movss [rcx+24],xmm1 / movss [rcx+28],xmm1 / ret
// We hook the first 14 bytes (the three movs) and land back on the 'ret'.
// While block_fov is set, both writes (+24 current, +28 target) are skipped: we write both ourselves. (0.16.1: after
// some aims the game blended 'current' toward its own 'target' - our FOV got smeared, frame ID lost its tag.)
static uint8_t* build_fov_stub(uintptr_t site) {
    uint8_t* start = g_code;
    Emit e{ g_code };
    e.bytes((void*)site, 4);                                    // mov [rcx+3C],r8d
    e.b({ 0xF3, 0x0F, 0x11, 0x0D }); e.rel(&g_data->game_fov); // movss [game_fov],xmm1 (what the game wanted)
    e.b({ 0x9C });                                              // pushfq
    e.b({ 0x80, 0x3D }); e.rel(&g_data->block_fov, 1); e.b({ 0x00 }); // cmp byte [block_fov],0
    uint8_t* j_run = e.jcc8(0x74);                              // je run_original
    e.b({ 0x9D });                                              // popfq
    uint8_t* j_tail = e.jcc8(0xEB);                             // jmp tail (skip both writes)
    e.land(j_run);
    e.b({ 0x9D });                                              // popfq
    e.bytes((void*)(site + 4), 5);                              // movss [rcx+24],xmm1
    e.bytes((void*)(site + 9), 5);                              // movss [rcx+28],xmm1
    e.land(j_tail);
    e.jmp_abs(site + 14);                                       // back to the 'ret'
    g_code = e.p;
    return start;
}

// Stub for the rotation write (rbx = camera struct). While blocking, skip the
// game's write but copy the 9 values it wanted to write into game_rot, so we
// can follow the game's camera direction (e.g. when the car turns).
// At this point the matrix sits on the stack: rows +44 <- [rsp+40..48],
// +50 <- [rsp+50..58], +5C <- [rsp+30..38] (see Otis_Inf's notes).
static uint8_t* build_rot_stub(uintptr_t site, size_t len) {
    static const uint8_t src[9] = { 0x40, 0x44, 0x48, 0x50, 0x54, 0x58, 0x30, 0x34, 0x38 };
    uint8_t* start = g_code;
    Emit e{ g_code };
    e.b({ 0x9C });                                              // pushfq
    e.b({ 0x48, 0x3B, 0x1D }); e.rel(&g_data->slot_a);         // cmp rbx,[slot_a]
    uint8_t* j_is = e.jcc8(0x74);                               // je is_cam
    e.b({ 0x48, 0x3B, 0x1D }); e.rel(&g_data->slot_b);         // cmp rbx,[slot_b]
    uint8_t* j_orig1 = e.jcc32(0x85);                           // jne run_original
    e.land(j_is);
    e.b({ 0x80, 0x3D }); e.rel(&g_data->block, 1); e.b({ 0x00 }); // cmp byte [block],0
    uint8_t* j_orig2 = e.jcc32(0x84);                           // je run_original
    e.b({ 0x48, 0xFF, 0x05 }); e.rel(&g_data->rot_hits);       // inc qword [rot_hits]
    e.b({ 0x9D });                                              // popfq
    e.b({ 0x50 });                                              // push rax
    for (int k = 0; k < 9; k++) {
        e.b({ 0x8B, 0x44, 0x24, (uint8_t)(src[k] + 8) });       // mov eax,[rsp+8+src]
        e.b({ 0x89, 0x05 }); e.rel(&g_data->game_rot[k]);       // mov [game_rot+k],eax
    }
    e.b({ 0x58 });                                              // pop rax
    uint8_t* j_tail = e.jmp32();                                // jmp back_to_game
    e.land32(j_orig1);
    e.land32(j_orig2);
    e.b({ 0x9D });                                              // popfq
    e.bytes((void*)site, len);                                  // original instructions
    e.land32(j_tail);
    e.jmp_abs(site + len);
    g_code = e.p;
    return start;
}

// Stub for the position write: run the game's own write, then (position path on and
// rcx is a camera struct) either
//   * anchor mode: camera position = Aiden's position + anchor_off, read at this
//     exact moment so there's no lag or pull-back, or
//   * offset mode: add eye_off to the game's position.
// The game rewrites the base position every update, so nothing accumulates.
static uint8_t* build_pos_offset_stub(uintptr_t site, size_t len) {
    uint8_t* start = g_code;
    Emit e{ g_code };
    e.bytes((void*)site, len);                                  // original write
    e.b({ 0x9C });                                              // pushfq
    e.b({ 0x48, 0x3B, 0x0D }); e.rel(&g_data->slot_a);         // cmp rcx,[slot_a]
    uint8_t* j_is = e.jcc8(0x74);                               // je is_cam
    e.b({ 0x48, 0x3B, 0x0D }); e.rel(&g_data->slot_b);         // cmp rcx,[slot_b]
    uint8_t* j_out1 = e.jcc32(0x85);                            // jne out
    e.land(j_is);
    e.b({ 0x48, 0xFF, 0x05 }); e.rel(&g_data->pos_hits);       // inc qword [pos_hits]
    e.b({ 0x80, 0x3D }); e.rel(&g_data->stereo, 1); e.b({ 0x00 }); // cmp byte [stereo],0
    uint8_t* j_out2 = e.jcc32(0x84);                            // je out
    e.b({ 0x48, 0x83, 0xEC, 0x10 });                            // sub rsp,16
    e.b({ 0xF3, 0x0F, 0x7F, 0x04, 0x24 });                      // movdqu [rsp],xmm0
    // remember where the game wanted the camera (used for vehicle / remote-camera detection)
    for (int i = 0; i < 3; i++) {
        e.b({ 0xF3, 0x0F, 0x10, 0x41, (uint8_t)(4 + 4 * i) });  // movss xmm0,[rcx+4+4i]
        e.b({ 0xF3, 0x0F, 0x11, 0x05 }); e.rel(&g_data->game_pos[i]); // movss [game_pos+i],xmm0
    }
    e.b({ 0x48, 0x83, 0x3D }); e.rel(&g_data->player_addr, 1); e.b({ 0x00 }); // cmp qword [player_addr],0
    uint8_t* j_offset = e.jcc32(0x84);                          // je offset_mode
    // anchor mode
    e.b({ 0x50 });                                              // push rax
    e.b({ 0x48, 0x8B, 0x05 }); e.rel(&g_data->player_addr);    // mov rax,[player_addr]
    for (int i = 0; i < 3; i++) {
        e.b({ 0xF3, 0x0F, 0x10, 0x40, (uint8_t)(4 * i) });      // movss xmm0,[rax+4i]
        e.b({ 0xF3, 0x0F, 0x58, 0x05 }); e.rel(&g_data->anchor_off[i]); // addss xmm0,[anchor_off+i]
        e.b({ 0xF3, 0x0F, 0x11, 0x41, (uint8_t)(4 + 4 * i) });  // movss [rcx+4+4i],xmm0
        e.b({ 0xF3, 0x0F, 0x11, 0x05 }); e.rel(&g_data->cam_written[i]); // movss [cam_written+i],xmm0
    }
    e.b({ 0x58 });                                              // pop rax
    uint8_t* j_restore = e.jmp32();                             // jmp restore
    // offset mode
    e.land32(j_offset);
    for (int i = 0; i < 3; i++) {
        uint8_t disp = (uint8_t)(4 + 4 * i);
        e.b({ 0xF3, 0x0F, 0x10, 0x41, disp });                  // movss xmm0,[rcx+4+4i]
        e.b({ 0xF3, 0x0F, 0x58, 0x05 }); e.rel(&g_data->eye_off[i]); // addss xmm0,[eye_off+i]
        e.b({ 0xF3, 0x0F, 0x11, 0x41, disp });                  // movss [rcx+4+4i],xmm0
    }
    e.land32(j_restore);
    e.b({ 0xF3, 0x0F, 0x6F, 0x04, 0x24 });                      // movdqu xmm0,[rsp]
    e.b({ 0x48, 0x83, 0xC4, 0x10 });                            // add rsp,16
    e.land32(j_out1);
    e.land32(j_out2);
    e.b({ 0x9D });                                              // popfq
    e.jmp_abs(site + len);
    g_code = e.p;
    return start;
}

// Stub for the game's head-position write:
//   movss [rsi+8B0],xmm0 / movss xmm0,[rsp+30] / movss [rsi+8AC],xmm1   (22 bytes)
// rsi = the character object (head at +8AC), r12 = its rig. We run the original
// instructions, then append rsi and r12 to ring buffers (which characters were updated).
static uint8_t* build_head_stub(uintptr_t site, size_t len) {
    uint8_t* start = g_code;
    Emit e{ g_code };
    e.bytes((void*)site, len);                                  // original instructions
    e.b({ 0x9C, 0x50, 0x51 });                                  // pushfq / push rax / push rcx
    e.b({ 0x8B, 0x05 }); e.rel(&g_data->head_ring_idx);        // mov eax,[ring_idx]
    e.b({ 0xFF, 0x05 }); e.rel(&g_data->head_ring_idx);        // inc dword [ring_idx]
    e.b({ 0x25, 0xFF, 0x00, 0x00, 0x00 });                      // and eax,255
    e.b({ 0x48, 0x8D, 0x0D }); e.rel(g_head_ring);             // lea rcx,[ring]
    e.b({ 0x48, 0x89, 0x34, 0xC1 });                            // mov [rcx+rax*8],rsi
    e.b({ 0x48, 0x8D, 0x0D }); e.rel(g_rig_ring);              // lea rcx,[rig ring]
    e.b({ 0x4C, 0x89, 0x24, 0xC1 });                            // mov [rcx+rax*8],r12
    e.b({ 0x59, 0x58, 0x9D });                                  // pop rcx / pop rax / popfq
    e.jmp_abs(site + len);
    g_code = e.p;
    return start;
}

// The position-write bytes appear several times in this build. Accept only a
// candidate that (like Otis_Inf's original) starts a function - 16-byte aligned,
// preceded by padding or a 'ret' - and sits close to the rotation write.
static uintptr_t locate_pos_near(uintptr_t rot) {
    Pattern pat = parse("8B 02 89 41 04 8B 42 04 89 41 08 8B 42 08 89 41 0C");
    auto hits = scan(pat, 16);
    uintptr_t pick = 0; int n = 0;
    for (uintptr_t h : hits) {
        intptr_t dist = (intptr_t)h - (intptr_t)rot;
        uint8_t prev = *(const uint8_t*)(h - 1);
        bool aligned = (h & 0xF) == 0;
        bool boundary = prev == 0xCC || prev == 0xC3 || prev == 0x90;
        log("Camera: pos candidate +0x%llX dist %lld aligned %d prev %02X",
            (unsigned long long)(h - g_status.engine_base), (long long)dist, aligned, prev);
        if (aligned && boundary && dist > -0x20000 && dist < 0x20000) { pick = h; n++; }
    }
    if (n == 1) { log("Camera: position write chosen at +0x%llX", (unsigned long long)(pick - g_status.engine_base)); return pick; }
    log("Camera: no single safe position-write candidate (%d) - 3D depth unavailable", n);
    return 0;
}

// After the game stores a bone's model-space transform ([rbx+rdi], rbx = bone list, rdi = bone*32):
// if it's Aiden's list and a bone we drive, keep a copy of the game's value and put ours in its place.
static uint8_t* build_bone_stub(uintptr_t site, size_t len) {
    uint8_t* start = g_code;
    Emit e{ g_code };
    e.bytes((void*)site, len);                                  // the game's two 16-byte stores
    e.b({ 0x9C, 0x50, 0x51, 0x52 });                            // pushfq / push rax / push rcx / push rdx
    e.b({ 0x48, 0x8B, 0x05 }); e.rel(&g_data->arm_base);       // mov rax,[arm_base]
    e.b({ 0x48, 0x39, 0xD8 });                                  // cmp rax,rbx
    uint8_t* j1 = e.jcc32(0x85);                                // jne done
    e.b({ 0x48, 0x89, 0xF8 });                                  // mov rax,rdi
    e.b({ 0x48, 0xC1, 0xE8, 0x05 });                            // shr rax,5
    e.b({ 0x48, 0x3D, 0x00, 0x02, 0x00, 0x00 });                // cmp rax,512
    uint8_t* j2 = e.jcc32(0x83);                                // jae done
    e.b({ 0x48, 0x8D, 0x0D }); e.rel(g_arm_map);               // lea rcx,[map]
    e.b({ 0x0F, 0xB6, 0x04, 0x01 });                            // movzx eax,byte [rcx+rax]
    e.b({ 0x85, 0xC0 });                                        // test eax,eax
    uint8_t* j3 = e.jcc32(0x84);                                // jz done
    e.b({ 0xFF, 0xC8 });                                        // dec eax
    e.b({ 0xC1, 0xE0, 0x05 });                                  // shl eax,5
    e.b({ 0x48, 0x8D, 0x0D }); e.rel(g_arm_raw);               // lea rcx,[raw]
    for (uint8_t o : { 0, 8, 16, 24 }) {
        e.b({ 0x48, 0x8B, 0x54, 0x3B, o });                     // mov rdx,[rbx+rdi+o]
        e.b({ 0x48, 0x89, 0x54, 0x01, o });                     // mov [rcx+rax+o],rdx
    }
    e.b({ 0xFF, 0x05 }); e.rel(&g_data->arm_raw_seq);          // inc dword [raw_seq]
    e.b({ 0x80, 0x3D }); e.rel(&g_data->arm_on, 1); e.b({ 0x00 }); // cmp byte [arm_on],0
    uint8_t* j4 = e.jcc32(0x84);                                // je done
    e.b({ 0x48, 0x8D, 0x0D }); e.rel(g_arm_data);              // lea rcx,[data]
    for (uint8_t o : { 0, 8, 16, 24 }) {
        e.b({ 0x48, 0x8B, 0x54, 0x01, o });                     // mov rdx,[rcx+rax+o]
        e.b({ 0x48, 0x89, 0x54, 0x3B, o });                     // mov [rbx+rdi+o],rdx
    }
    e.land32(j1); e.land32(j2); e.land32(j3); e.land32(j4);
    e.b({ 0x5A, 0x59, 0x58, 0x9D });                            // pop rdx / pop rcx / pop rax / popfq
    e.jmp_abs(site + len);
    g_code = e.p;
    return start;
}
static bool g_bone_hooks;
static void install_bone_hooks() {
    // "movaps xmm0,[rax] / movaps [rdi+rbx],xmm0 / movaps xmm1,[rax+10] / movaps [rdi+rbx+10],xmm1"
    static const uint8_t pat[16] = { 0x0F, 0x28, 0x00, 0x0F, 0x29, 0x04, 0x1F, 0x0F, 0x28, 0x48, 0x10, 0x0F, 0x29, 0x4C, 0x1F, 0x10 };
    int ok = 0;
    for (uint32_t rva : { 0x1B77D0Fu, 0x4D155Eu }) {
        uintptr_t site = g_status.engine_base + rva;
        if (memcmp((void*)site, pat, 16) != 0) { log("Camera: bone store at +0x%X not recognised", rva); continue; }
        uint8_t* stub = build_bone_stub(site, 16);
        if (g_code <= g_code_end && redirect(site, 16, stub)) ok++;
    }
    g_bone_hooks = ok == 2;
    log("Camera: bone hooks %d of 2 installed", ok);
}
bool bone_hooks_available() { return g_bone_hooks; }
void set_bone_override_target(uintptr_t bones) { if (g_data) g_data->arm_base = bones; }
void set_bone_override_enabled(bool on) { if (g_data) g_data->arm_on = on ? 1 : 0; }
void set_bone_slot(int bone, int slot) { if (g_arm_map && bone >= 0 && bone < kArmBones) g_arm_map[bone] = slot < 0 ? 0 : (uint8_t)(slot + 1); }
float* bone_slot_data(int slot) { return (g_arm_data && slot >= 0 && slot < kArmSlots) ? g_arm_data + slot * 8 : nullptr; }
const float* bone_slot_raw(int slot) { return (g_arm_raw && slot >= 0 && slot < kArmSlots) ? g_arm_raw + slot * 8 : nullptr; }
unsigned bone_raw_seq() { return g_data ? g_data->arm_raw_seq : 0; }

// ------------------------------------------------------------ setup thread
static DWORD WINAPI setup_thread(LPVOID) {
    // Find the engine module.
    HMODULE mod = nullptr;
    for (int i = 0; i < 300 && !(mod = GetModuleHandleA("Disrupt_b64.dll")); i++) Sleep(100);
    if (!mod) { set_msg("Disrupt_b64.dll not found"); return 0; }
    g_status.engine_base = (uintptr_t)mod;
    auto* dos = (IMAGE_DOS_HEADER*)mod;
    auto* nt = (IMAGE_NT_HEADERS64*)((uint8_t*)mod + dos->e_lfanew);
    g_status.engine_size = nt->OptionalHeader.SizeOfImage;
    g_status.engine_stamp = nt->FileHeader.TimeDateStamp;
    g_status.engine_found = true;
    log("Camera: Disrupt_b64.dll base %p size 0x%X build stamp %08X",
        (void*)mod, g_status.engine_size, g_status.engine_stamp);

    // Give the unpacker time to finish (the game is already rendering by now).
    set_msg("Searching the game's code for the camera...");
    Sleep(5000);

    // Sites (bytes from Otis_Inf's Watch_Dogs camera, game v1.06.329):
    //  capture : addss xmm8,[rax+0C] / addss xmm7,[rax+10] / addss xmm6,[rax+14]   (16 bytes)
    //            preceded by "lea rcx,[rip+x]" and followed by a call.
    //  write pos: mov eax,[rdx] / mov [rcx+4],eax / ... / mov [rcx+0C],eax        (17 bytes)
    //  write rot: 16 movss stores/loads writing the 3x3 matrix at rbx+44..+64      (87 bytes)
    const char* pat_capture = "48 8D 0D ?? ?? ?? ?? F3 44 0F 58 40 0C F3 0F 58 78 10 F3 0F 58 70 14 E8";
    const char* pat_pos = "8B 02 89 41 04 8B 42 04 89 41 08 8B 42 08 89 41 0C";
    const char* pat_rot =
        "F3 0F 11 43 5C F3 0F 10 44 24 38 F3 0F 11 4B 60 F3 0F 10 4C 24 40 "
        "F3 0F 11 43 64 F3 0F 10 44 24 44 F3 0F 11 4B 44 F3 0F 10 4C 24 48 "
        "F3 0F 11 43 48 F3 0F 10 44 24 50 F3 0F 11 4B 4C F3 0F 10 4C 24 54 "
        "F3 0F 11 43 50 F3 0F 10 44 24 58 F3 0F 11 4B 54 F3 0F 11 43 58";

    uintptr_t cap = 0, pos = 0, rot = 0;
    for (int attempt = 0; attempt < 6 && !(cap && rot); attempt++) {
        if (attempt) Sleep(3000);
        if (!cap) cap = locate("capture", pat_capture, 7, 0x17BCDA4);
        if (!rot) rot = locate("write rot", pat_rot, 0, 0x86847E);
    }
    // The position write is optional: without it the game keeps moving the
    // camera (it follows Aiden) and we only control where it looks.
    (void)pat_pos;
    pos = 0;
    {   // Known address first (Otis_Inf's build), else the safe nearby candidate.
        Pattern pp = parse("8B 02 89 41 04 8B 42 04 89 41 08 8B 42 08 89 41 0C");
        uintptr_t hint = g_status.engine_base + 0x85B9E0;
        if (range_readable(hint, 17) && matches_at((const uint8_t*)hint, pp)) pos = hint;
        else if (rot) pos = locate_pos_near(rot);
    }
    g_pos_hooked = pos != 0;
    // FOV write: "mov [rcx+3C],r8d / movss [rcx+24],xmm1 / movss [rcx+28],xmm1 / ret" (15 bytes)
    uintptr_t fov = locate("write fov", "44 89 41 3C F3 0F 11 49 24 F3 0F 11 49 28 C3", 0, 0x862220);
    g_fov_hooked = fov != 0;
    if (!(cap && rot)) {
        set_msg("Camera code not recognised in this game build (stamp %08X). Nothing was changed.",
                g_status.engine_stamp);
        return 0;
    }

    if (!alloc_stub_memory()) { set_msg("Could not allocate stub memory"); return 0; }
    uint8_t* s_cap = build_capture_stub(cap, 16);
    uint8_t* s_pos = pos ? build_pos_offset_stub(pos, 17) : nullptr;   // rcx = camera struct
    uint8_t* s_rot = build_rot_stub(rot, 87);          // rbx = camera struct
    uint8_t* s_fov = fov ? build_fov_stub(fov) : nullptr;
    if (g_code > g_code_end) { set_msg("Stub memory overflow"); return 0; }

    // Write sites first (they do nothing until block=1), capture last.
    bool ok = (!pos || redirect(pos, 17, s_pos)) && redirect(rot, 87, s_rot) && redirect(cap, 16, s_cap);
    if (ok && fov && !redirect(fov, 14, s_fov)) { log("Camera: FOV hook could not be written"); g_fov_hooked = false; }
    // Aiden's head position writer (Disrupt_b64.dll +0xBD26EB on build 55DC1ECD).
    if (ok) {
        uintptr_t head = locate("head write",
            "F3 0F 11 96 B4 08 00 00 F3 0F 11 86 B0 08 00 00 F3 0F 10 44 24 30 F3 0F 11 8E AC 08 00 00 F3 0F 10 4C 24 34",
            8, 0xBD26EB);
        if (head) {
            uint8_t* s_head = build_head_stub(head, 22);
            g_head_hooked = g_code <= g_code_end && redirect(head, 22, s_head);
        }
        log("Camera: head-position hook %s", g_head_hooked ? "installed - Aiden is found automatically" : "not available");
        install_bone_hooks();
    }
    if (!ok) { set_msg("Could not patch the game's code (write refused)"); return 0; }
    g_status.hooks_ok = true;
    set_msg("Camera hooks installed (rotation%s%s%s)", g_pos_hooked ? " + position (3D ready)" : ", no position (no 3D)",
            g_fov_hooked ? " + FOV" : ", FOV not found", g_head_hooked ? " + Aiden auto-find" : "");
    return 0;
}

void begin_setup() {
    static bool started = false;
    if (started) return;
    started = true;
    if (HANDLE t = CreateThread(nullptr, 0, setup_thread, nullptr, 0, nullptr)) CloseHandle(t);
}

// ------------------------------------------------------------ head look
static bool  g_head_look;
static float g_frozen_pos[3];

float base_yaw_deg();
bool head_look() { return g_head_look; }

bool stereo_available() { return g_pos_hooked && g_data; }
unsigned long long position_writes() { return g_data ? g_data->pos_hits : 0; }
void set_eye_offset(bool on, const float off[3]) {
    if (!g_data) return;
    for (int i = 0; i < 3; i++) if (!std::isfinite(off[i]) || fabsf(off[i]) > 25.0f) on = false;
    g_data->player_addr = 0;       // offset mode never anchors to Aiden (callers re-anchor right after if wanted)
    if (on && g_pos_hooked) { memcpy(g_data->eye_off, off, 12); g_data->stereo = 1; }
    else { float z[3] = { 0, 0, 0 }; memcpy(g_data->eye_off, z, 12); g_data->stereo = 0; g_data->player_addr = 0; }
}
// First-person anchor: camera position = Aiden's position + off (world units).
static bool readable_rw(uintptr_t a, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!a || !VirtualQuery((void*)a, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) || (mbi.Protect & PAGE_GUARD)) return false;
    return a + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}
bool set_anchor(uintptr_t player_addr, const float off[3]) {
    if (!g_data || !g_pos_hooked) return false;
    if (!player_addr || !readable_rw(player_addr, 12)) { g_data->player_addr = 0; return false; }
    for (int i = 0; i < 3; i++) if (!std::isfinite(off[i]) || fabsf(off[i]) > 25.0f) { g_data->player_addr = 0; return false; }
    memcpy(g_data->anchor_off, off, 12);
    g_data->player_addr = player_addr;
    g_data->stereo = 1;            // the anchor runs inside the offset path
    return true;
}

bool head_hook_available() { return g_head_hooked; }
size_t recent_head_objects(uintptr_t* out, size_t max) {
    if (!g_head_hooked || !g_data) return 0;
    uint32_t idx = g_data->head_ring_idx;
    size_t n = 0;
    for (size_t k = 1; k <= kRingSlots && n < max; k++) {          // newest first
        uintptr_t o = g_head_ring[(idx - k) & (kRingSlots - 1)];
        if (!o) continue;
        bool dup = false;
        for (size_t j = 0; j < n; j++) if (out[j] == o) { dup = true; break; }
        if (!dup) out[n++] = o;
    }
    return n;
}
unsigned head_writes() { return g_data ? g_data->head_ring_idx : 0; }
uintptr_t rig_for(uintptr_t obj) {
    if (!g_data || !g_rig_ring || !obj) return 0;
    uint32_t idx = g_data->head_ring_idx;
    for (size_t k = 1; k <= kRingSlots; k++) {
        size_t slot = (idx - k) & (kRingSlots - 1);
        if (g_head_ring[slot] == obj) return g_rig_ring[slot];
    }
    return 0;
}
float game_fov() { return g_data ? g_data->game_fov : 0.0f; }
bool anchored_camera_pos(float out[3]) {
    if (!g_data || !g_data->player_addr) return false;
    memcpy(out, g_data->cam_written, 12);
    return std::isfinite(out[0]) && (out[0] != 0 || out[1] != 0);
}
bool game_camera_pos(float out[3]) {
    if (!g_data || !g_data->stereo) return false;
    memcpy(out, g_data->game_pos, 12);
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]) && (out[0] != 0 || out[1] != 0);
}

// Right vector of the camera as we last wrote it (row 3 of the look matrix).
static float g_last_right[3] = { 0, -1, 0 }, g_last_fwd[3] = { 1, 0, 0 }, g_last_m[3][3];
void last_look(float m[3][3]) { memcpy(m, g_last_m, sizeof g_last_m); }
void camera_right(float out[3]) { memcpy(out, g_last_right, 12); }
void view_forward(float out[3]) { memcpy(out, g_last_fwd, 12); }

static float g_fov_override, g_fov_prev, g_fov_seen[2];
static bool g_rot_lag = true; static unsigned g_req_idx;
void set_rot_lag(bool on) { g_rot_lag = on; }
void set_request(unsigned idx) { g_req_idx = idx; }
static unsigned g_fov_changed;
unsigned fov_changed(float seen[2]) { if (seen) { seen[0] = g_fov_seen[0]; seen[1] = g_fov_seen[1]; } return g_fov_changed; }
void set_fov_override(float r) {
    g_fov_override = r;
    if (g_data) g_data->block_fov = (r > 0.1f && g_head_look) ? 1 : 0;
}

static bool cam_ready() { return g_data && g_data->slot_a && g_data->slot_b && g_status.hooks_ok; }

void set_head_look(bool on) {
    if (on && !cam_ready()) return;
    if (on && !g_head_look) {
        memcpy(g_frozen_pos, (void*)(g_data->slot_a + kCoords), sizeof g_frozen_pos);
        log("Camera: head look ON at %.2f %.2f %.2f", g_frozen_pos[0], g_frozen_pos[1], g_frozen_pos[2]);
    }
    g_head_look = on;
    if (g_data) {
        g_data->block = on ? 1 : 0;
        g_data->block_fov = (on && g_fov_override > 0.1f) ? 1 : 0;
    }
    if (!on) { g_fov_prev = 0; log("Camera: head look OFF"); }
}

void recenter(float head_yaw) { g_tuning.yaw_offset = g_tuning.invert_yaw ? head_yaw : -head_yaw; }

bool game_rot_valid() {
    if (!g_data || g_data->rot_hits == 0) return false;
    const float* r = g_data->game_rot;
    for (int i = 0; i < 9; i++) if (!std::isfinite(r[i]) || fabsf(r[i]) > 1.01f) return false;
    float len = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
    return len > 0.9f && len < 1.1f;            // row 0 must be a unit "forward" vector
}

// Direction the game's own camera faces, as a yaw angle. Row 0 of the look
// matrix is the forward axis; with our parameterisation forward = (cos y, -sin y).
float game_yaw_deg() {
    if (!game_rot_valid()) return 0.0f;
    float fx = g_data->game_rot[0], fy = g_data->game_rot[1];
    return atan2f(-fy, fx) * 57.2957795f;
}

// Where the game's own camera wants to look (row 0 of its intended rotation).
bool game_forward(float out[3]) {
    if (!game_rot_valid()) return false;
    memcpy(out, g_data->game_rot, 12);
    return true;
}

// The yaw that "straight ahead" (head recentered) maps to.
static bool  g_base_ovr;
static float g_base_ovr_yaw;
void set_base_yaw_override(bool on, float yaw_deg) { g_base_ovr = on; g_base_ovr_yaw = yaw_deg; }
float base_yaw_deg() {
    if (g_base_ovr) return g_base_ovr_yaw;
    return (g_tuning.follow_game && game_rot_valid()) ? game_yaw_deg() : 0.0f;
}
// The game's own camera pitch, in the same convention update_head_look uses (forward z = -sin pitch).
float game_pitch_deg() {
    if (!game_rot_valid()) return 0.0f;
    float fz = g_data->game_rot[2];
    return -asinf(fz < -1 ? -1 : fz > 1 ? 1 : fz) * 57.2957795f;
}

// Horizontal frame for moving the camera: forward, right, up in world space.
void base_frame(float fwd[3], float right[3], float up[3]) {
    // The world direction your head faces when looking "straight ahead" (as set by
    // recenter) is exactly base_yaw: the recenter offset cancels the head's own yaw.
    float y = base_yaw_deg() * 0.0174532925f;
    fwd[0] = cosf(y);  fwd[1] = -sinf(y); fwd[2] = 0;
    // The game's world is mirrored relative to our first guess (the eyes had to be
    // swapped), so "right" is the opposite of the look matrix's third row.
    right[0] = sinf(y); right[1] = cosf(y); right[2] = 0;
    up[0] = 0; up[1] = 0; up[2] = 1;
}

// Rotation matrices in DirectX row-vector convention (same as Otis_Inf's tool):
// look = Rx(-roll) * Ry(pitch) * Rz(-yaw), with roll starting at -90 degrees.
static void mat_mul(const float a[3][3], const float b[3][3], float out[3][3]) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
}

static void write_look(float yaw_deg, float pitch_deg, float roll_deg, bool remember = true);
void update_head_look(float head_yaw, float head_pitch, float head_roll) {
    if (!g_head_look || !cam_ready()) return;
    write_look(base_yaw_deg() + g_tuning.yaw_offset + (g_tuning.invert_yaw ? -head_yaw : head_yaw),
               g_tuning.invert_pitch ? -head_pitch : head_pitch,
               g_tuning.use_roll ? (g_tuning.invert_roll ? -head_roll : head_roll) : 0.0f);
}
void look_along_game_aim() {
    if (!g_head_look || !cam_ready() || !game_rot_valid()) return;
    write_look(game_yaw_deg(), game_pitch_deg(), 0.0f, false);   // not "where the view looks"
}
static void write_look(float yaw_deg, float pitch_deg, float roll_deg, bool remember) {
    const float d2r = 3.14159265f / 180.0f;
    float yaw = yaw_deg * d2r, pitch = pitch_deg * d2r, roll = (-90.0f + roll_deg) * d2r;

    float a = -roll, cx = cosf(a), sx = sinf(a);
    float Rx[3][3] = { { 1, 0, 0 }, { 0, cx, sx }, { 0, -sx, cx } };
    float cy = cosf(pitch), sy = sinf(pitch);
    float Ry[3][3] = { { cy, 0, -sy }, { 0, 1, 0 }, { sy, 0, cy } };
    float z = -yaw, cz = cosf(z), sz = sinf(z);
    float Rz[3][3] = { { cz, sz, 0 }, { -sz, cz, 0 }, { 0, 0, 1 } };
    float t[3][3], m[3][3];
    mat_mul(Rx, Ry, t);
    mat_mul(t, Rz, m);

    for (int i = 0; i < 9; i++) if (!std::isfinite((&m[0][0])[i])) return;   // never hand the game garbage
    // Rotation lag (0.16.4): the rotation is written straight into the camera struct at Present, exactly like the FOV,
    // so the game uses it one request sooner than the position (which our stub applies during the game's update).
    // Like the FOV, write each request's rotation one request late, so a frame's rotation matches its position and
    // the head pose it is paired with. (Without it the eye frame just before a game-view frame - phone, scope - was
    // drawn looking where the GAME camera looks: the view jumped up/down 7.5 times a second with the phone open.)
    static float prev_m[3][3]; static unsigned prev_idx; static bool have_prev; static ULONGLONG prev_t;
    ULONGLONG nowt = GetTickCount64();
    float w[3][3];
    bool use_prev = g_rot_lag && have_prev && nowt - prev_t < 300 && prev_idx + 1 == g_req_idx;
    memcpy(w, use_prev ? prev_m : m, sizeof w);
    memcpy(prev_m, m, sizeof m); prev_idx = g_req_idx; prev_t = nowt; have_prev = true;
    for (uintptr_t s : { g_data->slot_a, g_data->slot_b }) {
        // The structs move when the game loads a new area; never write blindly.
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)s, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
            !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) { set_head_look(false); return; }
        memcpy((void*)(s + kLook), w, sizeof w);
        if (remember) { memcpy(g_last_right, m[2], 12); memcpy(g_last_fwd, m[0], 12); }
        memcpy(g_last_m, m, sizeof g_last_m);
        if (g_fov_override > 0.1f) {          // the game keeps two FOV values (+24 current, +28 target)
            // Did anything change our FOV since we last wrote it? (Its write function is blocked - another code path?)
            float cur = *(float*)(s + kFov), tgt = *(float*)(s + kFov + 4);
            if (g_fov_prev > 0.1f && (cur != g_fov_prev || tgt != g_fov_prev)) { g_fov_changed++; g_fov_seen[0] = cur; g_fov_seen[1] = tgt; }
            *(float*)(s + kFov) = g_fov_override;
            *(float*)(s + kFov + 4) = g_fov_override;
        }
    }
    g_fov_prev = g_fov_override;
}

} // namespace wdvr::camera

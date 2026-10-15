# WDVR - Watch Dogs VR mod: project notes

Last session: 2026-10-14. Current version: **0.16.14**. Start with WDVR_HANDOFF.md (current state, build + install rules,
open items). History of every version is below the current-state summary.

## Installed files (E:\Watch_Dogs\bin)
- `dinput8.dll` - Ultimate ASI Loader x64 (ThirteenAG)
- `plugins\WDVR.asi` - the mod. NEVER put backup .asi files in plugins\ (the ASI loader loads every .asi there); backups stay in Claude outputs
- `plugins\openvr_api.dll` - SteamVR library
- `plugins\WDVR.ini` - settings (auto-saved), `plugins\wdvr.log` - log, rewritten each launch
- Source: `E:\Watch_Dogs\WDVR_build_kit.zip` (src + deps + build.sh; build with `bash build.sh` and llvm-mingw from GitHub)

## Source layout (0.11.0)
- `mod.cpp` - everything per frame: headset display (alternate-eye 3D), first-person anchor, view detection
  (vehicle / hacked camera), head steering, body turn + arm IK, HUD panel / phone / wheel / big screen,
  menu, settings (table-driven, `settings()`), snapshot (F10), crash logger.
- `camera.cpp` - code-signature search + stubs: camera capture, rotation write (captures game_rot), position write
  (eye offset / anchor), FOV write, head writer (Aiden object + rig rings), 2 bone stores (bone overrides).
- `framecap.cpp` - HUD split (draw redirect into our see-through texture + composite of the middle area), scene depth,
  frame ID (FOV tag found in the game's camera constant buffer via Map/Unmap/UpdateSubresource hooks).
- `input.cpp` - XInput hook: wands -> Xbox pad, right-stick override, `user_right_x()` (player's stick even while overridden).
- `vr.c` - OpenVR C API: scene app, submit, overlays (big screen, HUD panel, hand panels).

## Controls
- INSERT menu, F10 snapshot, Ctrl+F11 T-pose calibration.
- Numpad (in headset): 8/2 4/6 9/3 eye position, 5 recenter, 0 lean, + big screen, - show hacked camera, * / lag fix.

## Key facts (game build 55DC1ECD)
- Camera struct: pos +4, FOV +0x24/+0x28 (horizontal), look matrix +0x44 (row 0 forward = (cos y, -sin y, -sin p)).
- base_frame r = (sin y, cos y) is the view's LEFT (head right raises yaw -> view turns toward -r).
- The game aims/moves with its own camera controller (game_rot), steered via the right stick (XInput override).
- Aiden: head writer +0xBD26EB (obj rsi, head obj+0x8AC, rig r12). Pose = [[rig+0x10]+0x110]+0x1B0, count +0x28,
  bones [pose+0xB8] 32 B each (quat xyzw, pos xyz 0), model space z up. Entity matrix at rig->[8]->[0x10]->[8]+0x20.
  Bones: pelvis 1, spine 2-4, L arm 5/6/9/10 (+helpers 7,8,29,30, finger 19), R arm 47/48/51/52 (+49,50,72,73, finger 61),
  head 90. The game builds children from the parents we override.
- HUD: tonemap into a full-screen B8G8R8A8 texture with one draw, HUD drawn on top, 4-vertex pass to the backbuffer.

## 0.8.4 - diagnostics (aim/shoot + camera switches)
- F8 / menu "Dump camera recorder": logs last ~512 frames of camera history (struct_a/b, game cam pos/fwd, struct FOV vs
  game-intended FOV, dist/angle to Aiden, game pos/rot write counts per frame, aim/fire/remote/screen/vehicle flags; "<" marks
  changes) plus every camera struct the game read recently (capture stub ring at stub mem+0x2800, idx StubData+136).
- FOV stub now stores the game's intended FOV (xmm1) to StubData.game_fov (+132) even while we override it.
- F7 / menu "Learn aim": 8 s read+write hardware watch (DR RW=11) on struct_a/b +0x44 and +0x04; each code site's hits
  are split into total vs while-shooting (RT / right trigger / mouse). Sorted with shooting-heavy readers first.
- Next: use these logs to find the aim camera and the bullet-direction code.

## 0.8.4 test results -> 0.8.5
- Learn aim: every camera reader runs once per frame whether shooting or not -> the aim ray is computed every frame
  (that is why the reticle follows the headset). Readers get the camera via 0x1C568A0/0x1C568C0 (camera manager
  0x36396F0, id at [obj+0xBC]) or 0x1705F20. 0x166C0D7 = whole-struct memcpy (~115/frame, render views).
- Candidates hooked in 0.8.5 (F6 cycles off/ALL/A..E; right wand pose in world replaces camera pos/fwd):
  A +0x5FC302 ray {pos, 300.0, fwd} into [rbx]; B +0x6DF057 fwd*k regs; C +0x1E3D3DA pos regs + +0x1E3D40C fwd regs
  (then a call - likely ray test); D +0x161BA23 fwd copy to [rdi]; E +0x162EA97 fwd regs.
- Hacked camera at 01:18:50 WAS detected (9.5 m, 108 deg) -> remote view for 7 s. Map -> screen mode ON worked automatically.
- During aim, the game camera stays 2.0 m / 17 deg from Aiden (same struct), so aim = same camera, narrower FOV + DOF.

## 0.8.7
- BUG FIXED: remote view (hacked camera) detection worked but the first-person anchor (player_addr) was never cleared
  in offset mode, so the camera stayed at Aiden's head with the hack camera's rotation. set_eye_offset now clears it.
- Wand aim A-E: user reports no candidate made shots follow the wand (Aiden kept firing along his last orientation).
- Added F6 mode F: while aiming, camera rotation follows the right wand instead of the HMD (test if aim = camera).
- Game-intended FOV seen at 70 deg on foot (75 at startup) - may let us detect the aim zoom.

## 0.8.8
- 0.8.7 session: headset display ON but head-look block never ran (g_have_pose stayed false, "VR frame 1" repeated) -
  hmd.valid false or screen mode. Added a stall diagnosis (log + red on-screen banner with the reason).
- F6 aim test now shows a big on-screen label and "Aim test (F6): ..." at top of the menu.

## 0.8.9
- Hacked cameras now WORK (0.8.7 fix). Remote exit made stricter (<4.5 m & <25 deg, or vehicle <12 m) + 600 ms, so a
  camera right above Aiden no longer flips back to him briefly.
- Mode F result: view follows wand but gun aim unaffected -> the game aims with its OWN camera controller direction
  (game_rot, which we block from the struct), not the struct we write.
- Mode G: closed-loop right-stick steering (XInput override) turns the game camera controller toward the wand
  direction while LT held; view base yaw frozen (set_base_yaw_override) so the headset view stays put. Stick
  directions auto-learned (flip if error grows). Returns to the original direction after release.

## 0.9.0 - HUD investigation
- Problem: HUD/pop-ups are drawn in screen space at the edges; the headset only shows the middle part of the
  138 deg image (u 0.234-0.738), so edge HUD is cut off.
- Plan: find where the game draws its HUD (draws to the backbuffer after the final full-screen pass), redirect
  those draws to a separate transparent texture, and show it as a floating panel in VR (later: phone panel on the
  left wand, gadget wheel near the arm).
- F9 / menu "Capture one frame" (framecap.cpp): hooks the D3D11 context (OMSetRenderTargets, Draw*, PSSetShader,
  OMSetBlendState, RSSetViewports, ClearRTV, CopyResource, ExecuteCommandList, Dispatch) and logs one frame:
  passes per render target and each draw on the backbuffer.

## 0.9.1 - HUD panel + steering fix
- Frame captures (phone, weapon wheel): ~110 passes, no command lists. Second-last pass = full-screen 1920x1080
  fmt 90 (BGRA typeless) texture with 44-86 draws (HUD), last pass = 1 draw (4 verts) to the backbuffer.
  => scene is put into that texture with a full-screen draw, HUD drawn on top, then copied to the screen.
- HUD split (framecap.cpp): learns the texture copied to the screen (stable 30 frames); on bind, lets draw #0 through
  if it is Draw(3|4), then rebinds to our own BGRA texture (cleared to 0) for the rest, with blend states cloned to
  accumulate alpha. Headset image is HUD-free; HUD shown as head-locked SteamVR overlay "wdvr.hud" (dist/width/down
  in the menu, saved in ini), and drawn over the monitor via ImGui. Disabled in screen mode (map/menus).
- Steering G: v0.8.9 sign learning thrashed (error-based). Now learns from correlation of pushed stick vs the game
  camera's actual angle change, locks after 3 agreeing checks. Full-stick range 20 deg.

## 0.9.2 - HUD: middle stays in the world
- 0.9.1 log: game binds the HUD texture first with an sRGB view (91, scene draw) then a UNORM view (87, HUD) -> our
  texture was recreated twice per frame. Now one texture + cached view per format.
- Our own Present-time drawing (screen blit, ImGui) is ignored by the tracker (g_in_present) - target used to flip
  to our blit texture in screen mode.
- World-anchored HUD (hack markers, prompts, crosshair) must stay in the 3D picture to line up. Rule: HUD inside the
  middle area (headset-visible region x "size", default 0.8) is composited back into the game texture just before
  the final pass (premultiplied blend, scissor); the panel shows only the edges (middle blanked); monitor ImGui overlay
  draws only the edges. Menu: "Keep the middle of the HUD in the world" + size slider. Saved in ini.
- Frame capture now lists each HUD draw (index, type, count, pixel shader, blend) for later per-element sorting.

## 0.9.3
- Mode G works (user confirmed) but stick is saturated (game's max turn rate) -> sluggish. Added "aim finder"
  (menu button / F11 in mode G): snapshots +-24 MB around the camera struct and Aiden + Disrupt's RW data, turns the
  game camera with the stick (yaw, then pitch, then both), keeps floats whose change equals the turn (deg or rad, +-),
  then verifies each by writing +8 deg and checking the camera follows. Found vars are written directly every frame
  while aiming (instant). Falls back to the stick.
- Steering and mode F are off during hacked-camera view (triggers = zoom there).
- F10 snapshot: saves snapN_frame.bmp (final picture) and snapN_hud.bmp (HUD layer) in plugins\.
- Phone on left hand: screen rect (default guess 0.66,0.30-0.97,0.97) cut out of world composite + panel, shown as
  overlay "wdvr.phone" relative to the left controller (size/tilt/height/offset sliders, saved). Needs the real rect
  from a snapshot with the phone open.
- Open: profiler labels on people look tilted (need a snapshot).

## 0.9.4
- Snapshot 1 (phone open): phone screen occupies x 0.52-0.78, y 0.20-0.75 (drawn with a 3D tilt); when the phone is
  open the rest of the HUD is hidden. 0.9.3's guess rect (0.66-0.97 x 0.30-0.97) grabbed other HUD (user: "map").
  NOTE: snapshot BMPs are 32-bit BI_RGB - read alpha from raw bytes (PIL drops it).
- Phone rect now 0.51,0.19-0.79,0.76; only moved to the hand while OPEN: coverage of HUD alpha>40 in that rect
  (staging readback every 6 frames, 2-texture ring, DO_NOT_WAIT), open >35%, close <15%.
- Left wand lookup falls back to "the controller that isn't the right hand"; log spam fixed.
- Aim finder 0.9.3 never finished (10k candidates x 300 ms tests). Now 5 moves of filtering + binary search
  (write +8 deg into half the candidates, keep the half the camera follows), offset measured after settling.

## 0.9.5
- Snapshots: phone and weapon wheel are drawn on the right half with a 3D tilt that sways with camera motion (the
  phone warps as the head moves - game effect). Phone extent ~x0.50-0.82 y0.15-0.86; wheel ~x0.50-0.89 y0.28-0.63.
  When either is open the rest of the HUD is hidden.
- Detection: probe area x0.44-0.95 y0.08-0.92, alpha>40 coverage >8% = open (<3% closed); height of covered rows
  >0.42 = phone (left hand), else weapon wheel (right hand). Areas cut out: phone 0.44,0.10-0.87,0.92;
  wheel 0.45,0.18-0.93,0.72. Overlays wdvr.phone / wdvr.wheel (vr_hand_show(hand,...)).
- Aim finder crashed the game (0.9.4) writing +8 deg into ~400 candidates at once (null deref in Disrupt+0x1F146D0).
  Now: only the 64 candidates nearest the camera struct, tested 4 at a time, only sane angle values.

## 0.9.6 - arms (step 1: find the skeleton)
- Goal: Aiden's arms follow the wands (IK). Plan: (1) find the bone matrix array(s) for Aiden, (2) identify the
  arm/hand bones (compare arms-down vs aiming scans), (3) find the point where the game finishes animating and
  before it skins/draws (hardware watch on a hand bone -> writer code), (4) hook there and overwrite the upper
  arm / forearm / hand rotations with a 2-bone IK solved to the wand pose (mapped into game space like the
  wand-aim code), (5) hide the head mesh for first person.
- skeleton.cpp: Shift+F9 / menu "Find Aiden's skeleton": scans +-48 MB around Aiden's object for >=20 consecutive
  orthonormal matrices (4x4 pos-in-row, 3x4 pos-in-col, 4x4 pos-in-col) with positions within 2.5 m of the head
  (world) or of the origin (local); logs every bone position (relative to the head for world space).

## 0.9.7
- 0.9.6 scans (arms down / "aiming" - but the wands were asleep, so no aim happened) found 7 identical matrix lists
  (27-47 bones, 4x4 pos-in-row, local space) at -8.8 MB .. -42 MB from Aiden's object: identical between scans ->
  static rest/bind poses, not the live pose.
- Skeleton finder v2: +-96 MB, matrices AND quaternion+position layouts (32/48 byte strides), first pass collects
  >=16-entry lists, second look 1.5 s later keeps only lists where >=25% of entries changed; logs per-bone position
  and change amount (to spot the arms).

## 0.9.8
- Skeleton v2 scan: 522 still lists; the "moving" ones all sit in one ~16 KB block at -25.3 MB from Aiden's object
  (0x7B89F080..), overlapping in every layout, almost all positions identical (-0.60 -0.51 0.09) and changing by ~1
  everywhere -> a per-frame scratch buffer that is reused, not Aiden's live skeleton.
- Next approach: trace back from the head writer (+0xBD26EB writes Aiden's head pos to obj+0x8AC) - the code before
  it must read the head bone from the live pose. 0.9.8 logs 0x480 bytes of code around it ("Code +0x..." lines).

## 0.9.9
- Head-writer code (0.9.8 dump): Aiden obj (rsi) +0x8AC head = position from call 0x1960260(rcx=r12, edx=3, r8=out)
  plus a direction*const; same function called with idx 1 and 2 (other look-at points). So r12 = a rig/skeleton
  object and +0x1960260 = "get bone/marker position by index". Also writes look-at data at +0x8A0.., +0x8B8..0x8CC,
  and [obj+0x108] gets a position.
- 0.9.9: head stub also records r12 per slot (ring at stub mem+0x3000); camera::rig_for(obj). Startup logs 0x300
  bytes of the function at +0x1960260 ("Fn +0x..."); Shift+F9 now first dumps the rig object (0x200 bytes) and
  0x80 bytes behind each pointer in it ("Rig ..." lines), then runs the old scan.

## 0.9.10
- +0x1960260(rig, marker, out): skel = rig->[0x10]->vfunc(+0x140); bone = *(int*)(rig+0x20+marker*4);
  ok = +0x1D5A880(skel, bone, &local); world = +0xA6210(entity_matrix(+0x179A1C0(rig->[8]->[0x10]->[8])), local).
  Aiden's rig marker->bone table (rig+0x20): -1, 106, 95, 90(head, marker 3), 4,3,2,1, 271,316, 52,51,48,47,52,
  9,6,5,10, 314,315,269,270 -> skeleton has 300+ bones (face rig).
- 0.9.10: Shift+F9 logs the pose getter (vtable+0x140 of rig->[0x10]), follows it if it is a plain field read, and
  dumps the component and the pose object (+ one level of pointers). Startup dumps +0x1D5A880 code.

## 0.9.11 - Aiden's live pose found (layout)
- Pose getter (vtable+0x140 of rig->[0x10], at +0x1738FF0): lazy-update check on comp+0x108/0x109, then
  pose = [comp+0x110] + 0x1B0.
- +0x1D5A880(pose, bone, out): bone < [pose+0x28] (count); B = pose+0x98; if dirty[B+0x30][bone]==2 -> call
  +0x4D14B0(B) to recompute, clear flag; entry = [B+0x20] + bone*32: quaternion at +0, position at +0x10
  (model space). Entity matrix (+0x179A1C0) * pos = world.
- Shift+F9 now dumps all bone positions (model space) twice 1.5 s apart, marking moved (!) and dirty (*) bones.

## 0.9.12 - bones identified, first arm test
- Pose has 400 bones (model space: x forward, y left, z up, metres; feet at z 0, head ~1.5).
  Left arm: clavicle 5, upper arm 6, forearm 9, hand 10, fingers 11-46. Right arm: clavicle 47, upper arm 48,
  forearm 51, hand 52, helpers 49-50, fingers/props 53-88. Upper arm and forearm both 0.28 m. Head 90, pelvis 1,
  spine 2-4, legs 269-272 / 314-317, face 89-249. (Matches the rig's marker table: 52,51,48,47 and 10,9,6,5.)
- Many bones are flagged dirty (2) = computed lazily on demand (+0x4D14B0).
- Ctrl+F10 / menu "Arm test": a thread rewrites bones 48-88 rotated about the shoulder (default 70 deg forward
  lift), as fast as it can, to see if the drawn body uses this list. Quaternion order selectable.
- Ctrl+F9 / menu "Learn hand code": 3 s hardware write watch on the right hand position + its dirty flag.

## 0.9.13 - bone override hook
- 0.9.12 arm test (thread rewriting bones 48-88 in a loop): the arm DID move on screen, flickering and contorted
  -> the renderer uses this bone list. Write watch on the right hand (52) position: only two writers,
  +0x1B77D1F (~once per frame, main evaluation) and +0x4D156E (lazy recompute, +0x4D14B0). Both end with
  "movaps xmm0,[rax]; movaps [rdi+rbx],xmm0; movaps xmm1,[rax+10]; movaps [rdi+rbx+10],xmm1" (rbx = list,
  rdi = bone*32). Dirty flags set to 2 by +0x297BB9F, +0x1C219AF/+0x1C219D9 (also writes local list [rbx+0x10]),
  cleared by +0x1B80CCA, +0x1D36123 (bounding box pass), +0x1D5A970.
- Hooked both stores (16 bytes at +0x1B77D0F and +0x4D155E): for list == arm_base and map[bone] = slot+1, copy the
  game's value into raw[slot], and if arm_on, store data[slot] instead. Stub memory grown to 0x8000
  (map 0x4000, data 0x4200, raw 0x4A00; StubData arm_base@168 arm_on@176 arm_raw_seq@180).
- Arm test v2 runs through this hook (Present thread computes data from raw each frame). Option "only move the
  upper arm" (48-50) tells whether children follow their parent's stored transform by themselves.
- Startup now dumps +0x179A1C0 (entity -> world matrix) for the model->world transform needed by the IK.

## 0.9.14
- 0.9.13 arm test did nothing: my guard checked the position's 4th float (w), which is 0 in this engine, so it
  never wrote. Fixed (checks for any captured value); menu shows how many game bone writes the hook saw.
- Entity -> world matrix: +0x179A1C0 is "lea rax,[rcx+0x20]; ret" -> world matrix at entity+0x20,
  entity = rig->[8]->[0x10]->[8]. (Layout of that matrix still to confirm.)

## 0.10.0 - arms follow the wands (first version)
- arms_update (Present thread, inside the head-look block): wand pos/forward -> game world (same mapping as wand
  aim) -> Aiden model space via entity matrix (entity+0x20, layout auto-detected: translation in row 4 or column 4).
  Two-bone IK from the game's shoulder (raw bone 6 / 48), lengths from the game's pose, pole = down/out/back.
  Each bone is turned by the shortest rotation taking its limb direction (learned once in the bone's own frame from
  the untouched pose: upper->forearm, forearm->hand, hand->finger 19/61) to the new one; hand points along the wand.
  Overrides bones 6,9,10 (left) and 48,51,52 (right) through the bone-store hook (slots 0-5).
- Quaternion convention auto-check: right elbow built by the game must land on our elbow; if off >6 cm for 90 frames,
  flip convention (max 2 flips). Menu: on/off, wrist-behind-grip, elbows out, manual convention toggle.
- Only in first-person on foot with VR on (released in vehicles, hacked cameras, menus).

## 0.10.1 - T-pose calibration
- 0.10.0 user report: some tracking, but arms spinning / crossed. Log: auto convention flips happened (elbow error
  only 7-8 cm -> the game DOES build children from our overridden parents; flips were spurious) -> auto-flip removed.
- Arms now stay on the game's animation until calibrated. Ctrl+F11 / menu "Calibrate: T-pose": 3 s countdown (big
  on-screen text), then 0.5 s of samples with overrides off: wand positions/forward in model space vs expected hands
  (shoulder +-0.55 m sideways). Solves mirror (wands must then point +x), turn about z, scale (arm span ratio) and
  offset; saved in the ini. Every 2 s the log shows the right target, shoulder and wand forward in model space.
- Wrist: straight by default (hand along the forearm); option "hand points where the wand points".

## 0.10.2
- T-pose countdown never ended when started with VR off (sampling lives in the head-look block). Now: starting the
  calibration turns VR (headset display) on itself, the timing runs every frame, countdown 8 s with beeps each second
  (higher for the last 3), a beep when sampling starts, success/failure beep + 5 s on-screen result; sampling times out
  after 4 s with a reason (wands not tracked / Aiden not found / headset not tracking). Calibration saved immediately.

## 0.10.3
- T-pose log: wands L (0.77,0.33,1.48) R (-0.85,0.07,1.45), shoulders L (-0.26,0.16,1.40) R (0.08,0.02,1.45)
  (model space) -> Aiden's body is turned inside model space by the animation (shoulder line runs mostly along x).
  Model x is NOT his forward. Now: body frame from the shoulders (lateral = left shoulder - right shoulder, forward =
  lateral x up); T-pose expected hands = shoulders +- lateral*0.55; mirror check uses body forward; elbow pole uses
  body frame. Old calibrations are ignored (ini key arms_cal_v2) -> recalibrate.
- Coat pulled when the idle anim put a hand in the pocket: twist/helper bones were still animated. Now also
  overridden: left 7,8 (upper), 29,30 (forearm); right 49,50 (upper), 72,73 (forearm) (slots 6-13). Each keeps its
  learned place along the segment (t) and its rest rotation relative to the segment bone.

## 0.11.0 - head drives Aiden, arms rebuilt, cleanup, menus
User (0.10.3): arms closer to working; wants head rotation to turn Aiden ("so they are one"); aiming integrated with
the skeleton (conflicting animations/instructions); declutter everything; phone / maps too small and clipping; pause
menu and pause map show misaligned black blank spots.
- Cleanup: removed finder/watch/skeleton/proxy (dxgi build), flight recorder, aim learn, aim candidates A-E + F6 modes,
  aim finder, arm test, pose/rig dumps, memory watch, late camera, camera-struct ring, FOV capture, frame-capture logging,
  third-person POV / positional / mirror-lean / shoulder-aim options. Code 5885 -> ~3700 lines. Settings table-driven.
  StubData: arm_base@128, arm_on@136, game_pos@112 (capture/head/FOV stubs re-verified by disassembly).
- Head steering (replaces mode G): on foot the view's straight-ahead is a fixed yaw (turned by right pad / stick via
  input::user_right_x, smooth turn deg/s); the game camera is always steered (right-stick P-control) to the head
  direction, or to the right wand while LT is held -> Aiden walks where you look, shoots where the wand points.
  Stick directions learned by correlation, then saved (steer_sign_x/y, steer_locked_x/y); relearn if |err|>60 for 2 s.
  Paused in vehicles, hacked cameras, big screen; released (stick passes through) while phone / wheel is open.
- Wand mapping fix: tracking_to_world sent your right to base_frame r (= LEFT) -> wands were mirrored (why the T-pose
  needed a mirror). Fixed; calibration key now arms_cal_v3 -> redo the T-pose.
- Body turn: pelvis (bone 1) override = Rz(delta) * game pelvis, delta keeps his facing (pelvis forward learned in its
  own frame from the shoulders) on the view direction (dead zone slider, 360 deg/s). Children follow.
- Arms rebuilt: learned once per skeleton (30 frames, overrides off, most-bent elbow frame): each bone's limb axis and
  elbow-hinge axis in its own frame, segment lengths, helper offsets. Each frame bones are set from (limb direction,
  hinge = pole x dir) with frame_quat - nothing left from the animation (no more pocket/aim pose fighting). Hands:
  (wand forward, wand right) + per-hand twist slider, or straight wrist. Eye point for the wand mapping = head + eye
  offset (no stereo jitter). Bones not written this frame are unmapped (no stale data). NaN guard.
- Menus: HUD probe reads the whole HUD layer every 6 frames; HUD covering >40% (exit <20%) = full-screen menu / map ->
  big screen; screen mode also on camera stall. In screen mode the split composites the WHOLE HUD back (no panel,
  no cut-outs) -> fixes the black blank spots. Big screen 3.0 m wide at 2.0 m (sliders).
- Phone / wheel: wider cut-outs (phone 0.40-0.94 x 0.04-0.97, wheel 0.40-0.97 x 0.10-0.82), bigger overlays
  (phone_size 0.36 m, wheel_size 0.40 m - new ini keys).
- Next: check the log for "Full-screen menu open" in the pause menu / map; head-steering signs learned; T-pose result
  (should pick mirror 0 now); hand twist values; profiler label tilt still open (needs a snapshot).

## 0.11.1
- Right pad / stick turn (smooth, or new snap turn option 15-90 deg) turns the view's straight-ahead; the steering and
  body turn make Aiden follow, so view and Aiden turn together. Phone 0.50 m, wheel 0.56 m (new ini keys phone_size2 /
  wheel_size2, sliders up to 1.2 m).

## 0.11.2
- 0.11.1 log: phone / wheel "open" most of the time (right half ~10% covered by ordinary HUD) -> steering released the
  stick and froze the view yaw, so the stick turned Aiden (minimap) but not the VR view. Fixes: open now also requires
  the minimap corner (x<0.30, y>0.55) to be empty (<1%; the game hides it while the phone / wheel is open), closes when
  it reappears (>3%); while open, the view yaw follows the game camera's yaw changes so stick turning stays in sync.
- Also seen: full-screen menu detection worked (58% cover -> big screen); T-pose now picks mirror 0, turn ~0 (mapping
  fix confirmed); HUD target alternates between two textures when the phone opens/closes.

## 0.11.3 - BUG: several copies of the mod were running
- 0.11.0-0.11.2 deploys also copied WDVR_0110/0111/0112.asi into plugins\ as backups. Ultimate ASI Loader loads EVERY
  .asi there -> up to 4 instances hooked Present / XInput / camera and fought each other (incoherent stick turning,
  steering signs flipping, "not converging" spam; wdvr.log came from whichever instance opened it last).
- Fix: those three files overwritten with an empty no-op DLL (user asked to delete them); asi_main now takes a named
  mutex (WDVR_single_instance) so only the first copy ever starts and writes the log. Backups stay out of plugins\.

## 0.12.0 - arms reach, clavicles, scope, camera zoom, straight phone
User (0.11.3): turning works. Arms feel stubby / wands further than the hands; hand-in-pocket animation still shows;
sniper scope shows nothing (scope is a UI overlay, gun hidden); hacked-camera zoom (LT/RT) missing; phone still warps
in perspective when moving the head; wheel OK.
- Arms: the T-pose scale is no longer applied (2nd calibration had scale 1.51 from a half T-pose); hands go to the real
  wand distance ("Reach" slider, 1.0) and the arm may stretch up to 1.25x to get there ("Arm stretch").
  Clavicles 5 / 47 now overridden too: held fixed relative to the chest (bone 4) as learned from the frame where the
  arm hangs most freely (least elbow bend) during a 1 s learning window -> no shrug / pocket pose at the arm root.
  Shoulder = chest + learned clavicle offset + learned shoulder offset. "Relearn Aiden's arm pose" button.
- FOV stub captures the game's wanted FOV again (StubData.game_fov @140, camera::game_fov()).
- Hacked cameras / cutscenes: view FOV follows the game's zoom (tan ratio vs the widest FOV seen since entering).
- Scope: aiming with game FOV < 35 deg = scoped. Frames go L, R, scope: the scope frame is rendered along the right
  wand with the game's FOV and its HUD (scope overlay) composited; the middle 90% square goes on overlay wdvr.scope on
  the right wand facing the eye (size / up / back sliders). Eye frames get no HUD while scoped; panel hidden.
- Phone: corners of the tilted 3D phone found in the HUD read-back (extremes of x+y, x-y; probe every 2 frames while
  open), drawn straight onto a flat 600 x (600*aspect) texture with a bilinear-quad shader -> left-hand panel
  (phone_size3 = 0.30 m = the phone itself). Falls back to the plain cut-out if the shader fails.
- vr.c: hand overlays share hand_overlay()/hand_device(); vr_scope_show/hide.

## 0.12.1
- User (0.12.0): scope partly works but aim is off from the scope picture, and the game's scope reticle moved with the
  head; phone still warps when looked at directly (fine when looking away).
- Scope frame now renders exactly along the game's own aim (camera::look_along_game_aim: game_yaw / game_pitch, roll 0,
  not remembered as the view direction) -> picture centre = where bullets go, game reticle centred.
- Phone: corners = convex hull of the per-row outline, best-area quad (refined), proper projective homography in the
  shader (was bilinear). Shape from "Phone shape" (height/width 1.7) instead of the tilted on-screen shape.
  New option (on): while the phone is open Aiden's arms use the game's phone pose - the game draws the phone UI from
  where his hand holds the phone, so the wand-driven hand made it tilt/stretch when looked at.

## 0.12.2
- 0.12.1 reports: snapshots while scoped show NO scope overlay in the HUD layer (eye frames, forced wide FOV); the phone
  (game holding it) is a small tilted card mid-screen; user wants the phone like the wheel and the arms tracking;
  other guns hit high / right of where the wand points.
- Phone: back to the plain cut-out on the left wand (like the wheel), arms always follow the wands. Unwarp code removed.
- Aim convergence: while aiming, the game camera is steered so its centre line (from the game camera position,
  over Aiden's shoulder) passes through the point aim_dist (20 m) along the wand's line from your hand (eye world
  position + wand offset). Removes the shoulder-camera parallax (high/right).
- Scope: while scoped EVERY frame is the game's own view (its camera position, aim and FOV; full HUD) -> the scope
  picture on the right wand is exactly the flat game's scope view, reticle included, bullets at its centre. The eyes
  are re-submitted their last images (SteamVR reprojects them) until the scope comes down.

## 0.13.0
- User (0.12.2): sniper scope flawless (but world paused); wants real-time world, sniper-like accuracy for all guns,
  phone simply docked on the left wrist (still warped), arms stretch / float in front, pocket animation still binds.
- Game-view frames: frames go L, R, G where G = the game's own camera (position, aim, FOV) while scoped (scope picture
  on the right wand) OR while the phone is open (the phone panel is copied only from G frames, where the game draws
  it as in the normal game - not squashed by the 138 deg view). Eyes are re-fed their last images on G frames.
  "Scope: world holds still" option = old every-frame scope.
- Phone: plain cut-out panel docked on the left wrist (wrist_tilt -65, wrist_up 0.03, wrist_back 0.12 - new keys).
- Calibration rewritten (arms_cal_v4 -> redo T-pose): turn about vertical + offset (shoulder middle - wand middle at
  T-pose) stored in Aiden's BODY frame (fwd, left, up) and applied around the current shoulder middle. The old offset
  was fixed in model space, so when his body turned in model space the hands were pushed forward / sideways ("float").
  Arm stretch default 1.10 (arm_stretch2).
- Pocket detector: rebuilds the animation's own hand position from the bone hook's raw values (raw child = our parent *
  animation local -> local = conj(our previous parent) * raw) and checks it against pocket points (pelvis +-0.20 m
  sideways, 5 cm down, learned in the pelvis frame). While a hand is "in the pocket" (+0.3 s) up to 40 hip-area bones
  (>= 250, not legs 269-272/314-317, within 0.35 m of the pocket points, learned from the most relaxed learning frame)
  are held at their relaxed pose relative to the (turned) pelvis. Slots 17-56. Log: "Pocket detector: ...".
- Aiming for other guns: unchanged (convergence at aim_dist). The sniper's precision comes from seeing the game's own
  view; other guns can only be exact at one distance without the hit distance (depth) - possible next step.

## 0.14.0 - permanent gun calibration (aim distance from depth)
- framecap: tracks the depth buffer bound for each SetRenderTargets and counts draws per depth buffer; the full-screen
  one with the most draws (stable 30 frames) = scene depth (framecap::scene_depth()). Log "Depth: scene depth buffer".
- While aiming (not scoped / phone): frames L R L R G - G is the game's own aim camera. Its depth is copied to a staging
  texture and read next frame (DO_NOT_WAIT): centre pixel = how far the game's aim line goes = where the bullet hits.
  Steering then aims the game camera through the point where the wand line passes that hit (fixed-point, smoothed) ->
  correct at every distance. Scoped G frames are measured too.
- Depth -> metres needs the near plane: learned once automatically from flat ground below the crosshair (3 rows,
  camera height above Aiden's feet, ray angle from game pitch + FOV; 90 samples; standard z=n/(1-d) vs reversed z=n/d
  chosen by lower spread, accepted if spread < 25%). Saved in WDVR.ini (depth_model, depth_near) - permanent.
  Menu: status + "Redo aim depth calibration". Fallback: aim_dist (20 m) until calibrated / no depth.
- Settings keys from here on are kept stable (no more forced re-calibration unless the meaning changes).

## 0.14.1 - frame labels were one frame off
- 0.14.0: while aiming the LEFT eye flickered with the third-person (game-camera) view, the scope window showed the
  headset view again, the phone panel flickered. All = the image at Present was drawn with the camera written TWO
  frames earlier, not one: L R L R G -> the G image got labelled as the next L. The same off-by-one is why "swap eyes"
  had to be on. Now frame_delay = 2 and swap_eyes off (new keys frame_delay2 / swap_eyes2); slider kept.
- 0.14.0 depth calibration (standard, near 0.2733, spread 6%) may have been measured on mislabelled frames -> relearned
  (keys depth_near2 / depth_model2). Scene depth = format 44 (R24G8_TYPELESS), 1920x1080.
- Phone panel 0.34 m (phone_size5). Pocket detector: hysteresis 0.20 in / 0.28 out, coat hold only after 150 ms
  steady, kept 600 ms (it flickered in/out every few frames while standing).

## 0.14.2 - review pass
- HUD composite timing: the composite runs while the game renders the frame that reaches the NEXT Present; with delay 2
  that frame was requested one Present ago. hud_update now picks the world area from type_presented_next() (ring entry
  pose_count+1-delay) instead of g_next_type - otherwise the scope overlay / "no HUD" setting landed on the wrong frame.
- Aim depth: the game camera (position, aim, FOV, pitch, ground) is recorded with each game-view frame request
  (g_shot_ring) and used for the depth copy of that exact frame (was: values at copy time, 2 frames newer). Ground
  samples taken right of centre (x 0.70) - Aiden stands left of centre in the over-the-shoulder aim view.
- First-person anchor: view_frame called set_eye_offset (which clears the anchor) before set_anchor every frame - a
  window where the game could place the camera at its own third-person spot. Removed (set_anchor alone).
- Phone panel has its own texture and is only shown once it has a picture from a game-view frame (fallback after 30
  frames: plain copy) - no more flashes of the wheel's or a squashed picture.
- Re-verified FOV stub (game_fov @140) by disassembly.

## 0.15.1 - smooth driving
- Problem: stutter / "fighting between two positions" while driving. Cause: alternate-eye 3D - each eye is a
  different moment (car moves ~30-40 cm per frame at speed) and each eye updates at half rate; frames arriving a
  frame early/late (delay jitter) made an eye jump between two positions.
- Fix: in a vehicle (not a hacked camera) every frame is mono (no eye offset) and goes to both eyes. Option
  "3D while driving" (car_3d, default off) brings the old behaviour back.
- Mono frames are also copied into both eye images so phone/edge frames resubmit a current picture.
- Edge guard now covers any change involving mono (-1) or game view (2), not just 2.
- Aiden speed (speed lag fix) measured with QueryPerformanceCounter (GetTickCount64 stepped by 16 ms -> noisy).
- Log line every 4 s while driving: speed, frame time range, frames where Aiden's position didn't update.

## 0.16.0 - frame ID, live phone, red dot
- User (0.15.1): phone finally on the wrist - wants the game live (not paused) while it's open; other guns still aim off;
  sniper scope works again; asked for a red dot showing where bullets go (sniper-like, without a preview window).
- Root of all the frame-order trouble: our camera write (in Present, render thread) races the game's update thread, so a
  frame is drawn 1 or 2 (sometimes 3) requests after we wrote it - it changed between sessions (0.14.0 vs 0.14.2).
- Frame ID (framecap): every request's FOV gets a tag (x (1 + 5e-5 * (slot+1)), invisible). Hooks on Map(14)/Unmap(15)/
  UpdateSubresource(48) scan constant buffers for the tagged projection numbers in 6 forms (1/tan, /aspect, *aspect,
  tan, ...). Discovery: the (buffer size, offset, form) that matches in >= 80 of 90 frames -> locked; then only that spot
  is read. At Present the last decoded request = the frame being shown -> exact eye / pose / game-view labels.
  Not found: 6 rounds (~9 s) then rests 60 s (scanning costs time); falls back to frame_delay guessing + edge guard.
  Lost (120 frames without a tag) -> search again. Log: "Frame ID: locked - ..." and every 20 s "Frames: drawn 1/2/3
  requests late ...". frame_delay is set to the measured mode (HUD composite timing).
- Phone live: with frame ID locked, every 5th request is a game-view frame (wrist panel source), the rest normal eye
  frames (eye alternation by a flip counter, balanced). Without frame ID: old frozen behaviour. HUD panel isn't updated
  from game-view frames.
- Red dot: the game's aim line (game camera pos + forward recorded per request in FrameCam) is marched through the
  eye-frame depth; hit = where bullets land. SteamVR overlay "wdvr.dot" placed there (world -> room = inverse of
  tracking_to_world from the current head), facing the head, constant angular size (red_dot_size, 0.6 deg). Shown while
  aiming, not scoped, depth calibrated. Log every second: wand distance, bullet distance, how far apart.
- Depth march ignores surfaces closer than 1 m (Aiden's own arms / gun); depth read every 2nd eye frame while aiming.
- Depth calibration reset again (keys depth_near3 / depth_model3): the saved near plane 1.87 m came from mislabelled
  0.14.2 frames (real values are ~0.1-0.3 m). Relearns in ~1-2 s standing on flat ground.

## 0.16.1 - FOV timing: the real cause of game-view frames leaking into the eyes
- 0.16.0 test: frame ID locked instantly (960-byte camera CB, 1/tan at +128 etc.), 100% of frames identified, all
  "1 request late". Depth calibration clean (standard, near 0.228, spread 0.03). Aim "works great". Phone: third person
  flickered into an eye (fishbowl), phone UI super zoomed; probe flapped phone open/closed every 0.3 s.
- F10 snapshot = the LEFT EYE image (monitor mode 1) and it shows the third-person camera with the wide headset FOV.
  An eye-labelled frame (by FOV tag) with game-view position => position/direction and FOV come from DIFFERENT
  requests: FOV arrives 1 request after it's written, position/direction 2 (as 0.14.1 deduced). Explains everything:
  0.14.0 third person in the left eye (labels by delay 1), 0.14.2 "scope super magnified takes over the right eye"
  (eye frame with the scope's FOV), 0.16.0 fishbowl third person in an eye + zoomed phone (eye position, game FOV).
  Also: 0.16.0 labelled eyes by the FOV request = one off => L/R were swapped in 0.16.0.
- Fix: each request's FOV is written one request late (fov_lag, default on); the tag goes with it, so the decoded
  request = the position/direction request (log should now say "2 requests late").
- Phone live: runs of 3 game-view requests every 8 (E E E E E G G G); only the MIDDLE game-view frame feeds the wrist
  panel and the HUD probe (the game's phone UI may read the camera a frame early/late too - the middle one is safe
  either way). While the phone is open: no HUD composited into the eye frames and the HUD panel is hidden (eye frames
  draw the phone for your head - huge/warped); the phone shows only on the wrist. No plain-copy fallback in live mode.
- Snapshot log line now says what the frame shown was (type, request, monitor mode).

## 0.16.2 - phone stays live, FOV blend, car view
- 0.16.1 test: FOV fix confirmed (frames now 100% "2 requests late" = position and FOV coherent). Only ONE WDVR.asi
  installed - the "two versions" were the two phone modes: live (frame ID locked) at first, then frame ID was lost
  (15:26:35, right after an aim following a weapon switch: 0 tag matches anywhere afterwards, aim distances ~200 m =
  rendered FOV no longer exactly ours) -> fallback = frozen headset while the phone is open.
- Cause guess: the FOV write site sets +24 (current) and +28 (target); we blocked only +24, so a game FOV blend pulled
  current toward the game's target (smears the tag, slightly zooms). Now both writes are blocked while we drive the FOV
  (we write both ourselves in write_look).
- Phone always live (no frozen fallback): without frame ID the order is guessed (2 late, measured) + edge rules.
  While the phone is open the eyes are flat (one image to both eyes, every frame refreshes both) and 3 of every 12
  frames are game view (middle one -> wrist panel): the headset pauses 3 frames (~33 ms) 7.5x a second instead of
  each eye pausing 5 frames (0.16.1: "world flickering really bad" with 3 of 8 in 3D).
- Vehicles: 3D by default again (key car_3d2). "View turns with the car" (car_frame, default on): base yaw = yaw of
  Aiden's body matrix x axis + offset (starts at the chase camera's yaw, settles toward it over ~4 s only while
  moving > 4 m/s). The chase camera swings on springs, lags in turns and shakes at speed - the view followed it.
- Driving log now measures how far behind Aiden's head the camera was placed (pos stub stores what it wrote:
  StubData.cam_written @144), in ms of travel - tells whether the speed lead (fp_lead 15 ms) is right.

## 0.16.3 - frame ID diagnosis, no speed lead, smoother pocket hold
- 0.16.2 test log (user's subjective report still pending at build time): phone opened on the left wrist, frame ID
  locked at once (960 B +128 etc.), 100% "2 requests late". Driving 3D up to 154 km/h, position never stale.
- Frame ID lost AGAIN after weapon-wheel switches: 46:08-46:25 wheel, two pause menus, 46:32.7 "Head steering: not
  converging" (game camera ignored the stick > 2 s), tag missing from ~46:33.3, aims 46:34-46:37 (wand vs bullets 91 m
  apart), "direction confirmed" 46:38.2, relocked 46:39.7. The steering failure and the tag loss cover the SAME ~5 s
  window -> the game camera was in some state of its own (FOV not ours AND not answering the stick). Same trigger as
  0.16.1 (weapon switch then aim). Relock first listed a 192-byte buffer (+156 tan/aspect) next to the 960 B ones.
- "Camera behind head" measured exactly 0.0 ms every time (351-360 samples per 4 s): the anchor writes the camera from
  Aiden's head at the very moment the game updates it - no lag to compensate. The speed lead (fp_lead 15 ms) therefore
  only pushed the eye ahead of the head (64 cm at 154 km/h) and added the noise of the Present-side speed estimate
  (frames 7-26 ms) as camera jitter. Now default 0 (new key fp_lead2; numpad * / and the slider still adjust it).
- Diagnosis log "Frame ID check: ..." once a second (max 40) while frames carry no tag: the FOV the game actually drew
  with (read at the spot the tag was locked at = probe), what we asked, the game's own FOV, how often the camera's FOV
  fields (+24/+28) were changed by something other than us since the last check (its FOV function is blocked, so >0
  means another code path), aim / phone / scope / steering error, camera struct addresses. "not converging" line now
  shows the error, game yaw, view yaw, aim.
- Frame ID relock: keys sorted by hits then buffer size (main 960 B camera buffer first).
- Pocket hold: fades in/out over 0.25 s (slerp between the animation's coat bones and the held pose) instead of
  switching, kept 1.5 s (was 0.6 s) after the hand was last seen in the pocket - the detector still toggles every few
  hundred ms while idle, which made the coat snap.

## 0.16.4 - rotation timing (phone view jumping up/down)
- 0.16.3 test: driving smooth (speed lead 0). Phone on the wrist works smoothly, BUT the whole view flickers up and
  down while the phone is open. No frame ID loss this session (no "Frame ID check" lines): frames 100% "2 late",
  0% unidentified. Aim: wand vs bullets mostly < 2 m apart at 2-10 m.
- Cause (deduced): write_look writes the ROTATION straight into the camera struct at Present - exactly like the FOV,
  which 0.16.1 measured to be used one request sooner than the position (our stub applies position during the game's
  update). fov_lag fixed the FOV but the rotation was never lagged, so each frame was drawn with the NEXT request's
  rotation. Harmless between eye frames (head moved one frame), but with the phone pattern E..E G G G the last eye
  frame before each game-view run was drawn looking where the game camera looks -> 1 bad frame in 12 = view jumping
  7.5x a second (pitch differs most = "up and down"). Same reason 0.16.1 "world flickering really bad" (3 of 8).
- Fix: rotation written one request late too (rot_lag, default on, menu "Timing fix: rotation one frame late").
  camera::set_request(idx) gives the request number; lag only when the previous write was the previous request.
  Menu also has the FOV timing toggle now.
- Measurement: framecap rot_check - when the frame ID decodes a frame in the 960 B camera buffer, the 9 values of each
  recent request's look matrix are searched in that buffer (|value| match within 2e-6, >= 5 of 9 and unique). The
  20 s "Frames:" line now ends with "rotation from request -1 / same / +1 / +2". With rot_lag on, "same" should be
  ~100%. If it says +1 the lag isn't taking; if -1, rotation was already in sync - turn rot_lag off.

## 0.16.5 - close car camera, red dot after headset off/on, phone frame budget
- 0.16.4 test: PHONE FIXED (no more up/down jumping) - rotation lag confirmed by measurement: "rotation from request
  ... same" 94-100% with rot_lag on. Phone feels a little laggy (lower frame rate) but works.
- User: "driving worse, view stuck inside Aiden", and after pressing a wand button by mistake the car turned invisible,
  every car after that too. Log + snapshots 2/3 ("Press B to hide in car", car body not drawn, only headlight beams):
  the button switched the GAME's car camera to its close / hood view (the game hides the car body in that view, a
  game setting that persists). That camera is only 1.9-2.3 m from Aiden, so the mod's vehicle detection (game camera
  > 4.8 m) said "on foot" -> on-foot eye offset, head steering and body turn on a seated Aiden = view inside him.
  Fix: also a vehicle when the game camera is 0.8-4.8 m away AND looks away from Aiden (angle > 110 deg to enter,
  > 90 to stay; on foot it's ~15 deg). The invisible car itself is the game's camera mode: switch it back with the
  game's change-camera button while driving.
- Red dot: after the headset display was turned off and on (16:12) the dot overlay handle was stale -> ShowOverlay
  error 11 logged EVERY frame, no dot. vr_shutdown now clears it; error 10/11 also drops the handle; log once per change.
- Phone: 3 game-view frames in every 16 (was 12; ini phone_cycle, 8-32) -> headset gets 81% of frames instead of 75%;
  the wrist picture updates ~6x a second.
- Frame ID loss seen twice more (16:15:24, 16:15:35, both right after the phone closed / weapon wheel, before an aim):
  the game drew with FOV 68.75 deg (then 107.9 = in between) while our struct FOV was untouched (0 changes), camera
  structs unchanged. So the renderer takes its FOV from somewhere else in those moments (another camera / a blend in
  the render setup), not from the struct we write. Probe now keeps reading while the search rests (was "?").

## 0.16.6 - game crosshair hidden in the headset
- User: the game's own HUD crosshair is stuck to the headset view (HUD is screen space, our eye frames put the screen
  centre in front of your face) and distracts from the mod's red dot.
- framecap: second exclusion box (hud_exclude_area2); the HUD composite cuts the world area by both boxes (up to 16
  scissor rects). mod: box around the screen centre, half-height reticle_box (0.06 of the screen height = 65 px,
  same pixel width), on by default (hide_reticle). Not applied in the scope picture (game-view frames) or on the big
  screen. Menu: "Hide the game's crosshair in the headset" + "Crosshair box size" (0.02-0.15) if a bigger reticle
  still shows. Untested at build time.

## 0.16.7 - driving: no game camera influence at any speed
- 0.16.6 test: menus fine, game crosshair hidden OK. Driving: "the game's live camera (speed changes the camera's
  momentum) conflicts - make the view as steady as walking at any speed."
- Log: three drops from vehicle to "on foot" mid-drive (16:32:42 / 16:32:54 / 16:33:08, game camera 0.2-0.3 m from
  Aiden at 109-150 deg) for 4-12 s each -> on-foot offset, head steering and body turn in the car.
  Fix: sticky vehicle mode - leave only when the game camera is back over the shoulder (1.2-3.5 m, < 35 deg) for 0.5 s.
- Car forward (base yaw) no longer settles toward the chase camera (which swings with speed and lags in turns). It is
  learned from the direction of travel (speed > 22 km/h, car turning < 4 deg/s, within 45 deg; fast the first time,
  3 s smoothing after), saved as car_axis (offset from Aiden's body x axis). The chase camera is only the starting
  guess before the first learn. Log: "Car view: car forward learned ...".
- Steady seat (steady_seat, default on, menu checkbox): in a vehicle the eye is held at a fixed point in the car's
  frame (Aiden's entity matrix): the head's place in that frame is learned in the first 2.5 s after getting in, then
  (seat point - animated head) is added to the anchor offset each frame, so the driver animation (leaning into turns,
  bracing, bumps) no longer moves the view. The anchor still reads the head at the game's own moment (timing exact);
  the correction is computed at Present from head and matrix read together (both slow-changing). Ignored if > 0.4 m.

## 0.16.8 - use the game's own first-person car camera
- 0.16.7 test: view in vehicles "still broken". Log: first drive started with the car forward taken from the chase
  camera (-45 / -61 deg) and was only corrected when the travel direction was learned (16:47:51, -89.7 deg, saved as
  car_axis) - so the first ~40 s of the first drive looked 30-45 deg off. The user then unticked "View turns with the
  car" (car_frame=0 saved) -> following the chase camera again. Key renamed car_frame2 (back on by default).
- User's idea: with VR off, the game has a first-person car camera (snapshot: windshield, dashboard, gloved hands on
  the wheel, "Press F to hide in car"). Use it instead of fighting the game.
- 0.16.8: when in a vehicle and the game camera is within 0.8 m of Aiden's head (leave at 1.2 m) = the game's
  first-person car camera -> offset mode: camera = the game's own camera position + eye separation (+ lean), no
  head anchor / seat offset / speed lead. Rotation still = learned car forward + your head. Option use_cockpit
  (default on), menu "Use the game's first-person car camera when it is selected" (shows active / not active).
  Log "Car: using the game's first-person car camera". Vehicle detection also accepts a game camera < 4.8 m in front
  of Aiden looking away from him (any distance, was > 0.8 m), so the cockpit camera counts as a vehicle.
  (The 0.16.4 "invisible car" was the HOOD camera, ~2 m in front, which hides the car; the cockpit camera is the other
  mode and draws the interior.) Untested: whether the game's cockpit position itself shakes with speed.

## 0.16.9 - first-person car camera selected automatically
- 0.16.8 test: "worked perfectly" - the game's first-person car camera + our 3D/head look is the driving solution.
  User wants it selected automatically (the game resets its car camera each launch).
- Car camera modes from the game camera vs Aiden's head: chase (> ~4 m), hood (< 4.8 m, in front, angle > 110),
  first person (< 0.8 m).
- Learning: input.cpp records the rising edge time of every controller button the game sees (XInputGetState hook);
  mod.cpp records keyboard key presses (GetAsyncKeyState at Present, mod keys excluded). When the car camera mode
  changes in a car (not caused by us, > 1.5 s after getting in), the most recent press within 0.7 s is the candidate;
  the same candidate twice -> saved as cam_button (1..16 = controller bit + 1, 1000 + vk = key). Log "Car camera:
  learned the change-camera button (...)".
- Auto: after getting in (1.5 s, mode stable 0.8 s) the mod presses that button (controller: injected into the
  XInput state for 150 ms; key: SendInput scan code, released after 120 ms), waits 1 s, repeats until first person,
  max 4 per car; stops for that car if you switch away from first person yourself. Option auto_cockpit (default on),
  menu shows the learned button + "Forget the change-camera button".

## 0.16.10 - change-camera button fixed to D-pad left
- 0.16.9 test: first learned correctly (17:13:23 "controller D-pad left"), then re-learned Y at 17:14:28: getting OUT
  of the car (Y) moves the game camera (0.08 -> 1.25 m) while still counted as in the car = a "camera change" right
  after a press. The auto-select then pressed Y = in/out of the car over and over.
- Learning removed. The button is D-pad left (left wand: click the left edge of the left trackpad), key cam_button2
  (default 3 = bit 2 + 1). Safety: if the car is left within 3 s of an automatic press, automatic selection switches
  itself off for the session (log line). Still max 4 presses per car, stops if the player leaves first person.

## 0.16.11 - slow motion no longer pulls the view back in the car
- 0.16.10 test: driving works well, auto first person works (log: "selected automatically (1-3 presses)").
  Slow motion (focus) in the car: "the camera slings back a bit from Aiden".
- Log: the game's first-person camera normally 0.08-0.21 m from Aiden's head; in focus moments it went to
  0.56-0.80 m and 1.24-1.93 m (17:25:08, 17:26:07-09, 17:28:11-13), the mod then flapped first-person on/off at the
  old 1.2 m limit (head anchor + chase offsets in between).
- Fix: first-person mode stays on until the game camera is > 2.5 m away. Its usual spot is learned in the car's
  own frame (Aiden's body matrix) in the first 1.5 s of each first-person stint (then refined slowly). When the game
  camera is > 0.15 m off that spot (back < 0.08 m), the view is held there: head anchor (exact timing) + (usual spot
  - head) computed at Present. Log "Car: game's first-person camera pulled away ... / back".
- Seen too: aiming from the car switches the game to a chase-type camera (5 m) -> mod leaves first person for that.

## 0.16.12 - pistol-grip aim angle
- User: aim should come out of the wand's underside, not the tip, like other VR shooters (comfortable wrist).
- wand_aim_dir(): aim line = wand forward (-Z) tilted down toward the trigger side (-Y) by aim_tilt (default 40 deg,
  menu "Gun grip angle", -70..70 in case the sign is wrong on real hardware). Used for the steering aim point, the
  red dot / depth march (FrameCam wand_d), the right hand's pointing direction (hands follow wands: hand forward
  tilted the same way, using the wand's +Y axis transformed like the others; T-pose calibration and the wrist grip
  offset still use the untilted axis), and the scope picture (square to the tilted line; up/back along it).

## 0.16.13 - grip check in the log
- The once-a-second "Aim:" line now adds: tip pitch, aim-line pitch, head pitch (degrees, up = +), and the vertical
  component of the wand's +Y axis ("trackpad facing up", should be positive with a normal grip = axis sign as
  assumed by wand_aim_dir). Read it after the user aims at something level: aim line ~ head pitch = angle right.

## 0.16.14 - smaller crosshair cut-out
- 0.16.13 test: a whole mission played, "very good". Grip check (28 aim log lines): trackpad facing up 0.76-0.88
  (always positive = wand axes as assumed), tip +25..+41 deg while the aim line was -14..+1 with the head at -3..-7:
  the 40 deg grip angle puts the aim line where the player looks. Keep 40.
- User: the crosshair cut-out removes too much of other HUD near the centre. reticle_box 0.06 -> 0.025 (half-height,
  27 px; box ~30 x 54 px... width scaled 0.5625), new key reticle_box2, slider minimum 0.01.

## Rollback to 0.16.14 (2026-10-14 21:2x)
- Another chat worked on the folder from the older 0.16.2 notes and installed its own "0.17.0" (its own 0.16.3-0.16.6
  line: recenter countdown, car detection by standing height, ...). The user asked to go back to the version they
  were happiest with = 0.16.14 (full mission "very good"). Reinstalled the exact 0.16.14 build (sha1 49cc728f...),
  these notes / handoff / build kit, and the settings (their ini with this line's keys put back: car_axis -89.69,
  car_eye_height -0.141, cockpit / auto first person / D-pad left, rot_lag, phone_cycle 16, aim_tilt 40,
  reticle_box2 0.025). The other chat's files are kept in E:\Watch_Dogs\WDVR_backup_other_chat\ (its .asi renamed
  .asi.bak, outside plugins\ so the loader ignores it).

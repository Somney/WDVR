# WDVR handoff: read this first

## The project
WDVR is a VR conversion mod for **Watch Dogs (2014)**, Ubisoft Connect version, installed at `E:\Watch_Dogs`.
- **Hardware:** HTC Vive with Vive wands, running SteamVR.
- **The mod:** one plugin, `E:\Watch_Dogs\bin\plugins\WDVR.asi`, loaded by Ultimate ASI Loader (`bin\dinput8.dll`).
- **The user has no coding experience.** Claude writes and builds everything, installs it on their PC through the linked-computer tools (`mcp__remote-devices__*`), and the user tests in VR and reports back. Explain things in plain words, not code.

## Files on the user's PC
- `E:\Watch_Dogs\WDVR_build_kit.zip` contains everything needed to rebuild: `src/`, `build.sh`, `deps/` (imgui, minhook, openvr headers), and the notes.
- `E:\Watch_Dogs\WDVR_NOTES.md` is the full history. Each version records what changed, what was measured, and why. Read the top summary and the last few versions.
- `E:\Watch_Dogs\bin\plugins\`:
  - `wdvr.log`: rewritten every game start. This is the main source of evidence.
  - `WDVR.ini`: settings, saved automatically.
  - `snapN_frame.bmp` / `snapN_hud.bmp`: F10 snapshots of the frame and the HUD layer. The frame is the left-eye image, because monitor mode is 1.
  - `openvr_api.dll`.

## Building (in a fresh cloud workspace)
1. Stage `WDVR_build_kit.zip` from the PC and unzip it into a work folder.
2. Download the llvm-mingw toolchain from GitHub (mstorsjo/llvm-mingw releases, the Linux x86_64 "ucrt" build). Unpack it to `tools/llvm-mingw` inside the work folder, or set `CXX`/`CC` to its `x86_64-w64-mingw32-clang++`/`-clang`. The last builds used clang 23.1.3.
3. Run `bash build.sh`. The output is `out/WDVR.asi`. Some warnings in `vr.c` (CreateOverlay) are normal.

## Installing (hard-won rules)
- **Never put a backup `.asi` in `plugins\`.** The loader loads every `.asi` there, and copies fight each other.
- **Always copy the build to a NEW file name in `/mnt/user-data/outputs/`** (e.g. `WDVR_0163.asi`), then call `device_commit_files` with `force`. Re-committing from the same staged path has written a stale old file twice.
- **Then verify:** delete the local staged copy, `device_stage_files` the installed `WDVR.asi`, and compare `sha1sum`.
- After each version, append a section to the notes and refresh the build kit zip on the PC.

## Current state: version 0.16.14 (restored after another chat's 0.17.0 was rolled back - see notes)
- 0.16.13 confirmed: a full mission played well; grip angle 40 deg verified from the log (axes as assumed).
- 0.16.14: crosshair cut-out smaller (reticle_box2 0.025).
- 0.16.10 confirmed: driving good, first person picked automatically (D-pad left).
- 0.16.11: slow motion pulls the game's first-person camera back; the view is now held at its usual spot.
- 0.16.10: auto-select of the first-person car camera presses D-pad left (fixed; learning picked Y = exit car).
- 0.16.8 CONFIRMED: driving with the game's first-person car camera works perfectly.
- 0.16.8: in a car with the game's first-person (cockpit) camera selected, the headset uses that camera's position.
- 0.16.7: driving view independent of the game's chase camera: sticky vehicle mode, car forward from the driving
  direction (saved car_axis), steady seat point. Check the log for "Car view" / "Steady seat" lines and on-foot drops.
- 0.16.6 confirmed: game crosshair hidden, menus fine.
- 0.16.6: the game's crosshair is cut out of the headset picture (menu: hide crosshair + box size).
- 0.16.4 confirmed: phone works on the wrist with no flicker (rotation lag measured correct, keep it).
- 0.16.5: vehicle detection also recognises the game's close/hood car camera (the user switched to it by accident:
  car invisible + view inside Aiden); red dot fixed after headset off/on; phone uses 3 of 16 frames.
- Open: frame ID loss = the renderer draws with an FOV not from our struct (68.75 deg seen) for a few seconds after
  phone/wheel + aim. Next idea: find which camera the renderer reads then (capture-site ring of all structs, or a
  hardware watch on the 960 B buffer's source).

Things that work and are confirmed by the user:
- Driving smoothness (0.16.3, speed lead 0).
- Phone on the wrist without flicker (0.16.4).
- Head steering and smooth turning with the right pad.
- The sniper scope picture on the right wand.
- The red dot (where bullets land) and gun aiming in general ("aiming works great").
- Depth calibration: standard model, near plane 0.228 m. The calibration learns itself within about a second of standing on flat ground and is saved in the ini.
- Big screen for menus and the map.
- Weapon wheel on the right hand.

## Key facts about the game (all measured; details in the notes)
1. **Frame timing.**
   - The camera that the mod writes at Present n is drawn into the frame shown at Present n+2. This is measured 100% via the frame ID.
   - The game picks up the **FOV one frame later** than position and rotation. So the mod writes each request's FOV one request late (`fov_lag`). Without that, game-view frames leak into the eyes (third person in an eye, zoomed views).
2. **Frame ID.**
   - Every request's FOV carries an invisible tag (×(1 + 5e-5·(slot+1))).
   - Hooks on the D3D11 `Map`/`Unmap`/`UpdateSubresource` calls find the tagged value in the game's 960-byte camera constant buffer (offsets +128, +148, +192, +212). Log: "Frame ID: locked".
   - The 0.16.1 test lost the lock after an aim, probably from a game FOV blend. 0.16.2 blocks the game's +28 "target FOV" write too, so watch for "Frame ID: lost" in the log.
3. **Game-view frames.**
   - Frames drawn from the game's own camera feed the scope picture (every frame while scoped) and the phone panel.
   - The phone uses 3 game-view frames in every 12, and only the middle one feeds the wrist panel. While the phone is open the eyes go flat (mono).
4. **Driving.**
   - In 0.16.2 the car view follows the yaw of Aiden's body matrix ("View turns with the car") instead of the game's chase camera.
   - The log line "Driving: ... camera behind head X ms" measures how far the camera trails Aiden's head. Use it to decide whether the speed lead `fp_lead` (default 0.015 s) is right.
   - Alternate-eye 3D judders somewhat at speed by nature. Flat mode is an option in the menu.
5. **Code map** (`src/`):
   - `mod.cpp`: all per-frame logic, settings and the menu.
   - `camera.cpp`: machine-code hooks on the game's camera, FOV, head and bone writes. The `StubData` layout is pinned by `static_assert`.
   - `framecap.cpp`: HUD split, scene depth and frame ID.
   - `vr.c`: SteamVR and its overlays.
   - `input.cpp`: wand and controller input.

## Open items, in priority order
1. **Read the 0.16.4 test log** (rotation line, phone flicker) **and any "Frame ID check" lines.**
   - "Frame ID check" lines: is the drawn FOV a fixed zoom of ours, the game's own FOV, or a blend? Were the FOV fields
     changed by another code path? Then fix the cause (block that path / re-write FOV in the position stub / tolerate a scale).
2. **If driving still stutters** with lead 0, the remaining cause is alternate-eye 3D itself (flat mode is in the menu).
3. **Pocket animation:** it still flickers in and out at times.
4. **Arms:** they still float or stretch at times.
5. **Older item:** profiler labels tilt on people (needs an F10 snapshot).

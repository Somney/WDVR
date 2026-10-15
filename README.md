# WDVR — Watch Dogs VR

A VR mod for **Watch Dogs (2014, PC)**. Play the whole game in first person in a SteamVR headset, with motion
controllers.

- **First-person 3D view:** you see through Aiden's eyes. Looking around turns Aiden, and his body and arms follow
  your head and the wands.
- **Aiming with the right wand:** a red dot shows exactly where bullets will land. Sniper scopes show their picture on
  the wand itself.
- **Driving:** uses the game's own first-person car camera (dashboard and hands on the wheel), made 3D. The mod
  switches the car to it automatically, and it stays steady at any speed and during slow motion.
- **Phone and weapon wheel:** the phone is on your left wrist and the weapon wheel on your right hand. Menus and the map
  open on a big floating screen.
- **On-screen display:** markers stay in the world and the rest goes on a floating panel. The game's own crosshair is
  hidden so it doesn't stick to your face.

> **[⬇ Download WDVR 0.16.14](download/WDVR-0.16.14.zip)**
>
> Status: playable (version 0.16.14, a full mission played start to finish). Tested only on an **HTC Vive with Vive
> wands**. Other headsets may work through SteamVR. Other controllers will need button mapping changes.

## Requirements

- Watch Dogs (2014) for PC, Ubisoft Connect version. The mod finds the game's code by searching for it, but it was
  only tested on that version (`Disrupt_b64.dll` build stamp 55DC1ECD).
- SteamVR, with your headset set up.
- HTC Vive wands (both).
- Windows 10 or 11, 64-bit.

## Install

1. Download **[WDVR-0.16.14.zip](download/WDVR-0.16.14.zip)** (on that page, click the download button at the top right of the file view).
2. Open your Watch Dogs folder (the one that contains `bin\Watch_Dogs.exe`). For example
   `C:\Program Files (x86)\Ubisoft\Ubisoft Game Launcher\games\Watch_Dogs` or `E:\Watch_Dogs`.
3. Extract the zip **into that folder**, so these files end up here:
   ```
   Watch_Dogs\bin\dinput8.dll              (Ultimate ASI Loader - loads the mod)
   Watch_Dogs\bin\plugins\WDVR.asi         (the mod)
   Watch_Dogs\bin\plugins\openvr_api.dll   (SteamVR library)
   ```
   If Windows asks, replace files. If you already use another `dinput8.dll` mod loader, keep yours. Most of them load
   `.asi` files from `bin\plugins` too.
4. Start SteamVR, then start the game.

**Uninstall:** delete `bin\plugins\WDVR.asi`, plus `bin\dinput8.dll` if nothing else uses it.

> Only put **one** `WDVR.asi` in `bin\plugins`. The loader loads every `.asi` file in that folder, so a renamed backup
> copy would run as well and the two copies fight.

## First run

1. Load your save. A small green "WDVR ... INSERT = menu" line in the corner of the screen means the mod is running.
2. Press **INSERT** on the keyboard to open the menu, then tick **"VR on (game in the headset)"**.
3. Put the headset on, look straight ahead and press **numpad 5** to recenter.
4. **Calibrate the arms once:** press **Ctrl+F11**, stand up straight and hold both wands straight out to the sides
   in a T-pose until the countdown finishes. The result is saved.
5. Walk around for a few seconds on flat ground. The mod learns the game's depth by itself (for the red dot) and
   saves it.
6. Get into a car. The mod switches the car to the game's first-person view by pressing D-pad left for you.

Everything you change is saved to `bin\plugins\WDVR.ini`. A log of what the mod saw is written to
`bin\plugins\wdvr.log` each time the game starts. It's useful if something goes wrong.

## Controls

**Wands (they act as an Xbox controller):**

| Wand | Game button |
|---|---|
| Left trackpad (touch) | Move (left stick) |
| Left trackpad click: centre / up / right / down / left | L3 / D-pad up / right / down / left |
| Left trigger | Aim (LT) |
| Left grip / menu button | LB / Back |
| Right trackpad (touch, left-right) | Turn (smooth turning) |
| Right trackpad click: centre / up / right / down / left | R3 / Y / B / A / X |
| Right trigger | Shoot (RT) |
| Right grip / menu button | RB / Start |

You aim with the right wand: the gun fires where the wand points (pistol grip angle, adjustable in the menu). Your
head steers where Aiden faces and walks.

**Keyboard:**

| Key | Action |
|---|---|
| INSERT | Mod menu (all settings) |
| Numpad 5 | Recenter the view |
| Numpad 8 / 2, 4 / 6, 9 / 3 | Move your eye position forward/back, left/right, up/down |
| Numpad 0 | Lean on/off (your real head movement moves the view) |
| Numpad + | Big screen on/off |
| Numpad − | Show the hacked camera's view |
| Ctrl+F11 | Arm calibration (T-pose) |
| F10 | Snapshot of the current frame (for bug reports) |

## Troubleshooting

- **Nothing in the headset:** make sure SteamVR is running before the game, then tick "VR on" in the INSERT menu. The
  menu's first lines show the SteamVR status.
- **Car invisible from inside:** the game's *hood* camera hides the car. Press D-pad left (left trackpad, left edge)
  while driving until you're in the first-person view with the dashboard.
- **Arms floating or stretched:** redo the T-pose (Ctrl+F11).
- **Aim feels too high or low:** INSERT menu → "Gun grip angle" (default 40°).
- **The game's crosshair peeks out:** INSERT menu → "Crosshair box size".
- **Something else:** the INSERT menu has a "Copy report" button, and `bin\plugins\wdvr.log` has the details. Attach
  both to an issue.

## Building from source

The mod is plain C/C++ (Direct3D 11, OpenVR, MinHook, Dear ImGui) built with the
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) cross-compiler. It builds on Linux, or on Windows with MSYS2.

```sh
# get llvm-mingw (the "ucrt" x86_64 build for your system) and unpack it to tools/llvm-mingw
bash build.sh            # -> out/WDVR.asi
```

Source layout:

- `src/mod.cpp`: per-frame logic, headset display, view, body and arms, HUD, menu, settings.
- `src/camera.cpp`: hooks on the game's camera, field of view, head and bones.
- `src/framecap.cpp`: HUD split, scene depth, and frame identification.
- `src/input.cpp`: wands to XInput.
- `src/vr.c`: SteamVR and overlays.

`docs/DEVELOPMENT_NOTES.md` is the full engineering history: every version, what was measured and why.

## Credits and licences

- WDVR: see [LICENSE](LICENSE).
- The camera layout, code locations and instruction bytes come from **Otis_Inf's** Watch_Dogs camera tools
  (InjectableGenericCameraSystem, BSD-2-Clause, © Frans Bouma).
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT), [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause),
  [OpenVR](https://github.com/ValveSoftware/openvr) (BSD-3-Clause),
  [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (MIT, shipped in the release zip).
  Their licence texts are in `deps/` and in the release zip's `licenses` folder.

Watch Dogs is a trademark of Ubisoft. This is an unofficial fan mod, not affiliated with or endorsed by Ubisoft or
Valve. You need your own copy of the game.

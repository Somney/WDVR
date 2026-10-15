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

> **[⬇ Download the latest version](https://github.com/Somney/WDVR/releases/latest)** (the `WDVR-….zip` file under
> "Assets")
>
> Status: playable. Version 0.16.14 was played through a full mission. Tested only on an **HTC Vive with Vive
> wands**. Other headsets may work through SteamVR, but other controllers would need their buttons mapped first
> (see [Known issues](#known-issues)).

## Requirements

- Watch Dogs (2014) for PC, Ubisoft Connect version. The mod finds the game's code by searching for it, but it was
  only tested on that version (`Disrupt_b64.dll` build stamp 55DC1ECD).
- SteamVR, with your headset set up.
- HTC Vive wands (both).
- Windows 10 or 11, 64-bit.

## Install

1. Download the zip from the **[latest release](https://github.com/Somney/WDVR/releases/latest)**. Click
   `WDVR-<version>.zip` under "Assets", not the "Source code" files.
2. Open your Watch Dogs folder (the one that contains `bin\Watch_Dogs.exe`). For example
   `C:\Program Files (x86)\Ubisoft\Ubisoft Game Launcher\games\Watch_Dogs` or `E:\Watch_Dogs`.
3. Extract the zip **into that folder**, so these files end up here:
   ```
   Watch_Dogs\bin\dinput8.dll              (Ultimate ASI Loader - loads the mod)
   Watch_Dogs\bin\plugins\WDVR.asi         (the mod)
   Watch_Dogs\bin\plugins\openvr_api.dll   (SteamVR library)
   ```
   The zip also has a `README.txt` and a `licenses` folder, which end up in the game folder. They're harmless.
   If you already have a `dinput8.dll` from another mod (an ASI loader), you can keep yours: just make sure it loads
   `.asi` files from `bin\plugins`.
4. Start SteamVR, then start the game.

**Uninstall:** delete `bin\plugins\WDVR.asi`, plus `bin\dinput8.dll` if nothing else uses it.

> Only put **one** `WDVR.asi` in `bin\plugins`. The loader loads every `.asi` file in that folder, so a renamed backup
> copy would run as well and the two copies fight.

## First run

1. Load your save. A small green "WDVR ... INSERT = menu" line in the corner of the screen means the mod is running.
2. Press **INSERT** on the keyboard to open the menu, then tick **"VR on (game in the headset)"**. Do this each
   time you start the game. Press INSERT again to close the menu.
3. Put the headset on, look straight ahead and press **numpad 5** to recenter.
4. **Calibrate the arms once:** press **Ctrl+F11**, stand up straight and hold both wands straight out to the sides
   in a T-pose until the countdown finishes. The result is saved.
5. Walk around for a few seconds on flat ground. The mod learns the game's depth by itself (for the red dot) and
   saves it.
6. Get into a car. The mod switches the car to the game's first-person view by pressing D-pad left for you. If you
   switch away from it yourself, it leaves that car alone.

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
- **Something else:** open an [issue](https://github.com/Somney/WDVR/issues). Include the text from the INSERT
  menu's "Copy report" button (it copies a status summary to the clipboard) and attach `bin\plugins\wdvr.log`.

## Known issues

- **Controllers:** only Vive wands are mapped. Index, Oculus Touch and other controllers would need their buttons
  added to `src/input.cpp`.
- **Weapon switching:** after switching weapons, the game sometimes takes over the camera zoom for a few seconds. Aim
  can be off during that time. It recovers by itself.
- **Phone:** the frame rate drops slightly while the phone is open, because some frames go to drawing the phone
  screen.
- **Hands in pockets:** Aiden's idle "hands in pockets" animation can still flicker now and then, and the arms can
  float or stretch at times.
- **3D judder:** the headset shows each eye's picture from a slightly different moment, so very fast motion can judder
  a little. "3D while driving" in the menu can be turned off for a flat but smoother car view.

## Building from source

*Players can skip this section. The download above is ready to use on Windows. This part is only for people who
want to change the code.*

The mod is plain C/C++ (Direct3D 11, OpenVR, MinHook, Dear ImGui). It's compiled into the Windows file `WDVR.asi`
with the [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) compiler. The released versions were compiled on a
Linux machine, and you can do the same on Windows using WSL.

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
  Their licence texts are in `deps/` (the libraries) and in the release zip's `licenses` folder.

Watch Dogs is a trademark of Ubisoft. This is an unofficial fan mod, not affiliated with or endorsed by Ubisoft or
Valve. You need your own copy of the game.

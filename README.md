# Dead Space 3 Raw Input Mouse Fix (ASI)

> [!WARNING]
> This patch has been pretty much entirely been generated using AI; 
> I won't and will never claim to have enough knowledge or expertise to do this 
> kind of reverse-engineering by myself; I've tested about 70% of the game and so far spotted 
> no major issues (on my machine ofc, if you have issues feel free to open an Issue)
> except for small sub-second mouse freeze issues when the game tries to force the camera
> in a specific direction, but over the playthrough it happened rarely, and only lasted
> about 1 second
> If you're interested and want more (human generated) info, I have a blog post [here](https://slop-blog.enkhayzomachines.net/posts/dead-space-3-raw-input-mouse-fix) :)

A runtime patch for the 32-bit PC version of *Dead Space 3*. It replaces the game's averaged,
accelerated, clamped mouse values with relative Raw Input counts and removes the additional
third-person camera orientation smoothing. Camera collision and normal character-turn smoothing
remain intact.

The patch is temporary: it changes the running process only and never modifies game assets or the
game executable on disk.

## Compatibility

This is the executable the patch was developed and tested against:

- Dead Space 3 `1.0.0.1`
- PE timestamp `0x511E9327`
- PE image size `0x012D6000`
- SHA-256 `BAA971A30B4D5B1F7BA132F29EE4DD8B51FC02371284C629D8D26EB850B62FEC`

The SHA-256 is an identification value, not a runtime allowlist. The ASI recognizes the timestamp
and image size above as the known retail build, but a different executable can also initialize if
its reconstructed runtime image preserves every dependency of the patch. Before changing memory,
the plugin verifies the x86 PE and section layout, extended signatures and ABI-sensitive operands
at both hook targets, the writable frame counter, the retail DirectInput resolver strings, all nine
known callers of the mouse-rate filter, and all three chase-camera callers of the orientation
helper.

This fallback is intentionally conservative. A different outer DRM wrapper or metadata can pass;
moved or changed gameplay code cannot. Any failed check leaves the process unchanged and records
the precise reason in `DS3RawMouse.log`.

## Install and use

1. Use the plugin-only package if you already have a compatible **32-bit** ASI loader. Otherwise,
   the convenience package includes the tested x86
   [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) as `dinput8.dll`.
   Never overwrite an existing proxy DLL without first checking which mods use it.
2. Put `dinput8.dll`, `dinput8.ini`, `DS3RawMouse.asi`, and `ds3_raw_mouse.ini` in the Dead Space 3
   game directory. If supplying your own loader, keep its existing proxy DLL and add the other three
   files. The included `dinput8.ini` makes Ultimate ASI Loader scan plugins immediately, which is
   required by Dead Space 3's protected startup.
3. Advanced users may instead put `DS3RawMouse.asi` and `ds3_raw_mouse.ini` together in a plugin
   directory supported by their loader, such as `scripts` or `plugins`; keep `dinput8.ini` beside
   `dinput8.dll`.
4. Adjust the four sensitivity values in `ds3_raw_mouse.ini` if desired.
5. Start the genuine installed game through the EA App. The loader loads the plugin automatically,
   and the plugin waits for the protected game code to be reconstructed before installing hooks.
6. If the fix does not initialize, check `DS3RawMouse.log` beside the plugin or, when that directory
   is not writable, under `%LOCALAPPDATA%\DS3RawMouseFix`.

Restart the game before switching plugin builds. Do not install both `DS3RawMouse.asi` and a legacy
`DS3RawMouse.dll`; duplicate loads are refused. To uninstall, close the game and remove the ASI,
INI, and optional log. Remove the ASI loader proxy only if you installed it solely for this mod and
no other mod uses it.

Security software may inspect or warn about ASI loaders and runtime-hooking plugins because their
intended job is to load code into the game process and modify its in-memory instructions.

## Configuration

Sensitivity values are radians per raw mouse count; higher values turn faster.

- `ThirdPersonSensitivityX/Y`: used when the right mouse button is not held.
- `AimSensitivityX/Y`: used while the right mouse button is held.
- `SensitivityMultiplier`: scales all four sensitivities; valid values are `0.10` through `5.00`.
- `RawInput`: enables the raw-delta replacement on load.
- `RemoveCameraSmoothing`: enables the third-person orientation bypass on load.
- `Hotkeys`: enables the runtime controls below.
- `Diagnostics`: writes five-second activity counters to `DS3RawMouse.log`.

Runtime controls (default keys; they act only while the game window has focus):

- F6 (`ReloadConfig`): reload the INI, including patch switches, sensitivities, and hotkeys.
- F7 (`ToggleCameraSmoothing`): toggle the third-person smoothing bypass.
- F8 (`ToggleRawInput`): toggle raw mouse replacement.
- F9/F10 (`SensitivityDown`/`SensitivityUp`): lower/raise `SensitivityMultiplier` by `0.05`,
  preserving the four sensitivity ratios. The hotkey adjustment is temporary; F6 or a game
  restart restores the value saved in the INI.

Each key can be changed in the `[Hotkeys]` section of `ds3_raw_mouse.ini`: `F1`-`F24`, `Insert`,
`Delete`, `Home`, `End`, `PageUp`, `PageDown`, `Pause`, `ScrollLock`, or `Numpad0`-`Numpad9`,
optionally prefixed by `Ctrl+`, `Shift+`, and/or `Alt+` (for example `Ctrl+F7`), or `None` to
disable it. A key fires only with exactly the modifiers given.

For example, twelve F9 presses from the default `1.0` produce a multiplier of `0.40`; set
`SensitivityMultiplier=0.40` to make that the startup value.

### DS3 Debug Menu

The fix works alongside DS3 Debug Menu 1.2 (Nexus Mods). Install both as their
packages describe; the menu's `winmm.dll` loader and this package's `dinput8.dll` loader can be
present together (each plugin is loaded once), or you can use either loader alone with both ASI
files in its `plugins` folder.

About 15 seconds after the game starts, the menu checks that the game code is unmodified and
otherwise locks its features ("RUNTIME TEXT DID NOT SETTLE | BINDINGS LOCKED"). Earlier versions
of this fix had already installed their two hooks by then. When the menu is loaded, the fix now
waits until that check has finished (about 20 seconds after launch, while the game is still at
its intro or title screen) and logs `DS3 Debug Menu probe finished ... installing hooks`.

The menu uses F1, F7 (cleanup), Shift+F7 (checkpoint reload), and F8 (AI/static). While the menu
is loaded, this fix ignores its own bindings on F1, F7, and F8, so the menu's keys no longer switch
the camera patches off. To keep the two toggles, bind them to free keys, for example:

```ini
[Hotkeys]
ToggleCameraSmoothing=F11
ToggleRawInput=F12
```

The aim profile follows physical right-mouse-button state, not the game's remappable aim action.
Rebinding aim to another button therefore does not switch sensitivity profiles automatically.

## Build from source

Requirements:

- Windows
- Visual Studio with the Desktop development with C++ workload and x86 build tools
- CMake 3.20 or newer

From a Developer PowerShell or command prompt:

```powershell
cmake -S . -B build -A Win32
cmake --build build --config Release
```

The three mod/configuration files needed for the tested Ultimate ASI Loader setup will be in
`build\Release`:

```text
DS3RawMouse.asi
ds3_raw_mouse.ini
dinput8.ini
```

The targets use the static MSVC runtime, so a separate Visual C++ redistributable should not be
required. The ASI intentionally uses x86 inline assembly and cannot be built as x64.

## Test status and limitations

Over 70% of the campaign has been played with the patch enabled without major issues.
Normal third-person play and aiming have been validated for slow movement, rapid sweeps, repeated
360-degree turns, independent X/Y sensitivities, firing, weapon switching, melee, stomp, camera
collision, focus changes, and character following. Zero-G traversal/combat and turret sequences
have also been tested successfully.

The full campaign and co-op have not been comprehensively validated. Zero-G, turret, vehicle, and
other special mouse consumers receive the third-person sensitivity profile unless RMB is held.
If a sequence behaves incorrectly, F7 and F8 can isolate the smoothing and raw-input patches;
include the level/sequence, reproduction steps, and `DS3RawMouse.log` in a bug report.

## How it works

The retail executable reconstructs protected game code after launch, so an on-disk executable
patch is not suitable. An external ASI loader loads `DS3RawMouse.asi` during startup. The plugin
validates the PE layout, waits up to two minutes for the extended target signatures to become
ready, verifies their callers and runtime data dependencies, then:

1. observes the game's existing Raw Input window without taking mouse messages away from it;
2. replaces the accepted gameplay mouse-rate output with frame-rate-independent raw counts; and
3. changes the chase-camera interpolation factors to apply its target orientation immediately.

The original mouse filter still performs the game's device/mapping checks, and every intercepted
window message is forwarded to the original game window procedure.

## Misc

for more game fixes take a look at https://slop-blog.enkhayzomachines.net/fixes :)

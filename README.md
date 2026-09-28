# Dead Space 3 Raw Input Mouse Fix

> [!WARNING]
> This patch has been pretty much entirely been generated using AI; 
> I won't and will never claim to have enough knowledge or expertise to do this 
> kind of reverse-engineering by myself; I've tested about 70% of the game and so far spotted 
> no major issues (on my machine ofc, if you have issues feel free to open an Issue)
> except for small sub-second mouse freeze issues when the game tries to force the camera
> in a specific direction, but over the playthrough it happened rarely, and only lasted
> about 1 second, and a larger issue with mouse movement accumulating while the player's camera
> is frozen, I will fix it eventually

A runtime patch for the 32-bit PC version of *Dead Space 3*. It replaces the game's averaged,
accelerated, clamped mouse values with relative Raw Input counts and removes the additional
third-person camera orientation smoothing. Camera collision and normal character-turn smoothing
remain intact.

The patch is temporary: it changes the running process only and never modifies game assets or the
game executable on disk.

## Compatibility

Only this executable is currently supported:

- Dead Space 3 `1.0.0.1`
- PE timestamp `0x511E9327`
- SHA-256 `BAA971A30B4D5B1F7BA132F29EE4DD8B51FC02371284C629D8D26EB850B62FEC`

The DLL verifies the executable architecture, timestamp, image size, and both patched instruction
sequences before installing its hooks. An unsupported build fails closed and is left unchanged;
the reason is written to `DS3RawMouse.log`.

## Install and use

1. Download or build `DS3RawMouse.dll` and `DS3RawMouseLoader.exe`.
2. Put both files and `ds3_raw_mouse.ini` together in any writable directory. They do not need to
   be in the game directory.
3. Adjust the four sensitivity values in `ds3_raw_mouse.ini` if desired.
4. Start the genuine installed game through the EA App and wait for the main menu or gameplay.
5. Run `DS3RawMouseLoader.exe` once. The console confirms whether the DLL was loaded.

Repeat step 5 after every game restart. Do not run the loader twice in the same game process, and
restart the game before switching DLL builds. To uninstall, close the game and delete the patch
files.

If the loader cannot open the game, run it at the same privilege level as the game. Security
software may inspect or warn about the loader because its intended job is to load a DLL into the
running game process.

## Configuration

Sensitivity values are radians per raw mouse count; higher values turn faster.

- `ThirdPersonSensitivityX/Y`: used when the right mouse button is not held.
- `AimSensitivityX/Y`: used while the right mouse button is held.
- `RawInput`: enables the raw-delta replacement on load.
- `RemoveCameraSmoothing`: enables the third-person orientation bypass on load.
- `Hotkeys`: enables the runtime controls below.
- `Diagnostics`: writes five-second activity counters to `DS3RawMouse.log`.

Runtime controls:

- F6: reload the INI, including patch switches and sensitivities.
- F7: toggle the third-person smoothing bypass.
- F8: toggle raw mouse replacement.
- F9/F10: lower/raise every sensitivity by 5%, preserving their ratios. This multiplier is not
  saved and resets when the game restarts.

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

The three files needed at runtime will be in `build\Release`:

```text
DS3RawMouse.dll
DS3RawMouseLoader.exe
ds3_raw_mouse.ini
```

The targets use the static MSVC runtime, so a separate Visual C++ redistributable should not be
required. The DLL intentionally uses x86 inline assembly and cannot be built as x64.

## Test status and limitations

Over 70% of the campaign has been played with the patch enabled without major issues.
Normal third-person play and aiming have been validated for slow movement, rapid sweeps, repeated
360-degree turns, independent X/Y sensitivities, firing, weapon switching, melee, stomp, camera
collision, focus changes, and character following.

The full campaign and co-op have not been comprehensively validated. Zero-G, turret, vehicle, and
other special mouse consumers receive the third-person sensitivity profile unless RMB is held.
If a sequence behaves incorrectly, F7 and F8 can isolate the smoothing and raw-input patches;
include the level/sequence, reproduction steps, and `DS3RawMouse.log` in a bug report.

## How it works

The retail executable reconstructs protected game code after launch, so an on-disk executable
patch is not suitable. The loader uses `LoadLibraryW` in the active game process. The DLL then:

1. observes the game's existing Raw Input window without taking mouse messages away from it;
2. replaces the accepted gameplay mouse-rate output with frame-rate-independent raw counts; and
3. changes the chase-camera interpolation factors to apply its target orientation immediately.

The original mouse filter still performs the game's device/mapping checks, and every intercepted
window message is forwarded to the original game window procedure.

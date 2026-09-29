# Changelog

## 1.1.0 - 2026-09-29

- Converted the runtime patch into `DS3RawMouse.asi` for external ASI loaders.
- Validated Ultimate ASI Loader 9.7.4 as an x86 `dinput8.dll` proxy for Dead Space 3.
- Added the required `dinput8.ini` immediate-scan configuration.
- Added protected-code readiness polling for early startup loading.
- Added duplicate-module protection and fail-closed initialization.
- Added `%LOCALAPPDATA%\DS3RawMouseFix` as a log fallback when the game directory is not writable.
- Added the persistent `SensitivityMultiplier` setting, reloadable with F6.
- Retained the standalone injector only as an opt-in development build; it is not packaged.

## 1.0.0 - 2026-09-28

- Initial standalone `DS3RawMouse.dll` and `DS3RawMouseLoader.exe` release.
- Added raw mouse-rate replacement and third-person camera-smoothing removal.
- Added separate third-person and aim sensitivities, hotkeys, and diagnostics.

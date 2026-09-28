#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <malloc.h>

namespace {

#ifndef DS3_RAW_MOUSE_PRODUCT_BUILD
#define DS3_RAW_MOUSE_PRODUCT_BUILD 1
#endif

#ifndef DS3_RAW_MOUSE_VERSION
#define DS3_RAW_MOUSE_VERSION "1.0.0"
#endif

#ifndef DS3_RAW_MOUSE_LOG_FILENAME
#define DS3_RAW_MOUSE_LOG_FILENAME "DS3RawMouse.log"
#endif

// Supported executable: Dead Space 3 1.0.0.1, decrypted image addresses rebased to RVAs.
constexpr std::uintptr_t kMouseRateFilterRva = 0x0000D820;
constexpr std::uintptr_t kChaseOrientationDampRva = 0x0033EF10;
constexpr std::uintptr_t kFrameStampRva = 0x00EA9A54;
constexpr std::uintptr_t kThirdPersonRateReturnRva = 0x0033F407;
constexpr std::uintptr_t kAimRateReturnRva = 0x0034E6A5;
constexpr DWORD kSupportedTimeDateStamp = 0x511E9327;
constexpr DWORD kSupportedSizeOfImage = 0x012D6000;

constexpr std::uint8_t kRateFilterPrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14};
constexpr std::uint8_t kOrientationDampPrologue[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08};

using MouseRateFilter = unsigned(__thiscall*)(void* self, float frameTime,
                                               float* horizontal, float* vertical);
using ChaseOrientationDamp = void(__cdecl*)(const void* source, float pitchFactor,
                                             float yawFactor, void* output);

struct WindowHook {
    HWND window = nullptr;
    WNDPROC original = nullptr;
};

HMODULE g_self = nullptr;
std::uintptr_t g_imageBase = 0;
MouseRateFilter g_originalFilter = nullptr;
ChaseOrientationDamp g_originalOrientationDamp = nullptr;

std::atomic<LONG> g_rawX{0};
std::atomic<LONG> g_rawY{0};
std::atomic<ULONG> g_rawPackets{0};
std::atomic<ULONG> g_filterCalls{0};
std::atomic<ULONG> g_rateReplacements{0};
std::atomic<ULONG> g_thirdPersonCalls{0};
std::atomic<ULONG> g_aimCalls{0};
std::atomic<ULONG> g_otherCalls{0};
std::atomic<LONG> g_thirdPersonXMicros{1100};
std::atomic<LONG> g_thirdPersonYMicros{1650};
std::atomic<LONG> g_aimXMicros{1100};
std::atomic<LONG> g_aimYMicros{1100};
std::atomic<LONG> g_sensitivityPermille{1000};
volatile LONG g_dampBypasses = 0;
volatile LONG g_rateEnabled = 0;
volatile LONG g_dampEnabled = 0;
volatile LONG g_aimHeld = 0;
volatile LONG g_hotkeysEnabled = 1;
volatile LONG g_diagnosticsEnabled = 1;

CRITICAL_SECTION g_frameLock{};
std::uint32_t g_cachedFrame = 0xffffffffu;
LONG g_cachedX = 0;
LONG g_cachedY = 0;

CRITICAL_SECTION g_windowLock{};
WindowHook g_windows[8]{};

void BuildSiblingPath(const char* filename, char (&path)[MAX_PATH]) {
    if (GetModuleFileNameA(g_self, path, MAX_PATH) == 0) {
        path[0] = '\0';
        return;
    }
    char* slash = strrchr(path, '\\');
    if (slash != nullptr) slash[1] = '\0';
    else path[0] = '\0';
    strcat_s(path, filename);
}

void Log(const char* format, ...) {
    char path[MAX_PATH]{};
    BuildSiblingPath(DS3_RAW_MOUSE_LOG_FILENAME, path);
    FILE* file = nullptr;
    if (path[0] == '\0' || fopen_s(&file, path, "a") != 0 || file == nullptr) return;
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::fprintf(file, "%02u:%02u:%02u.%03u ", time.wHour, time.wMinute,
                 time.wSecond, time.wMilliseconds);
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(file, format, arguments);
    va_end(arguments);
    std::fputc('\n', file);
    std::fclose(file);
}

bool ReadBoolean(const char* path, const char* key, bool fallback) {
    char text[32]{};
    GetPrivateProfileStringA("Patch", key, fallback ? "1" : "0", text,
                             static_cast<DWORD>(sizeof(text)), path);
    return _stricmp(text, "1") == 0 || _stricmp(text, "true") == 0 ||
           _stricmp(text, "yes") == 0 || _stricmp(text, "on") == 0;
}

bool ReadSensitivity(const char* path, const char* key, LONG& micros) {
    char text[64]{};
    GetPrivateProfileStringA("Mouse", key, "", text, static_cast<DWORD>(sizeof(text)), path);
    if (text[0] == '\0') return false;
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    if (end == text || *end != '\0' || value < 0.000000001f || value > 0.1f) return false;
    micros = static_cast<LONG>(value * 1000000.0f + 0.5f);
    return true;
}

void LoadConfig() {
    char path[MAX_PATH]{};
    BuildSiblingPath("ds3_raw_mouse.ini", path);
    LONG thirdX = g_thirdPersonXMicros.load(std::memory_order_relaxed);
    LONG thirdY = g_thirdPersonYMicros.load(std::memory_order_relaxed);
    LONG aimX = g_aimXMicros.load(std::memory_order_relaxed);
    LONG aimY = g_aimYMicros.load(std::memory_order_relaxed);
    const bool gotThirdX = ReadSensitivity(path, "ThirdPersonSensitivityX", thirdX);
    const bool gotThirdY = ReadSensitivity(path, "ThirdPersonSensitivityY", thirdY);
    const bool gotAimX = ReadSensitivity(path, "AimSensitivityX", aimX);
    const bool gotAimY = ReadSensitivity(path, "AimSensitivityY", aimY);
    if (gotThirdX) g_thirdPersonXMicros.store(thirdX, std::memory_order_relaxed);
    if (gotThirdY) g_thirdPersonYMicros.store(thirdY, std::memory_order_relaxed);
    if (gotAimX) g_aimXMicros.store(aimX, std::memory_order_relaxed);
    if (gotAimY) g_aimYMicros.store(aimY, std::memory_order_relaxed);
#if DS3_RAW_MOUSE_PRODUCT_BUILD
    InterlockedExchange(&g_rateEnabled, ReadBoolean(path, "RawInput", true) ? 1 : 0);
    InterlockedExchange(&g_dampEnabled,
                        ReadBoolean(path, "RemoveCameraSmoothing", true) ? 1 : 0);
    InterlockedExchange(&g_hotkeysEnabled, ReadBoolean(path, "Hotkeys", true) ? 1 : 0);
    InterlockedExchange(&g_diagnosticsEnabled, ReadBoolean(path, "Diagnostics", true) ? 1 : 0);
#endif
    const bool complete = gotThirdX && gotThirdY && gotAimX && gotAimY;
    Log("config %s: third X=%.6f Y=%.6f; aim X=%.6f Y=%.6f (%s)",
        complete ? "loaded" : "defaults/partial", thirdX / 1000000.0,
        thirdY / 1000000.0, aimX / 1000000.0, aimY / 1000000.0, path);
}

bool SupportedGameImage() {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_imageBase);
    if (dos == nullptr || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        Log("unsupported executable: invalid DOS header");
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(g_imageBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->FileHeader.TimeDateStamp != kSupportedTimeDateStamp ||
        nt->OptionalHeader.SizeOfImage != kSupportedSizeOfImage) {
        Log("unsupported executable: machine=%04x timestamp=%08lx image_size=%08lx",
            nt->FileHeader.Machine, nt->FileHeader.TimeDateStamp,
            nt->OptionalHeader.SizeOfImage);
        return false;
    }
    return true;
}

void LogEffectiveSensitivity(const char* source) {
    const double multiplier = g_sensitivityPermille.load(std::memory_order_relaxed) / 1000.0;
    const double thirdX = g_thirdPersonXMicros.load(std::memory_order_relaxed) /
                          1000000.0 * multiplier;
    const double thirdY = g_thirdPersonYMicros.load(std::memory_order_relaxed) /
                          1000000.0 * multiplier;
    const double aimX = g_aimXMicros.load(std::memory_order_relaxed) /
                        1000000.0 * multiplier;
    const double aimY = g_aimYMicros.load(std::memory_order_relaxed) /
                        1000000.0 * multiplier;
    Log("%s: multiplier=%.2f third X=%.6f Y=%.6f; aim X=%.6f Y=%.6f",
        source, multiplier, thirdX, thirdY, aimX, aimY);
}

bool GameHasForeground() {
    DWORD processId = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &processId);
    return processId == GetCurrentProcessId();
}

bool RateEnabled() {
    return InterlockedCompareExchange(&g_rateEnabled, 0, 0) != 0;
}

bool DampEnabled() {
    return InterlockedCompareExchange(&g_dampEnabled, 0, 0) != 0;
}

bool AimHeld() {
    return InterlockedCompareExchange(&g_aimHeld, 0, 0) != 0;
}

void ClearRawDelta() {
    g_rawX.store(0, std::memory_order_relaxed);
    g_rawY.store(0, std::memory_order_relaxed);
    EnterCriticalSection(&g_frameLock);
    g_cachedFrame = 0xffffffffu;
    g_cachedX = 0;
    g_cachedY = 0;
    LeaveCriticalSection(&g_frameLock);
}

void GetFrameRawDelta(LONG& x, LONG& y) {
    const auto frame = *reinterpret_cast<volatile std::uint32_t*>(g_imageBase + kFrameStampRva);
    EnterCriticalSection(&g_frameLock);
    if (frame != g_cachedFrame) {
        g_cachedFrame = frame;
        g_cachedX = g_rawX.exchange(0, std::memory_order_acq_rel);
        g_cachedY = g_rawY.exchange(0, std::memory_order_acq_rel);
    }
    x = g_cachedX;
    y = g_cachedY;
    LeaveCriticalSection(&g_frameLock);
}

unsigned __fastcall HookMouseRateFilter(void* self, void*, float frameTime,
                                        float* horizontal, float* vertical) {
    const unsigned accepted = g_originalFilter(self, frameTime, horizontal, vertical);
    g_filterCalls.fetch_add(1, std::memory_order_relaxed);
    if (!RateEnabled() || (accepted & 0xffu) == 0 ||
        horizontal == nullptr || vertical == nullptr || frameTime <= 0.000001f ||
        !GameHasForeground()) {
        return accepted;
    }

    LONG x = 0;
    LONG y = 0;
    GetFrameRawDelta(x, y);
    const std::uintptr_t returnRva = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_imageBase;
    const bool aimProfile = AimHeld();
    if (aimProfile) {
        g_aimCalls.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_thirdPersonCalls.fetch_add(1, std::memory_order_relaxed);
    }
    if (returnRva != kThirdPersonRateReturnRva && returnRva != kAimRateReturnRva) {
        g_otherCalls.fetch_add(1, std::memory_order_relaxed);
    }
    const float multiplier = static_cast<float>(
        g_sensitivityPermille.load(std::memory_order_relaxed)) / 1000.0f;
    const LONG xMicros = aimProfile ? g_aimXMicros.load(std::memory_order_relaxed)
                                    : g_thirdPersonXMicros.load(std::memory_order_relaxed);
    const LONG yMicros = aimProfile ? g_aimYMicros.load(std::memory_order_relaxed)
                                    : g_thirdPersonYMicros.load(std::memory_order_relaxed);
    const float sensitivityX = static_cast<float>(xMicros) / 1000000.0f * multiplier;
    const float sensitivityY = static_cast<float>(yMicros) / 1000000.0f * multiplier;
    // The rate consumer applies the game's configured inversion downstream. Raw Input's
    // native positive-right/positive-down signs are the correct input at this seam.
    *horizontal = static_cast<float>(x) * sensitivityX / frameTime;
    *vertical = static_cast<float>(y) * sensitivityY / frameTime;
    g_rateReplacements.fetch_add(1, std::memory_order_relaxed);
    return accepted;
}

// The target has four ordinary stack arguments plus live hidden values in EAX/ECX/EDI.
// A C++ wrapper would clobber those registers before entering the trampoline. This gate
// changes only stack arg2/arg3 and tail-jumps without touching any general register.
__declspec(naked) void HookChaseOrientationDamp() {
    __asm {
        cmp dword ptr [g_dampEnabled], 0
        je passthrough
        // A factor of one collapses the complete target-to-rendered orientation error.
        mov dword ptr [esp + 8], 03f800000h
        mov dword ptr [esp + 12], 03f800000h
        lock inc dword ptr [g_dampBypasses]
    passthrough:
        jmp dword ptr [g_originalOrientationDamp]
    }
}

WNDPROC FindOriginalWindowProc(HWND window) {
    WNDPROC original = nullptr;
    EnterCriticalSection(&g_windowLock);
    for (const auto& entry : g_windows) {
        if (entry.window == window) {
            original = entry.original;
            break;
        }
    }
    LeaveCriticalSection(&g_windowLock);
    return original;
}

LRESULT CALLBACK CaptureWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INPUT) {
        UINT size = 0;
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size,
                            sizeof(RAWINPUTHEADER)) == 0 && size >= sizeof(RAWINPUT)) {
            auto* buffer = static_cast<std::uint8_t*>(_alloca(size));
            UINT received = size;
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buffer, &received,
                                sizeof(RAWINPUTHEADER)) == received) {
                const auto* input = reinterpret_cast<const RAWINPUT*>(buffer);
                if (input->header.dwType == RIM_TYPEMOUSE &&
                    (input->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                    const USHORT buttons = input->data.mouse.usButtonFlags;
                    if ((buttons & RI_MOUSE_RIGHT_BUTTON_DOWN) != 0) {
                        InterlockedExchange(&g_aimHeld, 1);
                    }
                    if ((buttons & RI_MOUSE_RIGHT_BUTTON_UP) != 0) {
                        InterlockedExchange(&g_aimHeld, 0);
                    }
                    if (RateEnabled()) {
                        g_rawX.fetch_add(input->data.mouse.lLastX, std::memory_order_relaxed);
                        g_rawY.fetch_add(input->data.mouse.lLastY, std::memory_order_relaxed);
                    }
                    g_rawPackets.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }

    WNDPROC original = FindOriginalWindowProc(window);
    return original != nullptr ? CallWindowProcW(original, window, message, wParam, lParam)
                               : DefWindowProcW(window, message, wParam, lParam);
}

bool SubclassRawWindow(HWND window) {
    if (window == nullptr) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId()) return false;

    EnterCriticalSection(&g_windowLock);
    for (auto& entry : g_windows) {
        if (entry.window != window) continue;
        const auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC));
        if (current == CaptureWindowProc) {
            LeaveCriticalSection(&g_windowLock);
            return true;
        }
        SetLastError(ERROR_SUCCESS);
        const auto prior = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(CaptureWindowProc)));
        if (prior != nullptr || GetLastError() == ERROR_SUCCESS) {
            entry.original = prior;
            LeaveCriticalSection(&g_windowLock);
            Log("re-subclassed raw-input window hwnd=%p", window);
            return true;
        }
        LeaveCriticalSection(&g_windowLock);
        return false;
    }

    WindowHook* freeEntry = nullptr;
    for (auto& entry : g_windows) {
        if (entry.window == nullptr) {
            freeEntry = &entry;
            break;
        }
    }
    if (freeEntry == nullptr) {
        LeaveCriticalSection(&g_windowLock);
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    const auto prior = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(CaptureWindowProc)));
    if (prior == nullptr && GetLastError() != ERROR_SUCCESS) {
        LeaveCriticalSection(&g_windowLock);
        return false;
    }
    freeEntry->window = window;
    freeEntry->original = prior;
    LeaveCriticalSection(&g_windowLock);
    Log("subclassed raw-input window hwnd=%p original=%p", window, prior);
    return true;
}

bool FindRegisteredMouse(HWND& target) {
    UINT count = 0;
    if (GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) == static_cast<UINT>(-1) ||
        count == 0) {
        return false;
    }
    RAWINPUTDEVICE devices[16]{};
    count = (std::min)(count, static_cast<UINT>(_countof(devices)));
    const UINT result = GetRegisteredRawInputDevices(devices, &count, sizeof(RAWINPUTDEVICE));
    if (result == static_cast<UINT>(-1)) return false;
    for (UINT index = 0; index < result; ++index) {
        if (devices[index].usUsagePage == 0x01 && devices[index].usUsage == 0x02 &&
            devices[index].hwndTarget != nullptr) {
            target = devices[index].hwndTarget;
            return true;
        }
    }
    return false;
}

void EnsureRawCapture() {
    if (!GameHasForeground()) {
        InterlockedExchange(&g_aimHeld, 0);
    }
    HWND target = nullptr;
    if (FindRegisteredMouse(target)) {
        SubclassRawWindow(target);
    }
}

bool SuspendOtherThreads(bool suspend) {
    const DWORD ownProcess = GetCurrentProcessId();
    const DWORD ownThread = GetCurrentThreadId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != ownProcess || entry.th32ThreadID == ownThread) continue;
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
            if (thread == nullptr) continue;
            if (suspend) SuspendThread(thread);
            else ResumeThread(thread);
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return true;
}

template <typename Function>
bool BuildTrampoline(std::uint8_t* target, const std::uint8_t* expected, std::size_t patchLength,
                     Function& original, const char* name) {
    if (std::memcmp(target, expected, patchLength) != 0) {
        Log("hook failed: %s prologue mismatch", name);
        return false;
    }
    auto* trampoline = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, patchLength + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (trampoline == nullptr) {
        Log("hook failed: %s trampoline allocation error=%lu", name, GetLastError());
        return false;
    }
    std::memcpy(trampoline, target, patchLength);
    trampoline[patchLength] = 0xE9;
    *reinterpret_cast<std::int32_t*>(trampoline + patchLength + 1) =
        static_cast<std::int32_t>((target + patchLength) - (trampoline + patchLength + 5));
    original = reinterpret_cast<Function>(trampoline);
    return true;
}

bool WriteJump(std::uint8_t* target, std::size_t patchLength, const void* replacement,
               const char* name) {
    DWORD oldProtection = 0;
    if (!VirtualProtect(target, patchLength, PAGE_EXECUTE_READWRITE, &oldProtection)) {
        Log("hook failed: %s VirtualProtect error=%lu", name, GetLastError());
        return false;
    }
    target[0] = 0xE9;
    *reinterpret_cast<std::int32_t*>(target + 1) = static_cast<std::int32_t>(
        reinterpret_cast<const std::uint8_t*>(replacement) - (target + 5));
    for (std::size_t index = 5; index < patchLength; ++index) target[index] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), target, patchLength);
    DWORD ignored = 0;
    VirtualProtect(target, patchLength, oldProtection, &ignored);
    return true;
}

bool InstallHooks() {
    auto* filter = reinterpret_cast<std::uint8_t*>(g_imageBase + kMouseRateFilterRva);
    auto* damp = reinterpret_cast<std::uint8_t*>(g_imageBase + kChaseOrientationDampRva);
    if (!BuildTrampoline(filter, kRateFilterPrologue, sizeof(kRateFilterPrologue),
                         g_originalFilter, "mouse rate filter") ||
        !BuildTrampoline(damp, kOrientationDampPrologue, sizeof(kOrientationDampPrologue),
                         g_originalOrientationDamp, "chase orientation damp")) {
        return false;
    }

    SuspendOtherThreads(true);
    const bool filterInstalled = WriteJump(filter, sizeof(kRateFilterPrologue),
                                           reinterpret_cast<const void*>(&HookMouseRateFilter),
                                           "mouse rate filter");
    const bool dampInstalled = WriteJump(damp, sizeof(kOrientationDampPrologue),
                                         reinterpret_cast<const void*>(&HookChaseOrientationDamp),
                                         "chase orientation damp");
    SuspendOtherThreads(false);
    return filterInstalled && dampInstalled;
}

void PollHotkeys() {
    static bool f6WasDown = false;
    static bool f7WasDown = false;
    static bool f8WasDown = false;
    static bool f9WasDown = false;
    static bool f10WasDown = false;
    const bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    const bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    const bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    const bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (InterlockedCompareExchange(&g_hotkeysEnabled, 0, 0) == 0) return;
    if (f6 && !f6WasDown) {
        LoadConfig();
        LogEffectiveSensitivity("F6 reload");
    }
    if (f7 && !f7WasDown) {
        const bool enabled = !DampEnabled();
        InterlockedExchange(&g_dampEnabled, enabled ? 1 : 0);
        Log("F7: chase orientation bypass %s", enabled ? "enabled" : "disabled");
    }
    if (f8 && !f8WasDown) {
        const bool enabled = !RateEnabled();
        InterlockedExchange(&g_rateEnabled, enabled ? 1 : 0);
        ClearRawDelta();
        Log("F8: raw rate replacement %s", enabled ? "enabled" : "disabled");
    }
    if (f9 && !f9WasDown) {
        const LONG multiplier = (std::max)(100L, g_sensitivityPermille.load() - 50L);
        g_sensitivityPermille.store(multiplier);
        LogEffectiveSensitivity("F9");
    }
    if (f10 && !f10WasDown) {
        const LONG multiplier = (std::min)(5000L, g_sensitivityPermille.load() + 50L);
        g_sensitivityPermille.store(multiplier);
        LogEffectiveSensitivity("F10");
    }
    f6WasDown = f6;
    f7WasDown = f7;
    f8WasDown = f8;
    f9WasDown = f9;
    f10WasDown = f10;
}

DWORD WINAPI Initialize(LPVOID) {
    g_imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    InitializeCriticalSection(&g_frameLock);
    InitializeCriticalSection(&g_windowLock);
    Log("DS3 Raw Mouse Fix %s loading", DS3_RAW_MOUSE_VERSION);
    if (!SupportedGameImage()) return 1;
    if (!InstallHooks()) return 1;
    LoadConfig();
#if DS3_RAW_MOUSE_PRODUCT_BUILD
    Log("initialized: raw=%u smoothing_bypass=%u hotkeys=%u diagnostics=%u; "
        "F6 reload, F7/F8 toggle, F9/F10 multiplier",
        RateEnabled() ? 1u : 0u, DampEnabled() ? 1u : 0u,
        InterlockedCompareExchange(&g_hotkeysEnabled, 0, 0) != 0 ? 1u : 0u,
        InterlockedCompareExchange(&g_diagnosticsEnabled, 0, 0) != 0 ? 1u : 0u);
#else
    Log("initialized disabled: F6 reload config; F7 orientation bypass; F8 raw rate; F9/F10 multiplier");
#endif
    LogEffectiveSensitivity("initial sensitivity");

    ULONG priorPackets = 0;
    ULONG priorCalls = 0;
    ULONG priorReplacements = 0;
    ULONG priorBypasses = 0;
    ULONG priorThirdPersonCalls = 0;
    ULONG priorAimCalls = 0;
    ULONG priorOtherCalls = 0;
    DWORD lastCaptureCheck = 0;
    DWORD lastReport = GetTickCount();
    for (;;) {
        PollHotkeys();
        const DWORD now = GetTickCount();
        if (now - lastCaptureCheck >= 100) {
            EnsureRawCapture();
            lastCaptureCheck = now;
        }
        if (now - lastReport >= 5000 &&
            InterlockedCompareExchange(&g_diagnosticsEnabled, 0, 0) != 0) {
            const ULONG packets = g_rawPackets.load();
            const ULONG calls = g_filterCalls.load();
            const ULONG replacements = g_rateReplacements.load();
            const ULONG thirdPersonCalls = g_thirdPersonCalls.load();
            const ULONG aimCalls = g_aimCalls.load();
            const ULONG otherCalls = g_otherCalls.load();
            const ULONG bypasses = static_cast<ULONG>(
                InterlockedCompareExchange(&g_dampBypasses, 0, 0));
            Log("5s stats: raw=%lu filter=%lu replaced=%lu profiles(third/aim)=%lu/%lu "
                "other_callers=%lu damp_bypass=%lu rate=%u damp=%u aim_held=%u",
                packets - priorPackets, calls - priorCalls, replacements - priorReplacements,
                thirdPersonCalls - priorThirdPersonCalls, aimCalls - priorAimCalls,
                otherCalls - priorOtherCalls, bypasses - priorBypasses,
                RateEnabled() ? 1u : 0u, DampEnabled() ? 1u : 0u, AimHeld() ? 1u : 0u);
            priorPackets = packets;
            priorCalls = calls;
            priorReplacements = replacements;
            priorThirdPersonCalls = thirdPersonCalls;
            priorAimCalls = aimCalls;
            priorOtherCalls = otherCalls;
            priorBypasses = bypasses;
            lastReport = now;
        }
        Sleep(2);
    }
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr, 0, Initialize, nullptr, 0, nullptr);
        if (thread != nullptr) CloseHandle(thread);
    }
    return TRUE;
}

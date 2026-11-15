#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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

// Verified runtime layout from Dead Space 3 1.0.0.1. Other executable wrappers are
// accepted only when the reconstructed game image retains this complete layout.
constexpr std::uintptr_t kMouseRateFilterRva = 0x0000D820;
constexpr std::uintptr_t kChaseOrientationDampRva = 0x0033EF10;
constexpr std::uintptr_t kFrameStampRva = 0x00EA9A54;
constexpr std::uintptr_t kInputManagerRva = 0x00DD1914;
constexpr std::uintptr_t kDirectInputFactoryNameRva = 0x00AA39FC;
constexpr std::uintptr_t kDirectInputModuleNameRva = 0x00AA3A10;
constexpr std::uintptr_t kThirdPersonRateReturnRva = 0x0033F407;
constexpr std::uintptr_t kAimRateReturnRva = 0x0034E6A5;
constexpr DWORD kSupportedTimeDateStamp = 0x511E9327;
constexpr DWORD kSupportedSizeOfImage = 0x012D6000;
constexpr DWORD kInitializationTimeoutMs = 120000;
constexpr DWORD kInitializationPollMs = 50;
// Raw counts older than this were collected while no camera consumer was running
// (scripted transitions, menus, cutscenes) and must not be replayed afterwards.
constexpr DWORD kStaleRawDeltaMs = 250;
constexpr std::size_t kMaximumSections = 32;

constexpr std::uint8_t kRateFilterPrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14};
constexpr std::uint8_t kOrientationDampPrologue[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08};
constexpr std::uint8_t kRateFilterSignaturePrefix[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0xD9, 0xEE, 0x53, 0x56, 0x8B, 0xF1,
    0x8B, 0x0D};
constexpr std::uint8_t kRateFilterSignatureSuffix[] = {
    0x8A, 0x81, 0x74, 0x05, 0x00, 0x00, 0x33, 0xDB, 0x88, 0x45, 0xFC, 0x3C,
    0x10, 0x73, 0x23};
constexpr std::uint8_t kOrientationDampSignature[] = {
    0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4,
    0x04, 0x55, 0x8B, 0x6B, 0x04, 0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC,
    0xD9, 0x43, 0x0C, 0x81, 0xEC, 0x1C, 0x02, 0x00, 0x00, 0xD9};
constexpr char kDirectInputFactoryName[] = "DirectInput8Create";
constexpr char kDirectInputModuleName[] = "DINPUT8.dll";

constexpr std::uintptr_t kRateFilterCallRvas[] = {
    0x00149F29, 0x0014F801, 0x00190720, 0x0024788D, 0x002788A9,
    0x00279327, 0x0027A4B5, 0x0033F402, 0x0034E6A0};
constexpr std::uintptr_t kOrientationDampCallRvas[] = {
    0x0034DA6D, 0x0034DB89, 0x0034DC60};

using MouseRateFilter = unsigned(__thiscall*)(void* self, float frameTime,
                                               float* horizontal, float* vertical);
using ChaseOrientationDamp = void(__cdecl*)(const void* source, float pitchFactor,
                                             float yawFactor, void* output);

struct WindowHook {
    HWND window = nullptr;
    WNDPROC original = nullptr;
};

struct GameImageLayout {
    DWORD timeDateStamp = 0;
    DWORD sizeOfImage = 0;
    WORD sectionCount = 0;
    IMAGE_SECTION_HEADER sections[kMaximumSections]{};
};

HMODULE g_self = nullptr;
HANDLE g_instanceMutex = nullptr;
std::uintptr_t g_imageBase = 0;
DWORD g_imageSize = 0;
MouseRateFilter g_originalFilter = nullptr;
ChaseOrientationDamp g_originalOrientationDamp = nullptr;
volatile LONG g_initializationStarted = 0;

std::atomic<LONG> g_rawX{0};
std::atomic<LONG> g_rawY{0};
std::atomic<ULONG> g_rawPackets{0};
std::atomic<ULONG> g_filterCalls{0};
std::atomic<ULONG> g_rateReplacements{0};
std::atomic<ULONG> g_thirdPersonCalls{0};
std::atomic<ULONG> g_aimCalls{0};
std::atomic<ULONG> g_otherCalls{0};
std::atomic<ULONG> g_staleDrops{0};
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
DWORD g_lastDrainTick = 0;

CRITICAL_SECTION g_windowLock{};
WindowHook g_windows[8]{};

// Hotkeys are read and polled only by the plugin's own worker thread.
constexpr UINT kModifierCtrl = 1;
constexpr UINT kModifierShift = 2;
constexpr UINT kModifierAlt = 4;

struct Hotkey {
    UINT virtualKey = 0;  // 0: unbound
    UINT modifiers = 0;
};

enum HotkeyAction : std::size_t {
    kHotkeyReload,
    kHotkeyToggleSmoothing,
    kHotkeyToggleRaw,
    kHotkeySensitivityDown,
    kHotkeySensitivityUp,
    kHotkeyCount
};

struct HotkeyBinding {
    const char* iniKey;
    const char* description;
    Hotkey defaultKey;
    Hotkey key;
    bool wasDown;
    bool yieldLogged;
};

HotkeyBinding g_hotkeys[kHotkeyCount] = {
    {"ReloadConfig", "reload INI", {VK_F6, 0}, {VK_F6, 0}, false, false},
    {"ToggleCameraSmoothing", "toggle smoothing bypass", {VK_F7, 0}, {VK_F7, 0}, false, false},
    {"ToggleRawInput", "toggle raw input", {VK_F8, 0}, {VK_F8, 0}, false, false},
    {"SensitivityDown", "multiplier -0.05", {VK_F9, 0}, {VK_F9, 0}, false, false},
    {"SensitivityUp", "multiplier +0.05", {VK_F10, 0}, {VK_F10, 0}, false, false},
};

// DS3 Debug Menu (Nexus Mods, "DS3 Debug Menu" 1.2) polls F1 (menu), F7/Shift+F7
// (cleanup/reload) and F8 (AI/static) globally. While it is loaded, bindings on those
// keys yield to it so one key press never drives both mods.
constexpr UINT kDebugMenuKeys[] = {VK_F1, VK_F7, VK_F8};
constexpr char kDebugMenuExport[] = "DS3DebugMenu_GetVersion";
bool g_debugMenuLoaded = false;

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

bool OpenLogFile(FILE*& file) {
    char path[MAX_PATH]{};
    BuildSiblingPath(DS3_RAW_MOUSE_LOG_FILENAME, path);
    if (path[0] != '\0' && fopen_s(&file, path, "a") == 0 && file != nullptr) {
        return true;
    }

    char directory[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableA("LOCALAPPDATA", directory,
                                                 static_cast<DWORD>(sizeof(directory)));
    if (length == 0 || length >= sizeof(directory)) return false;
    if (directory[length - 1] != '\\') strcat_s(directory, "\\");
    strcat_s(directory, "DS3RawMouseFix");
    if (!CreateDirectoryA(directory, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    strcpy_s(path, directory);
    strcat_s(path, "\\");
    strcat_s(path, DS3_RAW_MOUSE_LOG_FILENAME);
    return fopen_s(&file, path, "a") == 0 && file != nullptr;
}

void Log(const char* format, ...) {
    FILE* file = nullptr;
    if (!OpenLogFile(file)) return;
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
    if (end == text || *end != '\0' || !std::isfinite(value) ||
        value < 0.000000001f || value > 0.1f) {
        return false;
    }
    micros = static_cast<LONG>(value * 1000000.0f + 0.5f);
    return true;
}

bool ReadMultiplier(const char* path, const char* key, LONG& permille) {
    char text[64]{};
    GetPrivateProfileStringA("Mouse", key, "", text, static_cast<DWORD>(sizeof(text)), path);
    if (text[0] == '\0') return false;
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(value) ||
        value < 0.1f || value > 5.0f) {
        return false;
    }
    permille = static_cast<LONG>(value * 1000.0f + 0.5f);
    return true;
}

struct NamedKey {
    const char* name;
    UINT virtualKey;
};

constexpr NamedKey kNamedKeys[] = {
    {"Insert", VK_INSERT}, {"Delete", VK_DELETE}, {"Home", VK_HOME}, {"End", VK_END},
    {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT}, {"Pause", VK_PAUSE},
    {"ScrollLock", VK_SCROLL},
};

void TrimInPlace(char*& begin, char*& end) {
    while (begin < end && (*begin == ' ' || *begin == '\t')) ++begin;
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) --end;
}

bool TokenEquals(const char* begin, const char* end, const char* expected) {
    const std::size_t length = static_cast<std::size_t>(end - begin);
    return std::strlen(expected) == length && _strnicmp(begin, expected, length) == 0;
}

UINT ParseKeyToken(const char* begin, const char* end) {
    if (end - begin >= 2 && (*begin == 'F' || *begin == 'f')) {
        int number = 0;
        for (const char* digit = begin + 1; digit < end; ++digit) {
            if (*digit < '0' || *digit > '9' || number > 24) return 0;
            number = number * 10 + (*digit - '0');
        }
        return number >= 1 && number <= 24 ? static_cast<UINT>(VK_F1 + number - 1) : 0;
    }
    if (end - begin == 7 && _strnicmp(begin, "Numpad", 6) == 0 && begin[6] >= '0' &&
        begin[6] <= '9') {
        return static_cast<UINT>(VK_NUMPAD0 + (begin[6] - '0'));
    }
    for (const auto& named : kNamedKeys) {
        if (TokenEquals(begin, end, named.name)) return named.virtualKey;
    }
    return 0;
}

// "F7", "Ctrl+Shift+F7", "None" (or empty) for unbound. Returns false when invalid.
bool ParseHotkey(const char* text, Hotkey& result) {
    char buffer[64]{};
    strncpy_s(buffer, text, _TRUNCATE);
    char* begin = buffer;
    char* end = buffer + std::strlen(buffer);
    TrimInPlace(begin, end);
    if (begin == end || TokenEquals(begin, end, "None")) {
        result = {};
        return true;
    }

    Hotkey parsed{};
    for (;;) {
        char* plus = static_cast<char*>(std::memchr(begin, '+', static_cast<std::size_t>(end - begin)));
        char* tokenBegin = begin;
        char* tokenEnd = plus != nullptr ? plus : end;
        TrimInPlace(tokenBegin, tokenEnd);
        if (plus == nullptr) {
            parsed.virtualKey = ParseKeyToken(tokenBegin, tokenEnd);
            if (parsed.virtualKey == 0) return false;
            result = parsed;
            return true;
        }
        if (TokenEquals(tokenBegin, tokenEnd, "Ctrl") || TokenEquals(tokenBegin, tokenEnd, "Control")) {
            parsed.modifiers |= kModifierCtrl;
        } else if (TokenEquals(tokenBegin, tokenEnd, "Shift")) {
            parsed.modifiers |= kModifierShift;
        } else if (TokenEquals(tokenBegin, tokenEnd, "Alt")) {
            parsed.modifiers |= kModifierAlt;
        } else {
            return false;
        }
        begin = plus + 1;
    }
}

void FormatHotkey(const Hotkey& key, char (&text)[48]) {
    text[0] = '\0';
    if (key.virtualKey == 0) {
        strcpy_s(text, "None");
        return;
    }
    if ((key.modifiers & kModifierCtrl) != 0) strcat_s(text, "Ctrl+");
    if ((key.modifiers & kModifierShift) != 0) strcat_s(text, "Shift+");
    if ((key.modifiers & kModifierAlt) != 0) strcat_s(text, "Alt+");
    char name[16]{};
    if (key.virtualKey >= VK_F1 && key.virtualKey <= VK_F24) {
        sprintf_s(name, "F%u", key.virtualKey - VK_F1 + 1);
    } else if (key.virtualKey >= VK_NUMPAD0 && key.virtualKey <= VK_NUMPAD9) {
        sprintf_s(name, "Numpad%u", key.virtualKey - VK_NUMPAD0);
    } else {
        for (const auto& named : kNamedKeys) {
            if (named.virtualKey == key.virtualKey) strcpy_s(name, named.name);
        }
    }
    strcat_s(text, name);
}

void LoadHotkeys(const char* path) {
    for (auto& binding : g_hotkeys) {
        char text[64]{};
        constexpr char kMissing[] = "\x01";
        GetPrivateProfileStringA("Hotkeys", binding.iniKey, kMissing, text,
                                 static_cast<DWORD>(sizeof(text)), path);
        Hotkey key = binding.defaultKey;
        if (std::strcmp(text, kMissing) != 0 && !ParseHotkey(text, key)) {
            Log("hotkey %s=\"%s\" is not a valid key; keeping the default", binding.iniKey, text);
            key = binding.defaultKey;
        }
        if (key.virtualKey != binding.key.virtualKey || key.modifiers != binding.key.modifiers) {
            binding.wasDown = true;  // a key held while reloading must be released first
            binding.yieldLogged = false;
        }
        binding.key = key;
    }
    char summary[320]{};
    for (const auto& binding : g_hotkeys) {
        char name[48]{};
        FormatHotkey(binding.key, name);
        char part[96]{};
        sprintf_s(part, "%s%s=%s", summary[0] != '\0' ? ", " : "", binding.iniKey, name);
        strcat_s(summary, part);
    }
    Log("hotkeys: %s", summary);
}

void LoadConfig() {
    char path[MAX_PATH]{};
    BuildSiblingPath("ds3_raw_mouse.ini", path);
    LoadHotkeys(path);
    LONG thirdX = g_thirdPersonXMicros.load(std::memory_order_relaxed);
    LONG thirdY = g_thirdPersonYMicros.load(std::memory_order_relaxed);
    LONG aimX = g_aimXMicros.load(std::memory_order_relaxed);
    LONG aimY = g_aimYMicros.load(std::memory_order_relaxed);
    LONG multiplier = g_sensitivityPermille.load(std::memory_order_relaxed);
    const bool gotThirdX = ReadSensitivity(path, "ThirdPersonSensitivityX", thirdX);
    const bool gotThirdY = ReadSensitivity(path, "ThirdPersonSensitivityY", thirdY);
    const bool gotAimX = ReadSensitivity(path, "AimSensitivityX", aimX);
    const bool gotAimY = ReadSensitivity(path, "AimSensitivityY", aimY);
    const bool gotMultiplier = ReadMultiplier(path, "SensitivityMultiplier", multiplier);
    if (gotThirdX) g_thirdPersonXMicros.store(thirdX, std::memory_order_relaxed);
    if (gotThirdY) g_thirdPersonYMicros.store(thirdY, std::memory_order_relaxed);
    if (gotAimX) g_aimXMicros.store(aimX, std::memory_order_relaxed);
    if (gotAimY) g_aimYMicros.store(aimY, std::memory_order_relaxed);
    if (gotMultiplier) g_sensitivityPermille.store(multiplier, std::memory_order_relaxed);
#if DS3_RAW_MOUSE_PRODUCT_BUILD
    InterlockedExchange(&g_rateEnabled, ReadBoolean(path, "RawInput", true) ? 1 : 0);
    InterlockedExchange(&g_dampEnabled,
                        ReadBoolean(path, "RemoveCameraSmoothing", true) ? 1 : 0);
    InterlockedExchange(&g_hotkeysEnabled, ReadBoolean(path, "Hotkeys", true) ? 1 : 0);
    InterlockedExchange(&g_diagnosticsEnabled, ReadBoolean(path, "Diagnostics", true) ? 1 : 0);
#endif
    const bool complete = gotThirdX && gotThirdY && gotAimX && gotAimY && gotMultiplier;
    Log("config %s: multiplier=%.2f; third X=%.6f Y=%.6f; aim X=%.6f Y=%.6f (%s)",
        complete ? "loaded" : "defaults/partial", multiplier / 1000.0,
        thirdX / 1000000.0, thirdY / 1000000.0, aimX / 1000000.0,
        aimY / 1000000.0, path);
}

bool ReadMemory(const void* address, void* destination, std::size_t length) {
    SIZE_T bytesRead = 0;
    return length != 0 &&
           ReadProcessMemory(GetCurrentProcess(), address, destination, length, &bytesRead) != FALSE &&
           bytesRead == length;
}

bool ImageRangeValid(std::uintptr_t rva, std::size_t length) {
    return length != 0 && rva < g_imageSize && length <= g_imageSize - rva;
}

bool ReadImage(std::uintptr_t rva, void* destination, std::size_t length) {
    return ImageRangeValid(rva, length) &&
           ReadMemory(reinterpret_cast<const void*>(g_imageBase + rva), destination, length);
}

bool ImageBytesMatch(std::uintptr_t rva, const void* expected, std::size_t length) {
    std::uint8_t actual[64]{};
    if (length > sizeof(actual) || !ReadImage(rva, actual, length)) return false;
    return std::memcmp(actual, expected, length) == 0;
}

bool SectionNameEquals(const IMAGE_SECTION_HEADER& section, const char* expected) {
    char name[IMAGE_SIZEOF_SHORT_NAME + 1]{};
    std::memcpy(name, section.Name, IMAGE_SIZEOF_SHORT_NAME);
    return std::strcmp(name, expected) == 0;
}

const IMAGE_SECTION_HEADER* FindSection(const GameImageLayout& image, const char* name) {
    for (WORD index = 0; index < image.sectionCount; ++index) {
        if (SectionNameEquals(image.sections[index], name)) return &image.sections[index];
    }
    return nullptr;
}

bool SectionContains(const IMAGE_SECTION_HEADER& section, std::uintptr_t rva,
                     std::size_t length) {
    const std::uintptr_t sectionSize =
        (std::max)(static_cast<std::uintptr_t>(section.Misc.VirtualSize),
                   static_cast<std::uintptr_t>(section.SizeOfRawData));
    const std::uintptr_t sectionBegin = section.VirtualAddress;
    return length != 0 && rva >= sectionBegin && rva - sectionBegin < sectionSize &&
           length <= sectionSize - (rva - sectionBegin);
}

bool SectionHasCharacteristics(const IMAGE_SECTION_HEADER& section, DWORD required,
                               DWORD forbidden) {
    return (section.Characteristics & required) == required &&
           (section.Characteristics & forbidden) == 0;
}

bool InspectGameImage(GameImageLayout& image) {
    IMAGE_DOS_HEADER dos{};
    if (!ReadMemory(reinterpret_cast<const void*>(g_imageBase), &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0x40 || dos.e_lfanew > 0x1000) {
        Log("unsupported executable: invalid DOS header");
        return false;
    }

    IMAGE_NT_HEADERS32 nt{};
    if (!ReadMemory(reinterpret_cast<const void*>(g_imageBase + dos.e_lfanew), &nt,
                    sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER32) ||
        (nt.FileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) == 0 ||
        (nt.FileHeader.Characteristics & IMAGE_FILE_DLL) != 0 ||
        nt.FileHeader.NumberOfSections == 0 ||
        nt.FileHeader.NumberOfSections > kMaximumSections ||
        nt.OptionalHeader.SectionAlignment != 0x1000 ||
        nt.OptionalHeader.SizeOfImage <= kFrameStampRva + sizeof(std::uint32_t)) {
        Log("unsupported executable: invalid x86 PE layout");
        return false;
    }

    const std::uintptr_t sectionTableRva =
        static_cast<std::uintptr_t>(dos.e_lfanew) + offsetof(IMAGE_NT_HEADERS32, OptionalHeader) +
        nt.FileHeader.SizeOfOptionalHeader;
    const std::size_t sectionTableSize =
        static_cast<std::size_t>(nt.FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
    if (sectionTableRva > nt.OptionalHeader.SizeOfHeaders ||
        sectionTableSize > nt.OptionalHeader.SizeOfHeaders - sectionTableRva ||
        !ReadMemory(reinterpret_cast<const void*>(g_imageBase + sectionTableRva), image.sections,
                    sectionTableSize)) {
        Log("unsupported executable: invalid PE section table");
        return false;
    }

    image.timeDateStamp = nt.FileHeader.TimeDateStamp;
    image.sizeOfImage = nt.OptionalHeader.SizeOfImage;
    image.sectionCount = nt.FileHeader.NumberOfSections;
    g_imageSize = image.sizeOfImage;

    const auto* text = FindSection(image, ".text");
    const auto* readOnlyData = FindSection(image, ".rdata");
    const auto* writableData = FindSection(image, ".data");
    if (text == nullptr || readOnlyData == nullptr || writableData == nullptr ||
        !SectionHasCharacteristics(*text, IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE |
                                              IMAGE_SCN_MEM_READ,
                                   IMAGE_SCN_MEM_WRITE) ||
        !SectionHasCharacteristics(*readOnlyData,
                                   IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ,
                                   IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE) ||
        !SectionHasCharacteristics(*writableData,
                                   IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ |
                                       IMAGE_SCN_MEM_WRITE,
                                   IMAGE_SCN_MEM_EXECUTE) ||
        !SectionContains(*text, kMouseRateFilterRva, sizeof(kRateFilterSignaturePrefix)) ||
        !SectionContains(*text, kChaseOrientationDampRva,
                         sizeof(kOrientationDampSignature)) ||
        !SectionContains(*readOnlyData, kDirectInputFactoryNameRva,
                         sizeof(kDirectInputFactoryName)) ||
        !SectionContains(*readOnlyData, kDirectInputModuleNameRva,
                         sizeof(kDirectInputModuleName)) ||
        !SectionContains(*writableData, kFrameStampRva, sizeof(std::uint32_t))) {
        Log("unsupported executable: required code/data section layout is absent");
        return false;
    }

    for (const std::uintptr_t callRva : kRateFilterCallRvas) {
        if (!SectionContains(*text, callRva, 5)) {
            Log("unsupported executable: rate-filter caller RVA %08lx is outside .text",
                static_cast<unsigned long>(callRva));
            return false;
        }
    }
    for (const std::uintptr_t callRva : kOrientationDampCallRvas) {
        if (!SectionContains(*text, callRva, 5)) {
            Log("unsupported executable: orientation caller RVA %08lx is outside .text",
                static_cast<unsigned long>(callRva));
            return false;
        }
    }

    const bool knownBuild = image.timeDateStamp == kSupportedTimeDateStamp &&
                            image.sizeOfImage == kSupportedSizeOfImage;
    Log("executable metadata: timestamp=%08lx image_size=%08lx (%s)",
        image.timeDateStamp, image.sizeOfImage,
        knownBuild ? "known retail build" : "compatibility candidate");
    return true;
}

bool ExecutableBytesMatch(const std::uint8_t* address, const std::uint8_t* expected,
                          std::size_t length) {
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }

    const DWORD protection = memory.Protect & 0xffu;
    if (protection != PAGE_EXECUTE && protection != PAGE_EXECUTE_READ &&
        protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) {
        return false;
    }

    const auto regionBegin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    const auto regionEnd = regionBegin + memory.RegionSize;
    const auto bytesBegin = reinterpret_cast<std::uintptr_t>(address);
    if (bytesBegin < regionBegin || bytesBegin > regionEnd || length > regionEnd - bytesBegin) {
        return false;
    }

    std::uint8_t actual[64]{};
    if (length > sizeof(actual)) return false;
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), address, actual, length, &bytesRead) ||
        bytesRead != length) {
        return false;
    }
    return std::memcmp(actual, expected, length) == 0;
}

bool RuntimeSignaturesReady() {
    const auto* filter = reinterpret_cast<const std::uint8_t*>(
        g_imageBase + kMouseRateFilterRva);
    const auto* damp = reinterpret_cast<const std::uint8_t*>(
        g_imageBase + kChaseOrientationDampRva);
    std::uint32_t inputManagerAddress = 0;
    return ExecutableBytesMatch(filter, kRateFilterSignaturePrefix,
                                sizeof(kRateFilterSignaturePrefix)) &&
           ReadImage(kMouseRateFilterRva + sizeof(kRateFilterSignaturePrefix),
                     &inputManagerAddress, sizeof(inputManagerAddress)) &&
           inputManagerAddress == static_cast<std::uint32_t>(g_imageBase + kInputManagerRva) &&
           ExecutableBytesMatch(filter + sizeof(kRateFilterSignaturePrefix) +
                                    sizeof(inputManagerAddress),
                                kRateFilterSignatureSuffix,
                                sizeof(kRateFilterSignatureSuffix)) &&
           ExecutableBytesMatch(damp, kOrientationDampSignature,
                                sizeof(kOrientationDampSignature));
}

bool WaitForHookTargets() {
    const DWORD started = GetTickCount();
    if (RuntimeSignaturesReady()) {
        Log("protected game code already ready");
        return true;
    }

    Log("waiting up to %lu ms for protected game code", kInitializationTimeoutMs);
    for (;;) {
        const DWORD elapsed = GetTickCount() - started;
        if (elapsed >= kInitializationTimeoutMs) {
            Log("initialization timed out after %lu ms: runtime signatures were not ready",
                elapsed);
            return false;
        }
        Sleep(kInitializationPollMs);
        if (RuntimeSignaturesReady()) {
            Log("protected game code ready after %lu ms", GetTickCount() - started);
            return true;
        }
    }
}

bool RelativeCallTargets(std::uintptr_t callRva, std::uintptr_t targetRva) {
    std::uint8_t instruction[5]{};
    if (!ReadImage(callRva, instruction, sizeof(instruction)) || instruction[0] != 0xE8) {
        return false;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, instruction + 1, sizeof(displacement));
    const std::int64_t destination = static_cast<std::int64_t>(callRva) +
                                     sizeof(instruction) + displacement;
    return destination == static_cast<std::int64_t>(targetRva);
}

bool WritableMemoryAt(std::uintptr_t rva, std::size_t length) {
    if (!ImageRangeValid(rva, length)) return false;
    MEMORY_BASIC_INFORMATION memory{};
    const auto* address = reinterpret_cast<const void*>(g_imageBase + rva);
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const DWORD protection = memory.Protect & 0xffu;
    const bool writable = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
                          protection == PAGE_EXECUTE_READWRITE ||
                          protection == PAGE_EXECUTE_WRITECOPY;
    const auto regionBegin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    const auto regionEnd = regionBegin + memory.RegionSize;
    const auto bytesBegin = reinterpret_cast<std::uintptr_t>(address);
    return writable && bytesBegin >= regionBegin && bytesBegin <= regionEnd &&
           length <= regionEnd - bytesBegin;
}

bool ValidateCorrectionPaths() {
    if (!RuntimeSignaturesReady()) {
        Log("unsupported executable: reconstructed hook signatures changed");
        return false;
    }
    if (!ImageBytesMatch(kDirectInputFactoryNameRva, kDirectInputFactoryName,
                         sizeof(kDirectInputFactoryName)) ||
        !ImageBytesMatch(kDirectInputModuleNameRva, kDirectInputModuleName,
                         sizeof(kDirectInputModuleName))) {
        Log("unsupported executable: retail DirectInput resolver layout changed");
        return false;
    }
    if (!WritableMemoryAt(kFrameStampRva, sizeof(std::uint32_t))) {
        Log("unsupported executable: frame-stamp storage is unavailable");
        return false;
    }
    for (const std::uintptr_t callRva : kRateFilterCallRvas) {
        if (!RelativeCallTargets(callRva, kMouseRateFilterRva)) {
            Log("unsupported executable: rate-filter call relationship failed at RVA %08lx",
                static_cast<unsigned long>(callRva));
            return false;
        }
    }
    for (const std::uintptr_t callRva : kOrientationDampCallRvas) {
        if (!RelativeCallTargets(callRva, kChaseOrientationDampRva)) {
            Log("unsupported executable: orientation call relationship failed at RVA %08lx",
                static_cast<unsigned long>(callRva));
            return false;
        }
    }
    Log("runtime compatibility verified: hook ABIs, DirectInput anchors, frame stamp, "
        "9 rate callers, and 3 orientation callers");
    return true;
}

bool AcquireInstanceGuard() {
    wchar_t name[64]{};
    wsprintfW(name, L"Local\\DS3RawMouseFix-%lu", GetCurrentProcessId());
    HANDLE mutex = CreateMutexW(nullptr, FALSE, name);
    if (mutex == nullptr) {
        Log("initialization failed: could not create instance guard error=%lu", GetLastError());
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        Log("initialization refused: another DS3 Raw Mouse Fix module is already loaded");
        return false;
    }
    g_instanceMutex = mutex;
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
        const DWORD now = GetTickCount();
        if (now - g_lastDrainTick > kStaleRawDeltaMs) {
            if (g_cachedX != 0 || g_cachedY != 0) {
                g_staleDrops.fetch_add(1, std::memory_order_relaxed);
            }
            g_cachedX = 0;
            g_cachedY = 0;
        }
        g_lastDrainTick = now;
    }
    x = g_cachedX;
    y = g_cachedY;
    LeaveCriticalSection(&g_frameLock);
}

unsigned __fastcall HookMouseRateFilter(void* self, void*, float frameTime,
                                        float* horizontal, float* vertical) {
    const unsigned accepted = g_originalFilter(self, frameTime, horizontal, vertical);
    g_filterCalls.fetch_add(1, std::memory_order_relaxed);
    if (!RateEnabled()) return accepted;

    // Drain on every call: counts from a frame the game rejects are discarded here
    // instead of being carried into the next accepted frame.
    LONG x = 0;
    LONG y = 0;
    GetFrameRawDelta(x, y);
    if ((accepted & 0xffu) == 0 || horizontal == nullptr || vertical == nullptr ||
        frameTime <= 0.000001f || !GameHasForeground()) {
        return accepted;
    }

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

// Both hook targets start on an 8-byte boundary, so the whole jump (plus the untouched bytes
// that complete the qword) is published with one LOCK CMPXCHG8B. No thread can execute a
// partly written jump, and no thread is suspended: suspending every thread can deadlock when
// another plugin (for example DS3 Debug Menu) is suspending threads to install its own hooks.
bool WriteJumpAtomic(std::uint8_t* target, const std::uint8_t* expected, std::size_t patchLength,
                     const void* replacement, const char* name) {
    constexpr std::size_t kWord = sizeof(LONG64);
    if (patchLength < 5 || patchLength > kWord ||
        (reinterpret_cast<std::uintptr_t>(target) & (kWord - 1)) != 0) {
        Log("hook failed: %s target is not 8-byte aligned", name);
        return false;
    }
    DWORD oldProtection = 0;
    if (!VirtualProtect(target, kWord, PAGE_EXECUTE_READWRITE, &oldProtection)) {
        Log("hook failed: %s VirtualProtect error=%lu", name, GetLastError());
        return false;
    }

    std::uint8_t current[kWord]{};
    std::memcpy(current, target, kWord);
    std::uint8_t patched[kWord]{};
    std::memcpy(patched, current, kWord);
    patched[0] = 0xE9;
    const auto displacement = static_cast<std::int32_t>(
        reinterpret_cast<const std::uint8_t*>(replacement) - (target + 5));
    std::memcpy(patched + 1, &displacement, sizeof(displacement));
    for (std::size_t index = 5; index < patchLength; ++index) patched[index] = 0x90;

    LONG64 comparand = 0;
    LONG64 exchange = 0;
    std::memcpy(&comparand, current, kWord);
    std::memcpy(&exchange, patched, kWord);
    const bool prologueIntact = std::memcmp(current, expected, patchLength) == 0;
    const bool written = prologueIntact &&
        _InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(target), exchange,
                                      comparand) == comparand;
    FlushInstructionCache(GetCurrentProcess(), target, kWord);
    DWORD ignored = 0;
    VirtualProtect(target, kWord, oldProtection, &ignored);
    if (!written) Log("hook failed: %s entry changed while installing", name);
    return written;
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

    const bool filterInstalled = WriteJumpAtomic(
        filter, kRateFilterPrologue, sizeof(kRateFilterPrologue),
        reinterpret_cast<const void*>(&HookMouseRateFilter), "mouse rate filter");
    const bool dampInstalled = filterInstalled && WriteJumpAtomic(
        damp, kOrientationDampPrologue, sizeof(kOrientationDampPrologue),
        reinterpret_cast<const void*>(&HookChaseOrientationDamp), "chase orientation damp");
    return filterInstalled && dampInstalled;
}

bool FindDebugMenuModule(wchar_t (&modulePath)[MAX_PATH]) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (entry.hModule != g_self &&
                GetProcAddress(entry.hModule, kDebugMenuExport) != nullptr) {
                wcscpy_s(modulePath, entry.szExePath);
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

// The menu may load before or after this plugin, so look for it until it is found.
bool DetectDebugMenu(wchar_t (&modulePath)[MAX_PATH]) {
    if (g_debugMenuLoaded) return true;
    if (!FindDebugMenuModule(modulePath)) return false;
    g_debugMenuLoaded = true;
    Log("DS3 Debug Menu detected (%ls): hotkeys on F1/F7/F8 are left to the menu", modulePath);
    return true;
}

void DetectDebugMenu() {
    wchar_t modulePath[MAX_PATH]{};
    DetectDebugMenu(modulePath);
}

// DS3 Debug Menu 1.2 hashes the reconstructed game code about 15 s after it loads (then every
// 5 s, up to 10 attempts) and locks all of its game features ("RUNTIME TEXT DID NOT SETTLE")
// unless the hash matches the retail build. This plugin's two entry jumps change that code, so
// while the menu is loaded they are installed only once its probe has finished. The game is
// still in its intro or main menu by then, so no gameplay camera runs without the fix.
//
// The menu's log cannot be read while it runs (it is opened without sharing), but the menu
// writes its code dump DS3_Runtime_Text.bin beside itself before every attempt and stops after
// a verdict: the probe has finished once a dump was written and no other followed within one
// retry interval.
constexpr DWORD kDebugMenuDetectGraceMs = 3000;
constexpr DWORD kDebugMenuProbeQuietMs = 6500;
constexpr DWORD kDebugMenuNoDumpTimeoutMs = 30000;
constexpr DWORD kDebugMenuProbeTimeoutMs = 90000;
constexpr wchar_t kDebugMenuTextDump[] = L"DS3_Runtime_Text.bin";

ULONGLONG FileWriteTime(const wchar_t* path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return 0;
    ULARGE_INTEGER time{};
    time.LowPart = data.ftLastWriteTime.dwLowDateTime;
    time.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return time.QuadPart;
}

void WaitForDebugMenuTextProbe(DWORD initializationStarted) {
    wchar_t modulePath[MAX_PATH]{};
    while (!DetectDebugMenu(modulePath)) {
        if (GetTickCount() - initializationStarted >= kDebugMenuDetectGraceMs) return;
        Sleep(100);
    }

    wchar_t dumpPath[MAX_PATH]{};
    wcscpy_s(dumpPath, modulePath);
    wchar_t* slash = wcsrchr(dumpPath, L'\\');
    if (slash != nullptr) slash[1] = L'\0';
    wcscat_s(dumpPath, kDebugMenuTextDump);
    Log("waiting for DS3 Debug Menu's runtime-text probe before installing hooks");

    // Only dumps written from now on belong to this session's probe.
    FILETIME nowTime{};
    GetSystemTimeAsFileTime(&nowTime);
    ULARGE_INTEGER now{};
    now.LowPart = nowTime.dwLowDateTime;
    now.HighPart = nowTime.dwHighDateTime;
    ULONGLONG lastWrite = (std::max)(FileWriteTime(dumpPath), now.QuadPart);
    const DWORD started = GetTickCount();
    DWORD lastDumpTick = 0;
    unsigned attempts = 0;
    for (;;) {
        const ULONGLONG write = FileWriteTime(dumpPath);
        const DWORD tick = GetTickCount();
        if (write > lastWrite) {
            lastWrite = write;
            lastDumpTick = tick;
            ++attempts;
        }
        if (attempts != 0 && tick - lastDumpTick >= kDebugMenuProbeQuietMs) {
            Log("DS3 Debug Menu probe finished after %u attempt(s), %lu ms; installing hooks",
                attempts, tick - started);
            return;
        }
        if (attempts == 0 && tick - started >= kDebugMenuNoDumpTimeoutMs) {
            Log("no DS3 Debug Menu probe seen after %lu ms; installing hooks", tick - started);
            return;
        }
        if (tick - started >= kDebugMenuProbeTimeoutMs) {
            Log("DS3 Debug Menu probe still running after %lu ms; installing hooks",
                tick - started);
            return;
        }
        Sleep(250);
    }
}

bool HotkeyYieldsToDebugMenu(const Hotkey& key) {
    if (!g_debugMenuLoaded) return false;
    for (const UINT reserved : kDebugMenuKeys) {
        if (key.virtualKey == reserved) return true;
    }
    return false;
}

bool KeyDown(int virtualKey) {
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

void RunHotkeyAction(std::size_t action, const char* keyName) {
    switch (action) {
    case kHotkeyReload:
        LoadConfig();
        LogEffectiveSensitivity("reload");
        break;
    case kHotkeyToggleSmoothing: {
        const bool enabled = !DampEnabled();
        InterlockedExchange(&g_dampEnabled, enabled ? 1 : 0);
        Log("%s: chase orientation bypass %s", keyName, enabled ? "enabled" : "disabled");
        break;
    }
    case kHotkeyToggleRaw: {
        const bool enabled = !RateEnabled();
        InterlockedExchange(&g_rateEnabled, enabled ? 1 : 0);
        ClearRawDelta();
        Log("%s: raw rate replacement %s", keyName, enabled ? "enabled" : "disabled");
        break;
    }
    case kHotkeySensitivityDown:
        g_sensitivityPermille.store((std::max)(100L, g_sensitivityPermille.load() - 50L));
        LogEffectiveSensitivity(keyName);
        break;
    case kHotkeySensitivityUp:
        g_sensitivityPermille.store((std::min)(5000L, g_sensitivityPermille.load() + 50L));
        LogEffectiveSensitivity(keyName);
        break;
    default:
        break;
    }
}

void PollHotkeys() {
    const UINT modifiers = (KeyDown(VK_CONTROL) ? kModifierCtrl : 0u) |
                           (KeyDown(VK_SHIFT) ? kModifierShift : 0u) |
                           (KeyDown(VK_MENU) ? kModifierAlt : 0u);
    const bool active = InterlockedCompareExchange(&g_hotkeysEnabled, 0, 0) != 0 &&
                        GameHasForeground();
    for (std::size_t action = 0; action < kHotkeyCount; ++action) {
        HotkeyBinding& binding = g_hotkeys[action];
        if (binding.key.virtualKey == 0) continue;
        // Exact modifiers: plain F7 does not fire on Shift+F7 and vice versa.
        const bool down = KeyDown(static_cast<int>(binding.key.virtualKey)) &&
                          modifiers == binding.key.modifiers;
        const bool pressed = down && !binding.wasDown;
        binding.wasDown = down;
        if (!pressed || !active) continue;
        char keyName[48]{};
        FormatHotkey(binding.key, keyName);
        if (HotkeyYieldsToDebugMenu(binding.key)) {
            if (!binding.yieldLogged) {
                Log("%s (%s) ignored: the key belongs to DS3 Debug Menu; set %s in [Hotkeys] "
                    "to another key", keyName, binding.description, binding.iniKey);
                binding.yieldLogged = true;
            }
            continue;
        }
        RunHotkeyAction(action, keyName);
    }
}

DWORD WINAPI Initialize(LPVOID) {
    if (InterlockedCompareExchange(&g_initializationStarted, 1, 0) != 0) return 1;
    const DWORD initializationStarted = GetTickCount();
    g_imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    InitializeCriticalSection(&g_frameLock);
    InitializeCriticalSection(&g_windowLock);
    Log("DS3 Raw Mouse Fix %s loading", DS3_RAW_MOUSE_VERSION);
    if (!AcquireInstanceGuard()) return 1;
    GameImageLayout image{};
    if (!InspectGameImage(image)) return 1;
    if (!WaitForHookTargets()) return 1;
    if (!ValidateCorrectionPaths()) return 1;
    WaitForDebugMenuTextProbe(initializationStarted);
    if (!InstallHooks()) return 1;
    LoadConfig();
#if DS3_RAW_MOUSE_PRODUCT_BUILD
    Log("initialized: raw=%u smoothing_bypass=%u hotkeys=%u diagnostics=%u",
        RateEnabled() ? 1u : 0u, DampEnabled() ? 1u : 0u,
        InterlockedCompareExchange(&g_hotkeysEnabled, 0, 0) != 0 ? 1u : 0u,
        InterlockedCompareExchange(&g_diagnosticsEnabled, 0, 0) != 0 ? 1u : 0u);
#else
    Log("initialized disabled: patches start off; use the hotkeys listed above");
#endif
    LogEffectiveSensitivity("initial sensitivity");

    ULONG priorPackets = 0;
    ULONG priorCalls = 0;
    ULONG priorReplacements = 0;
    ULONG priorBypasses = 0;
    ULONG priorThirdPersonCalls = 0;
    ULONG priorAimCalls = 0;
    ULONG priorOtherCalls = 0;
    ULONG priorStaleDrops = 0;
    DWORD lastCaptureCheck = 0;
    DWORD lastModuleCheck = 0;
    DWORD lastReport = GetTickCount();
    for (;;) {
        const DWORD now = GetTickCount();
        if (now - lastModuleCheck >= 1000) {
            DetectDebugMenu();
            lastModuleCheck = now;
        }
        PollHotkeys();
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
            const ULONG staleDrops = g_staleDrops.load();
            const ULONG bypasses = static_cast<ULONG>(
                InterlockedCompareExchange(&g_dampBypasses, 0, 0));
            Log("5s stats: raw=%lu filter=%lu replaced=%lu profiles(third/aim)=%lu/%lu "
                "other_callers=%lu damp_bypass=%lu stale_drops=%lu rate=%u damp=%u aim_held=%u",
                packets - priorPackets, calls - priorCalls, replacements - priorReplacements,
                thirdPersonCalls - priorThirdPersonCalls, aimCalls - priorAimCalls,
                otherCalls - priorOtherCalls, bypasses - priorBypasses,
                staleDrops - priorStaleDrops,
                RateEnabled() ? 1u : 0u, DampEnabled() ? 1u : 0u, AimHeld() ? 1u : 0u);
            priorPackets = packets;
            priorCalls = calls;
            priorReplacements = replacements;
            priorThirdPersonCalls = thirdPersonCalls;
            priorAimCalls = aimCalls;
            priorOtherCalls = otherCalls;
            priorStaleDrops = staleDrops;
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

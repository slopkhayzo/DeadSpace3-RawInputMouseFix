#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

#ifndef DS3_RAW_MOUSE_DEFAULT_DLL
#define DS3_RAW_MOUSE_DEFAULT_DLL L"DS3RawMouse.dll"
#endif

fs::path ExecutableDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return fs::current_path();
    }
    path.resize(length);
    return fs::path(path).parent_path();
}

DWORD FindProcessId(const wchar_t* name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD processId = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, name) == 0) {
                processId = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return processId;
}

int wmain(int argc, wchar_t** argv) {
    const fs::path dllPath = fs::absolute(
        argc > 1 ? fs::path(argv[1]) : ExecutableDirectory() / DS3_RAW_MOUSE_DEFAULT_DLL);
    if (!fs::is_regular_file(dllPath)) {
        std::wcerr << L"DLL not found: " << dllPath << L"\n";
        return 2;
    }
    const DWORD processId = FindProcessId(L"deadspace3.exe");
    if (processId == 0) {
        std::wcerr << L"deadspace3.exe is not running. Start the game through the EA App, "
                       L"then run this loader again.\n";
        return 3;
    }

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, processId);
    if (process == nullptr) {
        std::cerr << "OpenProcess failed: " << GetLastError() << "\n";
        return 4;
    }

    const std::wstring pathText = dllPath.wstring();
    const SIZE_T bytes = (pathText.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remotePath == nullptr ||
        !WriteProcessMemory(process, remotePath, pathText.c_str(), bytes, nullptr)) {
        std::cerr << "Could not write the DLL path into the game process: " << GetLastError() << "\n";
        if (remotePath != nullptr) {
            VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        }
        CloseHandle(process);
        return 5;
    }

    const auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr);
    if (thread == nullptr) {
        std::cerr << "CreateRemoteThread failed: " << GetLastError() << "\n";
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return 6;
    }

    const DWORD waitResult = WaitForSingleObject(thread, 10000);
    if (waitResult != WAIT_OBJECT_0) {
        std::cerr << "Timed out waiting for LoadLibraryW inside the game process. Error: "
                  << GetLastError() << "\n";
        // Do not free the remote path while the remote thread may still read it.
        CloseHandle(thread);
        CloseHandle(process);
        return 7;
    }
    DWORD module = 0;
    GetExitCodeThread(thread, &module);
    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);

    if (module == 0) {
        std::cerr << "LoadLibraryW failed inside the game process.\n";
        return 8;
    }
    std::wcout << L"Loaded " << dllPath << L" into deadspace3.exe (PID " << processId << L").\n";
#ifdef DS3_RAW_MOUSE_LOADER_BUILD
    std::wcout << L"The raw-mouse and camera-smoothing fixes are now active.\n";
#endif
    return 0;
}

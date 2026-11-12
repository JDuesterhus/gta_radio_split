// Starts RadioSplit.exe outside the game's process tree.
//
// OBS captures a process and its child processes. A player created directly
// by the game is therefore part of the game's application audio capture,
// even though it only renders radio. Giving the player explorer.exe as its
// parent keeps it out of that tree. The new process still inherits the
// caller's token, so an elevated game can open the shared memory.
#pragma once

#include <windows.h>

#include <vector>

namespace radiosplit {

// exePath is the executable, command its mutable command line. On success,
// detached is true when the shell process was used as the parent. pid, when
// not null, receives the new process id. Falls back to a normal child when
// the shell cannot be used, so the player still starts.
inline bool SpawnDetachedPlayer(const wchar_t* exePath, wchar_t* command, bool& detached, DWORD* pid = nullptr) {
    detached = false;
    if (pid != nullptr) {
        *pid = 0;
    }

    HANDLE parent = nullptr;
    const HWND shell = GetShellWindow();
    if (shell != nullptr) {
        DWORD shellPid = 0;
        GetWindowThreadProcessId(shell, &shellPid);
        if (shellPid != 0) {
            parent = OpenProcess(PROCESS_CREATE_PROCESS, FALSE, shellPid);
        }
    }

    STARTUPINFOEXW startup{};
    std::vector<unsigned char> attributeStorage;
    bool listInitialized = false;
    bool parentSet = false;
    if (parent != nullptr) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        if (size != 0) {
            attributeStorage.resize(size);
            startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
            if (InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size)) {
                listInitialized = true;
                parentSet = UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                    PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &parent, sizeof parent, nullptr, nullptr) != FALSE;
            }
        }
    }

    startup.StartupInfo.cb = parentSet ? sizeof(STARTUPINFOEXW) : sizeof(STARTUPINFO);
    PROCESS_INFORMATION process{};
    DWORD flags = CREATE_NO_WINDOW;
    if (parentSet) {
        flags |= EXTENDED_STARTUPINFO_PRESENT;
    }
    BOOL started = CreateProcessW(exePath, command, nullptr, nullptr, FALSE, flags, nullptr, nullptr,
        &startup.StartupInfo, &process);

    if (started == FALSE && parentSet) {
        // The shell parent is best effort. A shell that cannot be opened
        // must not stop the side player from starting.
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        listInitialized = false;
        parentSet = false;
        startup.lpAttributeList = nullptr;
        startup.StartupInfo.cb = sizeof(STARTUPINFO);
        ZeroMemory(&process, sizeof process);
        started = CreateProcessW(exePath, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
            &startup.StartupInfo, &process);
    }

    const DWORD error = started == FALSE ? GetLastError() : ERROR_SUCCESS;
    if (listInitialized) {
        DeleteProcThreadAttributeList(startup.lpAttributeList);
    }
    if (parent != nullptr) {
        CloseHandle(parent);
    }
    if (started == FALSE) {
        SetLastError(error);
        return false;
    }

    if (pid != nullptr) {
        *pid = process.dwProcessId;
    }
    detached = parentSet;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

}  // namespace radiosplit

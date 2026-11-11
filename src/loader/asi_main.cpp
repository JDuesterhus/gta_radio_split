// One 32-bit ASI for GTA III, Vice City, and San Andreas 1.0-class executables.
// The executable name picks the audio engine. San Andreas is DirectSound.
// III and Vice City are Miles. Unknown executables are refused.
// Ultimate ASI Loader calls InitializeASI. Older loaders only call DllMain,
// so Start() is reached from both, guarded by g_started.
#include "common/ini.h"
#include "common/log.h"
#include "games/miles/miles_radio.h"
#include "games/sa/sa_radio.h"
#include "games/sa/version.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <string>

namespace {
HMODULE g_module = nullptr;
long g_started = 0;

std::wstring ModulePath(const wchar_t* fileName) {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return fileName;
    }
    std::wstring full(path, length);
    const std::size_t slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return fileName;
    }
    full.resize(slash + 1);
    full += fileName;
    return full;
}

bool LoadedByUltimateAsi() {
    const char* names[] = {
        "vorbisFile.dll", "vorbishooked.dll", "dinput8.dll", "dsound.dll",
        "winmm.dll",      "xlive.dll",       "version.dll", "binkw32.dll",
    };
    for (const char* name : names) {
        const HMODULE module = GetModuleHandleA(name);
        if (module != nullptr && GetProcAddress(module, "IsUltimateASILoader") != nullptr) {
            return true;
        }
    }
    return false;
}

void Start() {
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) {
        return;
    }
    const std::wstring logPath = ModulePath(L"RadioSplit.log");
    const std::wstring iniPath = ModulePath(L"RadioSplit.ini");
    radiosplit::LogOpen(logPath.c_str());
    radiosplit::Log("RadioSplit");

    radiosplit::Settings settings;
    if (!radiosplit::LoadSettings(iniPath.c_str(), settings)) {
        radiosplit::WriteDefaultSettings(iniPath.c_str());
        radiosplit::Log("Wrote RadioSplit.ini. Set OutputDevice, then start the game again");
    }
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    const char* exeName = exePath;
    for (const char* cursor = exePath; *cursor != '\0'; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            exeName = cursor + 1;
        }
    }

    auto same = [](const char* left, const char* right) {
        while (*left != '\0' && *right != '\0') {
            char a = *left++;
            char b = *right++;
            if (a >= 'A' && a <= 'Z') {
                a = static_cast<char>(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = static_cast<char>(b - 'A' + 'a');
            }
            if (a != b) {
                return false;
            }
        }
        return *left == '\0' && *right == '\0';
    };
    auto readDword = [](std::uintptr_t address, std::uint32_t& out) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof info) == 0) {
            return false;
        }
        if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            return false;
        }
        out = *reinterpret_cast<std::uint32_t*>(address);
        return true;
    };

    if (same(exeName, "gta_sa.exe") || same(exeName, "gta-sa.exe") || same(exeName, "gta_sa_compact.exe")) {
        // Edition and addresses are resolved from the loaded module. The Steam
        // exe is relocated, so a fixed 0x401000 read is not the version word.
        radiosplit::StartSaRadio(settings);
        return;
    }

    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
    auto relocated = [module, &readDword](std::uintptr_t preferred) {
        std::uint32_t word = 0;
        readDword(module + (preferred - 0x400000u), word);
        return word;
    };
    if (same(exeName, "gta3.exe")) {
        const radiosplit::TrilogyEdition edition =
            radiosplit::DetectTrilogyEdition(relocated(0x5C1E70), relocated(0x5C2130), relocated(0x5C6FD0));
        std::uintptr_t manager = 0;
        const char* name = "unlisted";
        if (edition == radiosplit::TrilogyEdition::V10) {
            manager = module + (0x8F3964u - 0x400000u);
            name = radiosplit::TrilogyEditionName(edition);
        } else if (edition == radiosplit::TrilogyEdition::V11) {
            manager = module + (0x8F3A18u - 0x400000u);
            name = radiosplit::TrilogyEditionName(edition);
        } else if (edition == radiosplit::TrilogyEdition::Steam) {
            manager = module + (0x903B58u - 0x400000u);
            name = radiosplit::TrilogyEditionName(edition);
        }
        const std::string host = std::string("GTA III ") + name;
        radiosplit::StartMilesRadio(settings, host.c_str(), manager);
        return;
    }

    if (same(exeName, "gta-vc.exe") || same(exeName, "gta_vc.exe")) {
        const radiosplit::TrilogyEdition edition =
            radiosplit::DetectTrilogyEdition(relocated(0x667BF0), relocated(0x667C40), relocated(0x666BA0));
        const char* name = edition == radiosplit::TrilogyEdition::Unsupported ? "unlisted" : radiosplit::TrilogyEditionName(edition);
        const std::string host = std::string("GTA Vice City ") + name;
        radiosplit::StartMilesRadio(settings, host.c_str(), 0);
        return;
    }

    radiosplit::Log("Host executable is not GTA III, Vice City, or San Andreas");
}

}  // namespace

extern "C" __declspec(dllexport) void InitializeASI() {
    Start();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
        if (!LoadedByUltimateAsi()) {
            Start();
        }
    }
    return TRUE;
}

#pragma once

#include <string>

namespace radiosplit {

struct Settings {
    bool enable = true;
    bool muteInGame = true;
    bool keepUserTracks = false;
    bool keepWctr = true;
    std::string outputDevice;
};

bool LoadSettings(const wchar_t* path, Settings& settings);
bool WriteDefaultSettings(const wchar_t* path);

}  // namespace radiosplit

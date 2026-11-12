#include "ini.h"

#include <cctype>
#include <fstream>
#include <string>

namespace radiosplit {
namespace {

std::string Trim(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool ParseBool(const std::string& value, bool fallback) {
    const std::string text = Trim(value);
    if (text == "1" || text == "true" || text == "yes" || text == "on") {
        return true;
    }
    if (text == "0" || text == "false" || text == "no" || text == "off") {
        return false;
    }
    return fallback;
}

}  // namespace

bool LoadSettings(const wchar_t* path, Settings& settings) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '[') {
            continue;
        }
        const std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = Trim(trimmed.substr(0, eq));
        const std::string value = Trim(trimmed.substr(eq + 1));
        if (key == "Enable") {
            settings.enable = ParseBool(value, settings.enable);
        } else if (key == "MuteInGame") {
            settings.muteInGame = ParseBool(value, settings.muteInGame);
        } else if (key == "OutputDevice") {
            settings.outputDevice = value;
        } else if (key == "CustomRadio" || key == "UserTracksInGame") {
            settings.keepCustomRadio = ParseBool(value, settings.keepCustomRadio);
        } else if (key == "TalkRadio" || key == "WctrInGame") {
            settings.keepTalkRadio = ParseBool(value, settings.keepTalkRadio);
        }
    }
    return true;
}

bool WriteDefaultSettings(const wchar_t* path) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file << "; RadioSplit\r\n"
            "; OutputDevice is a playback device name, or a unique piece of one.\r\n"
            "; Names are listed in RadioSplit.log. Leave this empty to use the Windows default playback device.\r\n"
            "; CustomRadio=1 leaves the custom-file station on the game speakers.\r\n"
            "; That is User Tracks in San Andreas, and the MP3 player in III and Vice City.\r\n"
            "; TalkRadio=1 leaves the talk stations on the game speakers.\r\n"
            "; That is WCTR, Chatterbox FM, K-Chat, and VCPR.\r\n"
            "Enable=1\r\n"
            "MuteInGame=1\r\n"
            "OutputDevice=\r\n"
            "CustomRadio=0\r\n"
            "TalkRadio=1\r\n";
    return static_cast<bool>(file);
}

}  // namespace radiosplit

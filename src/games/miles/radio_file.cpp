#include "radio_file.h"

#include <cctype>
#include <cstring>

namespace radiosplit {
namespace {

bool Equals(const char* text, const char* literal) {
    return std::strcmp(text, literal) == 0;
}

bool Contains(const char* text, const char* needle) {
    return std::strstr(text, needle) != nullptr;
}

void LowerInPlace(char* text) {
    for (; *text != '\0'; ++text) {
        *text = static_cast<char>(std::tolower(static_cast<unsigned char>(*text)));
    }
}

}  // namespace

bool Listed(const char* stem, const char* const* names) {
    for (const char* const* name = names; *name != nullptr; ++name) {
        if (Equals(stem, *name)) {
            return true;
        }
    }
    return false;
}

MilesStation ClassifyMilesStation(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return MilesStation::None;
    }

    const char* file = path;
    const char* folderStart = nullptr;
    for (const char* cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            folderStart = file;
            file = cursor + 1;
        }
    }

    char stem[260] = {};
    char folder[260] = {};
    for (std::size_t i = 0; file[i] != '\0' && i + 1 < sizeof stem; ++i) {
        stem[i] = file[i];
    }
    if (folderStart != nullptr && file > folderStart) {
        const std::size_t copy = static_cast<std::size_t>(file - folderStart - 1);
        if (copy < sizeof folder) {
            std::memcpy(folder, folderStart, copy);
            folder[copy] = '\0';
        }
    }
    char* dot = std::strrchr(stem, '.');
    if (dot != nullptr) {
        *dot = '\0';
    }
    LowerInPlace(stem);
    LowerInPlace(folder);

    if (Contains(stem, "amb")) {
        return MilesStation::None;
    }

    const char* talk[] = {"chat", "kchat", "vcpr", nullptr};
    if (Listed(stem, talk)) {
        return MilesStation::Talk;
    }

    // III station wavs, plus the Vice City station names Miles opens.
    const char* music[] = {
        "head", "class", "kjah", "rise", "lips", "game", "msx", "flash", "police",
        "wild", "fever", "vrock", "espant", "emotion", "wave", "taxi", nullptr,
    };
    if (Listed(stem, music)) {
        return MilesStation::Music;
    }

    // The MP3 player in III and Vice City opens songs from this folder.
    if (Equals(folder, "mp3")) {
        return MilesStation::Custom;
    }
    return MilesStation::None;
}

bool CaptureMilesStation(MilesStation station, bool keepTalkRadio, bool keepCustomRadio) {
    if (station == MilesStation::Music) {
        return true;
    }
    if (station == MilesStation::Talk) {
        return !keepTalkRadio;
    }
    if (station == MilesStation::Custom) {
        return !keepCustomRadio;
    }
    return false;
}

bool IsRadioStreamFile(const char* path) {
    return ClassifyMilesStation(path) != MilesStation::None;
}

}  // namespace radiosplit

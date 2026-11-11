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

bool IsRadioStreamFile(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return false;
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
        return false;
    }

    // III station wavs, plus the Vice City station names Miles opens.
    const char* stations[] = {
        "head", "class", "kjah", "rise", "lips", "game", "msx", "flash", "chat", "police",
        "wild", "kchat", "fever", "vrock", "vcpr", "espant", "emotion", "wave", "taxi",
    };
    for (const char* station : stations) {
        if (Equals(stem, station)) {
            return true;
        }
    }

    // Vice City keeps the radio directory separate from cutscene audio.
    return Equals(folder, "mp3");
}

}  // namespace radiosplit

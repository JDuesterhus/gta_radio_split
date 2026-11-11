#include "device_match.h"

#include <cctype>

namespace radiosplit {
namespace {

std::string Lower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

}  // namespace

int MatchDevice(const std::vector<std::string>& names, const std::string& want) {
    if (want.empty()) {
        return -1;
    }
    const std::string needle = Lower(want);
    int substring = -1;
    for (int i = 0; i < static_cast<int>(names.size()); ++i) {
        const std::string hay = Lower(names[i]);
        if (hay == needle) {
            return i;
        }
        if (substring < 0 && hay.find(needle) != std::string::npos) {
            substring = i;
        }
    }
    return substring;
}

int ChooseSideDevice(const std::vector<std::string>& names, const std::vector<char>& isDefault, const std::string& want) {
    (void)isDefault;
    if (want.empty()) {
        return -1;
    }
    const int found = MatchDevice(names, want);
    return found < 0 ? -2 : found;
}

}  // namespace radiosplit

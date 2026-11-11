#pragma once

#include <string>
#include <vector>

namespace radiosplit {

// Exact case-insensitive match wins. Otherwise the first substring match.
// An empty request matches nothing.
int MatchDevice(const std::vector<std::string>& names, const std::string& want);

// An explicit name uses MatchDevice and returns that index.
// An empty name returns -1, which means "open whatever Windows calls the default".
// An explicit name that matches nothing returns -2.
int ChooseSideDevice(const std::vector<std::string>& names, const std::vector<char>& isDefault, const std::string& want);

}  // namespace radiosplit

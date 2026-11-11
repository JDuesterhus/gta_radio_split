#pragma once

#include "common/ini.h"

#include <cstdint>

namespace radiosplit {

// iiiManager is the cMusicManager address for GTA III, or 0 for Vice City.
void StartMilesRadio(const Settings& settings, const char* gameName, std::uintptr_t iiiManager);

}  // namespace radiosplit

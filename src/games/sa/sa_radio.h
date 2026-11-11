#pragma once

#include "common/ini.h"

namespace radiosplit {

// Starts the San Andreas watcher. The thread lives until the process exits.
void StartSaRadio(const Settings& settings);

}  // namespace radiosplit

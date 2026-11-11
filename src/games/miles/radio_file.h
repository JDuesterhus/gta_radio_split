#pragma once

namespace radiosplit {

// True for a car-radio stream path. Cutscene and ambience paths are false.
bool IsRadioStreamFile(const char* path);

}  // namespace radiosplit

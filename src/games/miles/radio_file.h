#pragma once

namespace radiosplit {

enum class MilesStation {
    None,
    Music,
    Talk,
    Custom,
};

// Music stations always split. Talk is Chatterbox, K-Chat, and VCPR.
// Custom is a file in the MP3 player folder that is not itself a named station.
MilesStation ClassifyMilesStation(const char* path);

// True when this stream should be copied to RadioSplit.exe.
// keepTalkRadio and keepCustomRadio leave those groups on the game speakers.
bool CaptureMilesStation(MilesStation station, bool keepTalkRadio, bool keepCustomRadio);

// True for any station the plugin knows, before the keep-in-game switches.
bool IsRadioStreamFile(const char* path);

}  // namespace radiosplit

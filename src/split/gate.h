#pragma once

namespace radiosplit {

// CAERadioTrackManager::m_nRadioStatus values from the 1.0 layout.
// Playing is the only state whose streaming buffer is the car radio.
constexpr int kRadioStarting = 0;
constexpr int kRadioWaitingToPlay = 1;
constexpr int kRadioPlaying = 2;
constexpr int kRadioStopping = 3;
constexpr int kRadioStopped = 7;

// San Andreas station ids. WCTR is the talk station. User Tracks is the custom-file station.
// TalkRadio and CustomRadio are the ini names for these two groups.
constexpr int kStationWctr = 11;
constexpr int kStationUserTracks = 12;
constexpr int kStationOff = 13;

// True only while a vehicle radio is on the air. Cutscenes, menu music,
// and the police scanner do not use this status together with the vehicle check.
bool ShouldSplitRadio(int radioStatus, bool vehicleRadioActive);

// A kept station, and the radio-off position, stay on the game speakers.
bool StationStaysInGame(int station, bool keepCustomRadio, bool keepTalkRadio);

// Copy audio only while that station is actually playing.
bool ShouldFeedRadio(int radioStatus, bool vehicleRadioActive, bool staysInGame);

// After the side player has carried a station, keep the game buffer silent
// through a retune. The status leaves "playing" for about two seconds, and
// restoring the game volume there lets it bark the new song.
bool ShouldHoldGameMute(bool muteInGame, bool playerAlive, bool vehicleRadioActive, bool staysInGame, bool everHeard);

// Releasing the mute while the old song is still in the buffer plays a sliver
// of it from the game. Hold the silence until the radio status is stopped and
// the streaming buffer itself has stopped. A dead side player must release the
// game immediately so the stock radio is audible again.
bool ShouldKeepGameSilent(bool holdMute, bool alreadyMuted, int radioStatus, bool bufferPlaying,
    bool playerAlive = true);

// The game buffer stays audible unless the side device is actually running.
bool AllowMute(bool muteInGame, bool deviceRunning, bool split);

}  // namespace radiosplit

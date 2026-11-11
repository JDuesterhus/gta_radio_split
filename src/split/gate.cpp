#include "gate.h"

namespace radiosplit {

bool ShouldSplitRadio(int radioStatus, bool vehicleRadioActive) {
    return vehicleRadioActive && radioStatus == kRadioPlaying;
}

bool StationStaysInGame(int station, bool keepUserTracks, bool keepWctr) {
    if (station == kStationOff) {
        return true;
    }
    if (station == kStationUserTracks && keepUserTracks) {
        return true;
    }
    if (station == kStationWctr && keepWctr) {
        return true;
    }
    return false;
}

bool ShouldFeedRadio(int radioStatus, bool vehicleRadioActive, bool staysInGame) {
    return ShouldSplitRadio(radioStatus, vehicleRadioActive) && !staysInGame;
}

bool ShouldHoldGameMute(bool muteInGame, bool playerAlive, bool vehicleRadioActive, bool staysInGame, bool everHeard) {
    return muteInGame && playerAlive && vehicleRadioActive && !staysInGame && everHeard;
}

bool ShouldKeepGameSilent(bool holdMute, bool alreadyMuted, int radioStatus, bool bufferPlaying) {
    if (holdMute) {
        return true;
    }
    if (!alreadyMuted) {
        return false;
    }
    return radioStatus != kRadioStopped || bufferPlaying;
}

bool AllowMute(bool muteInGame, bool deviceRunning, bool split) {
    return muteInGame && deviceRunning && split;
}

}  // namespace radiosplit

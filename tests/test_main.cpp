#include "games/miles/detour.h"
#include "games/miles/ima_adpcm.h"
#include "games/miles/radio_file.h"
#include "games/sa/version.h"
#include "output/device_match.h"
#include "output/volume.h"
#include "split/byte_ring.h"
#include "split/gate.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failed = 0;

void Expect(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++g_failed;
    }
}

void TestGate() {
    using namespace radiosplit;
    Expect(ShouldSplitRadio(kRadioPlaying, true), "playing in vehicle splits");
    Expect(!ShouldSplitRadio(kRadioPlaying, false), "playing on foot does not split");
    Expect(!ShouldSplitRadio(kRadioStopped, true), "stopped does not split");
    Expect(!ShouldSplitRadio(kRadioStopping, true), "stopping does not split");
    Expect(!ShouldSplitRadio(kRadioStarting, true), "starting does not split");
    Expect(!ShouldSplitRadio(kRadioWaitingToPlay, true), "waiting does not split");

    Expect(ShouldFeedRadio(kRadioPlaying, true, false), "playing station is copied");
    Expect(!ShouldFeedRadio(kRadioStopped, true, false), "retune gap is not copied");
    Expect(!ShouldFeedRadio(kRadioPlaying, true, true), "kept station is not copied");
    Expect(ShouldHoldGameMute(true, true, true, false, true), "retune keeps the game muted");
    Expect(!ShouldHoldGameMute(true, true, true, false, false), "first song waits for real audio");
    Expect(!ShouldHoldGameMute(true, true, false, false, true), "leaving the vehicle releases the game");
    Expect(!ShouldHoldGameMute(true, true, true, true, true), "kept station releases the game");
    Expect(!ShouldHoldGameMute(true, false, true, false, true), "a dead player releases the game");
    Expect(ShouldKeepGameSilent(false, true, kRadioPlaying, true), "radio off still mutes a playing buffer");
    Expect(ShouldKeepGameSilent(false, true, kRadioStopping, false), "stopping stays muted");
    Expect(ShouldKeepGameSilent(false, true, kRadioStopped, true), "stopped status with a live buffer stays muted");
    Expect(!ShouldKeepGameSilent(false, true, kRadioStopped, false), "a stopped buffer releases the game");
    Expect(!ShouldKeepGameSilent(false, false, kRadioPlaying, true), "an already open buffer is left alone");
    Expect(!ShouldKeepGameSilent(false, true, kRadioPlaying, true, false), "a dead side player releases the game");
    Expect(!ShouldKeepGameSilent(false, true, kRadioPlaying, true, true, true), "a kept station releases the game");
    Expect(StationStaysInGame(kStationUserTracks, true, false), "custom radio option");
    Expect(!StationStaysInGame(kStationUserTracks, false, true), "custom radio still splits");
    Expect(StationStaysInGame(kStationWctr, false, true), "talk radio option");
    Expect(!StationStaysInGame(kStationWctr, true, false), "talk radio still splits");
    Expect(!StationStaysInGame(1, true, true), "a music station still splits");
    Expect(StationStaysInGame(kStationOff, false, false), "radio off stays with the game");
    Expect(!StationStaysInGame(-1, true, true), "an unknown station does not block the split");

    Expect(AllowMute(true, true, true), "mute only when split and device are up");
    Expect(!AllowMute(true, false, true), "no device means no mute");
    Expect(!AllowMute(true, true, false), "no split means no mute");
    Expect(!AllowMute(false, true, true), "MuteInGame=0 means no mute");
}

void TestVersion() {
    using namespace radiosplit;
    Expect(DetectSaEdition(0x53EC8B55u, 0) == SaEdition::Compact10US, "compact word");
    Expect(DetectSaEdition(0x16197BE9u, 0) == SaEdition::Hoodlum10US, "hoodlum word");
    Expect(DetectSaEdition(0x53EC8B55u, 0x458EA8u) == SaEdition::Steam, "steam entry point");
    Expect(DetectSaEdition(0x11111111u, 0) == SaEdition::Unsupported, "unknown word");
}

void TestVolume() {
    using namespace radiosplit;
    Expect(DecibelHundredthsToLinear(0) == 1.0f, "full scale");
    Expect(DecibelHundredthsToLinear(-10000) == 0.0f, "silence");
    Expect(DecibelHundredthsToLinear(100) == 1.0f, "above full clamps");
    const float sixDbDown = DecibelHundredthsToLinear(-600);
    Expect(std::fabs(sixDbDown - 0.501187f) < 0.001f, "-6 dB is about half");
}

void TestDeviceMatch() {
    using namespace radiosplit;
    const std::vector<std::string> names = {"Speakers (Realtek)", "CABLE Input (VB-Audio)", "Headphones"};
    Expect(MatchDevice(names, "") == -1, "empty device matches nothing");
    Expect(MatchDevice(names, "missing") == -1, "unknown device matches nothing");
    Expect(MatchDevice(names, "headphones") == 2, "exact match ignores case");
    Expect(MatchDevice(names, "VB-Audio") == 1, "substring match");
    Expect(MatchDevice(names, "Speakers (Realtek)") == 0, "exact beats a shorter substring");

    const std::vector<std::string> outputs = {
        "Realtek HD Audio 2nd output (Realtek(R) Audio)",
        "Lautsprecher (Atlas Air)",
        "Lautsprecher (Realtek(R) Audio)",
    };
    const std::vector<char> defaults = {0, 0, 1};
    Expect(ChooseSideDevice(outputs, defaults, "") == -1, "empty request asks Windows for the default");
    Expect(ChooseSideDevice(outputs, defaults, "Atlas") == 1, "explicit name still wins");
    Expect(ChooseSideDevice(outputs, defaults, "no such device") == -2, "unknown name is rejected");
}

void TestTrilogy() {
    using namespace radiosplit;
    constexpr std::uint32_t prologue = 0x53E58955u;
    Expect(DetectTrilogyEdition(prologue, 0, 0) == TrilogyEdition::V10, "1.0 prologue");
    Expect(DetectTrilogyEdition(0, prologue, 0) == TrilogyEdition::V11, "1.1 prologue");
    Expect(DetectTrilogyEdition(0, 0, prologue) == TrilogyEdition::Steam, "steam prologue");
    Expect(DetectTrilogyEdition(1, 2, 3) == TrilogyEdition::Unsupported, "unknown prologue");
}

void TestRadioFiles() {
    using namespace radiosplit;
    Expect(IsRadioStreamFile("C:\\Games\\GTA3\\audio\\HEAD.wav"), "III head radio");
    Expect(IsRadioStreamFile("C:\\Games\\GTA3\\audio\\Chat.WAV"), "III chatterbox");
    Expect(!IsRadioStreamFile("C:\\Games\\GTA3\\audio\\City.wav"), "III ambience stays");
    Expect(!IsRadioStreamFile("C:\\Games\\GTA3\\audio\\d1_stog.mp3"), "III cutscene stays");
    Expect(IsRadioStreamFile("C:\\Games\\VC\\MP3\\Wild.mp3"), "VC wildstyle");
    Expect(IsRadioStreamFile("AUDIO\\ESPANT.mp3"), "VC opens Espantoso as ESPANT.mp3");
    Expect(IsRadioStreamFile("AUDIO\\ESPANT.ADF"), "VC espantoso file on disk");
    Expect(!IsRadioStreamFile("AUDIO\\ESPANTOSO.MP3"), "menu title is not a stream name");
    Expect(IsRadioStreamFile("C:\\Games\\VC\\mp3\\my song.mp3"), "VC user track in the radio folder");
    Expect(!IsRadioStreamFile("C:\\Games\\VC\\MP3\\AMB_CITY.mp3"), "VC ambience name stays");
    Expect(!IsRadioStreamFile("C:\\Games\\VC\\audio\\ASS_1.mp3"), "VC cutscene stays");
    Expect(!IsRadioStreamFile("C:\\Games\\GTA3\\audio\\POLICE.wav"), "III police radio stays");
    Expect(!IsRadioStreamFile("C:\\Games\\VC\\audio\\TAXI.mp3"), "VC taxi radio stays");
    Expect(!IsRadioStreamFile(nullptr), "null path stays");

    Expect(ClassifyMilesStation("C:\\Games\\GTA3\\audio\\Chat.WAV") == MilesStation::Talk, "chatterbox is talk");
    Expect(ClassifyMilesStation("AUDIO\\KCHAT.mp3") == MilesStation::Talk, "k-chat is talk");
    Expect(ClassifyMilesStation("AUDIO\\VCPR.mp3") == MilesStation::Talk, "vcpr is talk");
    Expect(ClassifyMilesStation("AUDIO\\ESPANT.mp3") == MilesStation::Music, "espant is music");
    Expect(ClassifyMilesStation("C:\\Games\\GTA3\\audio\\HEAD.wav") == MilesStation::Music, "head is music");
    Expect(ClassifyMilesStation("C:\\Games\\VC\\MP3\\Wild.mp3") == MilesStation::Music, "named station in the mp3 folder stays music");
    Expect(ClassifyMilesStation("C:\\Games\\VC\\mp3\\my song.mp3") == MilesStation::Custom, "vc mp3 player file is custom");
    Expect(ClassifyMilesStation("C:\\Games\\GTA3\\MP3\\my song.mp3") == MilesStation::Custom, "iii mp3 player file is custom");
    Expect(ClassifyMilesStation("C:\\Games\\VC\\audio\\ASS_1.mp3") == MilesStation::None, "cutscene is not a station");
    Expect(ClassifyMilesStation("C:\\Games\\GTA3\\audio\\POLICE.wav") == MilesStation::None, "police radio is not a music station");
    Expect(ClassifyMilesStation("C:\\Games\\VC\\audio\\TAXI.mp3") == MilesStation::None, "taxi radio is not a music station");

    Expect(!CaptureMilesStation(MilesStation::Talk, true, false), "TalkRadio=1 stays in the game");
    Expect(CaptureMilesStation(MilesStation::Talk, false, false), "TalkRadio=0 goes to RadioSplit");
    Expect(CaptureMilesStation(MilesStation::Custom, true, false), "CustomRadio=0 goes to RadioSplit");
    Expect(!CaptureMilesStation(MilesStation::Custom, false, true), "CustomRadio=1 stays in the game");
    Expect(CaptureMilesStation(MilesStation::Music, true, true), "music always splits");
    Expect(!CaptureMilesStation(MilesStation::None, false, false), "other audio stays");
}

void TestStealLength() {
    using namespace radiosplit;
    const unsigned char prologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10};
    const unsigned char hotpatch[] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC};
    const unsigned char relativeCall[] = {0xE8, 0x00, 0x00, 0x00, 0x00};
    Expect(X86StealLength(prologue, 6, 5) == 6, "msvc prologue");
    Expect(X86StealLength(hotpatch, 5, 5) == 5, "hotpatch prologue");
    Expect(X86StealLength(relativeCall, 5, 5) == 0, "relative call is refused");
    const unsigned char miles[] = {0x66, 0xFF, 0x05, 0x34, 0xFA, 0x14, 0x21, 0x66, 0x83, 0x3D, 0x30, 0xFA, 0x14, 0x21, 0x00};
    Expect(X86StealLength(miles, 15, 5) == 7, "steam miles counter steals 7");
}

void TestImaAdpcm() {
    using namespace radiosplit;
    unsigned char block[16] = {
        0x00, 0x00, 0x00, 0x00,
        0x64, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    std::int16_t pcm[32] = {};
    Expect(DecodeImaAdpcmStereo(block, sizeof block, 16, pcm, 16) == 9, "one ima group is 9 frames");
    Expect(pcm[0] == 0 && pcm[1] == 100, "header predictors are the first frame");
    Expect(pcm[2] == 2 && pcm[3] == 100, "nibble 1 steps the left channel");
    Expect(pcm[16] == 2 && pcm[17] == 100, "later zero nibbles hold the predictor");

    block[2] = 200;
    Expect(DecodeImaAdpcmStereo(block, sizeof block, 16, pcm, 16) == 0, "an illegal index decodes nothing");

    std::vector<unsigned char> wide(1024, 0);
    std::vector<std::int16_t> widePcm((1024 - 7) * 2, 0);
    Expect(DecodeImaAdpcmStereo(wide.data(), wide.size(), 1024, widePcm.data(), 1024 - 7) == 1017,
        "1024-byte block is 1017 frames");
    wide[256 + 2] = 200;
    wide[512 + 2] = 200;
    Expect(DetectImaStereoBlockAlign(wide.data(), wide.size()) == 1024, "smaller aligns are rejected");

    std::vector<unsigned char> sized(512, 0);
    sized[256 + 2] = 200;
    Expect(DetectImaStereoBlockAlign(sized.data(), sized.size()) == 512, "smallest legal block align");
}

void TestRing() {
    radiosplit::ByteRing ring(8);
    const char* first = "abcd";
    ring.Write(first, 4);
    char out[8] = {};
    Expect(ring.Read(out, 4) == 4, "ring read size");
    Expect(std::string(out, 4) == "abcd", "ring read bytes");

    ring.Write("01234567", 8);
    ring.Write("WXYZ", 4);
    Expect(ring.Read(out, 8) == 8, "ring keeps the newest bytes");
    Expect(std::string(out, 8) == "4567WXYZ", "oldest bytes were dropped");
}

}  // namespace

int main() {
    TestGate();
    TestVersion();
    TestTrilogy();
    TestRadioFiles();
    TestStealLength();
    TestVolume();
    TestDeviceMatch();
    TestImaAdpcm();
    TestRing();
    if (g_failed != 0) {
        std::cerr << g_failed << " failed\n";
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}

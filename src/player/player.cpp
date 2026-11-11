// Separate process so the Windows volume mixer lists RadioSplit.exe on its own.
// An empty OutputDevice opens the default endpoint (a null WASAPI device id).
// The device is reopened when the game reports a new sample rate. A short
// cushion is kept so a station-change gap in the capture does not click.
#include "output/device_match.h"
#include "output/radio_shm.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#pragma warning(push)
#pragma warning(disable : 4100 4189 4244 4245 4389 4456 4505)
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_DSOUND
#define MA_NO_WINMM
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#pragma warning(pop)

namespace {

radiosplit::RadioShm* g_shm = nullptr;
HANDLE g_game = nullptr;
bool g_primed = false;

void CopyText(char* dst, std::size_t cap, const char* src) {
    if (cap == 0) {
        return;
    }
    std::size_t i = 0;
    if (src != nullptr) {
        for (; src[i] != '\0' && i + 1 < cap; ++i) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

void Fail(const char* text) {
    if (g_shm != nullptr) {
        CopyText(g_shm->playerError, sizeof g_shm->playerError, text);
        g_shm->playerStatus = radiosplit::kPlayerFailed;
    }
}

void DataCallback(ma_device* device, void* output, const void* input, ma_uint32 frameCount);

bool OpenPlayback(ma_device* device, ma_context* context, ma_device_id* deviceId, std::uint32_t rate, std::uint32_t channels) {
    g_primed = false;
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_s16;
    config.playback.channels = channels;
    config.playback.pDeviceID = deviceId;
    config.sampleRate = rate;
    config.periodSizeInMilliseconds = 20;
    config.dataCallback = DataCallback;
    config.pUserData = g_shm;
    if (ma_device_init(context, &config, device) != MA_SUCCESS) {
        return false;
    }
    if (ma_device_start(device) != MA_SUCCESS) {
        ma_device_uninit(device);
        return false;
    }
    return true;
}

bool GameGone() {
    if (g_game != nullptr && WaitForSingleObject(g_game, 0) == WAIT_OBJECT_0) {
        return true;
    }
    const std::uint32_t tick = g_shm->gameTick;
    return tick != 0 && GetTickCount() - tick > 3000;
}

void DataCallback(ma_device* device, void* output, const void*, ma_uint32 frameCount) {
    auto* shm = static_cast<radiosplit::RadioShm*>(device->pUserData);
    auto* dst = static_cast<std::int16_t*>(output);
    const std::uint32_t samples = frameCount * device->playback.channels;
    const std::uint32_t bytes = samples * sizeof(std::int16_t);
    const std::uint32_t rate = device->sampleRate == 0 ? 48000u : device->sampleRate;
    const std::uint32_t cushion = rate * device->playback.channels * static_cast<std::uint32_t>(sizeof(std::int16_t)) * 120u / 1000u;
    if (!g_primed) {
        if (radiosplit::RadioShmAvailable(shm) < cushion) {
            std::memset(dst, 0, bytes);
            return;
        }
        g_primed = true;
    }
    const std::uint32_t got = radiosplit::RadioShmRead(shm, dst, bytes);
    const std::uint32_t gotSamples = got / sizeof(std::int16_t);
    float gain = 1.0f;
    std::memcpy(&gain, &shm->gainBits, sizeof gain);
    if (gain < 0.0f) {
        gain = 0.0f;
    } else if (gain > 1.0f) {
        gain = 1.0f;
    }
    if (gain != 1.0f) {
        for (std::uint32_t i = 0; i < gotSamples; ++i) {
            const int scaled = static_cast<int>(static_cast<float>(dst[i]) * gain);
            if (scaled > 32767) {
                dst[i] = 32767;
            } else if (scaled < -32768) {
                dst[i] = -32768;
            } else {
                dst[i] = static_cast<std::int16_t>(scaled);
            }
        }
    }
    if (gotSamples < samples) {
        std::memset(dst + gotSamples, 0, (samples - gotSamples) * sizeof(std::int16_t));
    }
}

void MappingName(wchar_t* dst, std::size_t cap) {
    const wchar_t* found = wcsstr(GetCommandLineW(), L"Local\\RadioSplitAudio-");
    if (found == nullptr || cap == 0) {
        return;
    }
    std::size_t i = 0;
    for (; found[i] != L'\0' && found[i] != L' ' && found[i] != L'"' && i + 1 < cap; ++i) {
        dst[i] = found[i];
    }
    dst[i] = L'\0';
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    wchar_t mappingName[96] = L"Local\\RadioSplitAudio";
    MappingName(mappingName, 96);

    HANDLE mapping = nullptr;
    for (int attempt = 0; attempt < 200 && mapping == nullptr; ++attempt) {
        mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName);
        if (mapping == nullptr) {
            Sleep(10);
        }
    }
    if (mapping == nullptr) {
        return 1;
    }
    g_shm = static_cast<radiosplit::RadioShm*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(radiosplit::RadioShm)));
    if (g_shm == nullptr || g_shm->magic != radiosplit::kRadioShmMagic) {
        return 1;
    }
    if (g_shm->gamePid != 0) {
        g_game = OpenProcess(SYNCHRONIZE, FALSE, g_shm->gamePid);
    }

    while (g_shm->sampleRate == 0 || g_shm->channels == 0) {
        if (GameGone()) {
            return 0;
        }
        Sleep(10);
    }
    MemoryBarrier();

    ma_context context{};
    ma_device_id chosen{};
    ma_device_id* chosenId = nullptr;
    ma_context* contextPtr = nullptr;
    if (g_shm->requestName[0] != '\0') {
        if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) {
            Fail("playback device list failed");
            return 1;
        }
        contextPtr = &context;
        ma_device_info* playback = nullptr;
        ma_uint32 playbackCount = 0;
        if (ma_context_get_devices(&context, &playback, &playbackCount, nullptr, nullptr) != MA_SUCCESS) {
            Fail("playback device list failed");
            return 1;
        }
        std::vector<std::string> names;
        names.reserve(playbackCount);
        for (ma_uint32 i = 0; i < playbackCount; ++i) {
            names.emplace_back(playback[i].name);
        }
        const int index = radiosplit::MatchDevice(names, g_shm->requestName);
        if (index < 0) {
            Fail("OutputDevice was not found");
            return 1;
        }
        chosen = playback[index].id;
        chosenId = &chosen;
    }

    ma_device device{};
    std::uint32_t openedRate = g_shm->sampleRate;
    std::uint32_t openedChannels = g_shm->channels;
    if (!OpenPlayback(&device, contextPtr, chosenId, openedRate, openedChannels)) {
        Fail("playback device failed to open");
        return 1;
    }

    CopyText(g_shm->openedName, sizeof g_shm->openedName, device.playback.name);
    if (g_shm->openedName[0] == '\0') {
        CopyText(g_shm->openedName, sizeof g_shm->openedName, "Windows default");
    }
    g_shm->nameReady = 1;
    g_shm->playerStatus = radiosplit::kPlayerRunning;

    std::uint32_t rejectedRate = 0;
    for (;;) {
        g_shm->heartbeat = GetTickCount();
        if (GameGone()) {
            break;
        }
        const std::uint32_t wantRate = g_shm->sampleRate;
        const std::uint32_t wantChannels = g_shm->channels == 0 ? openedChannels : g_shm->channels;
        if (wantRate >= 8000 && wantRate != openedRate && wantRate != rejectedRate) {
            ma_device_stop(&device);
            ma_device_uninit(&device);
            if (OpenPlayback(&device, contextPtr, chosenId, wantRate, wantChannels)) {
                openedRate = wantRate;
                openedChannels = wantChannels;
                rejectedRate = 0;
            } else if (!OpenPlayback(&device, contextPtr, chosenId, openedRate, openedChannels)) {
                Fail("playback rate change failed");
                return 1;
            } else {
                rejectedRate = wantRate;
            }
        }
        Sleep(20);
    }

    ma_device_uninit(&device);
    if (contextPtr != nullptr) {
        ma_context_uninit(contextPtr);
    }
    return 0;
}

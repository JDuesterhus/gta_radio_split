#include "sa_radio.h"

#include "common/log.h"
#include "games/sa/version.h"
#include "games/miles/detour.h"
#include "output/device_match.h"
#include "output/radio_shm.h"
#include "output/volume.h"
#include "output/wasapi_out.h"
#include "split/gate.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <dsound.h>

#include <process.h>

#include "common/player_process.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace radiosplit {
namespace {

// San Andreas radio is one DirectSound buffer on the streaming channel.
// PCM is read from the play cursor, not from Unlock: the game often calls
// Unlock through a cached pointer, and the cursor is what is actually heard.
// The cursor is only skipped when it jumps farther than the time that passed
// (a station change). GetTickCount often stands still between 5 ms polls, so
// a same-tick movement is still real audio and must be kept.
// The game buffer stays at silence through a retune and until it has stopped
// after Radio Off. Restoring volume earlier plays a sliver of the old song.
// plugin-sdk, GTA SA 1.0 US (compact and hoodlum share these addresses).
// The Steam gta-sa.exe is ASLR'd and uses a different layout. Watch() fills
// the three globals from the running module base.
// CAEAudioHardware::m_pStreamingChannel is at +0xE10 on both.
// The audible stream is CAEAudioChannel::m_pDirectSoundBuffer at +0x8.
// +0x60090 on the channel is a looping silence buffer, not the radio.
// CAERadioTrackManager::m_nRadioStatus is the dword at +0x68.
// IsVehicleRadioActive ignores its this pointer and checks the player vehicle.
std::uintptr_t g_audioHardware = 0;
std::uintptr_t g_radioManager = 0;
std::uintptr_t g_isVehicleRadioActive = 0;
constexpr std::ptrdiff_t kStreamingChannelOffset = 0xE10;
constexpr std::ptrdiff_t kSoundBufferOffset = 0x8;
constexpr std::ptrdiff_t kRadioStatusOffset = 0x68;
// CAERadioTrackManager::m_RequestedSettings.StationID. This updates as soon as
// the player picks a station, including during the two-second retune gap.
constexpr std::ptrdiff_t kRequestedStationOffset = 0xAD;
constexpr int kSetVolumeIndex = 15;
constexpr int kVtableSlots = 32;

using SetVolumeFn = HRESULT(STDMETHODCALLTYPE*)(IDirectSoundBuffer*, LONG);
using UnlockFn = HRESULT(STDMETHODCALLTYPE*)(IDirectSoundBuffer*, void*, DWORD, void*, DWORD);

constexpr int kUnlockIndex = 19;

void* g_volumeTrampoline = nullptr;
void* g_unlockTrampoline = nullptr;
IDirectSoundBuffer* g_hookedBuffer = nullptr;
RadioShm* g_shm = nullptr;
wchar_t g_mappingName[96] = L"Local\\RadioSplitAudio";
bool g_detoursInstalled = false;
std::atomic<long> g_gameVolume{0};
std::atomic<bool> g_seenVolume{false};
std::atomic<bool> g_mute{false};
std::atomic<bool> g_split{false};
std::atomic<unsigned> g_capturedBytes{0};
std::atomic<unsigned> g_peak{0};
std::atomic<unsigned> g_unlockCalls{0};
std::atomic<unsigned> g_watchThread{0};
DWORD g_cursor = 0;
DWORD g_lastCaptureTick = 0;
DWORD g_pendingRate = 0;
int g_pendingRateTicks = 0;
bool g_haveCursor = false;
int g_blockAlign = 4;
int g_lockFailures = 0;

bool Readable(const void* address, std::size_t bytes) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof info) == 0) {
        return false;
    }
    if (info.State != MEM_COMMIT) {
        return false;
    }
    if ((info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    return at >= start && bytes <= info.RegionSize && at - start <= info.RegionSize - bytes;
}

HRESULT STDMETHODCALLTYPE SetVolumeHook(IDirectSoundBuffer* self, LONG volume) {
    // The detour sits on the shared DirectSound function. Only the radio buffer is silenced.
    if (self == g_hookedBuffer) {
        g_gameVolume.store(volume, std::memory_order_relaxed);
        g_seenVolume.store(true, std::memory_order_relaxed);
        if (g_mute.load(std::memory_order_acquire)) {
            volume = DSBVOLUME_MIN;
        }
    }
    auto* original = reinterpret_cast<SetVolumeFn>(g_volumeTrampoline);
    if (original == nullptr) {
        return S_OK;
    }
    return original(self, volume);
}

HRESULT STDMETHODCALLTYPE UnlockHook(IDirectSoundBuffer* self, void* first, DWORD firstBytes, void* second, DWORD secondBytes) {
    const bool ours = GetCurrentThreadId() == g_watchThread.load(std::memory_order_relaxed);
    if (!ours && self == g_hookedBuffer && g_split.load(std::memory_order_acquire)) {
        g_unlockCalls.fetch_add(1, std::memory_order_relaxed);
    }
    auto* original = reinterpret_cast<UnlockFn>(g_unlockTrampoline);
    if (original == nullptr) {
        return S_OK;
    }
    return original(self, first, firstBytes, second, secondBytes);
}

bool InstallBufferDetours(IDirectSoundBuffer* buffer) {
    if (buffer == nullptr || !Readable(buffer, sizeof(void*))) {
        return false;
    }
    g_hookedBuffer = buffer;
    if (g_detoursInstalled) {
        return g_volumeTrampoline != nullptr && g_unlockTrampoline != nullptr;
    }
    void** current = *reinterpret_cast<void***>(buffer);
    if (!Readable(current, sizeof(void*) * kVtableSlots)) {
        return false;
    }
    auto* rawVolume = current[kSetVolumeIndex];
    auto* rawUnlock = current[kUnlockIndex];
    const bool volumeOk = InstallStdcallDetour(rawVolume, reinterpret_cast<void*>(&SetVolumeHook), &g_volumeTrampoline);
    const bool unlockOk = InstallStdcallDetour(rawUnlock, reinterpret_cast<void*>(&UnlockHook), &g_unlockTrampoline);
    g_detoursInstalled = true;
    if (!volumeOk || !unlockOk) {
        unsigned char volumeBytes[8] = {};
        unsigned char unlockBytes[8] = {};
        if (Readable(rawVolume, sizeof volumeBytes)) {
            std::memcpy(volumeBytes, rawVolume, sizeof volumeBytes);
        }
        if (Readable(rawUnlock, sizeof unlockBytes)) {
            std::memcpy(unlockBytes, rawUnlock, sizeof unlockBytes);
        }
        Log("DirectSound detour failed. SetVolume %p %02X %02X %02X %02X %02X. Unlock %p %02X %02X %02X %02X %02X",
            rawVolume, volumeBytes[0], volumeBytes[1], volumeBytes[2], volumeBytes[3], volumeBytes[4],
            rawUnlock, unlockBytes[0], unlockBytes[1], unlockBytes[2], unlockBytes[3], unlockBytes[4]);
        return false;
    }
    Log("DirectSound hook installed");
    return true;
}

bool StartPlayer() {
    wchar_t gamePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, gamePath, MAX_PATH) == 0) {
        return false;
    }
    wchar_t* slash = nullptr;
    for (wchar_t* cursor = gamePath; *cursor != L'\0'; ++cursor) {
        if (*cursor == L'\\' || *cursor == L'/') {
            slash = cursor;
        }
    }
    if (slash == nullptr) {
        return false;
    }
    slash[1] = L'\0';
    wchar_t exePath[MAX_PATH] = {};
    lstrcpynW(exePath, gamePath, MAX_PATH);
    if (lstrlenW(exePath) + lstrlenW(L"RadioSplit.exe") >= MAX_PATH) {
        return false;
    }
    lstrcatW(exePath, L"RadioSplit.exe");

    wchar_t command[1024] = {};
    if (lstrlenW(exePath) + lstrlenW(g_mappingName) + 8 >= static_cast<int>(sizeof command / sizeof(command[0]))) {
        return false;
    }
    wsprintfW(command, L"\"%s\" %s", exePath, g_mappingName);

    bool detached = false;
    if (!SpawnDetachedPlayer(exePath, command, detached)) {
        Log("RadioSplit.exe failed to start (%lu)", GetLastError());
        return false;
    }
    char narrow[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, exePath, -1, narrow, static_cast<int>(sizeof narrow), nullptr, nullptr);
    Log("Started %s (%s)", narrow, detached ? "outside the game process tree" : "as a child of the game");
    return true;
}

RadioShm* CreateRadioShm() {
    wsprintfW(g_mappingName, L"Local\\RadioSplitAudio-%lu", GetCurrentProcessId());
    // The handle is intentionally kept open for the life of the game.
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(RadioShm), g_mappingName);
    if (mapping == nullptr) {
        Log("Radio shared memory failed (%lu)", GetLastError());
        return nullptr;
    }
    auto* shm = static_cast<RadioShm*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(RadioShm)));
    if (shm == nullptr) {
        return nullptr;
    }
    std::memset(shm, 0, sizeof(RadioShm));
    shm->capacity = kRadioShmCapacity;
    const float unity = 1.0f;
    std::memcpy(&shm->gainBits, &unity, sizeof unity);
    shm->magic = kRadioShmMagic;
    shm->gamePid = GetCurrentProcessId();
    shm->gameTick = GetTickCount();
    return shm;
}

void RestoreGameVolume() {
    auto* original = reinterpret_cast<SetVolumeFn>(g_volumeTrampoline);
    if (g_hookedBuffer != nullptr && original != nullptr && g_seenVolume.load(std::memory_order_relaxed)) {
        original(g_hookedBuffer, g_gameVolume.load(std::memory_order_relaxed));
    }
}

unsigned PcmPeak(const void* src, DWORD bytes) {
    const auto* samples = static_cast<const std::int16_t*>(src);
    const DWORD count = bytes / sizeof(std::int16_t);
    unsigned peak = 0;
    for (DWORD i = 0; i < count; i += 16) {
        int value = samples[i];
        if (value < 0) {
            value = -value;
        }
        if (static_cast<unsigned>(value) > peak) {
            peak = static_cast<unsigned>(value);
        }
    }
    return peak;
}

void NoteAudio(const void* src, DWORD bytes) {
    if (g_shm == nullptr || src == nullptr || bytes < sizeof(std::int16_t)) {
        return;
    }
    const unsigned peak = PcmPeak(src, bytes);
    unsigned seen = g_peak.load(std::memory_order_relaxed);
    while (peak > seen && !g_peak.compare_exchange_weak(seen, peak, std::memory_order_relaxed)) {
    }
    RadioShmWrite(g_shm, src, bytes);
    g_capturedBytes.fetch_add(bytes, std::memory_order_relaxed);
}

void CapturePlayedAudio(IDirectSoundBuffer* buffer) {
    if (g_shm == nullptr) {
        return;
    }
    DWORD status = 0;
    if (FAILED(buffer->GetStatus(&status)) || (status & DSBSTATUS_PLAYING) == 0) {
        g_haveCursor = false;
        g_lastCaptureTick = 0;
        return;
    }
    DSBCAPS caps{};
    caps.dwSize = sizeof caps;
    if (FAILED(buffer->GetCaps(&caps)) || caps.dwBufferBytes == 0) {
        return;
    }
    DWORD play = 0;
    DWORD write = 0;
    if (FAILED(buffer->GetCurrentPosition(&play, &write))) {
        return;
    }
    const DWORD size = caps.dwBufferBytes;
    const DWORD align = g_blockAlign > 0 ? static_cast<DWORD>(g_blockAlign) : 1u;
    const DWORD now = GetTickCount();
    if (!g_haveCursor) {
        g_cursor = play - (play % align);
        g_haveCursor = true;
        g_lastCaptureTick = now;
        return;
    }
    const DWORD elapsed = g_lastCaptureTick == 0 ? 0 : now - g_lastCaptureTick;
    g_lastCaptureTick = now;
    DWORD ahead = (play + size - g_cursor) % size;
    ahead -= ahead % align;
    if (ahead == 0) {
        return;
    }
    // GetTickCount often does not move between 5 ms polls. A same-tick read
    // still has new samples and has to be kept. Only a jump far past the time
    // that actually passed is a station change.
    const DWORD rate = g_shm->sampleRate >= 8000 ? g_shm->sampleRate : 48000u;
    const DWORD slack = rate * align / 5;
    const DWORD budget = elapsed * rate / 1000 * align + slack;
    if (ahead > slack && (elapsed == 0 || ahead > budget)) {
        g_cursor = play - (play % align);
        return;
    }
    void* first = nullptr;
    DWORD firstBytes = 0;
    void* second = nullptr;
    DWORD secondBytes = 0;
    if (FAILED(buffer->Lock(g_cursor, ahead, &first, &firstBytes, &second, &secondBytes, 0))) {
        ++g_lockFailures;
        return;
    }
    if (first != nullptr && firstBytes != 0) {
        NoteAudio(first, firstBytes);
    }
    if (second != nullptr && secondBytes != 0) {
        NoteAudio(second, secondBytes);
    }
    auto* original = reinterpret_cast<UnlockFn>(g_unlockTrampoline);
    if (original != nullptr) {
        original(buffer, first, firstBytes, second, secondBytes);
    } else {
        buffer->Unlock(first, firstBytes, second, secondBytes);
    }
    g_cursor = (g_cursor + firstBytes + secondBytes) % size;
}

struct StreamFormat {
    int sampleRate = 0;
    int channels = 0;
    int blockAlign = 0;
};

bool ReadStreamFormat(IDirectSoundBuffer* buffer, StreamFormat& format) {
    unsigned char raw[64] = {};
    DWORD written = 0;
    const HRESULT result = buffer->GetFormat(reinterpret_cast<WAVEFORMATEX*>(raw), sizeof raw, &written);
    if (FAILED(result) || written < sizeof(WAVEFORMATEX)) {
        return false;
    }
    const auto* wave = reinterpret_cast<WAVEFORMATEX*>(raw);
    if (wave->wFormatTag != WAVE_FORMAT_PCM || wave->wBitsPerSample != 16) {
        return false;
    }
    if (wave->nChannels != 1 && wave->nChannels != 2) {
        return false;
    }
    if (wave->nBlockAlign != wave->nChannels * 2 || wave->nSamplesPerSec == 0) {
        return false;
    }
    format.sampleRate = static_cast<int>(wave->nSamplesPerSec);
    format.channels = wave->nChannels;
    format.blockAlign = wave->nBlockAlign;
    return true;
}

// The buffer is created at 48000 Hz, then SetFrequency switches it to the
// track rate. The PCM in the buffer is at that playback rate.
void PublishPlaybackRate(IDirectSoundBuffer* buffer) {
    if (g_shm == nullptr || buffer == nullptr) {
        return;
    }
    DWORD freq = 0;
    if (FAILED(buffer->GetFrequency(&freq)) || freq < 8000 || freq > 96000) {
        return;
    }
    if (freq == g_shm->sampleRate) {
        g_pendingRate = 0;
        g_pendingRateTicks = 0;
        return;
    }
    // The first rate starts playback. Later changes have to sit still, because
    // a retune briefly reports another rate and restarting the device stutters.
    if (g_shm->sampleRate == 0) {
        g_shm->sampleRate = freq;
        Log("Radio playback rate: %lu Hz", freq);
        return;
    }
    if (freq != g_pendingRate) {
        g_pendingRate = freq;
        g_pendingRateTicks = 1;
        return;
    }
    if (++g_pendingRateTicks < 40) {
        return;
    }
    g_pendingRate = 0;
    g_pendingRateTicks = 0;
    g_shm->sampleRate = freq;
    Log("Radio playback rate: %lu Hz", freq);
}

IDirectSoundBuffer* StreamingBuffer() {
    if (!Readable(reinterpret_cast<void*>(g_audioHardware + kStreamingChannelOffset), sizeof(void*))) {
        return nullptr;
    }
    auto* channel = *reinterpret_cast<std::uint8_t**>(g_audioHardware + kStreamingChannelOffset);
    if (channel == nullptr || !Readable(channel + kSoundBufferOffset, sizeof(void*))) {
        return nullptr;
    }
    return *reinterpret_cast<IDirectSoundBuffer**>(channel + kSoundBufferOffset);
}

int RadioStatus() {
    const auto address = reinterpret_cast<void*>(g_radioManager + kRadioStatusOffset);
    if (!Readable(address, sizeof(int))) {
        return kRadioStopped;
    }
    return *static_cast<int*>(address);
}

bool VehicleRadioActive() {
    if (!Readable(reinterpret_cast<void*>(g_isVehicleRadioActive), 1)) {
        return false;
    }
    using Fn = bool(__thiscall*)(void*);
    const auto fn = reinterpret_cast<Fn>(g_isVehicleRadioActive);
    return fn(reinterpret_cast<void*>(g_radioManager));
}

int CurrentStation() {
    const auto address = reinterpret_cast<const char*>(g_radioManager + kRequestedStationOffset);
    if (!Readable(address, 1)) {
        return -1;
    }
    return *address;
}

struct CodeSpan {
    const unsigned char* data = nullptr;
    std::size_t size = 0;
};

std::uintptr_t CallTarget(const unsigned char* instruction) {
    std::int32_t relative = 0;
    std::memcpy(&relative, instruction + 1, sizeof relative);
    return reinterpret_cast<std::uintptr_t>(instruction + 5) + static_cast<std::uintptr_t>(relative);
}

std::uintptr_t ReadU32(const unsigned char* address) {
    std::uint32_t value = 0;
    std::memcpy(&value, address, sizeof value);
    return value;
}

void CollectCode(std::vector<CodeSpan>& spans) {
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
    if (base == 0 || !Readable(reinterpret_cast<void*>(base), sizeof(IMAGE_DOS_HEADER))) {
        return;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || !Readable(reinterpret_cast<const void*>(base + dos->e_lfanew), sizeof(IMAGE_NT_HEADERS))) {
        return;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const std::uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
    std::uintptr_t cursor = base;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof info) == 0) {
            break;
        }
        const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        const auto limit = regionEnd < end ? regionEnd : end;
        const DWORD protect = info.Protect & 0xFF;
        const bool executable = info.State == MEM_COMMIT &&
            (protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE ||
             protect == PAGE_EXECUTE_WRITECOPY);
        if (executable && limit > cursor) {
            spans.push_back(CodeSpan{reinterpret_cast<const unsigned char*>(cursor), static_cast<std::size_t>(limit - cursor)});
        }
        if (regionEnd <= cursor) {
            break;
        }
        cursor = regionEnd;
    }
}

bool IsVehicleRadio(const unsigned char* code) {
    return code[0] == 0xE8 && code[5] == 0x85 && code[6] == 0xC0 && code[7] == 0x74 && code[9] == 0x8A &&
        code[10] == 0x40 && code[11] == 0x1B && code[12] == 0x84 && code[13] == 0xC0 && code[14] == 0x74 &&
        code[16] == 0x3C && code[17] == 0x03 && code[18] == 0x74 && code[20] == 0x3C && code[21] == 0x02 &&
        code[22] == 0x75 && code[24] == 0xB0 && code[25] == 0x01 && code[26] == 0xC3 && code[27] == 0x32 &&
        code[28] == 0xC0 && code[29] == 0xC3;
}

bool IsRadioStatus(const unsigned char* code, unsigned char previous) {
    return (previous == 0xC3 || previous == 0xCC || previous == 0x90) && code[0] == 0x83 && code[1] == 0x79 &&
        code[2] == 0x68 && code[3] == 0x07 && code[4] == 0x75 && code[6] == 0x80 && code[7] == 0x39 && code[8] == 0x00;
}

std::uintptr_t MostCalledObject(const std::vector<CodeSpan>& spans, std::uintptr_t function) {
    std::uintptr_t keys[8] = {};
    int counts[8] = {};
    int used = 0;
    for (const CodeSpan& span : spans) {
        if (span.size < 10) {
            continue;
        }
        for (std::size_t i = 5; i + 5 <= span.size; ++i) {
            if (span.data[i] != 0xE8 || span.data[i - 5] != 0xB9) {
                continue;
            }
            if (CallTarget(span.data + i) != function) {
                continue;
            }
            const std::uintptr_t object = ReadU32(span.data + i - 4);
            int slot = 0;
            for (; slot < used; ++slot) {
                if (keys[slot] == object) {
                    break;
                }
            }
            if (slot == used) {
                if (used == 8) {
                    continue;
                }
                keys[used] = object;
                counts[used] = 1;
                ++used;
            } else {
                ++counts[slot];
            }
        }
    }
    int best = -1;
    for (int i = 0; i < used; ++i) {
        if (best < 0 || counts[i] > counts[best]) {
            best = i;
        }
    }
    return best < 0 ? 0 : keys[best];
}

std::uintptr_t FunctionStart(const unsigned char* data, std::size_t at) {
    const std::size_t begin = at > 0x9000 ? at - 0x9000 : 0;
    for (std::size_t i = at; i > begin + 3; --i) {
        if (data[i] != 0x55 || data[i + 1] != 0x8B || data[i + 2] != 0xEC) {
            continue;
        }
        const unsigned char previous = data[i - 1];
        if (previous == 0xC3 || previous == 0xCC || previous == 0x90 || previous == 0x00) {
            return reinterpret_cast<std::uintptr_t>(data + i);
        }
    }
    return 0;
}

bool FindSaCode(std::uintptr_t& hardware, std::uintptr_t& manager, std::uintptr_t& vehicleRadio) {
    std::vector<CodeSpan> spans;
    CollectCode(spans);
    hardware = 0;
    manager = 0;
    vehicleRadio = 0;
    int vehicleHits = 0;
    for (const CodeSpan& span : spans) {
        if (span.size < 32) {
            continue;
        }
        for (std::size_t i = 0; i + 30 <= span.size; ++i) {
            if (!IsVehicleRadio(span.data + i)) {
                continue;
            }
            vehicleRadio = reinterpret_cast<std::uintptr_t>(span.data + i);
            ++vehicleHits;
        }
        for (std::size_t i = 1; i + 9 <= span.size; ++i) {
            if (!IsRadioStatus(span.data + i, span.data[i - 1])) {
                continue;
            }
            const std::uintptr_t object = MostCalledObject(spans, reinterpret_cast<std::uintptr_t>(span.data + i));
            if (object != 0) {
                manager = object;
            }
        }
        for (std::size_t i = 1; i + 6 <= span.size; ++i) {
            const bool getter = span.data[i] == 0x8B && (span.data[i + 1] == 0x89 || span.data[i + 1] == 0x81) &&
                span.data[i + 2] == 0x10 && span.data[i + 3] == 0x0E && span.data[i + 4] == 0 && span.data[i + 5] == 0;
            const unsigned char previous = span.data[i - 1];
            if (!getter || (previous != 0xC3 && previous != 0xCC && previous != 0x90)) {
                continue;
            }
            const std::uintptr_t object = MostCalledObject(spans, reinterpret_cast<std::uintptr_t>(span.data + i));
            if (object != 0) {
                hardware = object;
            }
        }
    }
    if (hardware == 0) {
        for (const CodeSpan& span : spans) {
            if (span.size < 16) {
                continue;
            }
            for (std::size_t i = 0; i + 6 <= span.size; ++i) {
                if (span.data[i] != 0x89 || span.data[i + 1] != 0x86 || span.data[i + 2] != 0x10 || span.data[i + 3] != 0x0E ||
                    span.data[i + 4] != 0 || span.data[i + 5] != 0) {
                    continue;
                }
                const std::uintptr_t start = FunctionStart(span.data, i);
                if (start == 0) {
                    continue;
                }
                const std::uintptr_t object = MostCalledObject(spans, start);
                if (object != 0) {
                    hardware = object;
                }
            }
        }
    }
    return vehicleHits == 1 && hardware != 0 && manager != 0;
}

unsigned __stdcall Watch(void* param) {
    g_watchThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const Settings settings = *static_cast<Settings*>(param);
    delete static_cast<Settings*>(param);

    Log("CustomRadio=%d TalkRadio=%d", settings.keepCustomRadio ? 1 : 0, settings.keepTalkRadio ? 1 : 0);

    if (!settings.enable) {
        Log("Enable=0, stock mix left unchanged");
        return 0;
    }

    bool found = false;
    for (int attempt = 0; attempt < 150; ++attempt) {
        if (FindSaCode(g_audioHardware, g_radioManager, g_isVehicleRadioActive)) {
            found = true;
            break;
        }
        if (attempt == 0) {
            Log("Waiting for San Andreas code. Packed executables unpack after launch.");
        }
        Sleep(100);
    }
    if (!found) {
        Log("San Andreas radio code was not found. Stock mix left unchanged.");
        return 0;
    }
    Log("San Andreas radio: hardware %p manager %p vehicle %p", reinterpret_cast<void*>(g_audioHardware),
        reinterpret_cast<void*>(g_radioManager), reinterpret_cast<void*>(g_isVehicleRadioActive));

    SideOutput output;
    if (!output.Init()) {
        Log("WASAPI device list failed, stock mix left unchanged");
        return 0;
    }
    const std::vector<std::string>& names = output.PlaybackNames();
    Log("Playback devices:");
    for (std::size_t i = 0; i < names.size(); ++i) {
        Log("  [%u] %s%s", static_cast<unsigned>(i), names[i].c_str(),
            output.PlaybackDefaults()[i] != 0 ? " (default)" : "");
    }

    g_shm = CreateRadioShm();
    if (g_shm == nullptr) {
        return 0;
    }
    if (!settings.outputDevice.empty()) {
        std::size_t i = 0;
        for (; i + 1 < sizeof g_shm->requestName && i < settings.outputDevice.size(); ++i) {
            g_shm->requestName[i] = settings.outputDevice[i];
        }
        g_shm->requestName[i] = '\0';
    }
    if (!StartPlayer()) {
        Log("Radio stays in the game mix");
        return 0;
    }
    Log("Radio playback is a separate process. It appears in the volume mixer as RadioSplit.exe");

    IDirectSoundBuffer* hooked = nullptr;
    bool formatLogged = false;
    bool formatRejected = false;
    bool nameLogged = false;
    bool captureLogged = false;
    bool playerFailedLogged = false;
    bool splitWasOn = false;
    bool everHeard = false;
    int lastLoggedStation = -1;
    int silentTicks = 0;
    int stoppedWhilePlayingTicks = 0;

    for (;;) {
        Sleep(5);
        g_shm->gameTick = GetTickCount();
        if (!nameLogged && g_shm->nameReady != 0) {
            Log("Side device started: %s", g_shm->openedName);
            nameLogged = true;
        }
        if (!playerFailedLogged && g_shm->playerStatus == kPlayerFailed) {
            Log("RadioSplit.exe failed: %s", g_shm->playerError);
            playerFailedLogged = true;
        }
        IDirectSoundBuffer* buffer = StreamingBuffer();
        if (buffer == nullptr) {
            g_split.store(false, std::memory_order_release);
            continue;
        }
        if (buffer != hooked) {
            StreamFormat format;
            if (!ReadStreamFormat(buffer, format)) {
                if (!formatRejected) {
                    Log("Radio buffer is not 16-bit PCM, stock mix left unchanged");
                    formatRejected = true;
                }
                Sleep(500);
                continue;
            }
            if (!g_detoursInstalled) {
                const bool hookedFns = InstallBufferDetours(buffer);
                if (!hookedFns && !g_detoursInstalled) {
                    Sleep(50);
                    continue;
                }
            } else {
                g_hookedBuffer = buffer;
            }
            g_blockAlign = format.blockAlign;
            g_shm->channels = static_cast<std::uint32_t>(format.channels);
            MemoryBarrier();
            PublishPlaybackRate(buffer);
            if (g_shm->sampleRate == 0) {
                g_shm->sampleRate = static_cast<std::uint32_t>(format.sampleRate);
            }
            hooked = buffer;
            g_haveCursor = false;
            g_mute.store(false, std::memory_order_release);
            if (!formatLogged) {
                DSBCAPS caps{};
                caps.dwSize = sizeof caps;
                const DWORD bufferBytes = SUCCEEDED(buffer->GetCaps(&caps)) ? caps.dwBufferBytes : 0;
                Log("Radio buffer: %d Hz, %d channels, %lu bytes", format.sampleRate, format.channels, bufferBytes);
                formatLogged = true;
            }
        }

        PublishPlaybackRate(buffer);
        const int radioStatus = RadioStatus();
        const bool vehicle = VehicleRadioActive();
        const int station = CurrentStation();
        const bool stays = StationStaysInGame(station, settings.keepCustomRadio, settings.keepTalkRadio);
        const bool split = ShouldFeedRadio(radioStatus, vehicle, stays);
        g_split.store(split, std::memory_order_release);
        if (split) {
            CapturePlayedAudio(buffer);
        }

        const unsigned heartbeat = g_shm->heartbeat;
        const bool playerAlive = heartbeat != 0 && GetTickCount() - heartbeat < 1000;
        const bool haveAudio = g_peak.load(std::memory_order_relaxed) > 8;
        if (vehicle && haveAudio) {
            everHeard = true;
        }
        if (!vehicle) {
            everHeard = false;
        }
        const float gain = DecibelHundredthsToLinear(g_gameVolume.load(std::memory_order_relaxed));
        std::memcpy(&g_shm->gainBits, &gain, sizeof gain);

        // A retune drops the status off "playing" for about two seconds. The game
        // buffer stays silenced through that gap so it cannot bark the new song.
        // Turning the radio off restores volume only after that buffer has stopped,
        // otherwise the last bit of music comes out of the game.
        const bool hold = ShouldHoldGameMute(settings.muteInGame, playerAlive, vehicle, stays, everHeard);
        DWORD dsStatus = 0;
        const bool bufferPlaying = g_hookedBuffer != nullptr &&
            SUCCEEDED(g_hookedBuffer->GetStatus(&dsStatus)) && (dsStatus & DSBSTATUS_PLAYING) != 0;
        bool mute = ShouldKeepGameSilent(hold, g_mute.load(std::memory_order_relaxed), radioStatus, bufferPlaying);
        if (mute && !hold && radioStatus == kRadioStopped) {
            if (++stoppedWhilePlayingTicks > 40) {
                mute = false;
            }
        } else {
            stoppedWhilePlayingTicks = 0;
        }
        if (mute != g_mute.load(std::memory_order_relaxed)) {
            g_mute.store(mute, std::memory_order_release);
            if (!mute) {
                RestoreGameVolume();
            }
        }
        if (mute && g_hookedBuffer != nullptr) {
            auto* original = reinterpret_cast<SetVolumeFn>(g_volumeTrampoline);
            if (original != nullptr) {
                original(g_hookedBuffer, DSBVOLUME_MIN);
            } else {
                buffer->SetVolume(DSBVOLUME_MIN);
            }
        }

        if (!split && splitWasOn && g_shm != nullptr) {
            g_shm->readPos = g_shm->writePos;
            g_haveCursor = false;
        }
        if (split && !splitWasOn) {
            g_haveCursor = false;
        }
        if (vehicle && stays && station != lastLoggedStation && station != kStationOff && station >= 0) {
            Log("Station %d stays on the game speakers", station);
            lastLoggedStation = station;
        }

        if (split && !captureLogged && haveAudio) {
            Log("Capturing radio from the play cursor, peak %u, unlock calls %u",
                g_peak.load(std::memory_order_relaxed), g_unlockCalls.load(std::memory_order_relaxed));
            captureLogged = true;
        } else if (split && !captureLogged && ++silentTicks > 800) {
            Log("No radio samples captured. Unlock calls on the radio buffer: %u. Play-cursor reads failed: %d",
                g_unlockCalls.load(std::memory_order_relaxed), g_lockFailures);
            captureLogged = true;
        }

        if (split != splitWasOn) {
            Log(split ? "Radio split on" : "Radio split off");
            splitWasOn = split;
        }
    }
}

}  // namespace

void StartSaRadio(const Settings& settings) {
    auto* copy = new Settings(settings);
    const uintptr_t thread = _beginthreadex(nullptr, 0, Watch, copy, 0, nullptr);
    if (thread == 0) {
        delete copy;
        Log("Watcher thread failed to start");
        return;
    }
    CloseHandle(reinterpret_cast<HANDLE>(thread));
}

}  // namespace radiosplit

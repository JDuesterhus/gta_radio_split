// GTA III and Vice City play through Miles (mss32.dll), not DirectSound.
// A station is an AIL_open_stream file. Its bytes arrive later in
// AIL_load_sample_buffer, addressed by a sample handle rather than the stream.
// Retail Miles exports AIL_stream_sample_handle. The Steam mss32.dll does not,
// so the sample pointer has to be found inside the stream object.
// III station WAVs are IMA ADPCM. Vice City stations are MP3. Those bytes are
// decoded to 16-bit stereo here and then written to the same ring San Andreas
// uses. The compressed buffer is not wiped: Miles still has to play it, or the
// stream stalls. The game copy is silenced with AIL_set_stream_volume(0) only
// after decoded PCM is actually flowing.
#include "miles_radio.h"

#include "common/log.h"
#include "games/miles/detour.h"
#include "games/miles/ima_adpcm.h"
#include "games/miles/mp3_decode.h"
#include "games/miles/radio_file.h"
#include "output/device_match.h"
#include "output/radio_shm.h"
#include "output/wasapi_out.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <process.h>

#include "common/player_process.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace radiosplit {
namespace {

constexpr int kMaxStreams = 24;
constexpr unsigned kMusicModeGame = 1;
constexpr unsigned kIiiLastRadioTrack = 10;

using OpenStreamFn = void*(__stdcall*)(void* dig, const char* filename, int memory);
using CloseStreamFn = void(__stdcall*)(void* stream);
using StreamSampleFn = void*(__stdcall*)(void* stream);
using LoadBufferFn = void(__stdcall*)(void* sample, unsigned bufferNumber, const void* data, unsigned length);
using SetVolumeFn = void(__stdcall*)(void* stream, int volume);
using PauseStreamFn = void(__stdcall*)(void* stream, int paused);
using PlaybackRateFn = int(__stdcall*)(void* sample);
using StreamInfoFn = void(__stdcall*)(void* stream, int* datarate, int* sndtype, int* length, int* memory);

enum class Payload {
    Linear,
    ImaAdpcm,
    Mp3,
};

struct TrackedStream {
    void* stream = nullptr;
    void* sample = nullptr;
    bool radio = false;
    int volume = 127;
    Payload payload = Payload::Linear;
    bool described = false;
    int sndtype = -1;
    unsigned blockAlign = 0;
    // 0 unknown, 1 plaintext MP3, 2 MP3 with each byte XOR 0x22 (.adf), 3 not MP3.
    int mp3Mode = 0;
    Mp3Decoder* mp3 = nullptr;
};

OpenStreamFn g_openStream = nullptr;
CloseStreamFn g_closeStream = nullptr;
StreamSampleFn g_streamSample = nullptr;
LoadBufferFn g_loadBuffer = nullptr;
SetVolumeFn g_setStreamVolume = nullptr;
PauseStreamFn g_pauseStream = nullptr;
PlaybackRateFn g_playbackRate = nullptr;
PlaybackRateFn g_streamPlaybackRate = nullptr;
StreamInfoFn g_streamInfo = nullptr;

TrackedStream g_streams[kMaxStreams];
RadioShm* g_shm = nullptr;
wchar_t g_mappingName[96] = L"Local\\RadioSplitAudio";
std::uintptr_t g_iiiManager = 0;
std::atomic<bool> g_captured{false};
std::atomic<unsigned> g_lastRadioTick{0};
std::atomic<unsigned> g_openCount{0};
std::atomic<unsigned> g_radioOpenCount{0};
std::atomic<unsigned> g_otherOpenCount{0};
std::atomic<unsigned> g_loadCount{0};
std::atomic<unsigned> g_matchedLoads{0};
std::atomic<unsigned> g_copiedBytes{0};
int g_sampleOffset = -1;
bool g_muteInGame = true;
bool g_keepTalkRadio = true;
bool g_keepCustomRadio = false;
std::atomic<bool> g_dropRadio{false};
bool g_loggedFormat = false;
bool g_loggedOffset = false;
int g_decodeFailures = 0;

bool Readable(const void* address, std::size_t bytes) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof info) == 0) {
        return false;
    }
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    return at >= start && bytes <= info.RegionSize && at - start <= info.RegionSize - bytes;
}

bool IiiRadioIsOn() {
    if (g_iiiManager == 0 || !Readable(reinterpret_cast<void*>(g_iiiManager), 0x10)) {
        return false;
    }
    const auto* manager = reinterpret_cast<const unsigned char*>(g_iiiManager);
    if (manager[0] == 0) {
        return false;
    }
    const unsigned mode = manager[2];
    const unsigned track = manager[3];
    const unsigned inCar = manager[0xA];
    const unsigned announcement = manager[0xB];
    return mode == kMusicModeGame && inCar != 0 && announcement == 0 && track <= kIiiLastRadioTrack;
}

TrackedStream* FindStream(void* stream) {
    for (TrackedStream& entry : g_streams) {
        if (entry.stream == stream) {
            return &entry;
        }
    }
    return nullptr;
}

// Steam mss32 does not export the sample handle. The handle is a pointer stored
// somewhere in the stream object; the first load-buffer call discovers the offset.
int SampleOffsetInStream(void* stream, void* sample) {
    if (stream == nullptr || sample == nullptr || !Readable(stream, 0x400)) {
        return -1;
    }
    auto* base = static_cast<unsigned char*>(stream);
    for (int offset = 0; offset <= 0x400 - static_cast<int>(sizeof(void*)); offset += 4) {
        void* value = nullptr;
        std::memcpy(&value, base + offset, sizeof value);
        if (value == sample) {
            return offset;
        }
    }
    return -1;
}

TrackedStream* FindSample(void* sample) {
    if (sample == nullptr) {
        return nullptr;
    }
    for (TrackedStream& entry : g_streams) {
        // Only station streams. Learning the offset from a sound effect locks every
        // later station onto the wrong field.
        if (!entry.radio || entry.stream == nullptr) {
            continue;
        }
        if (entry.sample == nullptr && g_streamSample != nullptr) {
            entry.sample = g_streamSample(entry.stream);
        }
        if (entry.sample == nullptr && g_sampleOffset >= 0 && Readable(entry.stream, static_cast<std::size_t>(g_sampleOffset) + sizeof(void*))) {
            std::memcpy(&entry.sample, static_cast<unsigned char*>(entry.stream) + g_sampleOffset, sizeof entry.sample);
        }
        if (entry.sample == nullptr) {
            const int found = SampleOffsetInStream(entry.stream, sample);
            if (found >= 0) {
                g_sampleOffset = found;
                entry.sample = sample;
                if (!g_loggedOffset) {
                    Log("Miles sample pointer is at stream+%d", found);
                    g_loggedOffset = true;
                }
            }
        }
        if (entry.sample == sample) {
            return &entry;
        }
    }
    return nullptr;
}

void ReleaseDecoder(TrackedStream& entry) {
    DestroyMp3Decoder(entry.mp3);
    entry.mp3 = nullptr;
    entry.blockAlign = 0;
}

Payload PayloadForName(const char* filename) {
    if (filename == nullptr) {
        return Payload::Linear;
    }
    const char* dot = nullptr;
    for (const char* cursor = filename; *cursor != '\0'; ++cursor) {
        if (*cursor == '.') {
            dot = cursor;
        }
    }
    if (dot == nullptr) {
        return Payload::Linear;
    }
    char ext[8] = {};
    for (int i = 0; i < 7 && dot[i] != '\0'; ++i) {
        char ch = dot[i];
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
        ext[i] = ch;
    }
    if (std::strcmp(ext, ".mp3") == 0 || std::strcmp(ext, ".adf") == 0) {
        // Vice City stations are AUDIO\NAME.ADF. The file is an MP3 with every
        // byte XOR 0x22. Miles may hand us that ciphertext or the plaintext.
        return Payload::Mp3;
    }
    if (std::strcmp(ext, ".wav") == 0) {
        // The III station WAVs are IMA ADPCM, 32 kHz stereo, 1024-byte blocks.
        return Payload::ImaAdpcm;
    }
    return Payload::Linear;
}

void RememberStream(void* stream, bool radio, const char* filename) {
    TrackedStream* slot = FindStream(stream);
    if (slot == nullptr) {
        for (TrackedStream& entry : g_streams) {
            if (entry.stream == nullptr) {
                slot = &entry;
                break;
            }
        }
    }
    if (slot == nullptr) {
        slot = &g_streams[0];
    }
    ReleaseDecoder(*slot);
    slot->stream = stream;
    slot->sample = g_streamSample != nullptr ? g_streamSample(stream) : nullptr;
    slot->radio = radio;
    slot->volume = 127;
    slot->payload = PayloadForName(filename);
    slot->described = false;
    slot->sndtype = -1;
    slot->mp3Mode = 0;
    if (radio) {
        g_dropRadio.store(false, std::memory_order_release);
    }
}

bool OtherRadioOpen(void* except) {
    for (const TrackedStream& entry : g_streams) {
        if (entry.stream != nullptr && entry.stream != except && entry.radio) {
            return true;
        }
    }
    return false;
}

// Miles submits the next buffer before that audio is heard. Pause closes the
// stream, and the ring would otherwise keep playing that buffer. San Andreas
// never queues ahead of the play cursor, so the same pause is silent there.
void DropQueuedRadio() {
    g_dropRadio.store(true, std::memory_order_release);
    if (g_shm != nullptr) {
        g_shm->readPos = g_shm->writePos;
    }
}

void ForgetStream(void* stream) {
    if (TrackedStream* entry = FindStream(stream)) {
        const bool radio = entry->radio;
        ReleaseDecoder(*entry);
        *entry = {};
        if (radio && !OtherRadioOpen(nullptr)) {
            DropQueuedRadio();
        }
    }
}

void Silence(const void* data, unsigned length) {
    if (data == nullptr || length == 0) {
        return;
    }
    DWORD oldProtect = 0;
    auto* bytes = const_cast<void*>(data);
    if (VirtualProtect(bytes, length, PAGE_READWRITE, &oldProtect) == 0) {
        return;
    }
    std::memset(bytes, 0, length);
    DWORD ignored = 0;
    VirtualProtect(bytes, length, oldProtect, &ignored);
}

void Describe(TrackedStream& entry) {
    if (entry.described || g_streamInfo == nullptr || entry.stream == nullptr) {
        return;
    }
    int datarate = 0;
    int sndtype = -1;
    int length = 0;
    int memory = 0;
    g_streamInfo(entry.stream, &datarate, &sndtype, &length, &memory);
    entry.sndtype = sndtype;
    entry.described = true;
    Log("Miles stream info %p: datarate=%d sndtype=%d length=%d memory=%d payload=%d",
        entry.stream, datarate, sndtype, length, memory, static_cast<int>(entry.payload));
}

int Mp3FrameBytes(const unsigned char* data, unsigned available) {
    if (data == nullptr || available < 4 || data[0] != 0xFF || (data[1] & 0xE0) != 0xE0) {
        return 0;
    }
    const int version = (data[1] >> 3) & 3;
    const int layer = (data[1] >> 1) & 3;
    if (version == 1 || layer == 0) {
        return 0;
    }
    const int bitrateIndex = (data[2] >> 4) & 0xF;
    const int rateIndex = (data[2] >> 2) & 3;
    const int padding = (data[2] >> 1) & 1;
    if (bitrateIndex == 0 || bitrateIndex == 15 || rateIndex == 3) {
        return 0;
    }
    static const int kRate[4][3] = {
        {11025, 12000, 8000},
        {0, 0, 0},
        {22050, 24000, 16000},
        {44100, 48000, 32000},
    };
    static const int kBitrate[2][3][16] = {
        {
            {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},
            {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
            {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},
        },
        {
            {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
            {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
            {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
        },
    };
    const int rate = kRate[version][rateIndex];
    if (rate == 0) {
        return 0;
    }
    const int family = version == 3 ? 0 : 1;
    const int layerRow = layer == 1 ? 0 : (layer == 2 ? 1 : 2);
    const int kbps = kBitrate[family][layerRow][bitrateIndex];
    if (kbps == 0) {
        return 0;
    }
    int frame = 0;
    if (layer == 3) {
        frame = (12 * kbps * 1000 / rate + padding) * 4;
    } else if (version == 3) {
        frame = 144 * kbps * 1000 / rate + padding;
    } else {
        frame = 72 * kbps * 1000 / rate + padding;
    }
    return frame >= 4 ? frame : 0;
}

bool LooksLikeMp3(const unsigned char* data, unsigned length) {
    if (data == nullptr || length < 8) {
        return false;
    }
    const unsigned limit = length < 4096u ? length : 4096u;
    for (unsigned i = 0; i + 4 < limit; ++i) {
        const int frame = Mp3FrameBytes(data + i, limit - i);
        if (frame < 4) {
            continue;
        }
        const unsigned next = i + static_cast<unsigned>(frame);
        if (next + 4 <= length && Mp3FrameBytes(data + next, length - next) > 0) {
            return true;
        }
    }
    return false;
}

bool LooksLikeXorMp3(const unsigned char* data, unsigned length) {
    if (data == nullptr || length < 800) {
        return false;
    }
    const unsigned n = length < 4096u ? length : 4096u;
    std::vector<unsigned char> plain(n);
    for (unsigned i = 0; i < n; ++i) {
        plain[i] = static_cast<unsigned char>(data[i] ^ 0x22);
    }
    return LooksLikeMp3(plain.data(), n);
}

bool LinearToStereo(const unsigned char* data, unsigned length, int sndtype, std::vector<std::int16_t>& pcm) {
    if (data == nullptr || sndtype < 0 || sndtype > 3 || length == 0) {
        return false;
    }
    const bool stereo = (sndtype & 2) != 0;
    const bool bits16 = (sndtype & 1) != 0;
    const unsigned frameBytes = (stereo ? 2u : 1u) * (bits16 ? 2u : 1u);
    const unsigned frames = length / frameBytes;
    if (frames == 0) {
        return false;
    }
    pcm.reserve(frames * 2);
    for (unsigned i = 0; i < frames; ++i) {
        const unsigned char* src = data + static_cast<std::size_t>(i) * frameBytes;
        std::int16_t left = 0;
        std::int16_t right = 0;
        if (!bits16 && !stereo) {
            left = right = static_cast<std::int16_t>((static_cast<int>(src[0]) - 128) << 8);
        } else if (!bits16) {
            left = static_cast<std::int16_t>((static_cast<int>(src[0]) - 128) << 8);
            right = static_cast<std::int16_t>((static_cast<int>(src[1]) - 128) << 8);
        } else if (!stereo) {
            left = right = static_cast<std::int16_t>(src[0] | (src[1] << 8));
        } else {
            left = static_cast<std::int16_t>(src[0] | (src[1] << 8));
            right = static_cast<std::int16_t>(src[2] | (src[3] << 8));
        }
        pcm.push_back(left);
        pcm.push_back(right);
    }
    return true;
}

int PlaybackRate(const TrackedStream& entry, void* sample, int decodedRate) {
    if (decodedRate >= 8000 && decodedRate <= 96000) {
        return decodedRate;
    }
    int rate = 0;
    if (g_streamPlaybackRate != nullptr && entry.stream != nullptr) {
        rate = g_streamPlaybackRate(entry.stream);
    }
    if ((rate < 8000 || rate > 96000) && g_playbackRate != nullptr) {
        rate = g_playbackRate(sample);
    }
    if (rate < 8000 || rate > 96000) {
        rate = entry.payload == Payload::ImaAdpcm ? 32000 : 44100;
    }
    return rate;
}

void PublishRate(int rate) {
    if (g_shm == nullptr || rate < 8000 || rate > 96000) {
        return;
    }
    const auto published = static_cast<std::uint32_t>(rate);
    if (g_shm->sampleRate == published) {
        return;
    }
    if (g_shm->sampleRate != 0) {
        g_shm->readPos = g_shm->writePos;
    }
    g_shm->channels = 2;
    MemoryBarrier();
    g_shm->sampleRate = published;
}

void* __stdcall OpenStreamHook(void* dig, const char* filename, int memory) {
    void* stream = g_openStream(dig, filename, memory);
    if (stream != nullptr) {
        // Filename only. IiiRadioIsOn() is true for every stream opened while the
        // radio is up, which pulled speech (lib_a.wav and the rest) into the station.
        const MilesStation kind = ClassifyMilesStation(filename);
        const bool radio = CaptureMilesStation(kind, g_keepTalkRadio, g_keepCustomRadio);
        RememberStream(stream, radio, filename);
        g_openCount.fetch_add(1, std::memory_order_relaxed);
        const char* name = filename != nullptr ? filename : "(no name)";
        if (radio) {
            g_radioOpenCount.fetch_add(1, std::memory_order_relaxed);
            Log("Miles stream %p %s (radio)", stream, name);
            if (g_iiiManager != 0 && Readable(reinterpret_cast<void*>(g_iiiManager), 0x10)) {
                const auto* manager = reinterpret_cast<const unsigned char*>(g_iiiManager);
                Log("III music manager init=%u mode=%u track=%u inCar=%u announcement=%u",
                    manager[0], manager[2], manager[3], manager[0xA], manager[0xB]);
            }
        } else if (kind == MilesStation::Talk || kind == MilesStation::Custom) {
            Log("Miles stream %s (left in game, %s)", name, kind == MilesStation::Talk ? "talk" : "custom");
        } else {
            const unsigned count = g_otherOpenCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count <= 8 || count % 200 == 0) {
                Log("Miles stream %s (left in game, %u so far)", name, count);
            }
        }
    }
    return stream;
}

void __stdcall CloseStreamHook(void* stream) {
    ForgetStream(stream);
    g_closeStream(stream);
}

void __stdcall PauseStreamHook(void* stream, int paused) {
    if (TrackedStream* entry = FindStream(stream)) {
        if (entry->radio) {
            if (paused != 0) {
                DropQueuedRadio();
            } else {
                g_dropRadio.store(false, std::memory_order_release);
            }
        }
    }
    g_pauseStream(stream, paused);
}

void __stdcall SetStreamVolumeHook(void* stream, int volume) {
    if (TrackedStream* entry = FindStream(stream)) {
        if (volume < 0) {
            volume = 0;
        }
        entry->volume = volume;
        const unsigned heartbeat = g_shm != nullptr ? g_shm->heartbeat : 0;
        const bool playerLive = heartbeat != 0 && GetTickCount() - heartbeat < 1000 && g_captured.load(std::memory_order_relaxed);
        if (entry->radio && g_muteInGame && playerLive) {
            g_setStreamVolume(stream, 0);
            return;
        }
    }
    g_setStreamVolume(stream, volume);
}

void __stdcall LoadBufferHook(void* sample, unsigned bufferNumber, const void* data, unsigned length) {
    const unsigned loads = g_loadCount.fetch_add(1, std::memory_order_relaxed) + 1;
    TrackedStream* entry = FindSample(sample);
    if (entry == nullptr && (loads <= 6 || loads % 400 == 0)) {
        unsigned radioStreams = 0;
        for (const TrackedStream& tracked : g_streams) {
            if (tracked.stream != nullptr && tracked.radio) {
                ++radioStreams;
            }
        }
        Log("Miles load-buffer #%u sample %p bytes %u buffer %u matched no stream (%u radio streams, sample offset %d)",
            loads, sample, length, bufferNumber, radioStreams, g_sampleOffset);
    }
    if (entry != nullptr && entry->radio && data != nullptr && length != 0 && g_shm != nullptr) {
        g_lastRadioTick.store(GetTickCount(), std::memory_order_relaxed);
        Describe(*entry);
        const auto* bytes = static_cast<const unsigned char*>(data);
        std::vector<std::int16_t> pcm;
        int decodedRate = 0;
        const char* kind = nullptr;
        bool linear = false;
        bool waiting = false;
        if (entry->payload == Payload::Mp3) {
            if (entry->mp3Mode == 0) {
                if (LooksLikeMp3(bytes, length)) {
                    entry->mp3Mode = 1;
                } else if (LooksLikeXorMp3(bytes, length)) {
                    entry->mp3Mode = 2;
                } else if (length >= 800) {
                    entry->mp3Mode = 3;
                }
            }
            const unsigned char* src = bytes;
            std::vector<unsigned char> plain;
            if (entry->mp3Mode == 2) {
                plain.resize(length);
                for (unsigned i = 0; i < length; ++i) {
                    plain[i] = static_cast<unsigned char>(bytes[i] ^ 0x22);
                }
                src = plain.data();
            }
            if (entry->mp3Mode == 1 || entry->mp3Mode == 2) {
                if (entry->mp3 == nullptr) {
                    entry->mp3 = CreateMp3Decoder();
                }
                bool needMore = false;
                const std::size_t frames = FeedMp3Decoder(entry->mp3, src, length, pcm, decodedRate, needMore);
                if (frames > 0) {
                    kind = entry->mp3Mode == 2 ? "ADF MP3" : "MP3";
                } else if (needMore) {
                    waiting = true;
                } else {
                    DestroyMp3Decoder(entry->mp3);
                    entry->mp3 = nullptr;
                    entry->mp3Mode = 0;
                }
            }
            if (kind == nullptr && !waiting && entry->mp3Mode == 3 && entry->sndtype >= 0 && entry->sndtype <= 3) {
                if (LinearToStereo(bytes, length, entry->sndtype, pcm)) {
                    kind = "linear";
                    linear = true;
                }
            }
        } else {
            unsigned align = entry->blockAlign;
            if (align == 0 || length % align != 0) {
                const unsigned detected = DetectImaStereoBlockAlign(bytes, length);
                if (detected != 0) {
                    align = detected;
                    entry->blockAlign = detected;
                }
            }
            if (align != 0) {
                const std::size_t frames = (static_cast<std::size_t>(length) / align) * (align - 7u);
                pcm.resize(frames * 2);
                const std::size_t got = DecodeImaAdpcmStereo(bytes, length, align, pcm.data(), frames);
                if (got > 0) {
                    pcm.resize(got * 2);
                    kind = "IMA ADPCM";
                } else {
                    pcm.clear();
                }
            }
            if (kind == nullptr && entry->sndtype >= 0 && entry->sndtype <= 3) {
                pcm.clear();
                if (LinearToStereo(bytes, length, entry->sndtype, pcm)) {
                    kind = "linear";
                    linear = true;
                }
            }
        }
        if (kind == nullptr) {
            if (!waiting && g_decodeFailures < 4) {
                ++g_decodeFailures;
                const unsigned b0 = length > 0 ? bytes[0] : 0;
                const unsigned b1 = length > 1 ? bytes[1] : 0;
                const unsigned b2 = length > 2 ? bytes[2] : 0;
                const unsigned b3 = length > 3 ? bytes[3] : 0;
                Log("Miles radio decode produced nothing (sndtype %d payload %d bytes %u first %02X %02X %02X %02X). Game mix left audible",
                    entry->sndtype, static_cast<int>(entry->payload), length, b0, b1, b2, b3);
            }
        } else {
            const int rate = PlaybackRate(*entry, sample, decodedRate);
            PublishRate(rate);
            const float gain = static_cast<float>(entry->volume) / 127.0f;
            std::memcpy(&g_shm->gainBits, &gain, sizeof gain);
            const auto pcmBytes = static_cast<std::uint32_t>(pcm.size() * sizeof(std::int16_t));
            if (g_dropRadio.load(std::memory_order_acquire)) {
                g_shm->readPos = g_shm->writePos;
            } else {
                RadioShmWrite(g_shm, pcm.data(), pcmBytes);
                g_copiedBytes.fetch_add(pcmBytes, std::memory_order_relaxed);
            }
            g_matchedLoads.fetch_add(1, std::memory_order_relaxed);
            g_captured.store(true, std::memory_order_relaxed);
            if (!g_loggedFormat) {
                Log("Miles radio decoded: %u source bytes -> %u pcm bytes, %d Hz, %s", length, pcmBytes, rate, kind);
                g_loggedFormat = true;
            }
            const unsigned heartbeat = g_shm->heartbeat;
            const bool playerLive = heartbeat != 0 && GetTickCount() - heartbeat < 1000;
            if (g_muteInGame && playerLive) {
                // The game often sets the stream volume once, before any PCM exists.
                // Force it again here once the side player is up. Leave compressed
                // bytes alone: wiping an MP3 buffer makes Miles stop asking for more.
                if (g_setStreamVolume != nullptr) {
                    g_setStreamVolume(entry->stream, 0);
                }
                if (linear) {
                    Silence(data, length);
                }
            }
        }
    }
    g_loadBuffer(sample, bufferNumber, data, length);
}

void* Export(HMODULE library, const char* decorated, const char* plain) {
    if (void* symbol = GetProcAddress(library, decorated)) {
        return symbol;
    }
    return GetProcAddress(library, plain);
}

bool HookExport(HMODULE library, const char* decorated, const char* plain, void* hook, void** original) {
    void* target = Export(library, decorated, plain);
    if (target == nullptr) {
        Log("mss32.dll has no %s", plain);
        return false;
    }
    unsigned char bytes[8] = {};
    if (Readable(target, sizeof bytes)) {
        std::memcpy(bytes, target, sizeof bytes);
    }
    if (!InstallStdcallDetour(target, hook, original)) {
        Log("Could not hook %s at %p (%02X %02X %02X %02X %02X %02X %02X %02X)", plain, target,
            bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7]);
        return false;
    }
    Log("Hooked %s at %p", plain, target);
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

unsigned __stdcall Watch(void* param) {
    const Settings settings = *static_cast<Settings*>(param);
    delete static_cast<Settings*>(param);
    g_muteInGame = settings.muteInGame;
    g_keepTalkRadio = settings.keepTalkRadio;
    g_keepCustomRadio = settings.keepCustomRadio;
    Log("CustomRadio=%d TalkRadio=%d", g_keepCustomRadio ? 1 : 0, g_keepTalkRadio ? 1 : 0);

    if (!settings.enable) {
        Log("Enable=0, stock mix left unchanged");
        return 0;
    }

    HMODULE miles = nullptr;
    for (int attempt = 0; attempt < 600 && miles == nullptr; ++attempt) {
        miles = GetModuleHandleA("mss32.dll");
        Sleep(100);
    }
    if (miles == nullptr) {
        Log("mss32.dll did not load, stock mix left unchanged");
        return 0;
    }

    SideOutput output;
    if (!output.Init()) {
        Log("WASAPI device list failed, stock mix left unchanged");
        return 0;
    }
    Log("Playback devices:");
    for (std::size_t i = 0; i < output.PlaybackNames().size(); ++i) {
        Log("  [%u] %s%s", static_cast<unsigned>(i), output.PlaybackNames()[i].c_str(),
            output.PlaybackDefaults()[i] != 0 ? " (default)" : "");
    }
    const int deviceIndex = ChooseSideDevice(output.PlaybackNames(), output.PlaybackDefaults(), settings.outputDevice);
    if (deviceIndex < -1) {
        Log("OutputDevice unmatched (\"%s\"). Stock mix left unchanged", settings.outputDevice.c_str());
        return 0;
    }
    g_shm = CreateRadioShm();
    if (g_shm == nullptr) {
        return 0;
    }
    if (deviceIndex >= 0) {
        const std::string& name = output.PlaybackNames()[static_cast<std::size_t>(deviceIndex)];
        std::size_t i = 0;
        for (; i + 1 < sizeof g_shm->requestName && i < name.size(); ++i) {
            g_shm->requestName[i] = name[i];
        }
        g_shm->requestName[i] = '\0';
        Log("Side output: %s", name.c_str());
    } else {
        Log("OutputDevice is empty. Radio opens the Windows default playback device");
    }
    if (!StartPlayer()) {
        Log("Radio stays in the game mix");
        return 0;
    }

    if (!HookExport(miles, "_AIL_open_stream@12", "AIL_open_stream", reinterpret_cast<void*>(&OpenStreamHook),
            reinterpret_cast<void**>(&g_openStream)) ||
        !HookExport(miles, "_AIL_close_stream@4", "AIL_close_stream", reinterpret_cast<void*>(&CloseStreamHook),
            reinterpret_cast<void**>(&g_closeStream)) ||
        !HookExport(miles, "_AIL_load_sample_buffer@16", "AIL_load_sample_buffer", reinterpret_cast<void*>(&LoadBufferHook),
            reinterpret_cast<void**>(&g_loadBuffer))) {
        Log("Miles hooks failed, stock mix left unchanged");
        return 0;
    }
    g_streamSample = reinterpret_cast<StreamSampleFn>(Export(miles, "_AIL_stream_sample_handle@4", "AIL_stream_sample_handle"));
    g_setStreamVolume = reinterpret_cast<SetVolumeFn>(Export(miles, "_AIL_set_stream_volume@8", "AIL_set_stream_volume"));
    g_playbackRate = reinterpret_cast<PlaybackRateFn>(Export(miles, "_AIL_sample_playback_rate@4", "AIL_sample_playback_rate"));
    g_streamPlaybackRate = reinterpret_cast<PlaybackRateFn>(Export(miles, "_AIL_stream_playback_rate@4", "AIL_stream_playback_rate"));
    g_streamInfo = reinterpret_cast<StreamInfoFn>(Export(miles, "_AIL_stream_info@20", "AIL_stream_info"));
    Log("Miles exports: stream_sample_handle=%p set_stream_volume=%p sample_playback_rate=%p stream_playback_rate=%p stream_info=%p",
        static_cast<void*>(g_streamSample), static_cast<void*>(g_setStreamVolume), static_cast<void*>(g_playbackRate),
        static_cast<void*>(g_streamPlaybackRate), static_cast<void*>(g_streamInfo));
    if (g_streamSample == nullptr) {
        // Steam mss32 omits this export. Load-buffer matching falls back to a scan of the stream object.
        Log("Miles has no AIL_stream_sample_handle. Sample pointers will be searched inside each stream");
    }
    if (g_setStreamVolume != nullptr) {
        void* original = nullptr;
        if (HookExport(miles, "_AIL_set_stream_volume@8", "AIL_set_stream_volume", reinterpret_cast<void*>(&SetStreamVolumeHook),
                &original)) {
            g_setStreamVolume = reinterpret_cast<SetVolumeFn>(original);
        }
    }
    if (Export(miles, "_AIL_pause_stream@8", "AIL_pause_stream") != nullptr) {
        // The detour stores the trampoline before the patch is live.
        HookExport(miles, "_AIL_pause_stream@8", "AIL_pause_stream", reinterpret_cast<void*>(&PauseStreamHook),
            reinterpret_cast<void**>(&g_pauseStream));
    } else {
        Log("Miles has no AIL_pause_stream. Queued radio is dropped when the stream closes");
    }

    Log("Miles radio tap installed");
    bool nameLogged = false;
    int statTicks = 0;
    for (;;) {
        Sleep(10);
        g_shm->gameTick = GetTickCount();
        if (!nameLogged && g_shm->nameReady != 0) {
            Log("Side device started: %s (player status %u)", g_shm->openedName, g_shm->playerStatus);
            nameLogged = true;
        }
        if (++statTicks >= 200) {
            statTicks = 0;
            Log("Miles stats: opens=%u radio=%u other=%u loads=%u matched=%u copied=%u rate=%u heartbeat=%u nameReady=%u player=%u",
                g_openCount.load(std::memory_order_relaxed), g_radioOpenCount.load(std::memory_order_relaxed),
                g_otherOpenCount.load(std::memory_order_relaxed), g_loadCount.load(std::memory_order_relaxed),
                g_matchedLoads.load(std::memory_order_relaxed), g_copiedBytes.load(std::memory_order_relaxed),
                g_shm->sampleRate, g_shm->heartbeat, g_shm->nameReady, g_shm->playerStatus);
            if (g_shm->playerStatus == kPlayerFailed) {
                Log("RadioSplit.exe failed: %s", g_shm->playerError);
            }
        }
    }
}

}  // namespace

void StartMilesRadio(const Settings& settings, const char* gameName, std::uintptr_t iiiManager) {
    g_iiiManager = iiiManager;
    Log("Host: %s", gameName);
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

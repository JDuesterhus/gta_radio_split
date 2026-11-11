#include "wasapi_out.h"

#include "games/miles/mp3_decode.h"
#include "split/byte_ring.h"
#include "volume.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#pragma warning(push)
#pragma warning(disable : 4100 4189 4244 4245 4389 4456 4505)
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_DSOUND
#define MA_NO_WINMM
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#pragma warning(pop)

namespace radiosplit {
namespace {

constexpr std::size_t kRingBytes = 256 * 1024;

struct OutputState {
    ByteRing ring{kRingBytes};
    std::atomic<unsigned> gainBits{0x3f800000u};
    std::atomic<bool> accept{false};
};

void DataCallback(ma_device* device, void* output, const void*, ma_uint32 frameCount) {
    auto* state = static_cast<OutputState*>(device->pUserData);
    auto* dst = static_cast<std::int16_t*>(output);
    const std::size_t samples = static_cast<std::size_t>(frameCount) * device->playback.channels;
    const std::size_t bytes = samples * sizeof(std::int16_t);
    float gain = 0.0f;
    const unsigned gainBits = state->gainBits.load(std::memory_order_relaxed);
    std::memcpy(&gain, &gainBits, sizeof gain);
    if (!state->accept.load(std::memory_order_acquire) || gain <= 0.0f) {
        std::memset(dst, 0, bytes);
        return;
    }
    const std::size_t got = state->ring.Read(dst, bytes);
    const std::size_t gotSamples = got / sizeof(std::int16_t);
    if (gain != 1.0f) {
        for (std::size_t i = 0; i < gotSamples; ++i) {
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

}  // namespace

struct SideOutput::Ma {
    ma_context context{};
    ma_device device{};
    ma_device_id deviceId{};
    bool contextReady = false;
    bool deviceReady = false;
    OutputState state;
    std::vector<ma_device_id> ids;
};

SideOutput::SideOutput() : ma_(new Ma) {}

SideOutput::~SideOutput() {
    Stop();
    if (ma_ != nullptr && ma_->contextReady) {
        ma_context_uninit(&ma_->context);
    }
    delete ma_;
}

bool SideOutput::Init() {
    ma_context_config config = ma_context_config_init();
    if (ma_context_init(nullptr, 0, &config, &ma_->context) != MA_SUCCESS) {
        return false;
    }
    ma_->contextReady = true;

    ma_device_info* playback = nullptr;
    ma_uint32 playbackCount = 0;
    if (ma_context_get_devices(&ma_->context, &playback, &playbackCount, nullptr, nullptr) != MA_SUCCESS) {
        return false;
    }
    names_.reserve(playbackCount);
    ma_->ids.reserve(playbackCount);
    for (ma_uint32 i = 0; i < playbackCount; ++i) {
        names_.emplace_back(playback[i].name);
        defaults_.push_back(playback[i].isDefault ? 1 : 0);
        ma_->ids.push_back(playback[i].id);
    }
    return true;
}

bool SideOutput::Start(int deviceIndex, int sampleRate, int channels) {
    if (running_ && sampleRate_ == sampleRate && channels_ == channels) {
        return true;
    }
    Stop();
    if (deviceIndex >= static_cast<int>(ma_->ids.size())) {
        return false;
    }
    if (sampleRate <= 0 || (channels != 1 && channels != 2)) {
        return false;
    }

    ma_device_id* deviceId = nullptr;
    if (deviceIndex >= 0) {
        ma_->deviceId = ma_->ids[static_cast<std::size_t>(deviceIndex)];
        deviceId = &ma_->deviceId;
    }
    ma_->state.accept.store(false, std::memory_order_release);
    ma_->state.ring.Clear();

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_s16;
    config.playback.channels = static_cast<ma_uint32>(channels);
    config.playback.pDeviceID = deviceId;
    config.playback.shareMode = ma_share_mode_shared;
    config.sampleRate = static_cast<ma_uint32>(sampleRate);
    config.periodSizeInMilliseconds = 20;
    config.dataCallback = DataCallback;
    config.pUserData = &ma_->state;

    if (ma_device_init(&ma_->context, &config, &ma_->device) != MA_SUCCESS) {
        return false;
    }
    ma_->deviceReady = true;
    if (ma_device_start(&ma_->device) != MA_SUCCESS) {
        ma_device_uninit(&ma_->device);
        ma_->deviceReady = false;
        return false;
    }
    ma_->state.accept.store(true, std::memory_order_release);
    openedName_ = ma_->device.playback.name;
    running_ = true;
    sampleRate_ = sampleRate;
    channels_ = channels;
    return true;
}

void SideOutput::Stop() {
    if (ma_ == nullptr) {
        return;
    }
    ma_->state.accept.store(false, std::memory_order_release);
    if (ma_->deviceReady) {
        ma_device_stop(&ma_->device);
        ma_device_uninit(&ma_->device);
        ma_->deviceReady = false;
    }
    ma_->state.ring.Clear();
    running_ = false;
}

void SideOutput::SetLinearGain(float gain) {
    if (gain < 0.0f) {
        gain = 0.0f;
    } else if (gain > 1.0f) {
        gain = 1.0f;
    }
    unsigned bits = 0;
    std::memcpy(&bits, &gain, sizeof bits);
    ma_->state.gainBits.store(bits, std::memory_order_relaxed);
}

void SideOutput::SetVolumeHundredths(long volumeHundredths) {
    SetLinearGain(DecibelHundredthsToLinear(volumeHundredths));
}

void SideOutput::Push(const void* src, std::size_t bytes) {
    if (running_) {
        ma_->state.ring.Write(src, bytes);
    }
}

void SideOutput::Clear() {
    ma_->state.ring.Clear();
}

struct Mp3Decoder {
    ma_dr_mp3dec dec{};
    std::vector<unsigned char> pending;
};

Mp3Decoder* CreateMp3Decoder() {
    auto* decoder = new Mp3Decoder();
    ma_dr_mp3dec_init(&decoder->dec);
    return decoder;
}

void DestroyMp3Decoder(Mp3Decoder* decoder) {
    delete decoder;
}

std::size_t FeedMp3Decoder(Mp3Decoder* decoder, const unsigned char* data, std::size_t size,
    std::vector<std::int16_t>& pcm, int& sampleRate, bool& needMore) {
    needMore = false;
    if (decoder == nullptr || data == nullptr || size == 0) {
        return 0;
    }
    decoder->pending.insert(decoder->pending.end(), data, data + size);
    std::size_t produced = 0;
    for (int guard = 0; guard < 256 && decoder->pending.size() > 4; ++guard) {
        ma_dr_mp3dec_frame_info info{};
        std::int16_t frame[1152 * 2];
        // The failed call wipes the decoder. Put it back when the only problem
        // is a frame split across two Miles buffers, so the next chunk continues
        // the song instead of starting over.
        const ma_dr_mp3dec saved = decoder->dec;
        const int samples = ma_dr_mp3dec_decode_frame(&decoder->dec, decoder->pending.data(),
            static_cast<int>(decoder->pending.size()), frame, &info);
        if (samples <= 0) {
            if (info.frame_bytes > 0 && static_cast<std::size_t>(info.frame_bytes) < decoder->pending.size()) {
                decoder->pending.erase(decoder->pending.begin(), decoder->pending.begin() + info.frame_bytes);
                continue;
            }
            if (produced > 0) {
                decoder->dec = saved;
            } else {
                decoder->pending.clear();
            }
            break;
        }
        if (info.frame_bytes <= 0 || static_cast<std::size_t>(info.frame_bytes) > decoder->pending.size()) {
            break;
        }
        decoder->pending.erase(decoder->pending.begin(), decoder->pending.begin() + info.frame_bytes);
        if (samples <= 0 || samples > 1152 || (info.channels != 1 && info.channels != 2)) {
            continue;
        }
        sampleRate = info.hz;
        for (int i = 0; i < samples; ++i) {
            const std::int16_t left = frame[i * info.channels];
            const std::int16_t right = info.channels == 1 ? left : frame[i * info.channels + 1];
            pcm.push_back(left);
            pcm.push_back(right);
        }
        produced += static_cast<std::size_t>(samples);
    }
    if (decoder->pending.size() > 256u * 1024u) {
        decoder->pending.erase(decoder->pending.begin(), decoder->pending.end() - 64 * 1024);
    }
    needMore = produced == 0 && !decoder->pending.empty();
    return produced;
}

}  // namespace radiosplit

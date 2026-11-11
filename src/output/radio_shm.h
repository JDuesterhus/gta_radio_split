// One shared-memory ring per game process. The ASI writes 16-bit PCM.
// RadioSplit.exe reads it and plays it, which is what makes Windows show a
// second volume-mixer entry. An in-process stream stays inside gta_sa.exe.
// sampleRate is published last. The player waits for it, then opens the device.
// gameTick and heartbeat are millisecond clocks used to notice a dead peer.
#pragma once

#include <cstdint>
#include <cstring>

namespace radiosplit {

constexpr std::uint32_t kRadioShmMagic = 0x31534452u;
constexpr std::uint32_t kRadioShmCapacity = 1024u * 1024u;
constexpr wchar_t kRadioShmName[] = L"Local\\RadioSplitAudio";

struct RadioShm {
    std::uint32_t magic;
    std::uint32_t sampleRate;
    std::uint32_t channels;
    std::uint32_t gainBits;
    std::uint32_t writePos;
    std::uint32_t readPos;
    std::uint32_t heartbeat;
    std::uint32_t capacity;
    std::uint32_t gamePid;
    std::uint32_t nameReady;
    std::uint32_t gameTick;
    std::uint32_t playerStatus;
    char playerError[160];
    char openedName[256];
    char requestName[256];
    unsigned char data[kRadioShmCapacity];
};

constexpr std::uint32_t kPlayerRunning = 1;
constexpr std::uint32_t kPlayerFailed = 2;

inline void RadioShmWrite(RadioShm* shm, const void* src, std::uint32_t bytes) {
    if (shm == nullptr || src == nullptr || bytes == 0) {
        return;
    }
    const std::uint32_t capacity = shm->capacity;
    if (capacity == 0) {
        return;
    }
    const auto* input = static_cast<const unsigned char*>(src);
    if (bytes > capacity) {
        input += bytes - capacity;
        bytes = capacity;
    }
    const std::uint32_t write = shm->writePos;
    const std::uint32_t used = write - shm->readPos;
    if (used + bytes > capacity) {
        shm->readPos = write + bytes - capacity;
    }
    const std::uint32_t offset = write % capacity;
    const std::uint32_t first = bytes < (capacity - offset) ? bytes : (capacity - offset);
    std::memcpy(shm->data + offset, input, first);
    if (bytes > first) {
        std::memcpy(shm->data, input + first, bytes - first);
    }
    shm->writePos = write + bytes;
}

inline std::uint32_t RadioShmAvailable(const RadioShm* shm) {
    if (shm == nullptr || shm->capacity == 0) {
        return 0;
    }
    const std::uint32_t read = shm->readPos;
    const std::uint32_t write = shm->writePos;
    if (write - read > shm->capacity) {
        return shm->capacity;
    }
    return write - read;
}

inline std::uint32_t RadioShmRead(RadioShm* shm, void* dst, std::uint32_t bytes) {
    if (shm == nullptr || dst == nullptr || bytes == 0) {
        return 0;
    }
    const std::uint32_t capacity = shm->capacity;
    std::uint32_t read = shm->readPos;
    const std::uint32_t write = shm->writePos;
    if (write - read > capacity) {
        read = write - capacity;
    }
    const std::uint32_t have = write - read;
    const std::uint32_t take = bytes < have ? bytes : have;
    if (take == 0) {
        shm->readPos = read;
        return 0;
    }
    auto* output = static_cast<unsigned char*>(dst);
    const std::uint32_t offset = read % capacity;
    const std::uint32_t first = take < (capacity - offset) ? take : (capacity - offset);
    std::memcpy(output, shm->data + offset, first);
    if (take > first) {
        std::memcpy(output + first, shm->data, take - first);
    }
    shm->readPos = read + take;
    return take;
}

}  // namespace radiosplit

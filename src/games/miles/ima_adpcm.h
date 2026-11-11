#pragma once

// Microsoft IMA ADPCM, the format of the GTA III station WAVs (WAVE tag 17).
// A stereo block starts with an 8-byte header (predictor, index, reserved for
// each channel) and then groups of 8 bytes: 4 left, 4 right, low nibble first.
// Block align is 1024 on most stations and 2048 on Game FM. One block produces
// blockAlign - 7 stereo frames.

#include <cstddef>
#include <cstdint>

namespace radiosplit {

inline int ClampImaSample(int sample) {
    if (sample > 32767) {
        return 32767;
    }
    if (sample < -32768) {
        return -32768;
    }
    return sample;
}

inline int DecodeImaNibble(int& predictor, int& index, int nibble) {
    static const int kIndexTable[16] = {
        -1, -1, -1, -1, 2, 4, 6, 8,
        -1, -1, -1, -1, 2, 4, 6, 8,
    };
    static const int kStepTable[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
        19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
        130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
        337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
        2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
        5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
        15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
    };
    const int step = kStepTable[index];
    // Microsoft IMA WAV: ((2 * delta + 1) * step) >> 3. The bit-add form drifts off these files.
    const int delta = nibble & 7;
    int diff = ((2 * delta + 1) * step) >> 3;
    if ((nibble & 8) != 0) {
        diff = -diff;
    }
    predictor = ClampImaSample(predictor + diff);
    index += kIndexTable[nibble & 15];
    if (index < 0) {
        index = 0;
    } else if (index > 88) {
        index = 88;
    }
    return predictor;
}

// Smallest align in {256, 512, 1024, 2048, 4096} whose every block header has a
// legal index and a zero reserved byte. A larger multiple of the real align also
// looks legal, so the smallest match is the file's align.
inline unsigned DetectImaStereoBlockAlign(const unsigned char* data, std::size_t size) {
    const unsigned candidates[] = {256, 512, 1024, 2048, 4096};
    if (data == nullptr) {
        return 0;
    }
    for (unsigned align : candidates) {
        if (size < align || size % align != 0) {
            continue;
        }
        bool legal = true;
        for (std::size_t offset = 0; offset < size; offset += align) {
            const unsigned char* block = data + offset;
            if (block[2] > 88 || block[3] != 0 || block[6] > 88 || block[7] != 0) {
                legal = false;
                break;
            }
        }
        if (legal) {
            return align;
        }
    }
    return 0;
}

// Writes interleaved s16 stereo. Returns the number of frames written.
// Stops at the first block whose index byte is illegal.
inline std::size_t DecodeImaAdpcmStereo(const unsigned char* data, std::size_t size, unsigned blockAlign,
    std::int16_t* dst, std::size_t dstFrames) {
    if (data == nullptr || dst == nullptr || blockAlign < 16 || size < blockAlign || dstFrames == 0) {
        return 0;
    }
    std::size_t written = 0;
    for (std::size_t offset = 0; offset + blockAlign <= size && written < dstFrames; offset += blockAlign) {
        const unsigned char* block = data + offset;
        if (block[2] > 88 || block[6] > 88) {
            break;
        }
        int predLeft = static_cast<std::int16_t>(block[0] | (block[1] << 8));
        int indexLeft = block[2];
        int predRight = static_cast<std::int16_t>(block[4] | (block[5] << 8));
        int indexRight = block[6];
        dst[written * 2] = static_cast<std::int16_t>(predLeft);
        dst[written * 2 + 1] = static_cast<std::int16_t>(predRight);
        ++written;
        const unsigned char* cursor = block + 8;
        const unsigned char* end = block + blockAlign;
        while (cursor + 8 <= end && written < dstFrames) {
            int left[8];
            int right[8];
            for (int i = 0; i < 4; ++i) {
                const int byte = cursor[i];
                left[i * 2] = DecodeImaNibble(predLeft, indexLeft, byte & 0x0F);
                left[i * 2 + 1] = DecodeImaNibble(predLeft, indexLeft, byte >> 4);
            }
            for (int i = 0; i < 4; ++i) {
                const int byte = cursor[4 + i];
                right[i * 2] = DecodeImaNibble(predRight, indexRight, byte & 0x0F);
                right[i * 2 + 1] = DecodeImaNibble(predRight, indexRight, byte >> 4);
            }
            const std::size_t room = dstFrames - written;
            const std::size_t count = room < 8 ? room : 8;
            for (std::size_t i = 0; i < count; ++i) {
                dst[(written + i) * 2] = static_cast<std::int16_t>(left[i]);
                dst[(written + i) * 2 + 1] = static_cast<std::int16_t>(right[i]);
            }
            written += count;
            cursor += 8;
        }
    }
    return written;
}

}  // namespace radiosplit

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace radiosplit {

struct Mp3Decoder;

Mp3Decoder* CreateMp3Decoder();
void DestroyMp3Decoder(Mp3Decoder* decoder);

// Appends interleaved 16-bit stereo frames. A partial frame stays in the decoder.
// needMore is set when bytes were kept because they do not form a frame yet.
std::size_t FeedMp3Decoder(Mp3Decoder* decoder, const unsigned char* data, std::size_t size,
    std::vector<std::int16_t>& pcm, int& sampleRate, bool& needMore);

}  // namespace radiosplit

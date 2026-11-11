#pragma once

namespace radiosplit {

// DirectSound volume is hundredths of a decibel, from -10000 (silence) to 0 (full).
float DecibelHundredthsToLinear(long volumeHundredths);

}  // namespace radiosplit

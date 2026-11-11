#include "volume.h"

#include <cmath>

namespace radiosplit {

float DecibelHundredthsToLinear(long volumeHundredths) {
    if (volumeHundredths >= 0) {
        return 1.0f;
    }
    if (volumeHundredths <= -10000) {
        return 0.0f;
    }
    return std::pow(10.0f, static_cast<float>(volumeHundredths) / 2000.0f);
}

}  // namespace radiosplit

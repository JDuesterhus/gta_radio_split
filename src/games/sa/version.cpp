#include "version.h"

namespace radiosplit {

SaEdition DetectSaEdition(std::uint32_t wordAtImage1000, std::uint32_t entryRva) {
    if (entryRva == 0x458EA8u) {
        return SaEdition::Steam;
    }
    if (wordAtImage1000 == 0x16197BE9u) {
        return SaEdition::Hoodlum10US;
    }
    if (wordAtImage1000 == 0x53EC8B55u) {
        return SaEdition::Compact10US;
    }
    return SaEdition::Unsupported;
}

const char* SaEditionName(SaEdition edition) {
    switch (edition) {
    case SaEdition::Compact10US:
        return "GTA SA 1.0 US compact";
    case SaEdition::Hoodlum10US:
        return "GTA SA 1.0 US hoodlum";
    case SaEdition::Steam:
        return "GTA SA Steam";
    case SaEdition::Unsupported:
        return "unsupported executable";
    }
    return "unsupported executable";
}

TrilogyEdition DetectTrilogyEdition(std::uint32_t word10, std::uint32_t word11, std::uint32_t wordSteam) {
    constexpr std::uint32_t kPrologue = 0x53E58955u;
    if (word10 == kPrologue) {
        return TrilogyEdition::V10;
    }
    if (word11 == kPrologue) {
        return TrilogyEdition::V11;
    }
    if (wordSteam == kPrologue) {
        return TrilogyEdition::Steam;
    }
    return TrilogyEdition::Unsupported;
}

const char* TrilogyEditionName(TrilogyEdition edition) {
    switch (edition) {
    case TrilogyEdition::V10:
        return "1.0";
    case TrilogyEdition::V11:
        return "1.1";
    case TrilogyEdition::Steam:
        return "Steam";
    case TrilogyEdition::Unsupported:
        return "unsupported executable";
    }
    return "unsupported executable";
}

}  // namespace radiosplit

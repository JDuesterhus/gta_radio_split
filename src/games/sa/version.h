#pragma once

#include <cstdint>

namespace radiosplit {

enum class SaEdition {
    Unsupported,
    Compact10US,
    Hoodlum10US,
    Steam,
};

enum class TrilogyEdition {
    Unsupported,
    V10,
    V11,
    Steam,
};

// wordAtImage1000 is the dword at RVA 0x1000 (VA 0x401000 when the image base is 0x400000).
// Compact 1.0 US is the push-ebp prologue. Hoodlum 1.0 US is a jump into the extra section.
// The current Steam gta-sa.exe has the same prologue and is ASLR-relocated. Its
// AddressOfEntryPoint is 0x458EA8, which is what separates it from compact 1.0.
SaEdition DetectSaEdition(std::uint32_t wordAtImage1000, std::uint32_t entryRva);

const char* SaEditionName(SaEdition edition);

// The three words are the dwords at the 1.0, 1.1, and Steam prologue sites.
// A match is the MSVC prologue 0x53E58955.
TrilogyEdition DetectTrilogyEdition(std::uint32_t word10, std::uint32_t word11, std::uint32_t wordSteam);

const char* TrilogyEditionName(TrilogyEdition edition);

}  // namespace radiosplit

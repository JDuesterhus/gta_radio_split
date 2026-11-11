// 5-byte jmp over a prologue this file can copy into a trampoline.
// San Andreas DirectSound starts with the usual MSVC or hotpatch bytes.
// Steam mss32 starts with `66 FF 05 abs` (inc word ptr [counter]), which is
// an absolute address and stays valid when copied. A relative call is refused
// because moving it would retarget it.
#include "detour.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace radiosplit {
namespace {

int ModRmLength(const unsigned char* code, int available) {
    if (available < 1) {
        return -1;
    }
    const unsigned char modrm = code[0];
    const unsigned mod = modrm >> 6;
    const unsigned rm = modrm & 7;
    int length = 1;
    unsigned char sib = 0;
    if (mod != 3 && rm == 4) {
        if (available < length + 1) {
            return -1;
        }
        sib = code[length];
        ++length;
    }
    if (mod == 1) {
        length += 1;
    } else if (mod == 2) {
        length += 4;
    } else if (mod == 0 && rm == 5) {
        length += 4;
    } else if (mod == 0 && rm == 4 && (sib & 7) == 5) {
        length += 4;
    }
    return length <= available ? length : -1;
}

int OneInstruction(const unsigned char* code, int available) {
    if (available < 1) {
        return -1;
    }
    const unsigned char op = code[0];
    // Steam mss32 prefixes its exports with an operand-size override.
    if (op == 0x66) {
        const int inner = OneInstruction(code + 1, available - 1);
        return inner > 0 ? inner + 1 : -1;
    }
    if ((op >= 0x50 && op <= 0x5F) || op == 0x90) {
        return 1;
    }
    if (op == 0xFF) {
        const int modrm = ModRmLength(code + 1, available - 1);
        return modrm > 0 ? 1 + modrm : -1;
    }
    if (op == 0x6A) {
        return available >= 2 ? 2 : -1;
    }
    if (op == 0x68) {
        return available >= 5 ? 5 : -1;
    }
    if (op == 0x89 || op == 0x8B || op == 0x8D) {
        const int modrm = ModRmLength(code + 1, available - 1);
        return modrm > 0 ? 1 + modrm : -1;
    }
    if (op == 0x83 || op == 0x81) {
        const int modrm = ModRmLength(code + 1, available - 1);
        if (modrm <= 0) {
            return -1;
        }
        const int extra = op == 0x83 ? 1 : 4;
        return 1 + modrm + extra <= available ? 1 + modrm + extra : -1;
    }
    return -1;
}

}  // namespace

int X86StealLength(const unsigned char* code, int available, int minimum) {
    if (code == nullptr || minimum < 1) {
        return 0;
    }
    int total = 0;
    while (total < minimum) {
        const int size = OneInstruction(code + total, available - total);
        if (size <= 0) {
            return 0;
        }
        total += size;
    }
    return total;
}

bool InstallStdcallDetour(void* target, void* hook, void** trampoline) {
    if (target == nullptr || hook == nullptr || trampoline == nullptr) {
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(target);
    const int steal = X86StealLength(bytes, 16, 5);
    if (steal < 5 || steal > 16) {
        return false;
    }

    auto* gate = static_cast<unsigned char*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (gate == nullptr) {
        return false;
    }
    std::memcpy(gate, bytes, static_cast<std::size_t>(steal));
    gate[steal] = 0xE9;
    const auto resume = reinterpret_cast<std::uintptr_t>(bytes + steal);
    const auto resumeFrom = reinterpret_cast<std::uintptr_t>(gate + steal + 5);
    const auto back = static_cast<std::int32_t>(resume - resumeFrom);
    std::memcpy(gate + steal + 1, &back, sizeof back);
    // Publish the trampoline before any other thread can jump into the hook.
    *trampoline = gate;
    MemoryBarrier();

    DWORD oldProtect = 0;
    if (VirtualProtect(bytes, static_cast<std::size_t>(steal), PAGE_EXECUTE_READWRITE, &oldProtect) == 0) {
        VirtualFree(gate, 0, MEM_RELEASE);
        *trampoline = nullptr;
        return false;
    }
    unsigned char patch[16] = {};
    patch[0] = 0xE9;
    const auto hookRel = static_cast<std::int32_t>(
        reinterpret_cast<std::uintptr_t>(hook) - reinterpret_cast<std::uintptr_t>(bytes + 5));
    std::memcpy(patch + 1, &hookRel, sizeof hookRel);
    for (int i = 5; i < steal; ++i) {
        patch[i] = 0x90;
    }
    std::memcpy(bytes, patch, static_cast<std::size_t>(steal));
    VirtualProtect(bytes, static_cast<std::size_t>(steal), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), bytes, static_cast<std::size_t>(steal));
    return true;
}

}  // namespace radiosplit

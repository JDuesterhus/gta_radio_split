#pragma once

namespace radiosplit {

// How many leading bytes can be copied into a trampoline. 0 means the prologue
// is not one this plugin knows how to relocate.
int X86StealLength(const unsigned char* code, int available, int minimum);

bool InstallStdcallDetour(void* target, void* hook, void** trampoline);

}  // namespace radiosplit

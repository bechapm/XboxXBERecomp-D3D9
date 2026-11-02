#pragma once
#include <cstdint>

#include "cpu/x86_context.h"

// guest addresses for host code (cpu/dispatch.cpp)
namespace dispatch {
// a guest address that calls fn when the guest calls it through a pointer (call reg, call [mem]): a game's own screen
// classes, callbacks and function tables can hold host functions. The same fn gives the same address.
uint32_t HostThunk(X86Func* fn);
X86Func* Find(uint32_t guest);  // the recompiled function at that address, or null
}  // namespace dispatch

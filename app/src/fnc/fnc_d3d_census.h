// Stage 2 diagnostic: the Direct3D census of FNC (cvar fnc_d3d_census). A census build hooks every function of FNC's
// Direct3D range (tools/fnc_d3d_census_build.py writes the hooks to out/census/fnc_d3d_census_all.cpp; configure the PC
// build with -DSA2_EXTRA_SOURCES=<that file>). Each hook counts its calls and samples its callers; the log gets a
// report every 10 s. FNC has no reference game with named Direct3D functions, so this is how the functions Renut hooks
// are told apart (docs/fnc/stage2.md).
#pragma once

#include <cstdint>

struct PPCContext;

namespace fnc::census {

// Registers the census table once (the generated file passes its address list).
void Register(const uint32_t* addresses, int count);
// One call of census function `index` (its position in the address list); `base` is the guest memory base, for the
// shader-container check of r3..r6 on the first calls.
void Note(int index, PPCContext& ctx, const uint8_t* base);

}  // namespace fnc::census

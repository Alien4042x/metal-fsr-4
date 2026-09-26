#pragma once
#include <stdint.h>
// CPU-visible 8-bit buffers after the caller has completed its GPU fences.
// This ABI never owns or submits a game's D3D12 command list.
enum RifePairFormat : uint32_t { RIFE_PAIR_BGRA8=0, RIFE_PAIR_RGBA8=1 };
struct RifePairArgs {
    uint32_t version, size, op, result; // v2; init=0, pair=1, close=2
    uint64_t context;
    uint32_t width, height, scale, format;
    uint64_t frameA, frameB;
    uint64_t inputA, inputB, output;
    uint64_t bytesA, bytesB, bytesOut;
    uint32_t pitchA, pitchB, pitchOut, completed;
    double milliseconds;
    // Optional current real frame with UI. inputB is the matching HUD-free
    // frame; the native side restores static UI after interpolating A/B.
    uint64_t inputFull, bytesFull;
    uint32_t pitchFull, uiCoveragePermille;
};
static_assert(sizeof(RifePairArgs)==152,"RIFE PE/native ABI size");

#pragma once
#include <stdint.h>

// Experimental Direct FFX cube transport. All addresses are process-local
// mapped staging buffers; the native call neither owns nor submits D3D12 work.
struct MetalFxPairArgs {
    uint32_t version, size, op, completed; // 1; init=0, generate=1, close=2
    uint64_t context;
    uint32_t width, height, renderWidth, renderHeight;
    uint64_t frameA, frameB;
    uint64_t previous, current, depth, motion, full, output;
    uint64_t colorBytes, depthBytes, motionBytes;
    uint32_t colorPitch, depthPitch, motionPitch, fullPitch;
    float deltaSeconds, fieldOfViewDegrees, nearPlane, farPlane;
    uint32_t format, reserved; // format: 1=RGBA8, 2=BGRA8
    double gpuMilliseconds;
    uint64_t changedPixels; // Number of changed pixels in the diagnostic 8x8 sample grid.
    float motionScaleX, motionScaleY;
};
static_assert(sizeof(MetalFxPairArgs)==192,"MetalFX PE/native ABI size");

#pragma once
#include <stdint.h>
#include <stddef.h>
// Fixed-width process-local synchronous PE/Unix ABI; no COM/Metal pointer crosses it.
struct Fsr4FrameParams {
 uint32_t width,height,renderWidth,renderHeight;
 float jx,jy,mvx,mvy;
 uint32_t reset,inverted,jittered,reserved;
 float cx,cy,exposure,padding;
 uint32_t pitch[4],format[4],outputPitch;
};
struct FullMetalArgs {
 uint32_t version,size,op,pending,maxWidth,maxHeight,frame,completed;
 Fsr4FrameParams params;
 uint64_t input[4],inputBytes[4],output,outputBytes;
 double gpuMs;
 uint64_t liveBytes;
 uint32_t validated;
 char error[256];
};
static_assert(sizeof(Fsr4FrameParams)==100,"shader parameter layout");
static_assert(offsetof(FullMetalArgs,input)==136,"PE/native pointer alignment");
static_assert(sizeof(FullMetalArgs)==496,"full frame ABI");

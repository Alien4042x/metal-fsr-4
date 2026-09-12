// Experimental FSR4.0.2 FasterNetBlock<32,1>; AMD provenance in reference/AMD-LICENSE.md.
// Three fused matrix phases; quantization order retains pass4 versus fnb11 semantics.
#include <metal_stdlib>
#include <metal_simdgroup_matrix>
using namespace metal;
constant bool batchTrace [[function_constant(1)]];
#pragma clang fp contract(off)
constant bool foldedScale [[function_constant(0)]];
struct MatrixFnbShape {uint width;uint height;};
#ifndef FNB_RAW_TRACE
#define FNB_RAW_TRACE 1
#endif
kernel void fnb32_matrix(device const char* input [[buffer(0)]],constant char* weights [[buffer(1)]],
 constant float* bias [[buffer(2)]],constant float* scales [[buffer(3)]],
 device int* sums [[buffer(4)]],device char* values [[buffer(5)]],
 constant MatrixFnbShape& shape [[buffer(6)]],device char* output [[buffer(7)]],
#if FNB_RAW_TRACE
 device float* raw [[buffer(8)]],
#endif
 device const half* packedWeights [[buffer(9)]],
 uint2 group [[threadgroup_position_in_grid]],uint lane [[thread_index_in_simdgroup]],
 uint simd [[simdgroup_index_in_threadgroup]],uint tid [[thread_index_in_threadgroup]]) {
 // Two SIMD groups share a 10x4x16 input halo for an 8x2 output tile.
 // INT8 operands remain exact in half; arithmetic/quantization stays FP32.
 threadgroup half mixedStorage[2][256],expandedStorage[2][512];
 threadgroup float r0Storage[2][64],r1Storage[2][64];
 threadgroup half* mixed=mixedStorage[simd];threadgroup half* expanded=expandedStorage[simd];
 threadgroup float* r0=r0Storage[simd];threadgroup float* r1=r1Storage[simd];
 threadgroup half halo[640];
 uint firstX=group.x*8,firstY=group.y*2+simd;
 uint first=firstY*shape.width+firstX;
 for(uint i=tid;i<640;i+=64){
  int x=int(firstX)+int((i/16)%10)-1,y=int(group.y*2)+int(i/160)-1;
  halo[i]=(x>=0&&y>=0&&x<int(shape.width)&&y<int(shape.height))?input[(uint(y)*shape.width+uint(x))*32+i%16]:char(0);
 }
 threadgroup_barrier(mem_flags::mem_threadgroup);
 simdgroup_float8x8 s0(0.f),s1(0.f);
 for(uint k=0;k<144;k+=8){
  simdgroup_half8x8 aa,bb0,bb1;
  simdgroup_load(aa,halo+((simd+k/48)*10+(k/16)%3)*16+k%16,16);
  simdgroup_load(bb0,packedWeights+k*16,16);simdgroup_load(bb1,packedWeights+k*16+8,16);
  simdgroup_multiply_accumulate(s0,aa,bb0,s0);simdgroup_multiply_accumulate(s1,aa,bb1,s1);
 }
 simdgroup_store(s0,r0,8);simdgroup_store(s1,r1,8);threadgroup_barrier(mem_flags::mem_threadgroup);
 for(uint i=lane;i<256;i+=32){
  uint local=i/32,c=i%32,p=first+local;
  if(c<16){
   float sum=c<8?r0[local*8+c]:r1[local*8+c-8];
   float v=sum*(scales[0]*scales[5]);v=v+bias[c];v=v*(1.f/scales[1]);
   float q=clamp(rint(v),-128.f,127.f);mixed[i]=q;
   if(firstX+local<shape.width&&firstY<shape.height){if(batchTrace)sums[p*112+c]=int(sum);if(batchTrace)values[p*112+c]=char(q);
#if FNB_RAW_TRACE
    raw[p*112+c]=sum;
#endif
   }
  }else mixed[i]=firstX+local<shape.width&&firstY<shape.height?float(input[p*32+c]):0.f;
 }
 threadgroup_barrier(mem_flags::mem_threadgroup);
 for(uint base=0;base<64;base+=16){
  simdgroup_float8x8 e0(0.f),e1(0.f);
  for(uint k=0;k<32;k+=8){
   // Read-only operands throughout K loop; phase-boundary barriers remain.
   simdgroup_half8x8 aa,bb0,bb1;simdgroup_load(aa,mixed+k,32);
   simdgroup_load(bb0,packedWeights+2304+k*64+base,64);simdgroup_load(bb1,packedWeights+2304+k*64+base+8,64);
   simdgroup_multiply_accumulate(e0,aa,bb0,e0);simdgroup_multiply_accumulate(e1,aa,bb1,e1);
  }
  simdgroup_store(e0,r0,8);simdgroup_store(e1,r1,8);threadgroup_barrier(mem_flags::mem_threadgroup);
  for(uint i=lane;i<128;i+=32){uint local=i/16,c=base+i%16,p=first+local;float sum=i%16<8?r0[local*8+i%16]:r1[local*8+i%16-8];
   float v;if(foldedScale)v=sum*(scales[1]*scales[6]);else {v=sum*scales[1];v=v*scales[6];}
   v=v+bias[16+c];v=v*(1.f/scales[2]);float q=clamp(rint(v),0.f,127.f);expanded[local*64+c]=q;
   if(firstX+local<shape.width&&firstY<shape.height){if(batchTrace)sums[p*112+16+c]=int(sum);if(batchTrace)values[p*112+16+c]=char(q);
#if FNB_RAW_TRACE
    raw[p*112+16+c]=sum;
#endif
   }
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);
 }
 for(uint base=0;base<32;base+=16){
  simdgroup_float8x8 c0(0.f),c1(0.f);
  for(uint k=0;k<64;k+=8){
   // Read-only operands throughout K loop; phase-boundary barriers remain.
   simdgroup_half8x8 aa,bb0,bb1;simdgroup_load(aa,expanded+k,64);
   simdgroup_load(bb0,packedWeights+4352+k*32+base,32);simdgroup_load(bb1,packedWeights+4352+k*32+base+8,32);
   simdgroup_multiply_accumulate(c0,aa,bb0,c0);simdgroup_multiply_accumulate(c1,aa,bb1,c1);
  }
  simdgroup_store(c0,r0,8);simdgroup_store(c1,r1,8);threadgroup_barrier(mem_flags::mem_threadgroup);
  for(uint i=lane;i<128;i+=32){uint local=i/16,c=base+i%16,p=first+local;
   if(firstX+local<shape.width&&firstY<shape.height){float sum=i%16<8?r0[local*8+i%16]:r1[local*8+i%16-8];
    float v;if(foldedScale)v=sum*(scales[7]*scales[2]);else {v=sum*scales[7];v=v*scales[2];}
    v=v+bias[80+c];float residual=float(input[p*32+c])*scales[c/16];v=v+residual;v=v*(1.f/scales[3+c/16]);char q=char(clamp(rint(v),-128.f,127.f));
    if(batchTrace)sums[p*112+80+c]=int(sum);if(batchTrace)values[p*112+80+c]=q;output[p*32+c]=q;
#if FNB_RAW_TRACE
    raw[p*112+80+c]=sum;
#endif
   }
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);
 }
}

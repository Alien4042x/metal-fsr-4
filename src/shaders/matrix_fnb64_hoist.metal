// Experimental FSR4.0.2 FasterNetBlock<64,2>; AMD provenance in reference/AMD-LICENSE.md.
// Three fused matrix phases; quantization order retains bottleneck versus fnb9 semantics.
#include <metal_stdlib>
#include <metal_simdgroup_matrix>
using namespace metal;
constant bool batchTrace [[function_constant(1)]];
#pragma clang fp contract(off)
constant bool foldedScale [[function_constant(0)]];
struct MatrixFnb64Shape {uint width;uint height;};
#ifndef FNB64_RAW_TRACE
#define FNB64_RAW_TRACE 1
#endif
kernel void fnb64_matrix(device const char* input [[buffer(0)]],constant char* weights [[buffer(1)]],
 constant float* bias [[buffer(2)]],constant float* scales [[buffer(3)]],
 device int* sums [[buffer(4)]],device char* values [[buffer(5)]],
 constant MatrixFnb64Shape& shape [[buffer(6)]],device char* output [[buffer(7)]],
#if FNB64_RAW_TRACE
 device float* raw [[buffer(8)]],
#endif
 device const half* packedWeights [[buffer(9)]],
 uint group [[threadgroup_position_in_grid]],uint lane [[thread_index_in_threadgroup]]) {
 // Exact INT8-valued operands in half; all accumulators/scaling stay float.
 threadgroup half a[64];
 threadgroup float r0[64],r1[64];
 // Quantized integers only: half storage is exact; all arithmetic remains FP32.
 threadgroup half mixed[512],expanded[1024];
 uint first=group*8;
 // Pixel centers are invariant across both subgroup and spatial K loops.
 uint2 pixels=uint2(first+lane/8,first+lane/8+4);
 int2 centerX=int2(pixels%shape.width),centerY=int2(pixels/shape.width);
 for(uint subgroup=0;subgroup<32;subgroup+=16){
  simdgroup_float8x8 s0(0.f),s1(0.f);
  for(uint k=0;k<144;k+=8){
   for(uint i=lane;i<64;i+=32){
    uint p=first+i/8,t=k+i%8;int x=centerX[(i-lane)/32]+int((t/16)%3)-1,y=centerY[(i-lane)/32]+int(t/48)-1;
    a[i]=(p<shape.width*shape.height&&x>=0&&y>=0&&x<int(shape.width)&&y<int(shape.height))?float(input[(uint(y)*shape.width+uint(x))*64+subgroup+t%16]):0.f;

   }
   threadgroup_barrier(mem_flags::mem_threadgroup);
   simdgroup_half8x8 aa,bb0,bb1;simdgroup_load(aa,a,8);simdgroup_load(bb0,packedWeights+k*32+subgroup,32);simdgroup_load(bb1,packedWeights+k*32+subgroup+8,32);
   simdgroup_multiply_accumulate(s0,aa,bb0,s0);simdgroup_multiply_accumulate(s1,aa,bb1,s1);
   threadgroup_barrier(mem_flags::mem_threadgroup);
  }
  simdgroup_store(s0,r0,8);simdgroup_store(s1,r1,8);threadgroup_barrier(mem_flags::mem_threadgroup);
  for(uint i=lane;i<128;i+=32){uint local=i/16,c=subgroup+i%16,p=first+local;
   float sum=i%16<8?r0[local*8+i%16]:r1[local*8+i%16-8];
   float v;if(foldedScale)v=sum*(scales[0]*scales[5]);else {v=sum*scales[0];v=v*scales[5];}
   v=v+bias[c];v=v*(1.f/scales[1]);float q=clamp(rint(v),-128.f,127.f);mixed[local*64+c]=q;
   if(p<shape.width*shape.height){if(batchTrace)sums[p*224+c]=int(sum);if(batchTrace)values[p*224+c]=char(q);
#if FNB64_RAW_TRACE
    raw[p*224+c]=sum;
#endif
   }
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);
 }
 for(uint i=lane;i<256;i+=32){uint local=i/32,c=32+i%32,p=first+local;mixed[local*64+c]=p<shape.width*shape.height?float(input[p*64+c]):0.f;}
 threadgroup_barrier(mem_flags::mem_threadgroup);
 for(uint base=0;base<128;base+=16){
  simdgroup_float8x8 e0(0.f),e1(0.f);
  for(uint k=0;k<64;k+=8){
   // Read-only operands throughout K loop; phase-boundary barriers remain.
   simdgroup_half8x8 aa,bb0,bb1;simdgroup_load(aa,mixed+k,64);
   simdgroup_load(bb0,packedWeights+4608+k*128+base,128);simdgroup_load(bb1,packedWeights+4608+k*128+base+8,128);
   simdgroup_multiply_accumulate(e0,aa,bb0,e0);simdgroup_multiply_accumulate(e1,aa,bb1,e1);
  }
  simdgroup_store(e0,r0,8);simdgroup_store(e1,r1,8);threadgroup_barrier(mem_flags::mem_threadgroup);
  for(uint i=lane;i<128;i+=32){uint local=i/16,c=base+i%16,p=first+local;float sum=i%16<8?r0[local*8+i%16]:r1[local*8+i%16-8];
   float v;if(foldedScale)v=sum*(scales[1]*scales[6]);else {v=sum*scales[1];v=v*scales[6];}
   v=v+bias[32+c];v=v*(1.f/scales[2]);float q=clamp(rint(v),0.f,127.f);expanded[local*128+c]=q;
   if(p<shape.width*shape.height){if(batchTrace)sums[p*224+32+c]=int(sum);if(batchTrace)values[p*224+32+c]=char(q);
#if FNB64_RAW_TRACE
    raw[p*224+32+c]=sum;
#endif
   }
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);
 }
 for(uint base=0;base<64;base+=16){
  simdgroup_float8x8 c0(0.f),c1(0.f);
  for(uint k=0;k<128;k+=8){
   // Read-only operands throughout K loop; phase-boundary barriers remain.
   simdgroup_half8x8 aa,bb0,bb1;simdgroup_load(aa,expanded+k,128);
   simdgroup_load(bb0,packedWeights+12800+k*64+base,64);simdgroup_load(bb1,packedWeights+12800+k*64+base+8,64);
   simdgroup_multiply_accumulate(c0,aa,bb0,c0);simdgroup_multiply_accumulate(c1,aa,bb1,c1);
  }
  simdgroup_store(c0,r0,8);simdgroup_store(c1,r1,8);threadgroup_barrier(mem_flags::mem_threadgroup);
  for(uint i=lane;i<128;i+=32){uint local=i/16,c=base+i%16,p=first+local;
   if(p<shape.width*shape.height){float sum=i%16<8?r0[local*8+i%16]:r1[local*8+i%16-8];
    float v=sum*(scales[7]*scales[2]);
    v=v+bias[160+c];float residual=float(input[p*64+c])*scales[c/32];v=v+residual;v=v*(1.f/scales[3+c/32]);char q=char(clamp(rint(v),-128.f,127.f));
    if(batchTrace)sums[p*224+160+c]=int(sum);if(batchTrace)values[p*224+160+c]=q;output[p*64+c]=q;
#if FNB64_RAW_TRACE
    raw[p*224+160+c]=sum;
#endif
   }
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);
 }
}

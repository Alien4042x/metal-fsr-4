// Experimental FSR4.0.2 block402<16>; AMD provenance in reference/AMD-LICENSE.md.
// Three fused matrix phases; original block402 quantization order retained.
#include <metal_stdlib>
#include <metal_simdgroup_matrix>
using namespace metal;
constant bool batchTrace [[function_constant(1)]];
constant bool fusionCapture [[function_constant(2)]];
#pragma clang fp contract(off)
struct MatrixBlock16Shape {uint width;uint height;};
#ifndef BLOCK16_RAW_TRACE
#define BLOCK16_RAW_TRACE 1
#endif
// WineForge-Internal: fsr-lab/block13-ct13-fusion-v1.
kernel void block13_ct13_tile8x2(device const char* input [[buffer(0)]],constant char* weights [[buffer(1)]],
 constant float* bias [[buffer(2)]],constant float* scales [[buffer(3)]],
 device int* sums [[buffer(4)]],device char* values [[buffer(5)]],
 constant MatrixBlock16Shape& shape [[buffer(6)]],
#if BLOCK16_RAW_TRACE
 device float* raw [[buffer(7)]],
#endif
 device char* dense [[buffer(8)]],
 device const half* packedWeights [[buffer(9)]],
 device const half* ctWeights [[buffer(10)]],constant half* ctBias [[buffer(11)]],constant float* ctScales [[buffer(12)]],
 device int* ctSums [[buffer(13)]],device half* ctOutput [[buffer(14)]],
 uint2 group [[threadgroup_position_in_grid]],uint lane [[thread_index_in_simdgroup]],
 uint simd [[simdgroup_index_in_threadgroup]],uint tid [[thread_index_in_threadgroup]]) {
 // WineForge-Internal: fsr-lab/block13-ct13-tile8x2-shared-halo-v1.
 // Two SIMD groups compute one 8-pixel row each, sharing a 10x4x16 halo.
 // Matrix order, quantization and residual arithmetic match directhalo.
 threadgroup half mixedStorage[2][128],expandedStorage[2][256];
 threadgroup float r0Storage[2][64],r1Storage[2][64];
 threadgroup half* mixed=mixedStorage[simd];threadgroup half* expanded=expandedStorage[simd];
 threadgroup float* r0=r0Storage[simd];threadgroup float* r1=r1Storage[simd];
 threadgroup half4 haloVectors[160];
 threadgroup half* halo=reinterpret_cast<threadgroup half*>(haloVectors);
 uint firstX=group.x*8;int firstY=int(group.y*2+simd);
 uint first=uint(firstY)*shape.width+firstX;
 // WineForge-Internal: fsr4/halo-four-channel-address-reuse-v1.
 // 40 halo pixels x four aligned channel vectors; exact signed INT8 to half.
 for(uint v=tid;v<160;v+=64){
  uint pixel=v/4,c=(v%4)*4;
  int x=int(firstX)+int(pixel%10)-1,y=int(group.y*2)+int(pixel/10)-1;
  half4 value=half4(0);
  if(x>=0&&y>=0&&x<int(shape.width)&&y<int(shape.height)){
   auto source=reinterpret_cast<device const char4*>(input+(uint(y)*shape.width+uint(x))*16+c);
   value=half4(*source);
  }
  haloVectors[v]=value;
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
 for(uint i=lane;i<128;i+=32){
  uint local=i/16,c=i%16,p=first+local;
  float sum=c<8?r0[local*8+c]:r1[local*8+c-8];
  float v=sum*(scales[0]*scales[1]);v=v+bias[c];v=v*scales[4];
  float q=clamp(rint(v),-128.f,127.f);mixed[i]=q;
  if(firstX+local<shape.width&&firstY<int(shape.height)){if(batchTrace)sums[p*64+c]=int(sum);if(batchTrace)values[p*64+c]=char(q);
#if BLOCK16_RAW_TRACE
   raw[p*64+c]=sum;
#endif
  }
 }
 threadgroup_barrier(mem_flags::mem_threadgroup);
 for(uint base=0;base<32;base+=16){
  simdgroup_float8x8 e0(0.f),e1(0.f);
  for(uint k=0;k<16;k+=8){
   // Operands are immutable during this K loop. Keep phase boundary barriers.
   simdgroup_half8x8 aa,bb0,bb1;simdgroup_load(aa,mixed+k,16);
   simdgroup_load(bb0,packedWeights+2304+k*32+base,32);simdgroup_load(bb1,packedWeights+2304+k*32+base+8,32);
   simdgroup_multiply_accumulate(e0,aa,bb0,e0);simdgroup_multiply_accumulate(e1,aa,bb1,e1);
  }
  simdgroup_store(e0,r0,8);simdgroup_store(e1,r1,8);threadgroup_barrier(mem_flags::mem_threadgroup);
  for(uint i=lane;i<128;i+=32){uint local=i/16,c=base+i%16,p=first+local;float sum=i%16<8?r0[local*8+i%16]:r1[local*8+i%16-8];
   float v=sum*(scales[5]*scales[2]);
   v=v+bias[16+c];v=v*scales[6];float q=clamp(rint(v),0.f,127.f);expanded[local*32+c]=q;
   if(firstX+local<shape.width&&firstY<int(shape.height)){if(batchTrace)sums[p*64+16+c]=int(sum);if(batchTrace)values[p*64+16+c]=char(q);
#if BLOCK16_RAW_TRACE
    raw[p*64+16+c]=sum;
#endif
   }
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);
 }
 for(uint base=0;base<16;base+=16){
  simdgroup_float8x8 c0(0.f),c1(0.f);
  for(uint k=0;k<32;k+=8){
   // Operands are immutable during this K loop. Keep phase boundary barriers.
   simdgroup_half8x8 aa,bb0,bb1;simdgroup_load(aa,expanded+k,32);
   simdgroup_load(bb0,packedWeights+2816+k*16+base,16);simdgroup_load(bb1,packedWeights+2816+k*16+base+8,16);
   simdgroup_multiply_accumulate(c0,aa,bb0,c0);simdgroup_multiply_accumulate(c1,aa,bb1,c1);
  }
  simdgroup_store(c0,r0,8);simdgroup_store(c1,r1,8);threadgroup_barrier(mem_flags::mem_threadgroup);
  for(uint i=lane;i<128;i+=32){uint local=i/16,c=base+i%16,p=first+local;
   if(firstX+local<shape.width&&firstY<int(shape.height)){float sum=i%16<8?r0[local*8+i%16]:r1[local*8+i%16-8];
    float v=sum*(scales[3]*scales[7]);
    v=v+bias[48+c];float residual=float(input[p*16+c])*scales[0];v=v+residual;v=v*(1.f/scales[8]);char q=char(clamp(rint(v),-128.f,127.f));
    if(batchTrace){sums[p*64+48+c]=int(sum);values[p*64+48+c]=q;}
    // WineForge-Internal: fsr-lab/block16-direct-dense-v1.
    // This invocation owns exactly one final channel; preserve reference trace.
    // mixed is dead after expansion: reuse its exact-half storage for this INT8 result.
    mixed[local*16+c]=half(q);
    if(fusionCapture)dense[p*16+c]=q;
#if BLOCK16_RAW_TRACE
    raw[p*64+48+c]=sum;
#endif
   }else mixed[local*16+c]=0;
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);
 }
 // The transpose convolution consumes only this parent's16 channels: no halo or
 // neighboring threadgroup dependency. Keep original quantization/half bias rounding.
 for(uint z=0;z<4;++z){
  simdgroup_float8x8 acc(0.f);
  for(uint k=0;k<16;k+=8){
   simdgroup_half8x8 aa,bb;simdgroup_load(aa,mixed+k,16);simdgroup_load(bb,ctWeights+z*128+k*8,8);
   simdgroup_multiply_accumulate(acc,aa,bb,acc);
   threadgroup_barrier(mem_flags::mem_threadgroup);
  }
  simdgroup_store(acc,r0,8);threadgroup_barrier(mem_flags::mem_threadgroup);
  for(uint j=lane;j<64;j+=32){uint c=j%8;
   if(firstX+j/8<shape.width&&firstY<int(shape.height)){uint ow=shape.width*2;
    uint p=(uint(firstY)*2+z/2)*ow+(firstX+j/8)*2+z%2;
    float sum=r0[j];if(batchTrace)ctSums[p*8+c]=int(sum);
    float factor=ctScales[0]*ctScales[1];half product=half(sum*factor);ctOutput[p*8+c]=product+ctBias[c];
   }
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);
 }
}

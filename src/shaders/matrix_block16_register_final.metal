#define BLOCK16_RAW_TRACE 0
// Experimental FSR4.0.2 block402<16>; AMD provenance in reference/AMD-LICENSE.md.
// Three fused matrix phases; original block402 quantization order retained.
#include <metal_stdlib>
#include <metal_simdgroup_matrix>
using namespace metal;
constant bool batchTrace = false;
#pragma clang fp contract(off)
struct MatrixBlock16Shape {uint width;uint height;};
#ifndef BLOCK16_RAW_TRACE
#define BLOCK16_RAW_TRACE 1
#endif
kernel void block16_tile8x2(device const char* input [[buffer(0)]],constant char* weights [[buffer(1)]],
 device const float* bias [[buffer(2)]],constant float* scales [[buffer(3)]],
 device int* sums [[buffer(4)]],device char* values [[buffer(5)]],
 constant MatrixBlock16Shape& shape [[buffer(6)]],
#if BLOCK16_RAW_TRACE
 device float* raw [[buffer(7)]],
#endif
 device char* dense [[buffer(8)]],
 device const half* packedWeights [[buffer(9)]],
 uint2 group [[threadgroup_position_in_grid]],uint lane [[thread_index_in_simdgroup]],
 uint simd [[simdgroup_index_in_threadgroup]],uint tid [[thread_index_in_threadgroup]]) {
 // WineForge-Internal: fsr-lab/block16-tile8x2-shared-halo-v1.
 // Two SIMD groups compute two 8-pixel rows each, sharing a 10x6x16 halo.
 // Matrix order, quantization and residual arithmetic match directhalo.
 threadgroup half mixedStorage[2][128],expandedStorage[2][256];
 threadgroup float r0Storage[2][64],r1Storage[2][64];
 threadgroup half* mixed=mixedStorage[simd];threadgroup half* expanded=expandedStorage[simd];
 threadgroup float* r0=r0Storage[simd];threadgroup float* r1=r1Storage[simd];
 threadgroup half4 haloVectors[240];
 threadgroup half* halo=reinterpret_cast<threadgroup half*>(haloVectors);
 uint firstX=group.x*8;int firstY=int(group.y*4+simd);
 uint first=uint(firstY)*shape.width+firstX;
 // WineForge-Internal: fsr4/halo-four-channel-address-reuse-v1.
 // 40 halo pixels x four aligned channel vectors; exact signed INT8 to half.
 for(uint v=tid;v<240;v+=64){
  uint pixel=v/4,c=(v%4)*4;
  int x=int(firstX)+int(pixel%10)-1,y=int(group.y*4)+int(pixel/10)-1;
  half4 value=half4(0);
  if(x>=0&&y>=0&&x<int(shape.width)&&y<int(shape.height)){
   auto source=reinterpret_cast<device const char4*>(input+(uint(y)*shape.width+uint(x))*16+c);
   value=half4(*source);
  }
  haloVectors[v]=value;
 }

 threadgroup_barrier(mem_flags::mem_threadgroup);
 simdgroup_float8x8 s0(0.f),s1(0.f),t0(0.f),t1(0.f);
 // Spatial K steps retain the original order, with compile-time offsets.
 { // K=0
  simdgroup_half8x8 aa,ab,bb0,bb1;
  simdgroup_load(aa,halo+simd*160+0,16);
  simdgroup_load(ab,halo+simd*160+320,16);
  simdgroup_load(bb0,packedWeights+0,16);simdgroup_load(bb1,packedWeights+8,16);
  simdgroup_multiply_accumulate(s0,aa,bb0,s0);simdgroup_multiply_accumulate(s1,aa,bb1,s1);
  simdgroup_multiply_accumulate(t0,ab,bb0,t0);simdgroup_multiply_accumulate(t1,ab,bb1,t1);
 }
 { // K=8
  simdgroup_half8x8 aa,ab,bb0,bb1;
  simdgroup_load(aa,halo+simd*160+8,16);
  simdgroup_load(ab,halo+simd*160+328,16);
  simdgroup_load(bb0,packedWeights+128,16);simdgroup_load(bb1,packedWeights+136,16);
  simdgroup_multiply_accumulate(s0,aa,bb0,s0);simdgroup_multiply_accumulate(s1,aa,bb1,s1);
  simdgroup_multiply_accumulate(t0,ab,bb0,t0);simdgroup_multiply_accumulate(t1,ab,bb1,t1);
 }
 { // K=16
  simdgroup_half8x8 aa,ab,bb0,bb1;
  simdgroup_load(aa,halo+simd*160+16,16);
  simdgroup_load(ab,halo+simd*160+336,16);
  simdgroup_load(bb0,packedWeights+256,16);simdgroup_load(bb1,packedWeights+264,16);
  simdgroup_multiply_accumulate(s0,aa,bb0,s0);simdgroup_multiply_accumulate(s1,aa,bb1,s1);
  simdgroup_multiply_accumulate(t0,ab,bb0,t0);simdgroup_multiply_accumulate(t1,ab,bb1,t1);
 }
 { // K=24
  simdgroup_half8x8 aa,ab,bb0,bb1;
  simdgroup_load(aa,halo+simd*160+24,16);
  simdgroup_load(ab,halo+simd*160+344,16);
  simdgroup_load(bb0,packedWeights+384,16);simdgroup_load(bb1,packedWeights+392,16);
  simdgroup_multiply_accumulate(s0,aa,bb0,s0);simdgroup_multiply_accumulate(s1,aa,bb1,s1);
  simdgroup_multiply_accumulate(t0,ab,bb0,t0);simdgroup_multiply_accumulate(t1,ab,bb1,t1);
 }
 { // K=32
  simdgroup_half8x8 aa,ab,bb0,bb1;
  simdgroup_load(aa,halo+simd*160+32,16);
  simdgroup_load(ab,halo+simd*160+352,16);
  simdgroup_load(bb0,packedWeights+512,16);simdgroup_load(bb1,packedWeights+520,16);
  simdgroup_multiply_accumulate(s0,aa,bb0,s0);simdgroup_multiply_accumulate(s1,aa,bb1,s1);
  simdgroup_multiply_accumulate(t0,ab,bb0,t0);simdgroup_multiply_accumulate(t1,ab,bb1,t1);
 }
 { // K=40
  simdgroup_half8x8 aa,ab,bb0,bb1;
  simdgroup_load(aa,halo+simd*160+40,16);
  simdgroup_load(ab,halo+simd*160+360,16);
  simdgroup_load(bb0,packedWeights+640,16);simdgroup_load(bb1,packedWeights+648,16);
  simdgroup_multiply_accumulate(s0,aa,bb0,s0);simdgroup_multiply_accumulate(s1,aa,bb1,s1);
  simdgroup_multiply_accumulate(t0,ab,bb0,t0);simdgroup_multiply_accumulate(t1,ab,bb1,t1);
 }
 { // K=48
  simdgroup_half8x8 aa,ab,bb0,bb1;
  simdgroup_load(aa,halo+simd*160+160,16);
  simdgroup_load(ab,halo+simd*160+480,16);
  simdgroup_load(bb0,packedWeights+768,16);simdgroup_load(bb1,packedWeights+776,16);
  simdgroup_multiply_accumulate(s0,aa,bb0,s0);simdgroup_multiply_accumulate(s1,aa,bb1,s1);
  simdgroup_multiply_accumulate(t0,ab,bb0,t0);simdgroup_multiply_accumulate(t1,ab,bb1,t1);
 }
 { // K=56
  simdgroup_half8x8 aa,ab,bb0,bb1;
  simdgroup_load(aa,halo+simd*160+168,16);
  simdgroup_load(ab,halo+simd*160+488,16);
  simdgroup_load(bb0,packedWeights+896,16);simdgroup_load(bb1,packedWeights+904,16);
  simdgroup_multiply_accumulate(s0,aa,bb0,s0);simdgroup_multiply_accumulate(s1,aa,bb1,s1);
  simdgroup_multiply_accumulate(t0,ab,bb0,t0);simdgroup_multiply_accumulate(t1,ab,bb1,t1);
 }
 { // K=64
  simdgroup_half8x8 aa,ab,bb0,bb1;
  simdgroup_load(aa,halo+simd*160+176,16);
  simdgroup_load(ab,halo+simd*160+496,16);
  simdgroup_load(bb0,packedWeights+1024,16);simdgroup_load(bb1,packedWeights+1032,16);
  simdgroup_multiply_accumulate(s0,aa,bb0,s0);simdgroup_multiply_accumulate(s1,aa,bb1,s1);
  simdgroup_multiply_accumulate(t0,ab,bb0,t0);simdgroup_multiply_accumulate(t1,ab,bb1,t1);
 }
 { // K=72
  simdgroup_half8x8 aa,ab,bb0,bb1;
  simdgroup_load(aa,halo+simd*160+184,16);
  simdgroup_load(ab,halo+simd*160+504,16);
  simdgroup_load(bb0,packedWeights+1152,16);simdgroup_load(bb1,packedWeights+1160,16);
  simdgroup_multiply_accumulate(s0,aa,bb0,s0);simdgroup_multiply_accumulate(s1,aa,bb1,s1);
  simdgroup_multiply_accumulate(t0,ab,bb0,t0);simdgroup_multiply_accumulate(t1,ab,bb1,t1);
 }
 { // K=80
  simdgroup_half8x8 aa,ab,bb0,bb1;
  simdgroup_load(aa,halo+simd*160+192,16);
  simdgroup_load(ab,halo+simd*160+512,16);
  simdgroup_load(bb0,packedWeights+1280,16);simdgroup_load(bb1,packedWeights+1288,16);
  simdgroup_multiply_accumulate(s0,aa,bb0,s0);simdgroup_multiply_accumulate(s1,aa,bb1,s1);
  simdgroup_multiply_accumulate(t0,ab,bb0,t0);simdgroup_multiply_accumulate(t1,ab,bb1,t1);
 }
 { // K=88
  simdgroup_half8x8 aa,ab,bb0,bb1;
  simdgroup_load(aa,halo+simd*160+200,16);
  simdgroup_load(ab,halo+simd*160+520,16);
  simdgroup_load(bb0,packedWeights+1408,16);simdgroup_load(bb1,packedWeights+1416,16);
  simdgroup_multiply_accumulate(s0,aa,bb0,s0);simdgroup_multiply_accumulate(s1,aa,bb1,s1);
  simdgroup_multiply_accumulate(t0,ab,bb0,t0);simdgroup_multiply_accumulate(t1,ab,bb1,t1);
 }
 { // K=96
  simdgroup_half8x8 aa,ab,bb0,bb1;
  simdgroup_load(aa,halo+simd*160+320,16);
  simdgroup_load(ab,halo+simd*160+640,16);
  simdgroup_load(bb0,packedWeights+1536,16);simdgroup_load(bb1,packedWeights+1544,16);
  simdgroup_multiply_accumulate(s0,aa,bb0,s0);simdgroup_multiply_accumulate(s1,aa,bb1,s1);
  simdgroup_multiply_accumulate(t0,ab,bb0,t0);simdgroup_multiply_accumulate(t1,ab,bb1,t1);
 }
 { // K=104
  simdgroup_half8x8 aa,ab,bb0,bb1;
  simdgroup_load(aa,halo+simd*160+328,16);
  simdgroup_load(ab,halo+simd*160+648,16);
  simdgroup_load(bb0,packedWeights+1664,16);simdgroup_load(bb1,packedWeights+1672,16);
  simdgroup_multiply_accumulate(s0,aa,bb0,s0);simdgroup_multiply_accumulate(s1,aa,bb1,s1);
  simdgroup_multiply_accumulate(t0,ab,bb0,t0);simdgroup_multiply_accumulate(t1,ab,bb1,t1);
 }
 { // K=112
  simdgroup_half8x8 aa,ab,bb0,bb1;
  simdgroup_load(aa,halo+simd*160+336,16);
  simdgroup_load(ab,halo+simd*160+656,16);
  simdgroup_load(bb0,packedWeights+1792,16);simdgroup_load(bb1,packedWeights+1800,16);
  simdgroup_multiply_accumulate(s0,aa,bb0,s0);simdgroup_multiply_accumulate(s1,aa,bb1,s1);
  simdgroup_multiply_accumulate(t0,ab,bb0,t0);simdgroup_multiply_accumulate(t1,ab,bb1,t1);
 }
 { // K=120
  simdgroup_half8x8 aa,ab,bb0,bb1;
  simdgroup_load(aa,halo+simd*160+344,16);
  simdgroup_load(ab,halo+simd*160+664,16);
  simdgroup_load(bb0,packedWeights+1920,16);simdgroup_load(bb1,packedWeights+1928,16);
  simdgroup_multiply_accumulate(s0,aa,bb0,s0);simdgroup_multiply_accumulate(s1,aa,bb1,s1);
  simdgroup_multiply_accumulate(t0,ab,bb0,t0);simdgroup_multiply_accumulate(t1,ab,bb1,t1);
 }
 { // K=128
  simdgroup_half8x8 aa,ab,bb0,bb1;
  simdgroup_load(aa,halo+simd*160+352,16);
  simdgroup_load(ab,halo+simd*160+672,16);
  simdgroup_load(bb0,packedWeights+2048,16);simdgroup_load(bb1,packedWeights+2056,16);
  simdgroup_multiply_accumulate(s0,aa,bb0,s0);simdgroup_multiply_accumulate(s1,aa,bb1,s1);
  simdgroup_multiply_accumulate(t0,ab,bb0,t0);simdgroup_multiply_accumulate(t1,ab,bb1,t1);
 }
 { // K=136
  simdgroup_half8x8 aa,ab,bb0,bb1;
  simdgroup_load(aa,halo+simd*160+360,16);
  simdgroup_load(ab,halo+simd*160+680,16);
  simdgroup_load(bb0,packedWeights+2176,16);simdgroup_load(bb1,packedWeights+2184,16);
  simdgroup_multiply_accumulate(s0,aa,bb0,s0);simdgroup_multiply_accumulate(s1,aa,bb1,s1);
  simdgroup_multiply_accumulate(t0,ab,bb0,t0);simdgroup_multiply_accumulate(t1,ab,bb1,t1);
 }
 for(uint rowPair=0;rowPair<2;++rowPair){
 firstY=int(group.y*4+simd+rowPair*2);first=uint(firstY)*shape.width+firstX;
 if(batchTrace || BLOCK16_RAW_TRACE){
 if(rowPair==0){simdgroup_store(s0,r0,8);simdgroup_store(s1,r1,8);}
 else {simdgroup_store(t0,r0,8);simdgroup_store(t1,r1,8);}
 threadgroup_barrier(mem_flags::mem_threadgroup);
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
 } else {
  // Quantize accumulator fragments before storing the next matrix operand.
  simdgroup_float8x8 initialBias0,initialBias1;
  simdgroup_load(initialBias0,bias,0);simdgroup_load(initialBias1,bias+8,0);
  simdgroup_half8x8 initialQ0,initialQ1;
  for(uint element=0;element<2;++element){
   float v0=(rowPair==0?s0.thread_elements()[element]:t0.thread_elements()[element])*(scales[0]*scales[1]);
   float v1=(rowPair==0?s1.thread_elements()[element]:t1.thread_elements()[element])*(scales[0]*scales[1]);
   v0=v0+initialBias0.thread_elements()[element];v0=v0*scales[4];
   v1=v1+initialBias1.thread_elements()[element];v1=v1*scales[4];
   initialQ0.thread_elements()[element]=half(clamp(rint(v0),-128.f,127.f));
   initialQ1.thread_elements()[element]=half(clamp(rint(v1),-128.f,127.f));
  }
  simdgroup_store(initialQ0,mixed,16);simdgroup_store(initialQ1,mixed+8,16);
  threadgroup_barrier(mem_flags::mem_threadgroup);
 }
 for(uint base=0;base<32;base+=16){
  simdgroup_float8x8 e0(0.f),e1(0.f);
  for(uint k=0;k<16;k+=8){
   // Operands are immutable during this K loop. Keep phase boundary barriers.
   simdgroup_half8x8 aa,bb0,bb1;simdgroup_load(aa,mixed+k,16);
   simdgroup_load(bb0,packedWeights+2304+k*32+base,32);simdgroup_load(bb1,packedWeights+2304+k*32+base+8,32);
   simdgroup_multiply_accumulate(e0,aa,bb0,e0);simdgroup_multiply_accumulate(e1,aa,bb1,e1);
  }
  // Bias is broadcast across rows by a zero row stride. Do not assume lane coordinates.
  simdgroup_float8x8 bias0,bias1;
  simdgroup_load(bias0,bias+16+base,0);simdgroup_load(bias1,bias+16+base+8,0);
  simdgroup_half8x8 q0,q1;
  // Apple 32-lane 8x8 fragments: two elements per lane (qualified on M4 Pro).
  for(uint element=0;element<2;++element){
   float v0=e0.thread_elements()[element]*(scales[5]*scales[2]);
   float v1=e1.thread_elements()[element]*(scales[5]*scales[2]);
   v0=v0+bias0.thread_elements()[element];v0=v0*scales[6];
   v1=v1+bias1.thread_elements()[element];v1=v1*scales[6];
   q0.thread_elements()[element]=half(clamp(rint(v0),0.f,127.f));
   q1.thread_elements()[element]=half(clamp(rint(v1),0.f,127.f));
  }
  simdgroup_store(q0,expanded+base,32);simdgroup_store(q1,expanded+base+8,32);
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
  // Original input is already present in the shared halo, including safe edge padding.
  simdgroup_half8x8 residual0,residual1,finalQ0,finalQ1;
  simdgroup_load(residual0,halo+(simd+rowPair*2+1)*160+16,16);
  simdgroup_load(residual1,halo+(simd+rowPair*2+1)*160+24,16);
  simdgroup_float8x8 finalBias0,finalBias1;
  simdgroup_load(finalBias0,bias+48,0);simdgroup_load(finalBias1,bias+56,0);
  for(uint element=0;element<2;++element){
   float v0=c0.thread_elements()[element]*(scales[3]*scales[7]);
   float v1=c1.thread_elements()[element]*(scales[3]*scales[7]);
   v0=v0+finalBias0.thread_elements()[element];
   v1=v1+finalBias1.thread_elements()[element];
   float r0v=float(residual0.thread_elements()[element])*scales[0];
   float r1v=float(residual1.thread_elements()[element])*scales[0];
   v0=v0+r0v;v0=v0*(1.f/scales[8]);
   v1=v1+r1v;v1=v1*(1.f/scales[8]);
   finalQ0.thread_elements()[element]=half(clamp(rint(v0),-128.f,127.f));
   finalQ1.thread_elements()[element]=half(clamp(rint(v1),-128.f,127.f));
  }
  // Mixed storage is dead after expansion; reuse it for bounded INT8 output conversion.
  simdgroup_store(finalQ0,mixed,16);simdgroup_store(finalQ1,mixed+8,16);
  threadgroup_barrier(mem_flags::mem_threadgroup);
  for(uint i=lane;i<128;i+=32){uint local=i/16,c=i%16;
   if(firstX+local<shape.width&&firstY<int(shape.height))dense[(first+local)*16+c]=char(mixed[i]);
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);
 }
}
}

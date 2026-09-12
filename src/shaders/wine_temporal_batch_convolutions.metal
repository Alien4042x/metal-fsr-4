// Experimental FSR4.0.2 Conv/CT batch. AMD provenance: reference/AMD-LICENSE.md.
// INT8 products accumulated as exact FP32 integers (K<=128, absolute bound<2^24).
#include <metal_stdlib>
#include <metal_simdgroup_matrix>
using namespace metal;
#pragma clang fp contract(off)
constant bool batchTrace [[function_constant(1)]];
struct BatchConvShape {uint width;uint height;};
kernel void pass3402(device const char* input [[buffer(0)]],constant char* weights [[buffer(1)]],
 constant float* bias [[buffer(2)]],constant float* scales [[buffer(3)]],
 device int* sums [[buffer(4)]],device char* values [[buffer(5)]],
 constant BatchConvShape& shape [[buffer(6)]],uint3 group [[threadgroup_position_in_grid]],uint lane [[thread_index_in_threadgroup]]) {
 threadgroup float a[64],b[64],r[64];
 simdgroup_float8x8 acc(0.f);
 const uint IC=16,OC=32,K=64;uint first=group.x*8;
 uint ow=shape.width/2,count=ow*(shape.height/2);
 for(uint k=0;k<K;k+=8){for(uint j=lane;j<64;j+=32){uint p=first+j/8,t=k+j%8;
  uint x=p%ow*2,y=p/ow*2,tap=t/IC;
  a[j]=p<count?float(input[((y+tap/2)*shape.width+x+tap%2)*IC+t%IC]):0.f;
  b[j]=float(weights[(group.y*8+j%8)*K+k+j/8]);
 }
 threadgroup_barrier(mem_flags::mem_threadgroup);
 simdgroup_float8x8 aa,bb;simdgroup_load(aa,a,8);simdgroup_load(bb,b,8);simdgroup_multiply_accumulate(acc,aa,bb,acc);
 threadgroup_barrier(mem_flags::mem_threadgroup);
 }
 simdgroup_store(acc,r,8);threadgroup_barrier(mem_flags::mem_threadgroup);
 for(uint j=lane;j<64;j+=32){uint parent=first+j/8,c=group.y*8+j%8;if(parent>=count)continue;
 uint p=parent;
 uint i=p*OC+c;float sum=r[j];if(batchTrace)sums[i]=int(sum);
 float v=sum*scales[1];v=v*scales[0];v=v+bias[c];v=v*(1.f/scales[2+c/16]);short q=short(rint(v));values[i]=char(clamp(int(q),-128,127));
 }
}
kernel void pass6402(device const char* input [[buffer(0)]],constant char* weights [[buffer(1)]],
 constant float* bias [[buffer(2)]],constant float* scales [[buffer(3)]],
 device int* sums [[buffer(4)]],device char* values [[buffer(5)]],
 constant BatchConvShape& shape [[buffer(6)]],uint3 group [[threadgroup_position_in_grid]],uint lane [[thread_index_in_threadgroup]]) {
 threadgroup float a[64],b[64],r[64];
 simdgroup_float8x8 acc(0.f);
 const uint IC=32,OC=64,K=128;uint first=group.x*8;
 uint ow=shape.width/2,count=ow*(shape.height/2);
 for(uint k=0;k<K;k+=8){for(uint j=lane;j<64;j+=32){uint p=first+j/8,t=k+j%8;
  uint x=p%ow*2,y=p/ow*2,tap=t/IC;
  a[j]=p<count?float(input[((y+tap/2)*shape.width+x+tap%2)*IC+t%IC]):0.f;
  b[j]=float(weights[(group.y*8+j%8)*K+k+j/8]);
 }
 threadgroup_barrier(mem_flags::mem_threadgroup);
 simdgroup_float8x8 aa,bb;simdgroup_load(aa,a,8);simdgroup_load(bb,b,8);simdgroup_multiply_accumulate(acc,aa,bb,acc);
 threadgroup_barrier(mem_flags::mem_threadgroup);
 }
 simdgroup_store(acc,r,8);threadgroup_barrier(mem_flags::mem_threadgroup);
 for(uint j=lane;j<64;j+=32){uint parent=first+j/8,c=group.y*8+j%8;if(parent>=count)continue;
 uint p=parent;
 uint i=p*OC+c;float sum=r[j];if(batchTrace)sums[i]=int(sum);
 float v=sum*scales[1];v=v*scales[0];v=v+bias[c];v=v*(1.f/scales[2+c/32]);short q=short(rint(v));values[i]=char(clamp(int(q),-128,127));
 }
}
kernel void ct9402(device const char* input[[buffer(0)]],device const char* skip[[buffer(1)]],constant char* weights[[buffer(2)]],constant float* bias[[buffer(3)]],constant float* s[[buffer(4)]],device int* sums[[buffer(5)]],device char* output[[buffer(6)]],constant BatchConvShape& shape[[buffer(7)]],uint3 group [[threadgroup_position_in_grid]],uint lane [[thread_index_in_threadgroup]]){
 threadgroup float a[64],b[64],r[64];
 simdgroup_float8x8 acc(0.f);
 const uint IC=64,OC=32,K=64;uint first=group.x*8;
 uint count=shape.width*shape.height,ow=shape.width*2;
 for(uint k=0;k<K;k+=8){for(uint j=lane;j<64;j+=32){uint p=first+j/8,t=k+j%8;
  a[j]=p<count?float(input[p*IC+t]):0.f;
  b[j]=float(weights[group.z*OC*IC+(group.y*8+j%8)*IC+k+j/8]);
 }
 threadgroup_barrier(mem_flags::mem_threadgroup);
 simdgroup_float8x8 aa,bb;simdgroup_load(aa,a,8);simdgroup_load(bb,b,8);simdgroup_multiply_accumulate(acc,aa,bb,acc);
 threadgroup_barrier(mem_flags::mem_threadgroup);
 }
 simdgroup_store(acc,r,8);threadgroup_barrier(mem_flags::mem_threadgroup);
 for(uint j=lane;j<64;j+=32){uint parent=first+j/8,c=group.y*8+j%8;if(parent>=count)continue;
 uint p=(parent/shape.width*2+group.z/2)*ow+parent%shape.width*2+group.z%2;
 uint i=p*OC+c;float sum=r[j];if(batchTrace)sums[i]=int(sum);
 float factor=s[0]*s[1];float v=sum*factor;v=v+bias[c];float residual=float(skip[i])*s[2];v=v+residual;v=v*(1.f/s[3+c/16]);output[i]=char(clamp(int(rint(v)),-128,127));
 }
}
kernel void ct11402(device const char* input[[buffer(0)]],device const char* skip[[buffer(1)]],constant char* weights[[buffer(2)]],constant float* bias[[buffer(3)]],constant float* s[[buffer(4)]],device int* sums[[buffer(5)]],device char* output[[buffer(6)]],constant BatchConvShape& shape[[buffer(7)]],uint3 group [[threadgroup_position_in_grid]],uint lane [[thread_index_in_threadgroup]]){
 threadgroup float a[64],b[64],r[64];
 simdgroup_float8x8 acc(0.f);
 const uint IC=32,OC=16,K=32;uint first=group.x*8;
 uint count=shape.width*shape.height,ow=shape.width*2;
 for(uint k=0;k<K;k+=8){for(uint j=lane;j<64;j+=32){uint p=first+j/8,t=k+j%8;
  a[j]=p<count?float(input[p*IC+t]):0.f;
  b[j]=float(weights[group.z*OC*IC+(group.y*8+j%8)*IC+k+j/8]);
 }
 threadgroup_barrier(mem_flags::mem_threadgroup);
 simdgroup_float8x8 aa,bb;simdgroup_load(aa,a,8);simdgroup_load(bb,b,8);simdgroup_multiply_accumulate(acc,aa,bb,acc);
 threadgroup_barrier(mem_flags::mem_threadgroup);
 }
 simdgroup_store(acc,r,8);threadgroup_barrier(mem_flags::mem_threadgroup);
 for(uint j=lane;j<64;j+=32){uint parent=first+j/8,c=group.y*8+j%8;if(parent>=count)continue;
 uint p=(parent/shape.width*2+group.z/2)*ow+parent%shape.width*2+group.z%2;
 uint i=p*OC+c;float sum=r[j];if(batchTrace)sums[i]=int(sum);
 float factor=s[0]*s[1];float v=sum*factor;v=v+bias[c];float residual=float(skip[i])*s[2];v=v+residual;v=v*(1.f/s[3]);output[i]=char(clamp(int(rint(v)),-128,127));
 }
}
kernel void ct13402(device const char* input[[buffer(0)]],constant char* weights[[buffer(1)]],constant half* bias[[buffer(2)]],constant float* s[[buffer(3)]],device int* sums[[buffer(4)]],device half* output[[buffer(5)]],constant BatchConvShape& shape[[buffer(6)]],uint3 group [[threadgroup_position_in_grid]],uint lane [[thread_index_in_threadgroup]]){
 threadgroup float a[64],b[64],r[64];
 simdgroup_float8x8 acc(0.f);
 const uint IC=16,OC=8,K=16;uint first=group.x*8;
 uint count=shape.width*shape.height,ow=shape.width*2;
 for(uint k=0;k<K;k+=8){for(uint j=lane;j<64;j+=32){uint p=first+j/8,t=k+j%8;
  a[j]=p<count?float(input[p*IC+t]):0.f;
  b[j]=float(weights[group.z*OC*IC+(group.y*8+j%8)*IC+k+j/8]);
 }
 threadgroup_barrier(mem_flags::mem_threadgroup);
 simdgroup_float8x8 aa,bb;simdgroup_load(aa,a,8);simdgroup_load(bb,b,8);simdgroup_multiply_accumulate(acc,aa,bb,acc);
 threadgroup_barrier(mem_flags::mem_threadgroup);
 }
 simdgroup_store(acc,r,8);threadgroup_barrier(mem_flags::mem_threadgroup);
 for(uint j=lane;j<64;j+=32){uint parent=first+j/8,c=group.y*8+j%8;if(parent>=count)continue;
 uint p=(parent/shape.width*2+group.z/2)*ow+parent%shape.width*2+group.z%2;
 uint i=p*OC+c;float sum=r[j];if(batchTrace)sums[i]=int(sum);
 float factor=s[0]*s[1];half product=half(sum*factor);output[i]=product+bias[c];
 }
}

#include <metal_stdlib>
using namespace metal;
#pragma clang fp contract(off)
// WineForge-Internal: fsr4/mps-block-fp32-bridge-v1.
kernel void gather(device const char* input [[buffer(0)]],device float* output [[buffer(1)]],constant uint2& shape [[buffer(2)]],uint tid [[thread_position_in_grid]]){
 uint count=shape.x*shape.y;if(tid>=count*144)return;uint p=tid/144,k=tid%144;
 int x=int(p%shape.x)+int((k/16)%3)-1,y=int(p/shape.x)+int(k/48)-1;
 output[tid]=(x>=0&&y>=0&&x<int(shape.x)&&y<int(shape.y))?float(input[(uint(y)*shape.x+uint(x))*16+k%16]):0.f;
}
kernel void quantize(device const float* sums [[buffer(0)]],device float* middle [[buffer(1)]],device char* finalOutput [[buffer(2)]],device const char* input [[buffer(3)]],constant float* bias [[buffer(4)]],constant float* s [[buffer(5)]],constant uint2& cfg [[buffer(6)]],uint i [[thread_position_in_grid]]){
 uint stage=cfg.y,n=stage==1?32:16;if(i>=cfg.x*n)return;uint c=i%n;float v=sums[i];
 if(stage==0){v=v*(s[0]*s[1]);v=v+bias[c];v=v*s[4];middle[i]=clamp(rint(v),-128.f,127.f);}
 else if(stage==1){v=v*(s[5]*s[2]);v=v+bias[16+c];v=v*s[6];middle[i]=clamp(rint(v),0.f,127.f);}
 else{v=v*(s[3]*s[7]);v=v+bias[48+c];float res=float(input[i])*s[0];v=v+res;v=v*(1.f/s[8]);finalOutput[i]=char(clamp(rint(v),-128.f,127.f));}
}

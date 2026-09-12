#include <metal_stdlib>
using namespace metal;
#pragma clang fp contract(off)
// Semantic equivalent of signed packed dot4; no hardware instruction assumed.
inline int dot4i8(char4 a, char4 b, int acc) {
 int4 v=int4(a)*int4(b);
 return acc+v.x+v.y+v.z+v.w;
}
kernel void integer_block(device const char* input [[buffer(0)]],
 constant char* weights [[buffer(1)]],constant float* bias [[buffer(2)]],
 constant float* s [[buffer(3)]],device char* output [[buffer(4)]],
 constant uint2& shape [[buffer(5)]],device int* debug [[buffer(6)]],uint pos [[thread_position_in_grid]]) {
 if(pos>=shape.x*shape.y)return;
 char q0[16],q1[32];
 int px=int(pos%shape.x),py=int(pos/shape.x);
 for(uint c=0;c<16;++c){int acc=0;
  for(uint k=0;k<144;k+=4){int x=px+int((k/16)%3)-1,y=py+int(k/48)-1;
   char4 v=char4(0);if(x>=0&&y>=0&&x<int(shape.x)&&y<int(shape.y)){uint idx=(uint(y)*shape.x+uint(x))*16+k%16;v=char4(input[idx],input[idx+1],input[idx+2],input[idx+3]);}
   acc=dot4i8(v,char4(weights[c*144+k],weights[c*144+k+1],weights[c*144+k+2],weights[c*144+k+3]),acc);
  }
  if(pos==0)debug[c]=acc; float v=float(acc)*(s[0]*s[1]);v=v+bias[c];v=v*s[4];q0[c]=char(clamp(rint(v),-128.f,127.f));
 }
 for(uint c=0;c<32;++c){int acc=0;
  for(uint k=0;k<16;k+=4){uint w=2304+c*16+k;acc=dot4i8(char4(q0[k],q0[k+1],q0[k+2],q0[k+3]),char4(weights[w],weights[w+1],weights[w+2],weights[w+3]),acc);}
  if(pos==0)debug[16+c]=acc; float v=float(acc)*(s[5]*s[2]);v=v+bias[16+c];v=v*s[6];q1[c]=char(clamp(rint(v),0.f,127.f));
 }
 for(uint c=0;c<16;++c){int acc=0;
  for(uint k=0;k<32;k+=4){uint w=2816+c*32+k;acc=dot4i8(char4(q1[k],q1[k+1],q1[k+2],q1[k+3]),char4(weights[w],weights[w+1],weights[w+2],weights[w+3]),acc);}
  if(pos==0)debug[48+c]=acc; float v=float(acc)*(s[3]*s[7]);v=v+bias[48+c];float residual=float(input[pos*16+c])*s[0];v=v+residual;v=v*(1.f/s[8]);output[pos*16+c]=char(clamp(rint(v),-128.f,127.f));
 }
}

// FSR4.0.2 fused pre_common.hlsli downscale reduction; AMD reference/license retained.
#include <metal_stdlib>
using namespace metal;
#pragma clang fp contract(off)
struct Pass0Shape { uint width; uint height; };
kernel void pass0402(device const half* input [[buffer(0)]],
                     constant half* weights [[buffer(1)]], constant half* bias [[buffer(2)]],
                     device float* sums [[buffer(3)]], device char* output [[buffer(4)]],
                     constant Pass0Shape& shape [[buffer(5)]], uint i [[thread_position_in_grid]]) {
    uint ow=shape.width/2, oh=shape.height/2;
    if(i>=ow*oh*16) return;
    uint oc=i%16, pixel=i/16, x=(pixel%ow)*2,y=(pixel/ow)*2;
    // Fused image pre-shader: per-pixel dot pairs, then quad X and Y reductions.
    float lanes[4];
    for(uint tap=0;tap<4;++tap) {
        float acc=0;
        for(uint c=0;c<8;c+=2) {
            uint base=((y+tap/2)*shape.width+x+tap%2)*8+c;
            uint wi=oc*32+tap*8+c;
            float a=float(input[base])*float(weights[wi]);
            float b=float(input[base+1])*float(weights[wi+1]);
            acc=acc+(a+b);
        }
        lanes[tap]=acc;
    }
    float top=lanes[0]+lanes[1],bottom=lanes[2]+lanes[3];
    float sum=top+bottom;
    if(batchTrace)sums[i]=sum;
    float v=sum+float(bias[oc]); v=v*(1.f/float(0.012475206516683102));
    short q=short(rint(v)); output[i]=char(clamp(int(q),-128,127));
}

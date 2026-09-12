#include <metal_stdlib>
using namespace metal;
constant bool batchTrace [[function_constant(1)]];
kernel void mask_border(device uchar* b [[buffer(0)]],constant uint* p [[buffer(1)]],uint i [[thread_position_in_grid]]){
 if(i>=p[0]*p[1]||(i%p[0]<p[2]&&i/p[0]<p[3]))return;
 for(uint c=0;c<p[4];++c)b[i*p[4]+c]=0;
}

// WineForge-Internal: fsr4/typed-metal-temporal-v1.
// Arithmetic port of src/fsr_d3d12_temporal.hlsl; AMD license/provenance retained.
#include <metal_stdlib>
using namespace metal;
#pragma clang fp contract(off)
struct FrameParams {uint width,height,renderWidth,renderHeight;float jx,jy,mvx,mvy;uint reset,inverted,jittered,reserved;float cx,cy,exposure,padding;uint pitch[4],format[4],outputPitch;};
float dxExp(float v){return exp2(v*1.4426950408889634f);}
float3 dxExp(float3 v){return exp2(v*1.4426950408889634f);}
float3 dxLog(float3 v){return log2(v)*.6931471805599453f;}
float dxRcp(float v){return 1.f/v;}
float3 dxRcp(float3 v){return 1.f/v;}
float fromHalf(uint b){return float(as_type<half>(ushort(b)));}
float3 load3(device const float* b,uint i){return float3(b[i*3],b[i*3+1],b[i*3+2]);}
float4 load4(device const float* b,uint i){return float4(b[i*4],b[i*4+1],b[i*4+2],b[i*4+3]);}
void store3(device float* b,uint i,float3 v){for(uint c=0;c<3;c++)b[i*3+c]=v[c];}
void store4(device float* b,uint i,float4 v){for(uint c=0;c<4;c++)b[i*4+c]=v[c];}
ushort4 vload4(device const ushort* b){return ushort4(b[0],b[1],b[2],b[3]);}
#define W cfg.width
#define H cfg.height
#define RW cfg.renderWidth
#define RH cfg.renderHeight
#define jx cfg.jx
#define jy cfg.jy
#define mvx cfg.mvx
#define mvy cfg.mvy
#define reset cfg.reset
#define inverted cfg.inverted
#define jittered cfg.jittered
#define reserved cfg.reserved
#define cx cfg.cx
#define cy cfg.cy
#define exposure cfg.exposure
#define padding cfg.padding
struct Temporal {
 constant FrameParams& cfg;
 device const uchar *low,*depth,*motion,*inputExposure;
 device const float *oldHistory,*oldRecurrent;
 device ushort *features;
 device float *pre;
 device const ushort *network;
 device float *history,*recurrent;
 device uchar* output;
 float loadExposure(){return cfg.format[3]==54?float(*(device const half*)inputExposure):*(device const float*)inputExposure;}
 float3 loadColor(int2 p){auto b=low+uint(p.y)*cfg.pitch[0];if(cfg.format[0]==10){auto v=(device const half*)(b+uint(p.x)*8);return float3(v[0],v[1],v[2]);}auto v=(device const float*)(b+uint(p.x)*16);return float3(v[0],v[1],v[2]);}
 float readDepth(int2 p){uint v=*(device const uint*)(depth+uint(p.y)*cfg.pitch[1]+uint(p.x)*4);return cfg.format[1]==44?float(v&0xffffffu)/16777215.f:as_type<float>(v);}
 float2 readMotion(int2 p){auto b=motion+uint(p.y)*cfg.pitch[2];if(cfg.format[2]==34){auto v=(device const half*)(b+uint(p.x)*4);return float2(v[0],v[1]);}auto v=(device const float*)(b+uint(p.x)*8);return float2(v[0],v[1]);}
 void writeColor(uint i,float3 v){auto b=(device ushort*)(output+(i/W)*cfg.outputPitch+(i%W)*8);for(uint c=0;c<3;c++)b[c]=ushort(halfRNE(v[c]));b[3]=0x3c00;}
// Explicit binary16 RNE, already verified against1126 boundary values.
uint halfRNE(float value){
    uint bits=as_type<uint>(value),sign=(bits>>16)&32768,exponent=(bits>>23)&255,mantissa=bits&8388607;
    if(exponent==255)return sign|(mantissa?32256:31744);
    if(exponent>142)return sign|31744;
    if(exponent<113){if(exponent<102)return sign;mantissa|=8388608;uint shift=126-exponent,base=mantissa>>shift;
        uint remainder=mantissa&((1u<<shift)-1),halfway=1u<<(shift-1);if(remainder>halfway||(remainder==halfway&&(base&1)))base++;return sign|base;}
    uint base=((exponent-112)<<10)|(mantissa>>13),remainder=mantissa&8191;if(remainder>4096||(remainder==4096&&(base&1)))base++;return sign|base;
}
float effectiveExposure(){
    float e=loadExposure();return (e==0?1.f:e)/exposure;
}
float3 inputColor(int2 p){p=clamp(p,int2(0,0),int2(RW-1,RH-1));
    float3 c=loadColor(p)*effectiveExposure();
    return max(.1174f*dxLog(1.f+150.f*c),0.f);}
float3 ycc(float3 c){float y=c.r*.2126f+c.g*.7152f+c.b*.0722f;return float3(y,(c.b-y)/1.8556f,(c.r-y)/1.5748f);}
float4 linearSample(float2 p,bool rec){int2 base=int2(floor(p));float2 f=p-float2(base);float4 value=0;
    for(int y=0;y<2;y++)for(int x=0;x<2;x++){int2 at=clamp(base+int2(x,y),int2(0,0),int2(W-1,H-1));float weight=(x?f.x:1-f.x)*(y?f.y:1-f.y);
        float4 v=0;if(rec)v=load4(oldRecurrent,at.y*W+at.x);else v.xyz=load3(oldHistory,at.y*W+at.x);value+=v*weight;}
    return value;
}
float3 cubicSample(float2 p){float2 base=floor(p),f=p-base;
    float2 w0=f*(-.5f+f*(1.f-.5f*f)),w1=1.f+f*f*(-2.5f+1.5f*f),w2=f*(.5f+f*(2.f-1.5f*f)),w3=f*f*(-.5f+.5f*f),w12=w1+w2;
    float2 pos[3]={base-1.f,base+w2/w12,base+2.f};float2 weights[3]={w0,w12,w3};float3 sum=0;
    for(uint y=0;y<3;y++)for(uint x=0;x<3;x++)sum+=linearSample(float2(pos[x].x,pos[y].y),false).xyz*weights[x].x*weights[y].y;return sum;
}
float loadDepth(int2 p){if(any(p<0)||p.x>=int(RW)||p.y>=int(RH))return 0;
    return readDepth(p);
}
float2 loadMotion(int2 p){
    return readMotion(p);
}
void prepare(uint i){if(i>=W*H)return;int2 pixel=int2(i%W,i/W);
    float2 invScale=float2(RW,RH)/float2(W,H),scale=float2(W,H)/float2(RW,RH),origin=.5f*invScale-.5f;
    float2 pos=origin+float2(pixel)*invScale+float2(jx,jy);int2 base=int2(floor(pos));float3 color=0;float total=0,center=0;
    for(int y=0;y<2;y++)for(int x=0;x<2;x++){int2 at=base+int2(x,y);float2 d=(float2(at)-pos)*scale;
        float w=dxExp((-.5f/(.47f*.47f))*d.x*d.x)*dxExp((-.5f/(.47f*.47f))*d.y*d.y);color+=inputColor(at)*w;total+=w;center=max(center,w);}color/=total;
    int2 selected=int2(rint(float2(pixel)*invScale+origin));float nearest=loadDepth(selected);
    for(int x=-1;x<=1;x++)for(int y=-1;y<=1;y++){if(x==0&&y==0)continue;int2 at=int2(rint(float2(pixel+int2(x,y))*invScale+origin));float z=loadDepth(at);
        if(inverted?z>nearest:z<nearest){nearest=z;selected=at;}}
    float2 velocity=0;if(all(selected>=0)&&selected.x<int(RW)&&selected.y<int(RH))velocity=loadMotion(selected)*float2(mvx,mvy);
    if(jittered)velocity-=float2(cx,cy);bool invalid=any(isnan(velocity))||any(isinf(velocity))||any(velocity>1000000.f);
    float3 hist=reset?color:float3(0,0,0);float4 rec=0;uint decision=reset?1:invalid?2:0;
    if(!reset&&!invalid){float2 oldPos=float2(pixel)+velocity*float2(W,H);bool onscreen=all(oldPos>=0)&&oldPos.x<=W-1&&oldPos.y<=H-1;
        if(onscreen){hist=cubicSample(oldPos);rec=linearSample(oldPos,true);decision=4;}else decision=8;
        float alpha=.1f*center;hist=alpha*color+(1.f-alpha)*max(hist,0.f);}
    store3(pre,i,hist);
    float3 currentYcc=ycc(color),historyYcc=ycc(hist);float2 delta=currentYcc.yz-historyYcc.yz;uint bits[8];
    bits[0]=halfRNE(2.f*fromHalf(halfRNE(currentYcc.x))-1.f);bits[1]=halfRNE(2.f*fromHalf(halfRNE(historyYcc.x))-1.f);
    bits[2]=halfRNE(sqrt(delta.x*delta.x+delta.y*delta.y+1e-6f));for(uint c=0;c<4;c++)bits[3+c]=halfRNE(2.f*fromHalf(halfRNE(rec[c]))-1.f);bits[7]=0;
    uint featurePixel=(i/W)*((W+7u)&~7u)+i%W;
    for(uint c=0;c<8;c++)features[featurePixel*8+c]=ushort(bits[c]);
}
void reconstruct(uint i){if(i>=W*H)return;
    uint networkPixel=(i/W)*((W+7u)&~7u)+i%W;ushort4 raw0=vload4(network+networkPixel*8),raw1=vload4(network+networkPixel*8+4);float params[8];
    for(uint c=0;c<8;c++)params[c]=fromHalf(c<4?raw0[c]:raw1[c-4]);
    float ep=dxExp(params[0]),em=dxExp(-params[0]),corr=(ep-em)/(ep+em),sx=2.f/(1.f+dxExp(-params[1])),sy=2.f/(1.f+dxExp(-params[2]));
    float sx2=sx*sx,sy2=sy*sy,sxy=sx*sy;float2 invScale=float2(RW,RH)/float2(W,H),scale=float2(W,H)/float2(RW,RH);
    float2 pos=(.5f*invScale-.5f)+float2(i%W,i/W)*invScale+float2(jx,jy);int2 base=int2(rint(pos));float3 sum=0;float total=0;
    for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++){int2 at=base+int2(x,y);float2 d=(float2(at)-pos)*scale;
        float w=dxExp((-.5f/(.47f*.47f))*(d.x*d.x*sx2+2.f*corr*(d.x*d.y)*sxy+d.y*d.y*sy2));sum+=inputColor(at)*w;total+=w;}
    float blend=1.f/(1.f+dxExp(-params[3]));float3 model=sum/total*(1.f-blend)+load3(pre,i)*blend;
    float3 outputColor=clamp((dxExp(8.51788f*model)-1.f)/(150.f*effectiveExposure()),0.f,64000.f);
    // Make the already-validated RNE contract explicit before the typed store.
    float3 halfColor;for(uint c=0;c<3;c++)halfColor[c]=fromHalf(halfRNE(outputColor[c]));
    if(!reserved)writeColor(i,halfColor);
    store3(history,i,model);
    float4 rec;
    for(uint c=0;c<4;c++)rec[c]=1.f/(1.f+dxExp(-params[4+c]));store4(recurrent,i,rec);
}

// WineForge-Internal: fsr4/amd-rcas-model-colorspace-v1.
// Derived from AMD FsrRcasF / rcas.hlsl at 01446e6a74888bf349652fcf2cbf5f642d30c2bf.
// Copyright (C) 2025 Advanced Micro Devices, Inc. MIT notice: reference/AMD-LICENSE.md.
// History remains FP32 and unsharpened. Simulate the original RGBA16F RCAS
// intermediary on load, including zero-valued out-of-bounds texture loads.
float3 rcasLoad(int2 p){
    if(any(p<0)||p.x>=int(W)||p.y>=int(H))return 0;
    float3 c=clamp(load3(history,p.y*W+p.x),0.f,64000.f);
    for(uint k=0;k<3;k++)c[k]=fromHalf(halfRNE(c[k]));return c;
}
float rcasMediumRcp(float v){float b=as_type<float>(0x7ef19fffu-as_type<uint>(v));return b*(-b*v+2.f);}
float rcasLuma(float3 v){return v.b*.5f+(v.r*.5f+v.g);}
void sharpen(uint i){if(i>=W*H)return;int2 p=int2(i%W,i/W);
    float3 b=rcasLoad(p+int2(0,-1)),d=rcasLoad(p+int2(-1,0)),e=rcasLoad(p),f=rcasLoad(p+int2(1,0)),h=rcasLoad(p+int2(0,1));
    float bl=rcasLuma(b),dl=rcasLuma(d),el=rcasLuma(e),fl=rcasLuma(f),hl=rcasLuma(h);
    float nz=.25f*bl+.25f*dl+.25f*fl+.25f*hl-el;
    float range=max(max(max(bl,dl),el),max(fl,hl))-min(min(min(bl,dl),el),min(fl,hl));
    nz=saturate(abs(nz)*rcasMediumRcp(range));
    nz=-.5f*nz+1.f;
    float3 mn=min(min(min(b,d),f),h),mx=max(max(max(b,d),f),h);
    float lower=saturate(el/min(min(min(bl,dl),fl),hl));
    float3 hitMin=mn*dxRcp(4.f*mx)*lower,hitMax=(1.f-mx)*dxRcp(4.f*mn-4.f);
    float3 lobes=max(-hitMin,hitMax);
    float lobe=max(-.1875f,min(max(max(lobes.r,lobes.g),lobes.b),0.f))*padding;
    lobe*=nz;
    float3 c=(lobe*b+lobe*d+lobe*h+lobe*f+e)*rcasMediumRcp(4.f*lobe+1.f);
    c=max((dxExp(8.51788f*c)-1.f)/150.f,0.f)/effectiveExposure();
    // Keep finite FP16 storage for HDR peaks, as on the non-RCAS path.
    c=min(c,64000.f);
    for(uint k=0;k<3;k++)c[k]=fromHalf(halfRNE(c[k]));writeColor(i,c);
}
};
kernel void native_prepare(constant FrameParams& cfg [[buffer(0)]],device const uchar* low [[buffer(1)]],device const uchar* depth [[buffer(2)]],device const uchar* motion [[buffer(3)]],device const uchar* inputExposure [[buffer(4)]],device const float* oldHistory [[buffer(5)]],device const float* oldRecurrent [[buffer(6)]],device ushort* features [[buffer(7)]],device float* pre [[buffer(8)]],device const ushort* network [[buffer(9)]],device float* history [[buffer(10)]],device float* recurrent [[buffer(11)]],device uchar* output [[buffer(12)]],uint i [[thread_position_in_grid]]){
 Temporal t{cfg,low,depth,motion,inputExposure,oldHistory,oldRecurrent,features,pre,network,history,recurrent,output};t.prepare(i);
}
kernel void native_reconstruct(constant FrameParams& cfg [[buffer(0)]],device const uchar* low [[buffer(1)]],device const uchar* depth [[buffer(2)]],device const uchar* motion [[buffer(3)]],device const uchar* inputExposure [[buffer(4)]],device const float* oldHistory [[buffer(5)]],device const float* oldRecurrent [[buffer(6)]],device ushort* features [[buffer(7)]],device float* pre [[buffer(8)]],device const ushort* network [[buffer(9)]],device float* history [[buffer(10)]],device float* recurrent [[buffer(11)]],device uchar* output [[buffer(12)]],uint i [[thread_position_in_grid]]){
 Temporal t{cfg,low,depth,motion,inputExposure,oldHistory,oldRecurrent,features,pre,network,history,recurrent,output};t.reconstruct(i);
}
kernel void native_sharpen(constant FrameParams& cfg [[buffer(0)]],device const uchar* low [[buffer(1)]],device const uchar* depth [[buffer(2)]],device const uchar* motion [[buffer(3)]],device const uchar* inputExposure [[buffer(4)]],device const float* oldHistory [[buffer(5)]],device const float* oldRecurrent [[buffer(6)]],device ushort* features [[buffer(7)]],device float* pre [[buffer(8)]],device const ushort* network [[buffer(9)]],device float* history [[buffer(10)]],device float* recurrent [[buffer(11)]],device uchar* output [[buffer(12)]],uint i [[thread_position_in_grid]]){
 Temporal t{cfg,low,depth,motion,inputExposure,oldHistory,oldRecurrent,features,pre,network,history,recurrent,output};t.sharpen(i);
}

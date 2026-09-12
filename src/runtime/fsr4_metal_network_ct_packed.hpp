#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdexcept>
#include <cstdio>
#include "fsr4_metal_asset_paths.hpp"
// WineForge-Internal: fsr4/caller-owned-metal-network-encoder-v1.
// No Wine ABI, queue, submit, wait, or mapped-memory ownership in the graph.
class Fsr4MetalNetworkCtPacked {
 id<MTLDevice> device; NSString* tileSource;NSString* tailSource;NSString* fnb32Source;NSString* fnb64Source;bool tiledFnb32;bool packedCt;
 id<MTLComputePipelineState> pipelines[16],maskPipeline;
 id<MTLBuffer> constants[17][4],tensors[15],dummy,ctPacked;
 unsigned graphWidth=0,graphHeight=0;
 static void need(bool x,const char* why){if(!x)throw std::runtime_error(why);}
struct Layer {unsigned pass,kind,channels,skip;};
static constexpr Layer layers[]={
 {0,0,8,0},{1,1,16,0},{2,1,16,0},{3,2,16,0},
 {4,3,32,0},{5,3,32,0},{6,2,32,0},{7,3,64,0},
 {8,3,64,0},{9,3,64,0},{9,4,64,6},{10,3,32,0},
 {11,3,32,0},{11,4,32,3},{12,1,16,0},{13,1,16,0},{13,4,16,0}};
id<MTLLibrary> library(NSString* file,NSString* prefix){
 auto text=[NSString stringWithContentsOfFile:fsr4AssetPath(@"shaders",file) encoding:NSUTF8StringEncoding error:nil];need(text!=nil,"graph shader source");
 auto options=[MTLCompileOptions new];options.languageVersion=MTLLanguageVersion2_3;options.mathMode=MTLMathModeSafe;options.mathFloatingPointFunctions=MTLMathFloatingPointFunctionsPrecise;
 NSError* e=nil;auto lib=[device newLibraryWithSource:[prefix stringByAppendingString:text] options:options error:&e];
 need(lib&&e==nil,e?e.localizedDescription.UTF8String:"graph compile");return lib;
}
id<MTLComputePipelineState> pipeline(id<MTLLibrary> lib,NSString* name,int fold=-1,bool fused=false){
 auto c=[MTLFunctionConstantValues new];bool no=false;[c setConstantValue:&no type:MTLDataTypeBool atIndex:1];
 if(fold>=0){bool value=fold!=0;[c setConstantValue:&value type:MTLDataTypeBool atIndex:0];}
 if(fused)[c setConstantValue:&no type:MTLDataTypeBool atIndex:2];
 NSError* e=nil;auto f=[name isEqual:@"block16_tile8x2"]?[lib newFunctionWithName:name]:[lib newFunctionWithName:name constantValues:c error:&e];need(f&&e==nil,"graph entry");
 auto p=[device newComputePipelineStateWithFunction:f error:&e];need(p&&e==nil,e?e.localizedDescription.UTF8String:"graph pipeline");
 need(p.threadExecutionWidth==32&&p.maxTotalThreadsPerThreadgroup>=(([name isEqual:@"block16_tile8x2"]||[name isEqual:@"block13_ct13_tile8x2"])?64u:32u)&&p.staticThreadgroupMemoryLength<=device.maxThreadgroupMemoryLength,"graph dispatch limits");return p;
}
void initialize(id<MTLDevice> supplied,unsigned width,unsigned height){
 need(!device&&width&&height&&width<=3440&&height<=1440,"bounded graph extent/context");
 graphWidth=(width+7)&~7u;graphHeight=(height+7)&~7u;device=supplied;need(device&&[device supportsFamily:MTLGPUFamilyApple7],"Apple7 matrix admission");

 auto p0=library(@"wine_temporal_batch_pass0.metal",@"constant bool batchTrace [[function_constant(1)]];\n");
 auto b16=library(tileSource,@"");
 auto b32=library(fnb32Source,@"#define FNB_RAW_TRACE 0\n");
 auto b64=library(fnb64Source,@"#define FNB64_RAW_TRACE 0\n");
 auto conv=library(@"wine_temporal_batch_convolutions.metal",@"");
 auto tail=library(tailSource,@"#define BLOCK16_RAW_TRACE 0\n");
 pipelines[0]=pipeline(p0,@"pass0402");auto plain16=pipeline(b16,@"block16_tile8x2");
 auto plain32=pipeline(b32,@"fnb32_matrix",0),fold32=pipeline(b32,@"fnb32_matrix",1);
 auto plain64=pipeline(b64,@"fnb64_matrix",0),fold64=pipeline(b64,@"fnb64_matrix",1);
 for(unsigned i=1;i<15;++i){auto l=layers[i];
  if(l.kind==1)pipelines[i]=plain16;
  else if(l.kind==3)pipelines[i]=l.channels==32?(l.pass==11?fold32:plain32):(l.pass==9?fold64:plain64);
  else pipelines[i]=pipeline(conv,l.kind==2?(l.pass==3?@"pass3402":@"pass6402"):(l.pass==9?@"ct9402":@"ct11402"));
 }
 pipelines[15]=pipeline(tail,@"block13_ct13_tile8x2",-1,true);
 const size_t sizes[17][4]={
  {1024,32,0,0},{3328,256,36,6656},{3328,256,36,6656},{2048,128,16,0},
  {6400,448,32,12800},{6400,448,32,12800},{8192,256,16,0},
  {20992,896,32,41984},{20992,896,32,41984},{20992,896,32,41984},
  {8192,128,20,0},{6400,448,32,12800},{6400,448,32,12800},{2048,64,20,0},
  {3328,256,36,6656},{3328,256,36,6656},{512,16,8,0}};
 NSString* names[]={@"weights.bin",@"biases.bin",@"scales.bin",@"packed.bin"};
 size_t constantBytes=0;
 for(unsigned i=0;i<17;++i)for(unsigned j=0;j<4;++j)if(sizes[i][j]){
  auto data=[NSData dataWithContentsOfFile:fsr4AssetPath(@"weights",[NSString stringWithFormat:@"%u/%@",i,names[j]])];
  need(data&&data.length==sizes[i][j],"exact graph asset span");
  constants[i][j]=[device newBufferWithBytes:data.bytes length:data.length options:MTLResourceStorageModeShared];
  need(constants[i][j]!=nil,"graph weights allocation");constantBytes+=data.length;
 }
 // Prepare both arms identically; only candidate binds this immutable layout.
 _Float16 packed[512];auto original=static_cast<const int8_t*>(constants[16][0].contents);
 for(unsigned z=0;z<4;++z)for(unsigned k=0;k<16;++k)for(unsigned c=0;c<8;++c){
  packed[z*128+k*8+c]=_Float16(original[z*128+c*16+k]);
  need(float(packed[z*128+k*8+c])==float(original[z*128+c*16+k]),"exact CT weight packing");
 }
 ctPacked=[device newBufferWithBytes:packed length:sizeof(packed) options:MTLResourceStorageModeShared];need(ctPacked!=nil,"packed CT allocation");constantBytes+=sizeof(packed);
 // Dense activations only; disabled per-channel traces use a 256-byte dummy.
 // 36*outputPixels bytes, max 178329600 bytes. Two external maps add
 // 32*outputPixels (158515200 bytes at 3440x1440), plus page rounding.
 unsigned w=graphWidth,h=graphHeight,c=8;size_t tensorBytes=0;
 for(unsigned i=0;i<15;++i){auto l=layers[i];if(l.kind==0||l.kind==2){w/=2;h/=2;c*=2;}if(l.kind==4){w*=2;h*=2;c/=2;}
  size_t bytes=size_t(w)*h*c;tensorBytes+=bytes;need(tensorBytes<=192ull*1024*1024,"native activation budget");
  tensors[i]=[device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];need(tensors[i]!=nil,"graph activation allocation");
 }
 dummy=[device newBufferWithLength:256 options:MTLResourceStorageModeShared];need(dummy!=nil,"trace dummy");
 activationBytes=tensorBytes;assetBytes=constantBytes;
 printf("NATIVE_GRAPH_READY width=%u height=%u tensor_bytes=%zu constant_bytes=%zu layers=17 dispatches=16 fused_tail=1\n",width,height,tensorBytes,constantBytes);
}

 void mask(id<MTLCommandBuffer> cmd,id<MTLBuffer> b,unsigned w,unsigned h,unsigned vw,unsigned vh,unsigned bytes){
  if(w==vw&&h==vh)return;
  auto enc=[cmd computeCommandEncoder];need(enc!=nil,"mask encoder");[enc setComputePipelineState:maskPipeline];[enc setBuffer:b offset:0 atIndex:0];
  unsigned p[]={w,h,vw,vh,bytes};[enc setBytes:p length:sizeof(p) atIndex:1];
  [enc dispatchThreads:MTLSizeMake(size_t(w)*h,1,1) threadsPerThreadgroup:MTLSizeMake(32,1,1)];[enc endEncoding];
 }
public:
 id<MTLBuffer> diagnosticTensor(unsigned index)const{need(index<15,"diagnostic index");return tensors[index];}
 size_t activationBytes=0,assetBytes=0;
 Fsr4MetalNetworkCtPacked(id<MTLDevice> d,unsigned w,unsigned h,NSString* path,NSString* tailPath,NSString* fnb32Path,NSString* fnb64Path,bool tiled,bool packCt):tileSource(path),tailSource(tailPath),fnb32Source(fnb32Path),fnb64Source(fnb64Path),tiledFnb32(tiled),packedCt(packCt){need(path&&tailPath,"tile source paths");initialize(d,w,h);
  auto lib=library(@"fsr4_metal_mask.metal",@"");maskPipeline=pipeline(lib,@"mask_border");
 }
 Fsr4MetalNetworkCtPacked(const Fsr4MetalNetworkCtPacked&)=delete;
 Fsr4MetalNetworkCtPacked& operator=(const Fsr4MetalNetworkCtPacked&)=delete;
 void encode(id<MTLCommandBuffer> cmd,id<MTLBuffer> input,id<MTLBuffer> output,unsigned width,unsigned height,bool tailOnly=false){
  unsigned w=(width+7)&~7u,h=(height+7)&~7u,divisor=1;
  need(cmd&&width&&height&&w<=graphWidth&&h<=graphHeight&&input.length>=size_t(w)*h*16&&output.length>=size_t(w)*h*16,"native graph encode spans");
  if(tailOnly){w/=2;h/=2;divisor=2;}else mask(cmd,input,w,h,width,height,16);
 for(unsigned i=tailOnly?15:0;i<16;++i){auto l=layers[i];auto enc=[cmd computeCommandEncoder];need(enc!=nil,"graph encoder");
  [enc setComputePipelineState:pipelines[i]];
  auto bind=[&](id<MTLBuffer> b,unsigned index){need(b!=nil,"graph binding");[enc setBuffer:b offset:0 atIndex:index];};
  bind(i?tensors[i-1]:input,0);uint32_t shape[]={w,h};unsigned groups=(w*h+7)/8,gy=1,gz=1,threads=32;
  if(i==0){bind(constants[i][0],1);bind(constants[i][1],2);bind(dummy,3);bind(tensors[i],4);[enc setBytes:shape length:8 atIndex:5];
   w/=2;h/=2;divisor*=2;groups=(w*h*16+31)/32;
  }else if(l.kind==1||l.kind==3){
   for(unsigned j=0;j<3;++j)bind(constants[i][j],j+1);bind(dummy,4);bind(dummy,5);[enc setBytes:shape length:8 atIndex:6];bind(constants[i][3],9);
   if(i==15){bind(dummy,8);for(unsigned j=0;j<3;++j)bind(j==0&&packedCt?ctPacked:constants[16][j],10+j);bind(dummy,13);bind(output,14);}
   else bind(tensors[i],l.kind==1?8:7);
   if(l.kind==1||(tiledFnb32&&l.kind==3&&l.channels==32)){groups=(w+7)/8;gy=(h+1)/2;threads=64;}
  }else if(l.kind==2){
   for(unsigned j=0;j<3;++j)bind(constants[i][j],j+1);bind(dummy,4);bind(tensors[i],5);[enc setBytes:shape length:8 atIndex:6];
   w/=2;h/=2;divisor*=2;groups=(w*h+7)/8;gy=l.pass==3?4:8;
  }else{
   bind(tensors[l.skip-1],1);for(unsigned j=0;j<3;++j)bind(constants[i][j],j+2);bind(dummy,5);bind(tensors[i],6);[enc setBytes:shape length:8 atIndex:7];
   gy=l.pass==9?4:2;gz=4;w*=2;h*=2;divisor/=2;
  }
  [enc dispatchThreadgroups:MTLSizeMake(groups,gy,gz) threadsPerThreadgroup:MTLSizeMake(threads,1,1)];[enc endEncoding];
  if(i<15) mask(cmd,tensors[i],w,h,(width+divisor-1)/divisor,(height+divisor-1)/divisor, i==0?16:(l.kind==2?l.channels*2:l.kind==4?l.channels/2:l.channels));
 }
 }
};

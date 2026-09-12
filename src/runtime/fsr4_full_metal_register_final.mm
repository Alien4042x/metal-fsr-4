#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <unistd.h>
#include <memory>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include "fsr4_full_metal_abi.h"
#include "fsr4_metal_network_ct_packed.hpp"
// WineForge-Internal: fsr4/full-native-frame-owned-transport-v1.
static id<MTLDevice> device;
static void need(bool x,const char* why){if(!x)throw std::runtime_error(why);}
static id<MTLBuffer> alias(uint64_t ptr,uint64_t bytes,bool write){
 need(ptr&&bytes&&bytes<=80ull*1024*1024&&ptr%getpagesize()==0&&bytes%getpagesize()==0,"bounded page-aligned graph map");
 mach_vm_address_t base=ptr;mach_vm_size_t size=0;vm_region_basic_info_data_64_t info{};
 mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t object=MACH_PORT_NULL;
 auto rc=mach_vm_region(mach_task_self(),&base,&size,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&count,&object);
 if(object!=MACH_PORT_NULL)mach_port_deallocate(mach_task_self(),object);
 need(rc==KERN_SUCCESS&&base<=ptr&&ptr-base<=size&&bytes<=size-(ptr-base)&&(info.protection&VM_PROT_READ)&&(!write||(info.protection&VM_PROT_WRITE)),"mapped VM coverage/protection");
 auto b=[device newBufferWithBytesNoCopy:(void*)(uintptr_t)ptr length:bytes options:MTLResourceStorageModeShared deallocator:nil];
 need(b&&b.contents==(void*)(uintptr_t)ptr,"exact graph alias");return b;
}

class FullMetalContext {
 id<MTLCommandQueue> queue;
 id<MTLComputePipelineState> prepare,reconstruct,sharpen;
 std::unique_ptr<Fsr4MetalNetworkCtPacked> network;
 id<MTLBuffer> feature,prepared,neural,history[2],recurrent[2];
 unsigned maxW,maxH,parity=0,frames=0,lastW=0,lastH=0,lastRW=0,lastRH=0;
 float lastExposure=0;
 size_t own=0;
 id<MTLBuffer> buffer(size_t n){need(n&&n<=80ull*1024*1024&&own+n<=800ull*1024*1024,"native image allocation budget");own+=n;auto b=[device newBufferWithLength:n options:MTLResourceStorageModePrivate];need(b!=nil,"native image allocation");return b;}
public:
 explicit FullMetalContext(FullMetalArgs* a):maxW(a->maxWidth),maxH(a->maxHeight){
  need(maxW&&maxH&&maxW<=3440&&maxH<=1440,"full image admission");device=MTLCreateSystemDefaultDevice();need(device!=nil,"native device");queue=[device newCommandQueue];need(queue!=nil,"native queue");
  const size_t p=size_t(maxW)*maxH,pad=size_t((maxW+7)&~7u)*((maxH+7)&~7u);
  const size_t planned=pad*68+p*68+334512; // graph36P + feature16P + output16P, pre12P + histories56P.
  need(planned<800ull*1024*1024&&planned<device.recommendedMaxWorkingSetSize/4,"full working-set preflight");
  network=std::make_unique<Fsr4MetalNetworkCtPacked>(device,maxW,maxH,@"matrix_block16_register_final.metal",@"matrix_block13_ct13_prepacked.metal",@"matrix_fnb32_tile8x2.metal",@"matrix_fnb64_hoist.metal",true,true);own=network->activationBytes+network->assetBytes+256;
  feature=buffer(pad*16);neural=buffer(pad*16);prepared=buffer(p*12);
  for(unsigned i=0;i<2;++i){history[i]=buffer(p*12);recurrent[i]=buffer(p*16);}
  auto source=[NSString stringWithContentsOfFile:fsr4AssetPath(@"shaders",@"fsr4_metal_temporal.metal") encoding:NSUTF8StringEncoding error:nil];need(source!=nil,"typed temporal source");
  auto options=[MTLCompileOptions new];options.languageVersion=MTLLanguageVersion2_3;options.mathMode=MTLMathModeSafe;options.mathFloatingPointFunctions=MTLMathFloatingPointFunctionsPrecise;NSError* e=nil;
  auto lib=[device newLibraryWithSource:source options:options error:&e];need(lib&&e==nil,e?e.localizedDescription.UTF8String:"typed temporal compile");
  auto pipeline=[&](NSString* name){auto p=[device newComputePipelineStateWithFunction:[lib newFunctionWithName:name] error:&e];need(p&&e==nil,e?e.localizedDescription.UTF8String:"typed temporal pipeline");need(p.threadExecutionWidth==32&&p.maxTotalThreadsPerThreadgroup>=32,"typed temporal dispatch");return p;};
  prepare=pipeline(@"native_prepare");reconstruct=pipeline(@"native_reconstruct");sharpen=pipeline(@"native_sharpen");a->liveBytes=own;
  printf("NATIVE_FULL_READY four_tiles=1 directxy=1 max=%ux%u native_owned_bytes=%zu neural_layers=17 temporal=1 rcas=1\n",maxW,maxH,own);
 }
 void execute(FullMetalArgs* a){
  auto& p=a->params;need(p.width&&p.height&&p.width<=maxW&&p.height<=maxH&&p.renderWidth&&p.renderHeight&&p.renderWidth<=p.width&&p.renderHeight<=p.height,"logical frame admission");
  need(a->frame==frames&&(!frames?p.reset:true),"frame sequence/reset");need(p.reset|| (lastW==p.width&&lastH==p.height&&lastRW==p.renderWidth&&lastRH==p.renderHeight&&lastExposure==p.exposure),"resize/exposure resets history");
  need(std::isfinite(p.exposure)&&p.exposure>0&&std::isfinite(p.jx)&&std::isfinite(p.jy)&&std::isfinite(p.mvx)&&std::isfinite(p.mvy)&&std::isfinite(p.cx)&&std::isfinite(p.cy)&&std::isfinite(p.padding)&&p.reset<=1&&p.inverted<=1&&p.jittered<=1&&p.reserved<=1,"finite native parameters");
  need((p.format[0]==10||p.format[0]==2)&&(p.format[1]==41||p.format[1]==44)&&(p.format[2]==34||p.format[2]==16)&&(p.format[3]==54||p.format[3]==41),"typed transport formats");
  unsigned bpp[]={p.format[0]==10?8u:16u,4,p.format[2]==34?4u:8u,p.format[3]==54?2u:4u};id<MTLBuffer> input[4];
  for(unsigned i=0;i<4;++i){unsigned width=i==3?1:p.renderWidth,rows=i==3?1:p.renderHeight;
   need(p.pitch[i]%256==0&&p.pitch[i]>=width*bpp[i]&&uint64_t(p.pitch[i])*rows<=a->inputBytes[i],"input row bounds");input[i]=alias(a->input[i],a->inputBytes[i],false);
   need(a->input[i]!=a->output,"distinct output map");for(unsigned j=0;j<i;++j)need(a->input[i]!=a->input[j],"distinct input maps");
  }
  need(p.outputPitch%256==0&&p.outputPitch>=p.width*8&&uint64_t(p.outputPitch)*p.height<=a->outputBytes,"output row bounds");auto output=alias(a->output,a->outputBytes,true);
  auto cmd=[queue commandBuffer];need(cmd!=nil,"full command");
  auto temporal=[&](id<MTLComputePipelineState> pipeline){auto enc=[cmd computeCommandEncoder];need(enc!=nil,"temporal encoder");[enc setComputePipelineState:pipeline];[enc setBytes:&p length:sizeof(p) atIndex:0];
   for(unsigned i=0;i<4;++i)[enc setBuffer:input[i] offset:0 atIndex:i+1];
   id<MTLBuffer> buffers[]={history[parity],recurrent[parity],feature,prepared,neural,history[parity^1],recurrent[parity^1],output};
   for(unsigned i=0;i<8;++i)[enc setBuffer:buffers[i] offset:0 atIndex:5+i];
   [enc dispatchThreads:MTLSizeMake(size_t(p.width)*p.height,1,1) threadsPerThreadgroup:MTLSizeMake(32,1,1)];[enc endEncoding];
  };
  temporal(prepare);network->encode(cmd,feature,neural,p.width,p.height);temporal(reconstruct);if(p.reserved)temporal(sharpen);
  auto done=dispatch_semaphore_create(0);[cmd addCompletedHandler:^(id<MTLCommandBuffer>){dispatch_semaphore_signal(done);}];a->pending=1;[cmd commit];
  if(dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,10*NSEC_PER_SEC))!=0)_Exit(51);
  a->pending=0;need(cmd.status==MTLCommandBufferStatusCompleted,cmd.error?cmd.error.localizedDescription.UTF8String:"full GPU completion");
  a->gpuMs=(cmd.GPUEndTime-cmd.GPUStartTime)*1000;a->liveBytes=own;a->validated=1;parity^=1;++frames;
  lastW=p.width;lastH=p.height;lastRW=p.renderWidth;lastRH=p.renderHeight;lastExposure=p.exposure;
 }
};
static std::unique_ptr<FullMetalContext> context;
static int32_t invoke(void* ptr){@autoreleasepool{auto a=(FullMetalArgs*)ptr;try{
 need(a&&a->version==1&&a->size==sizeof(*a),"full ABI");a->completed=0;a->pending=0;a->validated=0;a->error[0]=0;
 if(a->op==0){need(!context,"one lab context");context=std::make_unique<FullMetalContext>(a);}
 else if(a->op==1){need(bool(context),"full native context");context->execute(a);}
 else if(a->op==2){context.reset();device=nil;}
 else throw std::runtime_error("full op");a->completed=1;fflush(stdout);return 0;
 }catch(const std::exception& e){if(a)snprintf(a->error,sizeof(a->error),"%s",e.what());fprintf(stderr,"FAIL native full frame %s\n",e.what());return 1;}}}
extern "C" {__attribute__((visibility("default"))) int32_t (*__wine_unix_call_funcs[])(void*)={invoke};}

#pragma once
#include "../../src/runtime/fsr_frame_generation_bridge.hpp"
#include "../rife/fg_d3d12.hpp"
#include "../rife/gpu_blend_probe.hpp"
#include "../metalfx-fg/fg_d3d12.hpp"
// WineForge-Internal: fsr4/cube-input-adapter-v1.
// Only scene data lives here. SDK calls, callbacks and synchronization are shared.
struct SceneFrameGeneration:FrameGenerationBridge {
 std::unique_ptr<rife_d3d12::FrameGeneration> rife;
 std::unique_ptr<gpu_blend_probe::Generator> blendProbe;
 std::unique_ptr<metalfx_d3d12::FrameGeneration> metalFx;
 std::atomic<ID3D12Resource*> capturedScene[2]{};
 std::atomic<uint64_t> capturedId[2]{};
 bool hudlessTest=false;
 bool nativeProxyTest=false;
 static ffxReturnCode_t generateWithHudless(ffxDispatchDescFrameGeneration* params,void* user){
  auto* self=static_cast<SceneFrameGeneration*>(user);
  FfxApiResource clean{};
  if(params){const unsigned slot=unsigned(params->frameID%2);
   if(self->capturedId[slot].load(std::memory_order_acquire)==params->frameID){
    auto* resource=self->capturedScene[slot].load(std::memory_order_acquire);
    if(resource)clean=ffxApiGetResourceDX12(resource,FFX_API_RESOURCE_STATE_COMPUTE_READ);
   }}
  if(self->blendProbe)
   return gpu_blend_probe::Generator::callbackHudless(params,self->blendProbe.get(),clean.resource?&clean:nullptr);
  if(self->metalFx)
   return metalfx_d3d12::FrameGeneration::callbackWithHudless(params,self->metalFx.get(),clean.resource?&clean:nullptr);
  return rife_d3d12::FrameGeneration::callbackWithHudless(params,self->rife.get(),clean.resource?&clean:nullptr);
 }
 void recordHudless(ID3D12Resource* resource){
  const uint64_t id=frames-1;const unsigned slot=unsigned(id%2);
  capturedScene[slot].store(resource,std::memory_order_release);
  capturedId[slot].store(id,std::memory_order_release);
 }
 void init(ID3D12Device* device){
  capturedId[0].store(UINT64_MAX);capturedId[1].store(UINT64_MAX);
  hudlessTest=GetEnvironmentVariableA("WF_CUBE_HUDLESS_TEST",nullptr,0)>0;
  nativeProxyTest=GetEnvironmentVariableA("WF_CUBE_NATIVE_FG_PROXY",nullptr,0)>0;
  need(!nativeProxyTest||hudlessTest,"native FG proxy test requires HUD-less capture");
  char option[2]{};DWORD n=GetEnvironmentVariableA("WF_CUBE_FG_ASYNC",option,2);
  need(!n||(n==1&&(option[0]=='0'||option[0]=='1')),"FG async option 0 or 1");
  ffxCreateContextDescFrameGeneration desc{};
  desc.flags=FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;desc.displaySize={::W,::H};desc.maxRenderSize={RW,RH};desc.backBufferFormat=FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
  FrameGenerationBridge::init(device,desc,L"amd_fidelityfx_framegeneration_dx12.dll",n==1&&option[0]=='1');
  char backend[16]{};DWORD backendLength=GetEnvironmentVariableA("WF_CUBE_FG_BACKEND",backend,sizeof(backend));
  if(backendLength){
   need(backendLength<sizeof(backend)&&
        (strcmp(backend,"rife")==0||strcmp(backend,"blend_probe")==0||strcmp(backend,"metalfx")==0),
        "FG backend must be rife, blend_probe or metalfx");
   if(strcmp(backend,"blend_probe")==0){
    blendProbe=std::make_unique<gpu_blend_probe::Generator>(device);
    if(hudlessTest)setGenerationOverride(&SceneFrameGeneration::generateWithHudless,this);
    else setGenerationOverride(&gpu_blend_probe::Generator::callback,blendProbe.get());
   }else if(strcmp(backend,"metalfx")==0){
    metalFx=std::make_unique<metalfx_d3d12::FrameGeneration>(device);
    if(hudlessTest)setGenerationOverride(&SceneFrameGeneration::generateWithHudless,this);
    else setGenerationOverride(&metalfx_d3d12::FrameGeneration::callback,metalFx.get());
   }else{
    rife=std::make_unique<rife_d3d12::FrameGeneration>(device);
    if(hudlessTest)setGenerationOverride(&SceneFrameGeneration::generateWithHudless,this);
    else setGenerationOverride(&rife_d3d12::FrameGeneration::callback,rife.get());
   }
   printf("FG_GENERATION_BACKEND %s provider_pacing=amd_3.1.6 hudless_test=%u\n",backend,unsigned(hudlessTest));
  }
 }
 void run(ID3D12GraphicsCommandList* list,ID3D12Resource* depth,ID3D12Resource* motion,float jx,float jy,float dt,bool reset,ID3D12Resource* nativeHudless=nullptr){
  if(metalFx)metalFx->setGeometry(depth,motion,dt);
  ffxDispatchDescFrameGenerationPrepareV2 pre{};
  pre.header.type=FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
  pre.commandList=list;pre.renderSize={RW,RH};pre.jitterOffset={jx,jy};pre.motionVectorScale={float(RW),float(RH)};pre.frameTimeDelta=dt;pre.reset=reset;pre.cameraNear=.1f;pre.cameraFar=100;pre.cameraFovAngleVertical=2*atanf(1/1.8f);pre.viewSpaceToMetersFactor=1;pre.depth=ffxApiGetResourceDX12(depth);pre.motionVectors=ffxApiGetResourceDX12(motion);pre.cameraUp[1]=1;pre.cameraRight[0]=1;pre.cameraForward[2]=1;
  const bool toggles=GetEnvironmentVariableA("WF_CUBE_FG_TOGGLE_TEST",nullptr,0)>0;
  const bool enabled=!toggles||(frames%8!=2&&frames%8!=3&&frames%8!=6);
  if(GetEnvironmentVariableA("WF_CUBE_FG_MISSING_CAMERA_TEST",nullptr,0)>0&&frames%8==4){
   for(unsigned i=0;i<3;++i)pre.cameraUp[i]=pre.cameraRight[i]=pre.cameraForward[i]=0;
  }
  if(GetEnvironmentVariableA("WF_CUBE_FG_LEGACY_TEST",nullptr,0)>0)
   prepareLegacy(pre,enabled,nativeHudless?ffxApiGetResourceDX12(nativeHudless,FFX_API_RESOURCE_STATE_COMPUTE_READ):FfxApiResource{});
  else prepareStandard(&pre.header,enabled);
 }
};

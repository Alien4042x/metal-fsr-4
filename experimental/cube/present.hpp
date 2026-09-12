#pragma once
#include "../../src/runtime/fsr_frame_generation_bridge.hpp"
// WineForge-Internal: fsr4/cube-input-adapter-v1.
// Only scene data lives here. SDK calls, callbacks and synchronization are shared.
struct SceneFrameGeneration:FrameGenerationBridge {
 void init(ID3D12Device* device){
  char option[2]{};DWORD n=GetEnvironmentVariableA("WF_CUBE_FG_ASYNC",option,2);
  need(!n||(n==1&&(option[0]=='0'||option[0]=='1')),"FG async option 0 or 1");
  ffxCreateContextDescFrameGeneration desc{};
  desc.flags=FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;desc.displaySize={::W,::H};desc.maxRenderSize={RW,RH};desc.backBufferFormat=FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
  FrameGenerationBridge::init(device,desc,L"amd_fidelityfx_framegeneration_dx12.dll",n==1&&option[0]=='1');
 }
 void run(ID3D12GraphicsCommandList* list,ID3D12Resource* depth,ID3D12Resource* motion,float jx,float jy,float dt,bool reset){
  ffxDispatchDescFrameGenerationPrepareV2 pre{};
  pre.commandList=list;pre.renderSize={RW,RH};pre.jitterOffset={jx,jy};pre.motionVectorScale={float(RW),float(RH)};pre.frameTimeDelta=dt;pre.reset=reset;pre.cameraNear=.1f;pre.cameraFar=100;pre.cameraFovAngleVertical=2*atanf(1/1.8f);pre.viewSpaceToMetersFactor=1;pre.depth=ffxApiGetResourceDX12(depth);pre.motionVectors=ffxApiGetResourceDX12(motion);pre.cameraUp[1]=1;pre.cameraRight[0]=1;pre.cameraForward[2]=1;
  const bool toggles=GetEnvironmentVariableA("WF_CUBE_FG_TOGGLE_TEST",nullptr,0)>0;
  const bool enabled=!toggles||(frames%8!=2&&frames%8!=3&&frames%8!=6);
  if(GetEnvironmentVariableA("WF_CUBE_FG_MISSING_CAMERA_TEST",nullptr,0)>0&&frames%8==4){
   for(unsigned i=0;i<3;++i)pre.cameraUp[i]=pre.cameraRight[i]=pre.cameraForward[i]=0;
  }
  if(GetEnvironmentVariableA("WF_CUBE_FG_LEGACY_TEST",nullptr,0)>0)prepareLegacy(pre,enabled);else prepare(pre,enabled);
 }
};

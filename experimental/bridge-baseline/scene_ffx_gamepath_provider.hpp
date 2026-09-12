#pragma once
#include "../../src/vendor/fsr-sdk-2.3.0/upscalers/include/ffx_upscale.h"
#include "../../src/vendor/fsr-sdk-2.3.0/api/include/dx12/ffx_api_dx12.h"
#include "fsr4_ffx_status.hpp"
// WineForge-Internal: fsr-lab/direct-ffx-scene-provider-v1.
struct SceneFFX {
 HMODULE module=nullptr;ffxContext context=nullptr;ffxContext dormant=nullptr;
 PfnFfxQuery query=nullptr;PfnFfxDispatch dispatch=nullptr;PfnFfxDestroyContext destroy=nullptr;PfnWfFsr4Status status=nullptr;
 uint64_t selected=0;int32_t phaseCount=0;unsigned frames=0,resets=0,creates=0,destroys=0;float jx=0,jy=0;
 bool ours=false;std::string label;WfFsr4Status last{};
 void init(ID3D12Device* device){
  char choice[32]{};need(GetEnvironmentVariableA("WF_CUBE_PROVIDER",choice,32)>0,"explicit provider selection");
  ours=strcmp(choice,"fsr4")==0;need(ours||strcmp(choice,"original31")==0,"known provider selection");
  module=LoadLibraryExW(L"amd_fidelityfx_loader_dx12.dll",nullptr,LOAD_LIBRARY_SEARCH_APPLICATION_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);need(module!=nullptr,"scene loader");
  query=reinterpret_cast<PfnFfxQuery>(GetProcAddress(module,"ffxQuery"));dispatch=reinterpret_cast<PfnFfxDispatch>(GetProcAddress(module,"ffxDispatch"));
  destroy=reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(module,"ffxDestroyContext"));auto create=reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(module,"ffxCreateContext"));
  status=reinterpret_cast<PfnWfFsr4Status>(GetProcAddress(module,"wfFsr4GetContextStatus"));need(query&&dispatch&&destroy&&create,"FFX exports");
  uint64_t count=0,ids[32]{};const char* names[32]{};ffxQueryDescGetVersions versions{};versions.header.type=FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  versions.createDescType=FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;versions.device=device;versions.outputCount=&count;
  need(query(nullptr,&versions.header)==0&&count>0&&count<=32,"provider count");const auto capacity=count;
  versions.versionIds=ids;versions.versionNames=names;need(query(nullptr,&versions.header)==0&&count<=capacity,"provider list");
  unsigned matches=0;for(uint64_t i=0;i<count;++i)if(names[i]&&(ours?ids[i]==wfFsr4VersionId:strcmp(names[i],"3.1.5")==0)){selected=ids[i];label=names[i];++matches;}
  need(matches==1&&(!ours||status),"unique requested provider available");
  ffxOverrideVersion version{};version.header.type=FFX_API_DESC_TYPE_OVERRIDE_VERSION;version.versionId=selected;
  ffxCreateBackendDX12Desc backend{};backend.header={FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12,&version.header};backend.device=device;
  ffxCreateContextDescUpscale desc{};desc.header={FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,&backend.header};desc.flags=FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE|FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
  desc.maxRenderSize={RW,RH};desc.maxUpscaleSize={W,H};need(create(&dormant,&desc.header,nullptr)==0&&dormant,"create dormant context");puts("DORMANT_CONTEXT_CREATED");need(create(&context,&desc.header,nullptr)==0&&context,"create selected FFX context");++creates;verify();
  ffxQueryDescUpscaleGetJitterPhaseCount phase{};phase.header.type=FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
  phase.renderWidth=RW;phase.displayWidth=W;phase.pOutPhaseCount=&phaseCount;need(query(&context,&phase.header)==0&&phaseCount>0&&phaseCount<=64,"jitter phases");
  printf("CONTEXT_CREATE count=%u selected=%s phases=%d\n",creates,label.c_str(),phaseCount);
 }
 void verify(){ffxQueryGetProviderVersion active{};active.header.type=FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
  need(query(&context,&active.header)==0&&active.versionId==selected,"actual provider identity");}
 void jitter(){ffxQueryDescUpscaleGetJitterOffset j{};j.header.type=FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;j.index=int(frames%unsigned(phaseCount));j.phaseCount=phaseCount;j.pOutX=&jx;j.pOutY=&jy;
  need(query(&context,&j.header)==0&&std::isfinite(jx)&&std::isfinite(jy),"finite jitter");}
 void run(ID3D12GraphicsCommandList* list,ID3D12Resource* color,ID3D12Resource* depth,ID3D12Resource* motion,ID3D12Resource* exposure,ID3D12Resource* output,float ms,bool reset){
  ffxDispatchDescUpscale x{};x.header.type=FFX_API_DISPATCH_DESC_TYPE_UPSCALE;x.commandList=list;
  x.color=ffxApiGetResourceDX12(color);x.depth=ffxApiGetResourceDX12(depth);x.motionVectors=ffxApiGetResourceDX12(motion);x.exposure=ffxApiGetResourceDX12(exposure);
  x.output=ffxApiGetResourceDX12(output,FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);x.renderSize={RW,RH};x.upscaleSize={W,H};x.jitterOffset={jx,jy};x.motionVectorScale={float(RW),float(RH)};
  x.frameTimeDelta=ms;x.preExposure=1;x.reset=reset;x.cameraNear=.1f;x.cameraFar=100;x.cameraFovAngleVertical=2*atanf(1/1.8f);x.viewSpaceToMetersFactor=1;x.enableSharpening=true;x.sharpness=.7f;
  need(dispatch(&context,&x.header)==0,"FFX scene dispatch");++frames;if(reset)++resets;if(frames==2){need(destroy(&dormant,nullptr)==0,"destroy dormant context");dormant=nullptr;puts("DORMANT_CONTEXT_DESTROYED_ACTIVE_SURVIVES");}
  if(ours){need(status(&context,&last)==0,"context telemetry");if(last.backend!=4)printf("UNEXPECTED_FALLBACK reason=%s\n",last.reason);
   need(last.backend==4&&last.fsr4Dispatches==frames&&last.originalDispatches==0,"actual FSR4 every frame");}
  else{last.backend=3;last.originalDispatches=frames;}
  if(frames==1)printf("FIRST_DISPATCH backend=%u fsr4_count=%llu original_count=%llu\n",last.backend,static_cast<unsigned long long>(last.fsr4Dispatches),static_cast<unsigned long long>(last.originalDispatches));
 }
 void close(){if(context){need(destroy(&context,nullptr)==0,"context destroy");context=nullptr;++destroys;printf("CONTEXT_DESTROY count=%u frames=%u resets=%u\n",destroys,frames,resets);}if(module){FreeLibrary(module);module=nullptr;}}
 ~SceneFFX(){if(dormant&&destroy)destroy(&dormant,nullptr);if(context&&destroy)destroy(&context,nullptr);if(module)FreeLibrary(module);}
};

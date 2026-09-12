#pragma once
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-pragmas"
#include "../../src/vendor/fsr-sdk-2.3.0/framegeneration/include/ffx_framegeneration.h"
#pragma clang diagnostic pop
// WineForge-Internal: fsr4/analytical-framegeneration-admission-v1.
struct SceneFrameGeneration {
 HMODULE module=nullptr;ffxContext context=nullptr;PfnFfxDestroyContext destroy=nullptr;PfnFfxDispatch dispatch=nullptr;PfnFfxConfigure configure=nullptr;
 unsigned frames=0;
 void init(ID3D12Device* device) {
 module=LoadLibraryExW(L"amd_fidelityfx_framegeneration_dx12.dll",nullptr,LOAD_LIBRARY_SEARCH_APPLICATION_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
 if(!module)printf("FG_LOAD_ERROR win32=%lu\n",GetLastError());
 need(module!=nullptr,"frame generation DLL load");
 auto query=reinterpret_cast<PfnFfxQuery>(GetProcAddress(module,"ffxQuery"));
 auto create=reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(module,"ffxCreateContext"));
 destroy=reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(module,"ffxDestroyContext"));
 dispatch=reinterpret_cast<PfnFfxDispatch>(GetProcAddress(module,"ffxDispatch"));configure=reinterpret_cast<PfnFfxConfigure>(GetProcAddress(module,"ffxConfigure"));
 need(query&&create&&destroy&&dispatch&&configure,"frame generation exports");
 uint64_t count=0,ids[32]{};const char* names[32]{};
 ffxQueryDescGetVersions versions{};versions.header.type=FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
 versions.createDescType=FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;versions.device=device;versions.outputCount=&count;
 auto rc=query(nullptr,&versions.header);printf("FG_QUERY_COUNT rc=%u count=%llu\n",unsigned(rc),(unsigned long long)count);
 need(rc==0&&count>0&&count<=32,"frame generation provider count");
 versions.versionIds=ids;versions.versionNames=names;need(query(nullptr,&versions.header)==0&&count<=32,"frame generation provider list");
 uint64_t selected=0;unsigned matches=0;
 for(uint64_t i=0;i<count;++i){printf("FG_PROVIDER id=%llu name=%s\n",(unsigned long long)ids[i],names[i]?names[i]:"null");if(names[i]&&strcmp(names[i],"3.1.6")==0){selected=ids[i];++matches;}}
 need(matches==1,"unique analytical3.1.6 provider");
 ffxOverrideVersion version{};version.header.type=FFX_API_DESC_TYPE_OVERRIDE_VERSION;version.versionId=selected;
 ffxCreateContextDescFrameGenerationVersion abi{};abi.header={FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION,&version.header};abi.version=FFX_FRAMEGENERATION_VERSION;
 ffxCreateBackendDX12Desc backend{};backend.header={FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12,&abi.header};backend.device=device;
 ffxCreateContextDescFrameGeneration desc{};desc.header={FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION,&backend.header};
 desc.flags=FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED|FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;desc.displaySize={W,H};desc.maxRenderSize={RW,RH};desc.backBufferFormat=FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
 rc=create(&context,&desc.header,nullptr);printf("FG_CREATE rc=%u context=%p\n",unsigned(rc),context);fflush(stdout);
 need(rc==0&&context,"frame generation context");
 ffxQueryGetProviderVersion actual{};actual.header.type=FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
 need(query(&context,&actual.header)==0&&actual.versionId==selected,"actual analytical provider");

 }
 void run(ID3D12GraphicsCommandList* list,ID3D12Resource* color,ID3D12Resource* depth,ID3D12Resource* motion,ID3D12Resource* output,float jx,float jy,float dt,bool reset){
  ffxConfigureDescFrameGeneration cfg{};cfg.header.type=FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;cfg.frameGenerationEnabled=true;cfg.flags=FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;cfg.generationRect={0,0,int(W),int(H)};cfg.frameID=frames;
  auto rc=configure(&context,&cfg.header);printf("FG_CONFIG frame=%u rc=%u\n",frames,unsigned(rc));need(rc==0,"FG configure");
  ffxDispatchDescFrameGenerationPrepareV2 pre{};pre.header.type=FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;pre.frameID=frames;pre.commandList=list;pre.renderSize={RW,RH};pre.jitterOffset={jx,jy};pre.motionVectorScale={float(RW),float(RH)};pre.frameTimeDelta=dt;pre.reset=reset;pre.cameraNear=.1f;pre.cameraFar=100;pre.cameraFovAngleVertical=2*atanf(1/1.8f);pre.viewSpaceToMetersFactor=1;pre.depth=ffxApiGetResourceDX12(depth);pre.motionVectors=ffxApiGetResourceDX12(motion);pre.cameraUp[1]=1;pre.cameraRight[0]=1;pre.cameraForward[2]=1;
  rc=dispatch(&context,&pre.header);printf("FG_PREPARE frame=%u rc=%u\n",frames,unsigned(rc));need(rc==0,"FG prepare");
  ffxDispatchDescFrameGeneration fg{};fg.header.type=FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;fg.commandList=list;fg.presentColor=ffxApiGetResourceDX12(color);fg.outputs[0]=ffxApiGetResourceDX12(output,FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);fg.numGeneratedFrames=1;fg.reset=reset;fg.backbufferTransferFunction=FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB;fg.minMaxLuminance[0]=0;fg.minMaxLuminance[1]=1000;fg.generationRect={0,0,int(W),int(H)};fg.frameID=frames;
  rc=dispatch(&context,&fg.header);printf("FG_DISPATCH frame=%u rc=%u requested=1 reported=%u reset=%u\n",frames,unsigned(rc),fg.numGeneratedFrames,unsigned(reset));fflush(stdout);need(rc==0,"FG dispatch");++frames;
 }
 void close(){if(context){need(destroy(&context,nullptr)==0,"FG destroy");context=nullptr;}if(module){FreeLibrary(module);module=nullptr;}}
 ~SceneFrameGeneration(){if(context&&destroy)destroy(&context,nullptr);if(module)FreeLibrary(module);}
};

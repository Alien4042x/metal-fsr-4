#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <stdexcept>
#include <algorithm>
#include <mutex>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-pragmas"
#include "../vendor/fsr-sdk-2.3.0/framegeneration/include/ffx_framegeneration.h"
#include "../vendor/fsr-sdk-2.3.0/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"
#pragma clang diagnostic pop
// WineForge-Internal: fsr4/shared-framegeneration-bridge-v1.
struct FrameGenerationBridge {
 // All host operations are serialized; callbacks must not acquire this mutex.
 std::recursive_mutex hostMutex;
 UINT W=0,H=0,maxRenderW=0,maxRenderH=0;
 const char* fallbackReason=nullptr;
 unsigned fallbackMessages=0;
 bool enabledLast=false;
 FrameGenerationBridge()=default;
 FrameGenerationBridge(const FrameGenerationBridge&)=delete;
 FrameGenerationBridge& operator=(const FrameGenerationBridge&)=delete;
 static void need(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
 static void check(HRESULT hr,const char* reason){need(SUCCEEDED(hr),reason);}

 HMODULE module=nullptr;ffxContext context=nullptr;PfnFfxDestroyContext destroy=nullptr;PfnFfxDispatch dispatch=nullptr;PfnFfxConfigure configure=nullptr;PfnFfxQuery query=nullptr;
 unsigned frames=0;ffxContext swapContext=nullptr;IDXGISwapChain4* swap=nullptr;PfnFfxCreateContext create=nullptr;
 bool asyncWorkloads=false;
 std::atomic<unsigned> realCallbacks{0},generatedCallbacks{0},callbackFailures{0};
 void openModule(const wchar_t* dllPath){
 std::lock_guard<std::recursive_mutex> lock(hostMutex);
 if(module)return;
 module=LoadLibraryExW(dllPath,nullptr,LOAD_LIBRARY_SEARCH_APPLICATION_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
 if(!module)printf("FG_LOAD_ERROR win32=%lu\n",GetLastError());
 need(module!=nullptr,"frame generation DLL load");
 query=reinterpret_cast<PfnFfxQuery>(GetProcAddress(module,"ffxQuery"));
 create=reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(module,"ffxCreateContext"));
 destroy=reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(module,"ffxDestroyContext"));
 dispatch=reinterpret_cast<PfnFfxDispatch>(GetProcAddress(module,"ffxDispatch"));configure=reinterpret_cast<PfnFfxConfigure>(GetProcAddress(module,"ffxConfigure"));
 need(query&&create&&destroy&&dispatch&&configure,"frame generation exports");
 }
 void init(ID3D12Device* device,const ffxCreateContextDescFrameGeneration& options,
           const wchar_t* dllPath,bool async) {
 std::lock_guard<std::recursive_mutex> lock(hostMutex);
 need(!context,"FG context initialized once");
 need(device&&dllPath&&options.displaySize.width&&options.displaySize.height&&options.maxRenderSize.width&&options.maxRenderSize.height,"FG init parameters");
 need(!options.header.pNext,"FG unsupported initialization chain");
 W=options.displaySize.width;H=options.displaySize.height;maxRenderW=options.maxRenderSize.width;maxRenderH=options.maxRenderSize.height;asyncWorkloads=async;
 printf("FG_SCHEDULING async=%u\n",unsigned(asyncWorkloads));
 openModule(dllPath);
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
 ffxCreateContextDescFrameGeneration desc=options;desc.header={FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION,&backend.header};

 if(asyncWorkloads)desc.flags|=FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;
 rc=create(&context,&desc.header,nullptr);printf("FG_CREATE rc=%u context=%p\n",unsigned(rc),context);fflush(stdout);
 need(rc==0&&context,"frame generation context");
 ffxQueryGetProviderVersion actual{};actual.header.type=FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
 need(query(&context,&actual.header)==0&&actual.versionId==selected,"actual analytical provider");

 }
 IDXGISwapChain4* makeSwap(HWND window,IDXGIFactory* factory,ID3D12CommandQueue* queue,DXGI_SWAP_CHAIN_DESC1* desc){
  std::lock_guard<std::recursive_mutex> lock(hostMutex);
  need(create&&!swapContext&&desc&&desc->Width==W&&desc->Height==H,"FG swap dimensions and lifetime");
  ffxCreateContextDescFrameGenerationSwapChainVersionDX12 abi{};abi.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;abi.version=FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
  ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 cfg{};cfg.header={FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12,&abi.header};cfg.swapchain=&swap;cfg.hwnd=window;cfg.desc=desc;cfg.dxgiFactory=factory;cfg.gameQueue=queue;
  auto rc=create(&swapContext,&cfg.header,nullptr);printf("FG_SWAPCHAIN_CREATE rc=%u swap=%p\n",unsigned(rc),swap);fflush(stdout);need(rc==0&&swapContext&&swap,"FG swapchain create");return swap;
 }
 // Call at swapchain creation, before the application acquires back buffers.
 // The SDK Wrap API replaces the caller-owned interface in place.
 IDXGISwapChain4* wrapSwap(IDXGISwapChain4*& applicationSwap,ID3D12CommandQueue* queue){
  std::lock_guard<std::recursive_mutex> lock(hostMutex);
  need(context&&!swapContext&&applicationSwap&&queue,"FG wrap lifetime");
  DXGI_SWAP_CHAIN_DESC1 desc{};check(applicationSwap->GetDesc1(&desc),"FG existing swap description");
  need(desc.Width==W&&desc.Height==H,"FG existing swap dimensions");
  ffxCreateContextDescFrameGenerationSwapChainVersionDX12 abi{};abi.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;abi.version=FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
  ffxCreateContextDescFrameGenerationSwapChainWrapDX12 cfg{};cfg.header={FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12,&abi.header};cfg.swapchain=&applicationSwap;cfg.gameQueue=queue;
  auto rc=create(&swapContext,&cfg.header,nullptr);
  printf("FG_SWAPCHAIN_WRAP rc=%u\n",unsigned(rc));fflush(stdout);
  need(rc==0&&swapContext&&applicationSwap,"FG existing swap wrap");
  swap=applicationSwap;return swap;
 }
 static D3D12_RESOURCE_STATES state(uint32_t f){unsigned s=0;
  if(f&FFX_API_RESOURCE_STATE_UNORDERED_ACCESS)s|=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  if(f&FFX_API_RESOURCE_STATE_COMPUTE_READ)s|=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  if(f&FFX_API_RESOURCE_STATE_PIXEL_READ)s|=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  if(f&FFX_API_RESOURCE_STATE_COPY_SRC)s|=D3D12_RESOURCE_STATE_COPY_SOURCE;
  if(f&FFX_API_RESOURCE_STATE_COPY_DEST)s|=D3D12_RESOURCE_STATE_COPY_DEST;
  if(f&FFX_API_RESOURCE_STATE_RENDER_TARGET)s|=D3D12_RESOURCE_STATE_RENDER_TARGET;
  return D3D12_RESOURCE_STATES(s);
 }
 static ffxReturnCode_t present(ffxCallbackDescFrameGenerationPresent* p,void* user){
  auto& self=*static_cast<FrameGenerationBridge*>(user);auto list=static_cast<ID3D12GraphicsCommandList*>(p->commandList);auto src=static_cast<ID3D12Resource*>(p->currentBackBuffer.resource);auto dst=static_cast<ID3D12Resource*>(p->outputSwapChainBuffer.resource);
  if(!list||!src||!dst){++self.callbackFailures;return FFX_API_RETURN_ERROR_PARAMETER;}
  D3D12_RESOURCE_BARRIER bars[2]{};for(auto& b:bars){b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;}
  bars[0].Transition={src,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,state(p->currentBackBuffer.state),D3D12_RESOURCE_STATE_COPY_SOURCE};bars[1].Transition={dst,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,state(p->outputSwapChainBuffer.state),D3D12_RESOURCE_STATE_COPY_DEST};
  for(auto& b:bars)if(b.Transition.StateBefore!=b.Transition.StateAfter)list->ResourceBarrier(1,&b);
  list->CopyResource(dst,src);
  for(auto& b:bars){std::swap(b.Transition.StateBefore,b.Transition.StateAfter);if(b.Transition.StateBefore!=b.Transition.StateAfter)list->ResourceBarrier(1,&b);}
  if(p->isGeneratedFrame)++self.generatedCallbacks;else ++self.realCallbacks;
  return FFX_API_RETURN_OK;
 }
 static ffxReturnCode_t generate(ffxDispatchDescFrameGeneration* p,void* user){auto& self=*static_cast<FrameGenerationBridge*>(user);auto rc=self.dispatch(&self.context,&p->header);if(rc!=0)++self.callbackFailures;printf("FG_CALLBACK_DISPATCH frame=%llu rc=%u count=%u\n",(unsigned long long)p->frameID,unsigned(rc),p->numGeneratedFrames);return rc;}
 void prepare(ffxDispatchDescFrameGenerationPrepareV2 pre,bool enabled){
  std::lock_guard<std::recursive_mutex> lock(hostMutex);
  need(context&&swapContext,"FG configured before prepare");
  fallbackReason=nullptr;
  if(enabled){
   auto dot=[](const float* a,const float* b){float n=0;for(unsigned i=0;i<3;++i)n+=a[i]*b[i];return n;};
   bool finiteCamera=true;
   for(unsigned i=0;i<3;++i)finiteCamera=finiteCamera&&std::isfinite(pre.cameraPosition[i])&&std::isfinite(pre.cameraUp[i])&&std::isfinite(pre.cameraRight[i])&&std::isfinite(pre.cameraForward[i]);
   const bool cameraBasis=finiteCamera&&std::abs(dot(pre.cameraUp,pre.cameraUp)-1.f)<.01f&&std::abs(dot(pre.cameraRight,pre.cameraRight)-1.f)<.01f&&std::abs(dot(pre.cameraForward,pre.cameraForward)-1.f)<.01f&&std::abs(dot(pre.cameraUp,pre.cameraRight))<.01f&&std::abs(dot(pre.cameraUp,pre.cameraForward))<.01f&&std::abs(dot(pre.cameraRight,pre.cameraForward))<.01f;
   if(pre.header.pNext)fallbackReason="unsupported_prepare_chain";
   else if(!pre.commandList||!pre.depth.resource||!pre.motionVectors.resource)fallbackReason="missing_frame_resources";
   else if(!pre.renderSize.width||!pre.renderSize.height||pre.renderSize.width>maxRenderW||pre.renderSize.height>maxRenderH)fallbackReason="unsupported_render_extent";
   else if(!cameraBasis)fallbackReason="missing_or_invalid_camera";
   else if(!std::isfinite(pre.cameraFovAngleVertical)||pre.cameraFovAngleVertical<=0||pre.cameraFovAngleVertical>=3.141593f||!std::isfinite(pre.cameraNear)||!std::isfinite(pre.cameraFar)||!std::isfinite(pre.viewSpaceToMetersFactor)||pre.viewSpaceToMetersFactor<=0)fallbackReason="invalid_camera_projection";
   if(fallbackReason){enabled=false;if(fallbackMessages++<16)printf("FG_FALLBACK frame=%u reason=%s action=disable_fg_keep_upscaling\n",frames,fallbackReason);}
  }
  if(enabled!=enabledLast)drain();
  ffxConfigureDescFrameGeneration cfg{};cfg.header.type=FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;cfg.frameGenerationEnabled=enabled;cfg.swapChain=swap;cfg.frameGenerationCallback=generate;cfg.frameGenerationCallbackUserContext=this;cfg.presentCallback=present;cfg.presentCallbackUserContext=this;cfg.generationRect={0,0,int(W),int(H)};cfg.frameID=frames;
  cfg.allowAsyncWorkloads=asyncWorkloads;
  auto rc=configure(&context,&cfg.header);printf("FG_CONFIG frame=%u rc=%u enabled=%u\n",frames,unsigned(rc),unsigned(enabled));need(rc==0,"FG configure");
  if(enabled){
   pre.header.type=FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;pre.frameID=frames;pre.reset=pre.reset||!enabledLast;
   rc=dispatch(&context,&pre.header);printf("FG_PREPARE frame=%u rc=%u\n",frames,unsigned(rc));need(rc==0,"FG prepare");
  }
  enabledLast=enabled;++frames;
 }
 // Explicit compatibility path: no fabricated world-camera data. The provider
 // is still AMD 3.1.6; only its documented legacy input descriptor is used.
 unsigned long long legacySerial=0;
 ffxConfigureDescFrameGeneration legacyConfig{};
 void setLegacyHudless(FfxApiResource color){
  std::lock_guard<std::recursive_mutex> lock(hostMutex);
  need(context&&enabledLast,"HUD-less configuration requires prepared FG");
  legacyConfig.HUDLessColor=color;
  need(configure(&context,&legacyConfig.header)==0,"HUD-less configuration");
 }

 void prepareLegacy(const ffxDispatchDescFrameGenerationPrepareV2& input,bool enabled){
  std::lock_guard<std::recursive_mutex> lock(hostMutex);
  need(context&&swapContext,"FG legacy initialized");
  need(!input.header.pNext,"FG legacy unsupported chain");
  need(!enabled||(input.commandList&&input.depth.resource&&input.motionVectors.resource&&input.renderSize.width&&input.renderSize.height),"FG legacy resources");
  if(enabled!=enabledLast)drain();
  legacySerial+=(input.reset||!enabledLast)?2:1;
  ffxConfigureDescFrameGeneration cfg{};cfg.header.type=FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;cfg.frameGenerationEnabled=enabled;cfg.swapChain=swap;cfg.frameGenerationCallback=generate;cfg.frameGenerationCallbackUserContext=this;cfg.presentCallback=present;cfg.presentCallbackUserContext=this;cfg.generationRect={0,0,int(W),int(H)};cfg.frameID=legacySerial;cfg.allowAsyncWorkloads=asyncWorkloads;
  legacyConfig=cfg;auto rc=configure(&context,&cfg.header);need(rc==0,"FG legacy configure");
  if(enabled){
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
   ffxDispatchDescFrameGenerationPrepare pre{};pre.header.type=FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
#pragma clang diagnostic pop
   pre.frameID=legacySerial;pre.commandList=input.commandList;pre.renderSize=input.renderSize;pre.jitterOffset=input.jitterOffset;pre.motionVectorScale=input.motionVectorScale;pre.frameTimeDelta=input.frameTimeDelta;pre.cameraNear=input.cameraNear;pre.cameraFar=input.cameraFar;pre.cameraFovAngleVertical=input.cameraFovAngleVertical;pre.viewSpaceToMetersFactor=input.viewSpaceToMetersFactor;pre.depth=input.depth;pre.motionVectors=input.motionVectors;
   rc=dispatch(&context,&pre.header);printf("FG_LEGACY_PREPARE frame=%u id=%llu rc=%u world_camera=unavailable\n",frames,legacySerial,unsigned(rc));need(rc==0,"FG legacy prepare");
  }
  enabledLast=enabled;++frames;
 }
 HRESULT presentFrame(UINT interval,UINT flags){
  std::lock_guard<std::recursive_mutex> lock(hostMutex);
  return swap?swap->Present(interval,flags):DXGI_ERROR_INVALID_CALL;
 }
 void drain(){std::lock_guard<std::recursive_mutex> lock(hostMutex);if(swapContext){ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait{};wait.header.type=FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;need(dispatch(&swapContext,&wait.header)==0,"FG wait presents");printf("FG_PRESENT_TOTAL real=%u generated=%u failures=%u\n",realCallbacks.load(),generatedCallbacks.load(),callbackFailures.load());need(callbackFailures==0,"FG callback errors");}}
 void close(){std::lock_guard<std::recursive_mutex> lock(hostMutex);if(swapContext){drain();if(context){ffxConfigureDescFrameGeneration cfg{};cfg.header.type=FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;cfg.swapChain=swap;need(configure(&context,&cfg.header)==0,"disable FG");}}if(context){need(destroy(&context,nullptr)==0,"FG destroy");context=nullptr;}if(swapContext){need(destroy(&swapContext,nullptr)==0,"FG swapchain destroy");swapContext=nullptr;swap=nullptr;}}
 ~FrameGenerationBridge(){if(context&&destroy)destroy(&context,nullptr);if(swapContext&&destroy)destroy(&swapContext,nullptr);if(module)FreeLibrary(module);}
};

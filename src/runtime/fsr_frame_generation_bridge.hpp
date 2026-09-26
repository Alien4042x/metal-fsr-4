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
#include "fsr_fg_camera_input.hpp"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-pragmas"
#include "../vendor/fsr-sdk-2.3.0/framegeneration/include/ffx_framegeneration.h"
#include "../vendor/fsr-sdk-2.3.0/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"
#pragma clang diagnostic pop
// WineForge-Internal: fsr4/shared-framegeneration-bridge-v1.
struct FrameGenerationBridge {
 // All host operations are serialized; callbacks must not acquire this mutex.
 std::recursive_mutex hostMutex;

 // CPU submission timing only; callback counts do not measure displayed frames.
 struct Timing {
  unsigned count=0,gaps=0,lastReal=0,lastGenerated=0,failed=0;
  long long previous=0;double waitSum=0,waitMax=0,bodySum=0,bodyMax=0,gapSum=0,gapMax=0;
  static bool enabled(){static const bool on=[](){wchar_t v[2]{};return GetEnvironmentVariableW(L"METAL_FSR4_TRACE",v,2)==1&&v[0]==L'1';}();return on;}
  static long long now(){LARGE_INTEGER t{};QueryPerformanceCounter(&t);return t.QuadPart;}
  static double ms(long long ticks){static const double frequency=[](){LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return double(f.QuadPart);}();return ticks*1000.0/frequency;}
  void record(const char* stage,const void* owner,long long start,long long acquired,long long end,unsigned real,unsigned generated,bool error){
   double wait=ms(acquired-start),body=ms(end-acquired);
   if(previous){double gap=ms(start-previous);gapSum+=gap;gapMax=std::max(gapMax,gap);++gaps;}previous=start;
   waitSum+=wait;waitMax=std::max(waitMax,wait);bodySum+=body;bodyMax=std::max(bodyMax,body);failed+=error;++count;
   if(count<120)return;
   fprintf(stderr,"FG_CPU_TIMING stage=%s context=%p calls=%u window_end_qpc_ms=%.6f lock_mean_ms=%.6f lock_max_ms=%.6f body_mean_ms=%.6f body_max_ms=%.6f entry_gap_mean_ms=%.6f entry_gap_max_ms=%.6f real_callback_delta=%u generated_callback_delta=%u errors=%u displayed_frames=unknown\n",stage,owner,count,ms(end),waitSum/count,waitMax,bodySum/count,bodyMax,gaps?gapSum/gaps:0,gapMax,real-lastReal,generated-lastGenerated,failed);
   lastReal=real;lastGenerated=generated;count=gaps=failed=0;waitSum=waitMax=bodySum=bodyMax=gapSum=gapMax=0;
  }
 } presentTiming;
 struct TimingScope {
  Timing& timing;FrameGenerationBridge& bridge;const char* stage;long long start,acquired;bool error=false;
  TimingScope(Timing& t,FrameGenerationBridge& b,const char* name,long long begin):timing(t),bridge(b),stage(name),start(begin),acquired(begin?Timing::now():0){}
  ~TimingScope(){if(start)timing.record(stage,&bridge,start,acquired,Timing::now(),bridge.realCallbacks.load(),bridge.generatedCallbacks.load(),error);}
 };

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
 bool legacyCameraSupported=false;
 int standardCameraMode=-1;
 ID3D12Device* providerDevice=nullptr;
 ffxCreateContextDescFrameGeneration providerOptions{};
 uint64_t legacyProvider=0,mlProvider=0,activeProvider=0,failedProvider=0;
 bool mlCreationFailed=false;
 std::atomic<unsigned> realCallbacks{0},generatedCallbacks{0},callbackFailures{0},dispatchReports{0};
 FfxApiFrameGenerationDispatchFunc generationOverride=nullptr;
 void* generationOverrideUser=nullptr;
 bool allowMlProvider=true;
 void setGenerationOverride(FfxApiFrameGenerationDispatchFunc callback,void* user){generationOverride=callback;generationOverrideUser=user;allowMlProvider=callback==nullptr;}
 void openModule(const wchar_t* dllPath){
 std::lock_guard<std::recursive_mutex> lock(hostMutex);
 if(module)return;
 wchar_t adapter[32768]{};
 const DWORD adapterLength=GetEnvironmentVariableW(L"METAL_FG401_ADAPTER",adapter,32768);
 if(adapterLength){
  need(adapterLength<32768,"FG adapter path");
  auto loaded=LoadLibraryExW(adapter,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if(!loaded)printf("FG_ADAPTER_LOAD_ERROR win32=%lu\n",GetLastError());
  need(loaded!=nullptr,"FG adapter load");
  printf("FG_ADAPTER_LOADED path=%ls\n",adapter);
 }
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
 uint64_t selected=0;unsigned matches=0,mlMatches=0;
 mlProvider=0;
 auto is401=[](const char* name){return name&&!strncmp(name,"4.0.1",5)&&(!name[5]||name[5]==' '||name[5]=='*'||name[5]=='-'||name[5]==')');};
 for(uint64_t i=0;i<count;++i){printf("FG_PROVIDER id=%llu name=%s\n",(unsigned long long)ids[i],names[i]?names[i]:"null");if(names[i]&&strcmp(names[i],"3.1.6")==0){selected=ids[i];++matches;}if(is401(names[i])){mlProvider=ids[i];++mlMatches;}}
 if(mlMatches!=1)mlProvider=0;
 need(matches==1,"unique analytical3.1.6 provider");
 legacyProvider=selected;
 char requestedMode[32]{};
 const DWORD requestedLength=GetEnvironmentVariableA("METAL_FSR4_FG",requestedMode,sizeof(requestedMode));
 const bool initialMetalFx401=requestedLength>0&&requestedLength<sizeof(requestedMode)&&
                              strcmp(requestedMode,"metalfx401")==0;
 if(initialMetalFx401){need(mlProvider!=0,"MetalFX 4.0.1 compatibility provider");selected=mlProvider;}
 ffxOverrideVersion version{};version.header.type=FFX_API_DESC_TYPE_OVERRIDE_VERSION;version.versionId=selected;
 ffxCreateContextDescFrameGenerationVersion abi{};abi.header={FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION,&version.header};abi.version=FFX_FRAMEGENERATION_VERSION;
 ffxCreateBackendDX12Desc backend{};backend.header={FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12,&abi.header};backend.device=device;
 ffxCreateContextDescFrameGeneration desc=options;desc.header={FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION,&backend.header};

 if(asyncWorkloads)desc.flags|=FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;
 rc=create(&context,&desc.header,nullptr);printf("FG_CREATE rc=%u context=%p\n",unsigned(rc),context);fflush(stdout);
 need(rc==0&&context,"frame generation context");
 ffxQueryGetProviderVersion actual{};actual.header.type=FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
 need(query(&context,&actual.header)==0&&actual.versionId==selected,"actual analytical provider");
 legacyCameraSupported=actual.versionName&&strcmp(actual.versionName,"3.1.6")==0;
 activeProvider=selected;providerOptions=options;providerOptions.header.pNext=nullptr;
 providerDevice=device;providerDevice->AddRef();mlCreationFailed=false;failedProvider=0;standardCameraMode=-1;
 printf("FG_INITIAL_PROVIDER active=%llx legacy=%llx metalfx401=%u\n",
        (unsigned long long)activeProvider,(unsigned long long)legacyProvider,unsigned(initialMetalFx401));

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
 static ffxReturnCode_t generate(ffxDispatchDescFrameGeneration* p,void* user){auto& self=*static_cast<FrameGenerationBridge*>(user);auto rc=self.dispatch(&self.context,&p->header);if(rc!=0)++self.callbackFailures;else self.generatedCallbacks.fetch_add(p->numGeneratedFrames);auto report=++self.dispatchReports;if(rc!=0||report<=4||report%120==0)printf("FG_CALLBACK_DISPATCH frame=%llu rc=%u count=%u\n",(unsigned long long)p->frameID,unsigned(rc),p->numGeneratedFrames);return rc;}
 // Switch only at a preparation boundary, after the previous presents drain.
 // Keep the old context until the replacement has been created and verified.
 bool selectCameraProvider(bool cameraAvailable){
  std::lock_guard<std::recursive_mutex> lock(hostMutex);
  const uint64_t desired=cameraAvailable&&allowMlProvider&&mlProvider&&!mlCreationFailed?mlProvider:legacyProvider;
  if(!desired||desired==activeProvider||desired==failedProvider)return false;
  need(providerDevice&&context&&swapContext,"FG provider switch initialized");
  drain();
  ffxConfigureDescFrameGeneration off{};off.header.type=FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
  off.swapChain=swap;off.frameGenerationEnabled=false;
  need(configure(&context,&off.header)==0,"FG disable before provider switch");
  enabledLast=false;drain();
  ffxOverrideVersion version{};version.header.type=FFX_API_DESC_TYPE_OVERRIDE_VERSION;version.versionId=desired;
  ffxCreateContextDescFrameGenerationVersion abi{};abi.header={FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION,&version.header};abi.version=FFX_FRAMEGENERATION_VERSION;
  ffxCreateBackendDX12Desc backend{};backend.header={FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12,&abi.header};backend.device=providerDevice;
  auto options=providerOptions;options.header={FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION,&backend.header};
  if(asyncWorkloads)options.flags|=FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;
  ffxContext replacement=nullptr;auto rc=create(&replacement,&options.header,nullptr);
  ffxQueryGetProviderVersion actual{};actual.header.type=FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
  const bool ready=rc==0&&replacement&&query(&replacement,&actual.header)==0&&actual.versionId==desired;
  if(!ready){
   if(replacement)need(destroy(&replacement,nullptr)==0,"FG rejected provider cleanup");
   failedProvider=desired;if(desired==mlProvider)mlCreationFailed=true;
   fprintf(stderr,"FG_PROVIDER_SWITCH_FAILED target=%llx rc=%u retained=%llx\n",(unsigned long long)desired,unsigned(rc),(unsigned long long)activeProvider);
   return false;
  }
  if(destroy(&context,nullptr)!=0){destroy(&replacement,nullptr);need(false,"FG old provider destroy");}
  context=replacement;activeProvider=desired;failedProvider=0;legacyCameraSupported=desired==legacyProvider;
  standardCameraMode=-1;
  fprintf(stderr,"FG_PROVIDER_SWITCH active=%llx camera=%u\n",(unsigned long long)activeProvider,unsigned(cameraAvailable));
  return true;
 }
 // Accept either SDK camera contract. The currently selected 3.1.6 provider
 // supports legacy preparation when an application has no usable world camera.
 void prepareStandard(const ffxDispatchDescHeader* input,bool enabled){
  std::lock_guard<std::recursive_mutex> lock(hostMutex);
  auto normalized=fsrFgCamera::normalize(input);
  need(normalized.status!=fsrFgCamera::Status::unsupported,normalized.reason);
  const int mode=normalized.status==fsrFgCamera::Status::camera?1:0;
  if(enabled&&selectCameraProvider(mode!=0))normalized.prepare.reset=true;
  if(standardCameraMode>=0&&standardCameraMode!=mode){drain();normalized.prepare.reset=true;}
  if(normalized.status==fsrFgCamera::Status::camera)prepare(normalized.prepare,enabled);
  else if(legacyCameraSupported)prepareLegacy(normalized.prepare,enabled);
  else prepare(normalized.prepare,false);
  standardCameraMode=mode;
 }
 void prepare(ffxDispatchDescFrameGenerationPrepareV2 pre,bool enabled){
  std::lock_guard<std::recursive_mutex> lock(hostMutex);
  need(context&&swapContext,"FG configured before prepare");
  fallbackReason=nullptr;
  if(enabled){
   const bool cameraBasis=fsrFgCamera::valid(pre);
   if(pre.header.pNext)fallbackReason="unsupported_prepare_chain";
   else if(!pre.commandList||!pre.depth.resource||!pre.motionVectors.resource)fallbackReason="missing_frame_resources";
   else if(!pre.renderSize.width||!pre.renderSize.height||pre.renderSize.width>maxRenderW||pre.renderSize.height>maxRenderH)fallbackReason="unsupported_render_extent";
   else if(!cameraBasis)fallbackReason="missing_or_invalid_camera";
   else if(!std::isfinite(pre.cameraFovAngleVertical)||pre.cameraFovAngleVertical<=0||pre.cameraFovAngleVertical>=3.141593f||!std::isfinite(pre.cameraNear)||!std::isfinite(pre.cameraFar)||!std::isfinite(pre.viewSpaceToMetersFactor)||pre.viewSpaceToMetersFactor<=0)fallbackReason="invalid_camera_projection";
   if(fallbackReason){enabled=false;if(fallbackMessages++<16)printf("FG_FALLBACK frame=%u reason=%s action=disable_fg_keep_upscaling\n",frames,fallbackReason);}
  }
  if(enabled!=enabledLast)drain();
  ffxConfigureDescFrameGeneration cfg{};cfg.header.type=FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;cfg.frameGenerationEnabled=enabled;cfg.swapChain=swap;cfg.frameGenerationCallback=generationOverride?generationOverride:generate;cfg.frameGenerationCallbackUserContext=generationOverride?generationOverrideUser:this;
  // WineForge-Internal: fsr4/sdk-owned-ui-composition-v1.
  // A null present callback selects FidelityFX's built-in UI compositor. It
  // consumes a registered currentUI resource when the game provides one and
  // otherwise performs the documented backbuffer copy. The former callback
  // only copied currentBackBuffer and silently discarded currentUI.
  cfg.presentCallback=nullptr;cfg.presentCallbackUserContext=nullptr;cfg.generationRect={0,0,int(W),int(H)};cfg.frameID=frames;
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

 void registerUi(FfxApiResource ui,uint32_t flags){
  std::lock_guard<std::recursive_mutex> lock(hostMutex);
  need(swapContext,"UI registration requires FG swapchain");
  ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 cfg{};
  cfg.header.type=FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
  cfg.uiResource=ui;cfg.flags=flags;
  need(configure(&swapContext,&cfg.header)==0,"FG UI registration");
 }

 void prepareLegacy(const ffxDispatchDescFrameGenerationPrepareV2& input,bool enabled,FfxApiResource hudless={}){
  std::lock_guard<std::recursive_mutex> lock(hostMutex);
  need(context&&swapContext,"FG legacy initialized");
  need(!input.header.pNext,"FG legacy unsupported chain");
  need(!enabled||(input.commandList&&input.depth.resource&&input.motionVectors.resource&&input.renderSize.width&&input.renderSize.height),"FG legacy resources");
  if(enabled!=enabledLast)drain();
  legacySerial+=(input.reset||!enabledLast)?2:1;
  ffxConfigureDescFrameGeneration cfg{};cfg.header.type=FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;cfg.frameGenerationEnabled=enabled;cfg.swapChain=swap;cfg.frameGenerationCallback=generationOverride?generationOverride:generate;cfg.frameGenerationCallbackUserContext=generationOverride?generationOverrideUser:this;cfg.HUDLessColor=hudless;
  // Keep presentation and UI composition in the SDK for the legacy path too.
  cfg.presentCallback=nullptr;cfg.presentCallbackUserContext=nullptr;cfg.generationRect={0,0,int(W),int(H)};cfg.frameID=legacySerial;cfg.allowAsyncWorkloads=asyncWorkloads;
  legacyConfig=cfg;auto rc=configure(&context,&cfg.header);need(rc==0,"FG legacy configure");
  if(enabled){
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
   ffxDispatchDescFrameGenerationPrepare pre{};pre.header.type=FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
#pragma clang diagnostic pop
   pre.frameID=legacySerial;pre.commandList=input.commandList;pre.renderSize=input.renderSize;pre.jitterOffset=input.jitterOffset;pre.motionVectorScale=input.motionVectorScale;pre.frameTimeDelta=input.frameTimeDelta;pre.cameraNear=input.cameraNear;pre.cameraFar=input.cameraFar;pre.cameraFovAngleVertical=input.cameraFovAngleVertical;pre.viewSpaceToMetersFactor=input.viewSpaceToMetersFactor;pre.depth=input.depth;pre.motionVectors=input.motionVectors;
   rc=dispatch(&context,&pre.header);if(rc!=0||frames<4||frames%120==0)printf("FG_LEGACY_PREPARE frame=%u id=%llu rc=%u world_camera=unavailable\n",frames,legacySerial,unsigned(rc));need(rc==0,"FG legacy prepare");
  }
  enabledLast=enabled;++frames;
 }
 HRESULT presentFrame(UINT interval,UINT flags){
  auto begin=Timing::enabled()?Timing::now():0;
  std::lock_guard<std::recursive_mutex> lock(hostMutex);
  TimingScope timing(presentTiming,*this,"provider_present",begin);
  auto hr=swap?swap->Present(interval,flags):DXGI_ERROR_INVALID_CALL;timing.error=FAILED(hr);return hr;
 }
 void drain(){std::lock_guard<std::recursive_mutex> lock(hostMutex);if(swapContext){ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait{};wait.header.type=FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;need(dispatch(&swapContext,&wait.header)==0,"FG wait presents");printf("FG_PRESENT_TOTAL real=%u generated=%u failures=%u\n",realCallbacks.load(),generatedCallbacks.load(),callbackFailures.load());need(callbackFailures==0,"FG callback errors");}}
 void close(){std::lock_guard<std::recursive_mutex> lock(hostMutex);if(swapContext){drain();if(context){ffxConfigureDescFrameGeneration cfg{};cfg.header.type=FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;cfg.swapChain=swap;need(configure(&context,&cfg.header)==0,"disable FG");}}if(context){need(destroy(&context,nullptr)==0,"FG destroy");context=nullptr;}if(swapContext){need(destroy(&swapContext,nullptr)==0,"FG swapchain destroy");swapContext=nullptr;swap=nullptr;}if(providerDevice){providerDevice->Release();providerDevice=nullptr;}legacyProvider=mlProvider=activeProvider=0;}
 ~FrameGenerationBridge(){if(context&&destroy)destroy(&context,nullptr);if(swapContext&&destroy)destroy(&swapContext,nullptr);if(module)FreeLibrary(module);}
};

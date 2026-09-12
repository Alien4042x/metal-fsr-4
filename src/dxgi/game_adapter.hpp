#pragma once
// WineForge-Internal: fsr4/generic-dxgi-game-fg-adapter-v1.
#include "../runtime/fsr_frame_generation_bridge.hpp"
#include "../vendor/fsr-sdk-2.3.0/upscalers/include/ffx_upscale.h"
#include <vector>
#include <memory>
namespace wfGameFg {
thread_local bool insideSdk=false;
struct SdkScope {bool previous=insideSdk;SdkScope(){insideSdk=true;}~SdkScope(){insideSdk=previous;}};
static bool requested(){wchar_t v[2]{};return GetEnvironmentVariableW(L"Enabled_Metal_FG",v,2)==1&&v[0]==L'1';}
class Swap;
static std::mutex registryMutex;
static std::vector<Swap*> swaps;
class Swap final:public IDXGISwapChain4 {
 std::atomic<ULONG> refs{1};
public:
 std::recursive_mutex mutex;
 std::unique_ptr<FrameGenerationBridge> fgOwner=std::make_unique<FrameGenerationBridge>();
 FrameGenerationBridge& fg=*fgOwner;
 IDXGISwapChain4* inner=nullptr;
 ID3D12CommandQueue* queue=nullptr;
 ID3D12Device* device=nullptr;
 DXGI_SWAP_CHAIN_DESC1 desc{};
 UINT contextFlags=0;
 bool prepared=false,broken=false;
 ID3D12Resource* hudlessBuffers[2]{};bool hudlessStates[2]{};unsigned hudlessCaptures=0;
 static bool captureRequested(){char v[2]{};return GetEnvironmentVariableA("METAL_FG_CAPTURE_HUDLESS",v,2)==1&&v[0]=='1';}
 void releaseHudless(){for(auto& r:hudlessBuffers)if(r){r->Release();r=nullptr;}hudlessStates[0]=hudlessStates[1]=false;}
 // Exact current backbuffer identity is required; size alone is insufficient.
 bool captureBeforeUi(ID3D12GraphicsCommandList* list,ID3D12Resource* color,D3D12_RESOURCE_STATES before){
  std::lock_guard<std::recursive_mutex> l(mutex);SdkScope scope;
  if(!captureRequested()||!prepared||!fg.enabledLast)return false;
  ID3D12Resource* back=nullptr;if(FAILED(inner->GetBuffer(inner->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&back))))return false;
  const bool match=back==color;back->Release();if(!match)return false;
  // AMD Resource Lifetime: async HUDLess input must be double buffered.
  const unsigned slot=fg.frames%2;auto& hudless=hudlessBuffers[slot];auto& hudlessReadable=hudlessStates[slot];
  if(!hudless){auto d=color->GetDesc();d.Flags=D3D12_RESOURCE_FLAG_NONE;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;FrameGenerationBridge::check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&hudless)),"HUD-less capture texture");}
  auto transition=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){if(a==b)return;D3D12_RESOURCE_BARRIER bar{};bar.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;bar.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};list->ResourceBarrier(1,&bar);};
  transition(color,before,D3D12_RESOURCE_STATE_COPY_SOURCE);
  if(hudlessReadable)transition(hudless,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyResource(hudless,color);
  transition(hudless,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);hudlessReadable=true;
  transition(color,D3D12_RESOURCE_STATE_COPY_SOURCE,before);
  fg.setLegacyHudless(ffxApiGetResourceDX12(hudless,FFX_API_RESOURCE_STATE_COMPUTE_READ));
  if(++hudlessCaptures<=4||hudlessCaptures%128==0)fprintf(stderr,"FG_HUDLESS_CAPTURE count=%u source=current_backbuffer boundary=upscale_list_close buffers=2 slot=%u width=%u height=%u\n",hudlessCaptures,slot,desc.Width,desc.Height);
  return true;
 }

 DXGI_COLOR_SPACE_TYPE colorSpace=DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
 Swap(ID3D12CommandQueue* q):queue(q){FrameGenerationBridge::check(queue->GetDevice(IID_PPV_ARGS(&device)),"FG game device");queue->AddRef();}
 void idle(){
  ID3D12Fence* fence=nullptr;FrameGenerationBridge::check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"FG idle fence");
  HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
  if(!event){fence->Release();throw std::runtime_error("FG idle event");}
  HRESULT hr=queue->Signal(fence,1);if(SUCCEEDED(hr))hr=fence->SetEventOnCompletion(1,event);
  DWORD wait=SUCCEEDED(hr)?WaitForSingleObject(event,10000):WAIT_FAILED;
  CloseHandle(event);fence->Release();FrameGenerationBridge::need(wait==WAIT_OBJECT_0,"FG game queue idle");
 }
 void disable(){if(fg.context&&fg.enabledLast){ffxDispatchDescFrameGenerationPrepareV2 empty{};fg.prepareLegacy(empty,false);}prepared=false;}
 void resetContext(){disable();fg.drain();idle();releaseHudless();if(fg.context){FrameGenerationBridge::need(fg.destroy(&fg.context,nullptr)==0,"FG game context destroy");fg.context=nullptr;}fg.enabledLast=false;}
 ~Swap(){SdkScope scope;try {std::lock_guard<std::recursive_mutex> l(mutex);idle();fg.close();releaseHudless();}catch(const std::exception& e){fprintf(stderr,"FG_GAME_QUARANTINE cleanup=%s resources_retained_until_process_exit=1\n",e.what());(void)fgOwner.release();inner=nullptr;device=nullptr;queue=nullptr;}if(inner)inner->Release();if(device)device->Release();if(queue)queue->Release();}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result) override {
  if(!result)return E_POINTER;*result=nullptr;
  if(iid==__uuidof(IUnknown)||iid==__uuidof(IDXGIObject)||iid==__uuidof(IDXGIDeviceSubObject)||iid==__uuidof(IDXGISwapChain)||iid==__uuidof(IDXGISwapChain1)||iid==__uuidof(IDXGISwapChain2)||iid==__uuidof(IDXGISwapChain3)||iid==__uuidof(IDXGISwapChain4)){*result=static_cast<IDXGISwapChain4*>(this);AddRef();return S_OK;}
  return E_NOINTERFACE;
 }
 ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
 ULONG STDMETHODCALLTYPE Release() override {ULONG count=--refs;if(!count){{std::lock_guard<std::mutex> l(registryMutex);swaps.erase(std::remove(swaps.begin(),swaps.end(),this),swaps.end());}delete this;}return count;}
 bool feed(const ffxDispatchDescUpscale& in,UINT flags){
  std::lock_guard<std::recursive_mutex> l(mutex);SdkScope scope;
  if(broken||!requested()||colorSpace!=DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709||in.header.pNext||!in.depth.resource||!in.motionVectors.resource||!in.commandList||!in.renderSize.width||!in.renderSize.height||in.renderSize.width>desc.Width||in.renderSize.height>desc.Height){disable();return false;}
  if(prepared){disable();fprintf(stderr,"FG_GAME_FALLBACK reason=multiple_upscales_before_present\n");return false;}
  UINT mapped=flags&(FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS|FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION|FFX_UPSCALE_ENABLE_DEPTH_INVERTED|FFX_UPSCALE_ENABLE_DEPTH_INFINITE);
  if(fg.context&&(mapped!=contextFlags||in.renderSize.width>fg.maxRenderW||in.renderSize.height>fg.maxRenderH))resetContext();
  if(!fg.context){ffxCreateContextDescFrameGeneration options{};options.displaySize={desc.Width,desc.Height};options.maxRenderSize=in.renderSize;options.backBufferFormat=ffxApiGetSurfaceFormatDX12(desc.Format);options.flags=mapped;fg.init(device,options,L"amd_fidelityfx_framegeneration_dx12.dll",true);contextFlags=mapped;fprintf(stderr,"FG_GAME_CONTEXT output=%ux%u flags=%u provider=3.1.6 input=legacy_no_world_camera\n",desc.Width,desc.Height,mapped);}
  ffxDispatchDescFrameGenerationPrepareV2 input{};
  input.commandList=in.commandList;input.renderSize=in.renderSize;input.jitterOffset=in.jitterOffset;input.motionVectorScale=in.motionVectorScale;input.frameTimeDelta=in.frameTimeDelta;input.reset=in.reset;input.cameraNear=in.cameraNear;input.cameraFar=in.cameraFar;input.cameraFovAngleVertical=in.cameraFovAngleVertical;input.viewSpaceToMetersFactor=in.viewSpaceToMetersFactor;input.depth=in.depth;input.motionVectors=in.motionVectors;
  fg.prepareLegacy(input,true);prepared=true;return true;
 }
 HRESULT present(UINT interval,UINT flags,const DXGI_PRESENT_PARAMETERS* params){
  std::lock_guard<std::recursive_mutex> l(mutex);SdkScope scope;
  if(flags&DXGI_PRESENT_TEST)return params?inner->Present1(interval,flags,params):inner->Present(interval,flags);
  try {if(!prepared)disable();auto hr=params?inner->Present1(interval,flags,params):fg.presentFrame(interval,flags);prepared=false;return hr;}catch(const std::exception& e){fprintf(stderr,"FG_GAME_FATAL present=%s\n",e.what());broken=true;return DXGI_ERROR_DEVICE_RESET;}
 }
 HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size, const void *data) override {return inner->SetPrivateData(guid, data_size, data);}
 HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *object) override {return inner->SetPrivateDataInterface(guid, object);}
 HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size, void *data) override {return inner->GetPrivateData(guid, data_size, data);}
 HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **parent) override {return inner->GetParent(riid, parent);}
 HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {return inner->GetDevice(riid, device);}
 HRESULT STDMETHODCALLTYPE Present(UINT sync_interval, UINT flags) override {return present(sync_interval,flags,nullptr);}
 HRESULT STDMETHODCALLTYPE GetBuffer(UINT buffer_idx, REFIID riid, void **surface) override {return inner->GetBuffer(buffer_idx, riid, surface);}
 HRESULT STDMETHODCALLTYPE SetFullscreenState(WINBOOL fullscreen, IDXGIOutput *target) override {return inner->SetFullscreenState(fullscreen, target);}
 HRESULT STDMETHODCALLTYPE GetFullscreenState(WINBOOL *fullscreen, IDXGIOutput **target) override {return inner->GetFullscreenState(fullscreen, target);}
 HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *desc) override {return inner->GetDesc(desc);}
 HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT buffer_count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) override {std::lock_guard<std::recursive_mutex> l(mutex);SdkScope scope;try{resetContext();auto hr=inner->ResizeBuffers(buffer_count, width, height, format, flags);if(SUCCEEDED(hr)){inner->GetDesc1(&desc);fg.W=desc.Width;fg.H=desc.Height;}return hr;}catch(...){broken=true;return DXGI_ERROR_DEVICE_RESET;}}
 HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *target_mode_desc) override {return inner->ResizeTarget(target_mode_desc);}
 HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **output) override {return inner->GetContainingOutput(output);}
 HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *stats) override {return inner->GetFrameStatistics(stats);}
 HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *last_present_count) override {return inner->GetLastPresentCount(last_present_count);}
 HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1 *pDesc) override {return inner->GetDesc1(pDesc);}
 HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pDesc) override {return inner->GetFullscreenDesc(pDesc);}
 HRESULT STDMETHODCALLTYPE GetHwnd(HWND *pHwnd) override {return inner->GetHwnd(pHwnd);}
 HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void **ppUnk) override {return inner->GetCoreWindow(refiid, ppUnk);}
 HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS *pPresentParameters) override {return present(SyncInterval,PresentFlags,pPresentParameters);}
 WINBOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override {return inner->IsTemporaryMonoSupported();}
 HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput **ppRestrictToOutput) override {return inner->GetRestrictToOutput(ppRestrictToOutput);}
 HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA *pColor) override {return inner->SetBackgroundColor(pColor);}
 HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA *pColor) override {return inner->GetBackgroundColor(pColor);}
 HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override {return inner->SetRotation(Rotation);}
 HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION *pRotation) override {return inner->GetRotation(pRotation);}
 HRESULT STDMETHODCALLTYPE SetSourceSize(UINT width, UINT height) override {return inner->SetSourceSize(width, height);}
 HRESULT STDMETHODCALLTYPE GetSourceSize(UINT *width, UINT *height) override {return inner->GetSourceSize(width, height);}
 HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT max_latency) override {return inner->SetMaximumFrameLatency(max_latency);}
 HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *max_latency) override {return inner->GetMaximumFrameLatency(max_latency);}
 HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override {return inner->GetFrameLatencyWaitableObject();}
 HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F *matrix) override {return inner->SetMatrixTransform(matrix);}
 HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F *matrix) override {return inner->GetMatrixTransform(matrix);}
 UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override {return inner->GetCurrentBackBufferIndex();}
 HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE colour_space, UINT *colour_space_support) override {return inner->CheckColorSpaceSupport(colour_space, colour_space_support);}
 HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE colour_space) override {std::lock_guard<std::recursive_mutex> l(mutex);try{disable();auto hr=inner->SetColorSpace1(colour_space);if(SUCCEEDED(hr))colorSpace=colour_space;return hr;}catch(...){return DXGI_ERROR_DEVICE_RESET;}}
 HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT buffer_count, UINT width, UINT height, DXGI_FORMAT format, UINT flags, const UINT *node_mask, IUnknown *const *present_queue) override {std::lock_guard<std::recursive_mutex> l(mutex);SdkScope scope;try{resetContext();auto hr=inner->ResizeBuffers1(buffer_count, width, height, format, flags, node_mask, present_queue);if(SUCCEEDED(hr)){inner->GetDesc1(&desc);fg.W=desc.Width;fg.H=desc.Height;}return hr;}catch(...){broken=true;return DXGI_ERROR_DEVICE_RESET;}}
 HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE type, UINT size, void *metadata) override {return inner->SetHDRMetaData(type, size, metadata);}
};
static bool createSwap(IDXGIFactory* factory,IUnknown* device,HWND hwnd,const DXGI_SWAP_CHAIN_DESC1* desc,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,IDXGIOutput* restrictOutput,IDXGISwapChain1** result,HRESULT& status){
 if(insideSdk||!requested())return false;
 if(!device||!desc||!result||restrictOutput||(fullscreen&&!fullscreen->Windowed)||!desc->Width||!desc->Height||desc->SampleDesc.Count!=1||(desc->Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&desc->Format!=DXGI_FORMAT_B8G8R8A8_UNORM)){
  fprintf(stderr,"FG_GAME_FALLBACK create=unsupported_swapchain format=%u windowed=%u\n",desc?unsigned(desc->Format):0u,fullscreen?unsigned(fullscreen->Windowed):1u);return false;
 }
 ID3D12CommandQueue* queue=nullptr;if(FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))))return false;
 try {SdkScope scope;std::unique_ptr<Swap> obj(new Swap(queue));queue->Release();queue=nullptr;obj->desc=*desc;
  wchar_t path[32768]{};HMODULE self=nullptr;FrameGenerationBridge::need(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&createSwap),&self),"FG proxy module");
  DWORD n=GetModuleFileNameW(self,path,32768);FrameGenerationBridge::need(n&&n<32768,"FG proxy path");std::wstring dll(path,n);auto slash=dll.find_last_of(L"\\/");FrameGenerationBridge::need(slash!=std::wstring::npos,"FG proxy directory");dll.resize(slash+1);dll+=L"amd_fidelityfx_framegeneration_dx12.dll";
  obj->fg.openModule(dll.c_str());obj->fg.W=desc->Width;obj->fg.H=desc->Height;
  auto copy=*desc;obj->inner=obj->fg.makeSwap(hwnd,factory,obj->queue,&copy);
  {std::lock_guard<std::mutex> l(registryMutex);swaps.push_back(obj.get());}
  *result=static_cast<IDXGISwapChain1*>(obj.release());status=S_OK;fprintf(stderr,"FG_GAME_SWAPCHAIN attached=1 output=%ux%u\n",desc->Width,desc->Height);return true;
 }catch(const std::exception& e){if(queue)queue->Release();fprintf(stderr,"FG_GAME_FALLBACK create=%s\n",e.what());status=E_FAIL;return false;}
}
class Factory final:public IDXGIFactory7 {
 std::atomic<ULONG> refs{1};IDXGIFactory* inner;
public:
 explicit Factory(IDXGIFactory* ptr):inner(ptr){}
 ~Factory(){inner->Release();}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result) override {
  if(!result)return E_POINTER;*result=nullptr;
  if(iid==__uuidof(IUnknown)||iid==__uuidof(IDXGIObject)||iid==__uuidof(IDXGIFactory)||iid==__uuidof(IDXGIFactory1)||iid==__uuidof(IDXGIFactory2)||iid==__uuidof(IDXGIFactory3)||iid==__uuidof(IDXGIFactory4)||iid==__uuidof(IDXGIFactory5)||iid==__uuidof(IDXGIFactory6)||iid==__uuidof(IDXGIFactory7)){
   IUnknown* supported=nullptr;HRESULT hr=inner->QueryInterface(iid,reinterpret_cast<void**>(&supported));if(FAILED(hr))return hr;supported->Release();*result=static_cast<IDXGIFactory7*>(this);AddRef();return S_OK;
  }return E_NOINTERFACE;
 }
 ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
 ULONG STDMETHODCALLTYPE Release() override{ULONG n=--refs;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size, const void *data) override {IDXGIFactory* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return E_NOINTERFACE;}auto result=target->SetPrivateData(guid, data_size, data);target->Release();return result;}
 HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *object) override {IDXGIFactory* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return E_NOINTERFACE;}auto result=target->SetPrivateDataInterface(guid, object);target->Release();return result;}
 HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size, void *data) override {IDXGIFactory* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return E_NOINTERFACE;}auto result=target->GetPrivateData(guid, data_size, data);target->Release();return result;}
 HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **parent) override {IDXGIFactory* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return E_NOINTERFACE;}auto result=target->GetParent(riid, parent);target->Release();return result;}
 HRESULT STDMETHODCALLTYPE EnumAdapters(UINT adapter_idx, IDXGIAdapter **adapter) override {IDXGIFactory* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return E_NOINTERFACE;}auto result=target->EnumAdapters(adapter_idx, adapter);target->Release();return result;}
 HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND window, UINT flags) override {IDXGIFactory* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return E_NOINTERFACE;}auto result=target->MakeWindowAssociation(window, flags);target->Release();return result;}
 HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND *window) override {IDXGIFactory* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return E_NOINTERFACE;}auto result=target->GetWindowAssociation(window);target->Release();return result;}
 HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* device,DXGI_SWAP_CHAIN_DESC* desc,IDXGISwapChain** swapchain) override {
  if(desc&&swapchain){
   DXGI_SWAP_CHAIN_DESC1 modern{};modern.Width=desc->BufferDesc.Width;modern.Height=desc->BufferDesc.Height;modern.Format=desc->BufferDesc.Format;modern.SampleDesc=desc->SampleDesc;modern.BufferUsage=desc->BufferUsage;modern.BufferCount=desc->BufferCount;modern.Scaling=DXGI_SCALING_STRETCH;modern.SwapEffect=desc->SwapEffect;modern.AlphaMode=DXGI_ALPHA_MODE_IGNORE;modern.Flags=desc->Flags;
   DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen{};fullscreen.Windowed=desc->Windowed;fullscreen.RefreshRate=desc->BufferDesc.RefreshRate;fullscreen.ScanlineOrdering=desc->BufferDesc.ScanlineOrdering;fullscreen.Scaling=desc->BufferDesc.Scaling;
   IDXGISwapChain1* result=nullptr;HRESULT hr=E_FAIL;
   if(createSwap(inner,device,desc->OutputWindow,&modern,&fullscreen,nullptr,&result,hr)){
    if(FAILED(hr))return hr;hr=result->QueryInterface(IID_PPV_ARGS(swapchain));result->Release();return hr;
   }
  }
  return inner->CreateSwapChain(device,desc,swapchain);
 }
 HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE swrast, IDXGIAdapter **adapter) override {IDXGIFactory* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return E_NOINTERFACE;}auto result=target->CreateSoftwareAdapter(swrast, adapter);target->Release();return result;}
 HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter, IDXGIAdapter1 **ppAdapter) override {IDXGIFactory1* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return E_NOINTERFACE;}auto result=target->EnumAdapters1(Adapter, ppAdapter);target->Release();return result;}
 WINBOOL STDMETHODCALLTYPE IsCurrent() override {IDXGIFactory1* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return 0;}auto result=target->IsCurrent();target->Release();return result;}
 WINBOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override {IDXGIFactory2* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return 0;}auto result=target->IsWindowedStereoEnabled();target->Release();return result;}
 HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown *pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) override {IDXGIFactory2* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return E_NOINTERFACE;}HRESULT intercepted=E_FAIL;if(createSwap(inner,pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain,intercepted)){target->Release();return intercepted;}auto result=target->CreateSwapChainForHwnd(pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);target->Release();return result;}
 HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown *pDevice, IUnknown *pWindow, const DXGI_SWAP_CHAIN_DESC1 *pDesc, IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) override {IDXGIFactory2* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return E_NOINTERFACE;}auto result=target->CreateSwapChainForCoreWindow(pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);target->Release();return result;}
 HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource, LUID *pLuid) override {IDXGIFactory2* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return E_NOINTERFACE;}auto result=target->GetSharedResourceAdapterLuid(hResource, pLuid);target->Release();return result;}
 HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle, UINT wMsg, DWORD *pdwCookie) override {IDXGIFactory2* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return E_NOINTERFACE;}auto result=target->RegisterStereoStatusWindow(WindowHandle, wMsg, pdwCookie);target->Release();return result;}
 HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent, DWORD *pdwCookie) override {IDXGIFactory2* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return E_NOINTERFACE;}auto result=target->RegisterStereoStatusEvent(hEvent, pdwCookie);target->Release();return result;}
 void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) override {IDXGIFactory2* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return;}target->UnregisterStereoStatus(dwCookie);target->Release();}
 HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND WindowHandle, UINT wMsg, DWORD *pdwCookie) override {IDXGIFactory2* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return E_NOINTERFACE;}auto result=target->RegisterOcclusionStatusWindow(WindowHandle, wMsg, pdwCookie);target->Release();return result;}
 HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD *pdwCookie) override {IDXGIFactory2* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return E_NOINTERFACE;}auto result=target->RegisterOcclusionStatusEvent(hEvent, pdwCookie);target->Release();return result;}
 void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) override {IDXGIFactory2* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return;}target->UnregisterOcclusionStatus(dwCookie);target->Release();}
 HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown *pDevice, const DXGI_SWAP_CHAIN_DESC1 *pDesc, IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) override {IDXGIFactory2* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return E_NOINTERFACE;}auto result=target->CreateSwapChainForComposition(pDevice, pDesc, pRestrictToOutput, ppSwapChain);target->Release();return result;}
 UINT STDMETHODCALLTYPE GetCreationFlags() override {IDXGIFactory3* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return 0;}auto result=target->GetCreationFlags();target->Release();return result;}
 HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID luid, REFIID iid, void **adapter) override {IDXGIFactory4* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return E_NOINTERFACE;}auto result=target->EnumAdapterByLuid(luid, iid, adapter);target->Release();return result;}
 HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID iid, void **adapter) override {IDXGIFactory4* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return E_NOINTERFACE;}auto result=target->EnumWarpAdapter(iid, adapter);target->Release();return result;}
 HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE feature, void *support_data, UINT support_data_size) override {IDXGIFactory5* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return E_NOINTERFACE;}auto result=target->CheckFeatureSupport(feature, support_data, support_data_size);target->Release();return result;}
 HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT adapter_idx, DXGI_GPU_PREFERENCE gpu_preference, REFIID iid, void **adapter) override {IDXGIFactory6* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return E_NOINTERFACE;}auto result=target->EnumAdapterByGpuPreference(adapter_idx, gpu_preference, iid, adapter);target->Release();return result;}
 HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE event, DWORD *cookie) override {IDXGIFactory7* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return E_NOINTERFACE;}auto result=target->RegisterAdaptersChangedEvent(event, cookie);target->Release();return result;}
 HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD cookie) override {IDXGIFactory7* target=nullptr;if(FAILED(inner->QueryInterface(IID_PPV_ARGS(&target)))){return E_NOINTERFACE;}auto result=target->UnregisterAdaptersChangedEvent(cookie);target->Release();return result;}
};
static void wrapFactory(REFIID iid,void** value){
 if(insideSdk||!requested()||!value||!*value)return;
 auto original=static_cast<IUnknown*>(*value);IDXGIFactory* base=nullptr;if(FAILED(original->QueryInterface(IID_PPV_ARGS(&base))))return;
 try {auto wrapper=new Factory(base);void* result=nullptr;HRESULT hr=wrapper->QueryInterface(iid,&result);wrapper->Release();if(SUCCEEDED(hr)){original->Release();*value=result;}}catch(...){base->Release();}
}
}
extern "C" __declspec(dllexport) unsigned WINAPI WfFgPrepareUpscale(const ffxDispatchDescUpscale* input,UINT createFlags){
 if(!input)return 0;
 try {ID3D12Device* device=nullptr;auto list=static_cast<ID3D12GraphicsCommandList*>(input->commandList);if(!list||FAILED(list->GetDevice(IID_PPV_ARGS(&device))))return 0;
  std::lock_guard<std::mutex> lock(wfGameFg::registryMutex);wfGameFg::Swap* selected=nullptr;unsigned matches=0;
  for(auto swap:wfGameFg::swaps){std::lock_guard<std::recursive_mutex> l(swap->mutex);if(swap->device==device&&(!input->upscaleSize.width||input->upscaleSize.width==swap->desc.Width)&&(!input->upscaleSize.height||input->upscaleSize.height==swap->desc.Height)){selected=swap;++matches;}}
  device->Release();if(matches!=1)return 0;return selected->feed(*input,createFlags)?1:0;
 }catch(const std::exception& e){fprintf(stderr,"FG_GAME_FATAL prepare=%s\n",e.what());return 2;}
}
extern "C" __declspec(dllexport) unsigned WINAPI WfFgProbeDrain(){
 try{std::lock_guard<std::mutex> lock(wfGameFg::registryMutex);unsigned total=0;for(auto s:wfGameFg::swaps){std::lock_guard<std::recursive_mutex> l(s->mutex);s->fg.drain();total+=s->fg.generatedCallbacks.load();}return total;}catch(...){return 0;}
}

extern "C" __declspec(dllexport) unsigned WINAPI WfFgCaptureBeforeUi(ID3D12GraphicsCommandList* list,ID3D12Resource* color,D3D12_RESOURCE_STATES state){
 if(!list||!color)return 0;
 try{std::lock_guard<std::mutex> lock(wfGameFg::registryMutex);for(auto s:wfGameFg::swaps)if(s->captureBeforeUi(list,color,state))return 1;}catch(const std::exception& e){fprintf(stderr,"FG_HUDLESS_CAPTURE_ERROR reason=%s\n",e.what());}
 return 0;
}

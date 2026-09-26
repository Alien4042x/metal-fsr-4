#include <mutex>
// WineForge-Internal: fsr-lab/dll-owned-native-transport-v1.
// Experimental ONE-context/base-list route, with explicit caller display rebind.
// No Scene callback. Not a production game deployment.
#include <functional>
#include <array>
static void check(HRESULT h,const char* message){if(FAILED(h)){fprintf(stderr,"FAIL BRIDGE_HRESULT reason=%s hr=0x%08lx\n",message,static_cast<unsigned long>(h));fflush(stderr);throw std::runtime_error(message);}}
static void need(bool v,const char* message){if(!v)throw std::runtime_error(message);}
#include "dll_native_split.hpp"
#if defined(FSR4_PROVIDER_INLINE_HELPER)
// WineForge-Internal: fsr4/provider-owned-native-helper-v1.
// The FidelityFX provider PE owns the native entry; no D3D12 preload is needed.
extern "C" int WINAPI LabFullFrame(const wchar_t*, FullMetalArgs*);
#endif
class DllNativeTransport {
 inline static std::mutex ownerMutex;
 inline static DllNativeTransport* owner=nullptr;
 UINT maxW,maxH;bool initialized=false;
 ID3D12Device* d;HMODULE module=nullptr;
 using Call=int(WINAPI*)(const wchar_t*,FullMetalArgs*);Call call=nullptr;
 FullMetalArgs args{};ID3D12Resource* inputs[4]{};ID3D12Resource* output=nullptr;
 D3D12_RESOURCE_DESC descriptions[5]{};D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[5]{};UINT64 capacities[5]{};
 std::unique_ptr<DllNativeSplit> split;
 std::array<ID3D12Resource*,5> retained{};
 unsigned frames=0;void** queueTable=nullptr;void* originalExecute=nullptr;
 void finishPrevious(){
  if(split){if(!split->abandoned)split->completion();split.reset();for(auto& r:retained){if(r)r->Release();r=nullptr;}}
 }
 ID3D12Resource* buffer(UINT64 size,D3D12_HEAP_TYPE type){
  D3D12_HEAP_PROPERTIES h{};h.Type=type;h.CreationNodeMask=h.VisibleNodeMask=1;
  D3D12_RESOURCE_DESC r{};r.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;r.Width=size;r.Height=r.DepthOrArraySize=r.MipLevels=r.SampleDesc.Count=1;r.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* result=nullptr;check(d->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&r,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&result)),"transport buffer");return result;
 }
 static void barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};list->ResourceBarrier(1,&x);}
 void installQueueObserver(){
  ID3D12CommandQueue* probe=nullptr;D3D12_COMMAND_QUEUE_DESC desc{};check(d->CreateCommandQueue(&desc,IID_PPV_ARGS(&probe)),"queue rendezvous");
  queueTable=*reinterpret_cast<void***>(probe);originalExecute=queueTable[10];need(!DllNativeSplit::queueOriginal,"single DLL queue observer");
  DWORD protection;check(VirtualProtect(queueTable+10,sizeof(void*),PAGE_READWRITE,&protection)?S_OK:E_FAIL,"queue vtable protect");
  DllNativeSplit::queueOriginal=reinterpret_cast<DllNativeSplit::Execute>(originalExecute);queueTable[10]=reinterpret_cast<void*>(&DllNativeSplit::execute);
  DWORD old;need(VirtualProtect(queueTable+10,sizeof(void*),protection,&old)!=0,"queue vtable restore protection");probe->Release();
 }
 bool initialize(){
  if(initialized)return true;
  if(owner&&owner!=this)return false;
  wchar_t path[2048];DWORD length=GetEnvironmentVariableW(L"METAL_FSR4_LIBRARY",path,2048);need(length&&length<2048,"explicit native path required");
#if defined(FSR4_PROVIDER_INLINE_HELPER)
  call=&LabFullFrame;
#else
  // WineForge-Internal: fsr4/explicit-central-helper-path-v1.
  wchar_t helper[32768];DWORD helperLength=GetEnvironmentVariableW(L"METAL_FSR4_HELPER",helper,32768);
  need(helperLength&&helperLength<32768&&helper[0]==L'Z'&&helper[1]==L':'&&helper[2]==L'\\',"absolute Metal FSR helper path required");
  module=LoadLibraryW(helper);need(module!=nullptr,"native bridge DLL");call=reinterpret_cast<Call>(GetProcAddress(module,"LabFullFrame"));need(call!=nullptr,"native entry");
#endif
  args.version=1;args.size=sizeof(args);args.maxWidth=maxW;args.maxHeight=maxH;
  int rc=call(path,&args);need(rc==0&&args.completed,"native context init");
  try{installQueueObserver();}catch(...){args.op=2;call(nullptr,&args);throw;}
  initialized=true;owner=this;
  puts("DLL_NATIVE_READY own_transport=1 scene_callback=0 model=4.0.2 lazy=1");
  return true;
 }
public:
 DllNativeTransport(ID3D12Device* device,UINT w,UINT h):maxW(w),maxH(h),d(device){}
 ~DllNativeTransport(){
  std::lock_guard<std::mutex> lock(ownerMutex);
  if(!initialized){if(module)FreeLibrary(module);return;}
  try{finishPrevious();if(queueTable){DWORD protection,old;need(VirtualProtect(queueTable+10,sizeof(void*),PAGE_READWRITE,&protection)!=0,"restore queue access");queueTable[10]=originalExecute;need(VirtualProtect(queueTable+10,sizeof(void*),protection,&old)!=0,"restore queue protection");DllNativeSplit::queueOriginal=nullptr;}
   args.op=2;need(call(nullptr,&args)==0&&args.completed,"native close");for(auto r:inputs)if(r)r->Release();if(output)output->Release();if(module)FreeLibrary(module);owner=nullptr;
   }catch(...){puts("BRIDGE_TRANSPORT_TEARDOWN_DEGRADED reason=exception_during_teardown (was ExitProcess(84); restoring queue hook and unloading helper best-effort)");try{if(queueTable){DWORD p,o;VirtualProtect(queueTable+10,sizeof(void*),PAGE_READWRITE,&p);queueTable[10]=originalExecute;VirtualProtect(queueTable+10,sizeof(void*),p,&o);}DllNativeSplit::queueOriginal=nullptr;}catch(...){}try{if(call){args.op=2;call(nullptr,&args);}}catch(...){}if(module)FreeLibrary(module);owner=nullptr;}
 }
 bool record(ID3D12GraphicsCommandList* list,const LabNativeFramePacket& packet){
  std::lock_guard<std::mutex> lock(ownerMutex);
  if(!initialize())return false;
  if(split&&split->abandoned)finishPrevious();
  if(split&&!split->submitted)return false;
  finishPrevious();
  // First context only; unknown formats/extents were admitted by provider before here.
  for(unsigned i=0;i<5;++i){if(i!=3&&!packet.resources[i])return false;}
  if(!output){UINT64 total=0;for(unsigned i=0;i<5;++i){
   if(i==3&&!packet.resources[i]){capacities[i]=4096;fp[i].Footprint={DXGI_FORMAT_R16_FLOAT,1,1,1,256};inputs[i]=buffer(capacities[i],D3D12_HEAP_TYPE_UPLOAD);void* value=nullptr;D3D12_RANGE noRead{};check(inputs[i]->Map(0,&noRead,&value),"default exposure map");memset(value,0,4096);*static_cast<uint16_t*>(value)=0x3c00;D3D12_RANGE written{0,2};inputs[i]->Unmap(0,&written);total+=4096;continue;}
   auto desc=packet.resources[i]->GetDesc();UINT64 size=0;descriptions[i]=desc;d->GetCopyableFootprints(&desc,0,1,0,&fp[i],nullptr,nullptr,&size);if(i==1&&desc.Format==DXGI_FORMAT_R32G8X24_TYPELESS)need(fp[i].Footprint.Format==DXGI_FORMAT_R32_TYPELESS,"D32S8 depth-plane footprint");size=UINT64(fp[i].Footprint.RowPitch)*fp[i].Footprint.Height;capacities[i]=(size+4095)&~UINT64(4095);need(capacities[i]&&capacities[i]<=80ull*1024*1024,"transport cap");total+=capacities[i];if(i<4)inputs[i]=buffer(capacities[i],D3D12_HEAP_TYPE_READBACK);else output=buffer(capacities[i],D3D12_HEAP_TYPE_UPLOAD);}need(total<=192ull*1024*1024,"total transport cap");}
  for(unsigned i=0;i<5;++i){if(i==3&&!packet.resources[i]){if(descriptions[i].Width)return false;continue;}auto desc=packet.resources[i]->GetDesc();if(desc.Width!=descriptions[i].Width||desc.Height!=descriptions[i].Height||desc.Format!=descriptions[i].Format)return false;}
  for(unsigned i=0;i<5;++i){retained[i]=packet.resources[i];if(retained[i])retained[i]->AddRef();}
  for(unsigned i=0;i<4;++i){auto r=packet.resources[i];if(!r)continue;barrier(list,r,packet.states[i],D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=r;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=inputs[i];to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=fp[i];list->CopyTextureRegion(&to,0,0,0,&from,nullptr);barrier(list,r,D3D12_RESOURCE_STATE_COPY_SOURCE,packet.states[i]);}
  split=std::make_unique<DllNativeSplit>(d,list,[this,packet](){
   args.op=1;args.frame=frames;args.params=packet.params;D3D12_RANGE empty{};
   for(unsigned i=0;i<4;++i){void* p=nullptr;D3D12_RANGE read{0,SIZE_T(capacities[i])};check(inputs[i]->Map(0,&read,&p),"native input map");args.input[i]=uintptr_t(p);args.inputBytes[i]=capacities[i];args.params.pitch[i]=fp[i].Footprint.RowPitch;}
   void* p=nullptr;check(output->Map(0,&empty,&p),"native output map");args.output=uintptr_t(p);args.outputBytes=capacities[4];args.params.outputPitch=fp[4].Footprint.RowPitch;
    int rc=call(nullptr,&args);if(args.pending||rc||!args.completed||!args.validated){printf("FAIL DLL_NATIVE rc=%d detail=%s (upscaled frame skipped, process continues)\n",rc,args.error);for(auto r:inputs)r->Unmap(0,&empty);output->Unmap(0,&empty);return;}
   for(auto r:inputs)r->Unmap(0,&empty);D3D12_RANGE written{0,SIZE_T(capacities[4])};output->Unmap(0,&written);++frames;printf("DLL_NATIVE_FRAME frame=%u gpu_ms=%.6f\n",frames,args.gpuMs);
  });
  auto r=packet.resources[4];barrier(list,r,packet.states[4],D3D12_RESOURCE_STATE_COPY_DEST);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=output;from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=fp[4];to.pResource=r;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;D3D12_BOX box{0,0,0,packet.params.width,packet.params.height,1};list->CopyTextureRegion(&to,0,0,0,&from,&box);barrier(list,r,D3D12_RESOURCE_STATE_COPY_DEST,packet.states[4]);return true;
 }
};

// WineForge-Internal: fsr4/standard-ffx-provider-with-original-fallback-v1.
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "fsr4_d3d12_context.hpp"
#include "fsr4_ffx_status.hpp"
#include "fsr_adapter_trace.hpp"
#include "../vendor/fsr-sdk-2.3.0/api/include/dx12/ffx_api_dx12.h"

namespace {
constexpr char versionName[]="4.0.2 WineForge experimental";
struct Original {
    HMODULE module=nullptr;
    PfnFfxCreateContext create=nullptr;PfnFfxDestroyContext destroy=nullptr;PfnFfxConfigure configure=nullptr;PfnFfxQuery query=nullptr;PfnFfxDispatch dispatch=nullptr;
    std::wstring directory;bool enabled=false,upgrade31=false,ready=false,traceEnabled=false;
};
Original original;
std::once_flag initialized;
struct State {
    std::mutex mutex;
    std::unique_ptr<Fsr4D3D12Context> renderer;
    WfFsr4Status status;
    bool originalNeedsReset=false;
    bool disabled=false;
    UINT upscaleFlags=0;
};
std::mutex contextsMutex;
std::unordered_map<ffxContext,std::shared_ptr<State>> contexts;
bool environment(const wchar_t* key) {wchar_t value[8]{};return GetEnvironmentVariableW(key,value,8)==1&&value[0]==L'1';}
bool initialize() {
    std::call_once(initialized,[]{
        HMODULE self=nullptr;
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&ffxCreateContext),&self))return;
        wchar_t path[32768];const DWORD n=GetModuleFileNameW(self,path,32768);if(!n||n==32768)return;
        original.directory.assign(path,n);const auto slash=original.directory.find_last_of(L"\\/");if(slash==std::wstring::npos)return;original.directory.resize(slash+1);
        const auto name=original.directory+L"wf_original_fidelityfx_loader_dx12.dll";
        original.module=LoadLibraryExW(name.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!original.module||original.module==self)return;
        original.create=reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(original.module,"ffxCreateContext"));
        original.destroy=reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(original.module,"ffxDestroyContext"));
        original.configure=reinterpret_cast<PfnFfxConfigure>(GetProcAddress(original.module,"ffxConfigure"));
        original.query=reinterpret_cast<PfnFfxQuery>(GetProcAddress(original.module,"ffxQuery"));
        original.dispatch=reinterpret_cast<PfnFfxDispatch>(GetProcAddress(original.module,"ffxDispatch"));
        original.ready=original.create&&original.destroy&&original.configure&&original.query&&original.dispatch;
        if(!original.ready)return;
        // Original version-name pointers must remain valid across context lifetimes.
        HMODULE pinned=nullptr;if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN,name.c_str(),&pinned)){original.ready=false;return;}
        const auto ini=original.directory+L"wf-fsr4.ini";
        original.enabled=environment(L"Enabled_Metal_FSR4")||GetPrivateProfileIntW(L"FSR4",L"Enable",0,ini.c_str())==1;
        original.upgrade31=environment(L"METAL_FSR4_UPGRADE31")||GetPrivateProfileIntW(L"FSR4",L"UpgradeFSR31",0,ini.c_str())==1;
        original.traceEnabled=environment(L"METAL_FSR4_TRACE")||GetPrivateProfileIntW(L"FSR4",L"Trace",0,ini.c_str())==1;
        if(original.traceEnabled)traceInit(original.directory,true);
    });
    return original.ready;
}
struct Version {uint64_t id;const char* name;};
std::vector<Version> versions(void* device) {
    uint64_t count=0;ffxQueryDescGetVersions q{};q.header.type=FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType=FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;q.device=device;q.outputCount=&count;
    if(original.query(nullptr,&q.header)!=FFX_API_RETURN_OK||!count||count>128)return {};
    const auto capacity=count;std::vector<uint64_t> ids(count);std::vector<const char*> names(count);
    q.versionIds=ids.data();q.versionNames=names.data();
    if(original.query(nullptr,&q.header)!=FFX_API_RETURN_OK||count>capacity)return {};
    std::vector<Version> out;for(size_t i=0;i<count;++i)if(names[i])out.push_back({ids[i],names[i]});return out;
}
std::shared_ptr<State> find(ffxContext* context) {
    if(!context||!*context)return {};
    std::lock_guard<std::mutex> lock(contextsMutex);auto it=contexts.find(*context);return it==contexts.end()?nullptr:it->second;
}
// Opt-in handoff to our process-local DXGI adapter; absent adapter is a no-op.
void notifyFrameGeneration(const ffxDispatchDescUpscale& input,UINT flags){
    if(!environment(L"Enabled_Metal_FG"))return;
    auto module=GetModuleHandleW(L"dxgi.dll");
    using Prepare=unsigned(WINAPI*)(const ffxDispatchDescUpscale*,UINT);
    auto prepare=module?reinterpret_cast<Prepare>(GetProcAddress(module,"WfFgPrepareUpscale")):nullptr;
    if(prepare){auto result=prepare(&input,flags);if(result==2)throw std::runtime_error("FG adapter prepare failed");}
}
void reason(State& state,const char* text) {std::snprintf(state.status.reason,sizeof(state.status.reason),"%s",text);}
}

extern "C" FFX_API_ENTRY ffxReturnCode_t ffxCreateContext(ffxContext* context,ffxCreateContextDescHeader* header,const ffxAllocationCallbacks* callbacks) {
    try {
        if(!initialize())return FFX_API_RETURN_NO_PROVIDER;
        if(!context||!header)return FFX_API_RETURN_ERROR_PARAMETER;
        if(!original.enabled||callbacks||header->type!=FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE)return original.create(context,header,callbacks);
        ID3D12Device* device=nullptr;const ffxOverrideVersion* requested=nullptr;bool supportedChain=true;
        const ffxCreateContextDescUpscaleVersion* apiVersion=nullptr;
        unsigned depth=0;
        for(auto p=header->pNext;p;p=p->pNext){if(++depth>16){supportedChain=false;break;}
            if(p->type==FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12&&!device)device=reinterpret_cast<ffxCreateBackendDX12Desc*>(p)->device;
            else if(p->type==FFX_API_DESC_TYPE_OVERRIDE_VERSION&&!requested)requested=reinterpret_cast<ffxOverrideVersion*>(p);
            else if(p->type==FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION&&!apiVersion)apiVersion=reinterpret_cast<ffxCreateContextDescUpscaleVersion*>(p);
            else supportedChain=false;}
        if(!supportedChain||!device)return original.create(context,header,callbacks);
        const auto available=versions(device);uint64_t fallbackId=0;bool request31=false;
        for(const auto& v:available){if(!fallbackId||std::strncmp(v.name,"3.1.",4)==0)fallbackId=v.id;
            if(requested&&requested->versionId==v.id)request31=std::strncmp(v.name,"3.1.",4)==0;}
        const bool explicitOurs=requested&&requested->versionId==wfFsr4VersionId;
        const bool wantsOurs=explicitOurs||!requested||(original.upgrade31&&request31);
        if(!wantsOurs)return original.create(context,header,callbacks);
        if(!fallbackId)return FFX_API_RETURN_NO_PROVIDER;
        auto create=*reinterpret_cast<ffxCreateContextDescUpscale*>(header);
        ffxCreateBackendDX12Desc backend{};backend.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;backend.device=device;
        ffxOverrideVersion selected{};selected.header.type=FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        selected.versionId=explicitOurs||!requested?fallbackId:requested->versionId;
        ffxCreateContextDescUpscaleVersion version{};if(apiVersion)version=*apiVersion;version.header.pNext=nullptr;
        selected.header.pNext=apiVersion?&version.header:nullptr;backend.header.pNext=&selected.header;create.header.pNext=&backend.header;
        // Store copied descriptors with the context: FFX requires them to outlive create.
        struct Descriptors {ffxCreateContextDescUpscale create;ffxCreateBackendDX12Desc backend;ffxOverrideVersion selected;ffxCreateContextDescUpscaleVersion version;};
        auto descriptors=std::make_shared<Descriptors>(Descriptors{create,backend,selected,version});
        descriptors->create.header.pNext=&descriptors->backend.header;descriptors->backend.header.pNext=&descriptors->selected.header;
        descriptors->selected.header.pNext=apiVersion?&descriptors->version.header:nullptr;
        // A shared owner keeps both the copied API chain and renderer alive.
        struct OwnedState:State {std::shared_ptr<Descriptors> descriptors;};
        auto state=std::make_shared<OwnedState>();state->descriptors=descriptors;state->upscaleFlags=create.flags;
        auto rc=original.create(context,&descriptors->create.header,callbacks);if(rc!=FFX_API_RETURN_OK)return rc;
        try {state->renderer=std::make_unique<Fsr4D3D12Context>(device,create,original.directory+L"dxcompiler.dll");reason(*state,"FSR4 ready; awaiting compatible dispatch");}
        catch(const std::exception& e){reason(*state,e.what());}
        try {std::lock_guard<std::mutex> lock(contextsMutex);contexts.emplace(*context,state);}
        catch(...){original.destroy(context,callbacks);throw;}
        if(original.traceEnabled)traceCreate(context,&descriptors->create.header,FFX_API_RETURN_OK);return FFX_API_RETURN_OK;
    }catch(const std::bad_alloc&){return FFX_API_RETURN_ERROR_MEMORY;}catch(...){return FFX_API_RETURN_ERROR_RUNTIME_ERROR;}
}
extern "C" FFX_API_ENTRY ffxReturnCode_t ffxDestroyContext(ffxContext* context,const ffxAllocationCallbacks* callbacks) {
    try {if(!initialize())return FFX_API_RETURN_NO_PROVIDER;auto state=find(context);if(!state)return original.destroy(context,callbacks);
        std::lock_guard<std::mutex> lock(state->mutex);const auto key=*context;const auto rc=original.destroy(context,callbacks);
        if(rc==FFX_API_RETURN_OK){std::lock_guard<std::mutex> mapLock(contextsMutex);contexts.erase(key);}return rc;
    }catch(...){return FFX_API_RETURN_ERROR_RUNTIME_ERROR;}
}
extern "C" FFX_API_ENTRY ffxReturnCode_t ffxConfigure(ffxContext* context,const ffxConfigureDescHeader* header) {
    try {if(!initialize())return FFX_API_RETURN_NO_PROVIDER;auto state=find(context);
        if(!state){const auto rc=original.configure(context,header);
            if(original.traceEnabled)traceConfigure(context,header,rc,false,false);return rc;}
        std::lock_guard<std::mutex> lock(state->mutex);const auto rc=original.configure(context,header);
        // Unknown configuration may change original semantics. Stay on original
        // until a new context instead of silently ignoring its effect in FSR4.
        if(rc==FFX_API_RETURN_OK){state->disabled=true;state->status.backend=0;reason(*state,"configuration selects original provider");}
        if(original.traceEnabled)traceConfigure(context,header,rc,true,state->disabled);
        return rc;
    }catch(...){return FFX_API_RETURN_ERROR_RUNTIME_ERROR;}
}
extern "C" FFX_API_ENTRY ffxReturnCode_t ffxQuery(ffxContext* context,ffxQueryDescHeader* header) {
    try {if(!initialize())return FFX_API_RETURN_NO_PROVIDER;if(!header)return FFX_API_RETURN_ERROR_PARAMETER;
        auto state=find(context);
        if(state){std::lock_guard<std::mutex> lock(state->mutex);
            if(header->type==FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION&&state->renderer&&!state->disabled&&(state->status.backend==4||(!state->status.fsr4Dispatches&&!state->status.originalDispatches))){auto* q=reinterpret_cast<ffxQueryGetProviderVersion*>(header);q->versionId=wfFsr4VersionId;q->versionName=versionName;return FFX_API_RETURN_OK;}
            return original.query(context,header);}
        if(!context&&original.enabled&&header->type==FFX_API_QUERY_DESC_TYPE_GET_VERSIONS&&!header->pNext){
            auto* q=reinterpret_cast<ffxQueryDescGetVersions*>(header);
            if(q->createDescType!=FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE)return original.query(context,header);
            if(!q->outputCount)return FFX_API_RETURN_ERROR_PARAMETER;auto available=versions(q->device);
            if(available.empty())return original.query(context,header);
            for(const auto& v:available)if(v.id==wfFsr4VersionId)return FFX_API_RETURN_ERROR_PARAMETER;
            const uint64_t capacity=*q->outputCount;*q->outputCount=available.size()+1;
            if(!q->versionIds&&!q->versionNames)return FFX_API_RETURN_OK;
            if(capacity<*q->outputCount)return FFX_API_RETURN_ERROR_PARAMETER;
            if(q->versionIds)q->versionIds[0]=wfFsr4VersionId;if(q->versionNames)q->versionNames[0]=versionName;
            for(size_t i=0;i<available.size();++i){if(q->versionIds)q->versionIds[i+1]=available[i].id;if(q->versionNames)q->versionNames[i+1]=available[i].name;}return FFX_API_RETURN_OK;}
        return original.query(context,header);
    }catch(const std::bad_alloc&){return FFX_API_RETURN_ERROR_MEMORY;}catch(...){return FFX_API_RETURN_ERROR_RUNTIME_ERROR;}
}
extern "C" FFX_API_ENTRY ffxReturnCode_t ffxDispatch(ffxContext* context,const ffxDispatchDescHeader* header) {
    try {if(!initialize())return FFX_API_RETURN_NO_PROVIDER;if(original.traceEnabled)traceDispatch(context,header);auto state=find(context);if(!state)return original.dispatch(context,header);
        if(!header)return FFX_API_RETURN_ERROR_PARAMETER;std::lock_guard<std::mutex> lock(state->mutex);
        if(header->type==FFX_API_DISPATCH_DESC_TYPE_UPSCALE){const auto& request=*reinterpret_cast<const ffxDispatchDescUpscale*>(header);
            if(state->renderer&&!state->disabled){std::string why;
                if(state->renderer->dispatch(request,why)){notifyFrameGeneration(request,state->upscaleFlags);++state->status.fsr4Dispatches;state->status.backend=4;state->status.allocatedBytes=state->renderer->allocatedBytes();state->originalNeedsReset=true;reason(*state,"FSR4 dispatched");if(original.traceEnabled)traceRoute(4,state->status.fsr4Dispatches,state->status.originalDispatches,state->status.reason);return FFX_API_RETURN_OK;}
                reason(*state,why.c_str());state->renderer->invalidateHistory();}
            auto copy=request;if(state->originalNeedsReset)copy.reset=true;
            const auto rc=original.dispatch(context,&copy.header);
            if(rc==FFX_API_RETURN_OK){notifyFrameGeneration(copy,state->upscaleFlags);++state->status.originalDispatches;state->status.backend=0;state->originalNeedsReset=false;if(original.traceEnabled)traceRoute(0,state->status.fsr4Dispatches,state->status.originalDispatches,state->status.reason);}return rc;
        }
        const auto rc=original.dispatch(context,header);if(state->renderer)state->renderer->invalidateHistory();return rc;
    }catch(const std::bad_alloc&){return FFX_API_RETURN_ERROR_MEMORY;}catch(...){return FFX_API_RETURN_ERROR_RUNTIME_ERROR;}
}
extern "C" __declspec(dllexport) uint32_t wfFsr4GetContextStatus(ffxContext* context,WfFsr4Status* status) {
    try {if(!status||status->size!=sizeof(*status))return FFX_API_RETURN_ERROR_PARAMETER;auto state=find(context);if(!state)return FFX_API_RETURN_NO_PROVIDER;
        std::lock_guard<std::mutex> lock(state->mutex);*status=state->status;return FFX_API_RETURN_OK;
    }catch(...){return FFX_API_RETURN_ERROR_RUNTIME_ERROR;}
}
// WineForge-Internal: fsr4/explicit-diagnostic-history-copy-v1.
extern "C" __declspec(dllexport) uint32_t wfFsr4RecordHistoryReadback(ffxContext* context,void* commandList,void* readback,uint32_t w,uint32_t h){
    try {auto state=find(context);if(!state)return FFX_API_RETURN_NO_PROVIDER;
        std::lock_guard<std::mutex> lock(state->mutex);
        if(state->disabled||state->status.backend!=4||!state->renderer)return FFX_API_RETURN_NO_PROVIDER;
        return state->renderer->recordHistoryReadback(static_cast<ID3D12GraphicsCommandList*>(commandList),static_cast<ID3D12Resource*>(readback),w,h)?
            FFX_API_RETURN_OK:FFX_API_RETURN_ERROR_PARAMETER;
    }catch(...){return FFX_API_RETURN_ERROR_RUNTIME_ERROR;}
}
extern "C" __declspec(dllexport) uint32_t wfFsr4RecordTensorReadback(ffxContext* context,void* commandList,void* readback,uint32_t index,uint32_t w,uint32_t h){
    try {auto state=find(context);if(!state)return FFX_API_RETURN_NO_PROVIDER;
        std::lock_guard<std::mutex> lock(state->mutex);
        if(state->disabled||state->status.backend!=4||!state->renderer)return FFX_API_RETURN_NO_PROVIDER;
        return state->renderer->recordTensorReadback(static_cast<ID3D12GraphicsCommandList*>(commandList),static_cast<ID3D12Resource*>(readback),index,w,h)?
            FFX_API_RETURN_OK:FFX_API_RETURN_ERROR_PARAMETER;
    }catch(...){return FFX_API_RETURN_ERROR_RUNTIME_ERROR;}
}

#ifdef WF_FSR4_GPU_PROFILE
extern "C" __declspec(dllexport) uint32_t wfFsr4SetProfileHeap(ffxContext* context,void* heap){
    try{auto state=find(context);if(!state)return FFX_API_RETURN_NO_PROVIDER;std::lock_guard<std::mutex> lock(state->mutex);
        return state->renderer&&state->renderer->setProfileHeap(static_cast<ID3D12QueryHeap*>(heap))?FFX_API_RETURN_OK:FFX_API_RETURN_ERROR_PARAMETER;
    }catch(...){return FFX_API_RETURN_ERROR_RUNTIME_ERROR;}
}
#endif

#ifdef WF_FSR4_GPU_PROFILE
extern "C" __declspec(dllexport) uint32_t wfFsr4SetAmplification(ffxContext* context,unsigned group,unsigned count){
    try{auto state=find(context);if(!state)return FFX_API_RETURN_NO_PROVIDER;std::lock_guard<std::mutex> lock(state->mutex);
        return state->renderer&&state->renderer->setAmplification(group,count)?FFX_API_RETURN_OK:FFX_API_RETURN_ERROR_PARAMETER;
    }catch(...){return FFX_API_RETURN_ERROR_RUNTIME_ERROR;}
}
#endif

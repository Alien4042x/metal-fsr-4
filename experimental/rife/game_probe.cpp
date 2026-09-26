#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <mutex>
#include <string>
#include <atomic>
#include <map>
#include <memory>
#include <vector>
#include "../../src/vendor/fsr-sdk-2.3.0/api/include/dx12/ffx_api_dx12.h"
#include "../../src/runtime/fsr_fg_camera_input.hpp"
#include "../../src/vendor/fsr-sdk-2.3.0/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"

// Forward-only contract observation. No RIFE inference or provider spoofing.
namespace {
std::once_flag once;bool ready=false;
PfnFfxCreateContext createFn=nullptr;PfnFfxDestroyContext destroyFn=nullptr;
PfnFfxConfigure configureFn=nullptr;PfnFfxQuery queryFn=nullptr;PfnFfxDispatch dispatchFn=nullptr;
bool initialize(){
    std::call_once(once,[]{
        HMODULE self=nullptr;wchar_t path[32768]{};
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&initialize),&self))return;
        DWORD size=GetModuleFileNameW(self,path,32768);if(!size||size>=32768)return;
        std::wstring file(path,size);auto slash=file.find_last_of(L"\\/");if(slash==std::wstring::npos)return;
        // WineForge-Internal: fsr4/process-scoped-fg-observer-original-v1.
        // An external Wine builtin proxy delegates through the original Z:
        // alias; a path-specific native override keeps that load distinct.
        wchar_t originalPath[32768]{};
        const DWORD originalLength=GetEnvironmentVariableW(L"METAL_FSR4_ORIGINAL_FG",originalPath,32768);
        if(originalLength>=32768){fprintf(stderr,"FG_FORWARD_ORIGINAL invalid=too_long\n");return;}
        if(originalLength){
            const bool drivePath=originalLength>=3&&
                ((originalPath[0]>=L'A'&&originalPath[0]<=L'Z')||(originalPath[0]>=L'a'&&originalPath[0]<=L'z'))&&
                originalPath[1]==L':'&&(originalPath[2]==L'\\'||originalPath[2]==L'/');
            const bool uncPath=originalLength>=3&&originalPath[0]==L'\\'&&originalPath[1]==L'\\';
            if(!drivePath&&!uncPath){fprintf(stderr,"FG_FORWARD_ORIGINAL invalid=relative_path\n");return;}
            file.assign(originalPath,originalLength);
        }else{file.resize(slash+1);file+=L"metal-rife-amd-fg.dll";}
        auto module=LoadLibraryExW(file.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);if(!module)return;
        if(module==self){fprintf(stderr,"FG_FORWARD_ORIGINAL invalid=self\n");return;}
        createFn=reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(module,"ffxCreateContext"));destroyFn=reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(module,"ffxDestroyContext"));
        configureFn=reinterpret_cast<PfnFfxConfigure>(GetProcAddress(module,"ffxConfigure"));queryFn=reinterpret_cast<PfnFfxQuery>(GetProcAddress(module,"ffxQuery"));dispatchFn=reinterpret_cast<PfnFfxDispatch>(GetProcAddress(module,"ffxDispatch"));
        ready=createFn&&destroyFn&&configureFn&&queryFn&&dispatchFn;
        fprintf(stderr,"FG_FORWARD_ONLY ready=%u inference=disabled forwarding=AMD\n",unsigned(ready));
    });return ready;
}
void resource(const char* role,const FfxApiResource& r){
    auto *p=static_cast<ID3D12Resource*>(r.resource);if(!p){fprintf(stderr,"RIFE_RESOURCE role=%s null=1\n",role);return;}
    const auto d=p->GetDesc();fprintf(stderr,"RIFE_RESOURCE role=%s resource=%p state=%u dxgi=%u extent=%llux%u samples=%u flags=%u\n",role,p,r.state,unsigned(d.Format),(unsigned long long)d.Width,d.Height,d.SampleDesc.Count,unsigned(d.Flags));
}
struct PresentRoute { FfxApiPresentCallbackFunc callback; void* user; ffxContext context; std::atomic<unsigned> calls{0}; };
std::mutex routesMutex;
std::map<ffxContext,std::vector<std::unique_ptr<PresentRoute>>> routes;
ffxReturnCode_t present(ffxCallbackDescFrameGenerationPresent* p,void* user){
    auto& route=*static_cast<PresentRoute*>(user);unsigned n=++route.calls;
    if(p&&(n<=8||n%120==0)){
        fprintf(stderr,"RIFE_PRESENT context=%p call=%u frame=%llu generated=%u command=%p\n",route.context,n,(unsigned long long)p->frameID,unsigned(p->isGeneratedFrame),p->commandList);
        resource("callback_color",p->currentBackBuffer);resource("callback_ui",p->currentUI);resource("callback_output",p->outputSwapChainBuffer);
    }
    return route.callback(p,route.user);
}
PresentRoute* routeFor(ffxContext context,FfxApiPresentCallbackFunc callback,void* user){
    std::lock_guard<std::mutex> lock(routesMutex);auto& entries=routes[context];
    for(auto& entry:entries)if(entry->callback==callback&&entry->user==user)return entry.get();
    // Keep immutable callback pairs alive until the provider destroys its context.
    auto route=std::make_unique<PresentRoute>();route->callback=callback;route->user=user;route->context=context;
    auto* result=route.get();entries.push_back(std::move(route));return result;
}
}
extern "C" __declspec(dllexport) ffxReturnCode_t ffxCreateContext(ffxContext* c,ffxCreateContextDescHeader* d,const ffxAllocationCallbacks* a){
    if(!initialize())return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    auto rc=createFn(c,d,a);fprintf(stderr,"RIFE_CONTEXT type=%llx rc=%u context=%p\n",(unsigned long long)(d?d->type:0),rc,c?*c:nullptr);return rc;
}
extern "C" __declspec(dllexport) ffxReturnCode_t ffxDestroyContext(ffxContext* c,const ffxAllocationCallbacks* a){if(!initialize())return FFX_API_RETURN_ERROR_RUNTIME_ERROR;auto context=c?*c:nullptr;auto rc=destroyFn(c,a);if(!rc){std::lock_guard<std::mutex> lock(routesMutex);routes.erase(context);}return rc;}
extern "C" __declspec(dllexport) ffxReturnCode_t ffxQuery(ffxContext* c,ffxQueryDescHeader* d){if(!initialize())return FFX_API_RETURN_ERROR_RUNTIME_ERROR;return queryFn(c,d);}
extern "C" __declspec(dllexport) ffxReturnCode_t ffxConfigure(ffxContext* c,const ffxConfigureDescHeader* d){
    if(!initialize())return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    if(d&&d->type==FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION){
        static std::atomic<unsigned> count{0};auto n=++count;
        if(n<=8||n%120==0){auto& cfg=*reinterpret_cast<const ffxConfigureDescFrameGeneration*>(d);
            fprintf(stderr,"RIFE_CONFIG count=%u context=%p frame=%llu enabled=%u async=%u generation_callback=%p present_callback=%p swap=%p\n",n,c?*c:nullptr,(unsigned long long)cfg.frameID,unsigned(cfg.frameGenerationEnabled),unsigned(cfg.allowAsyncWorkloads),(void*)cfg.frameGenerationCallback,(void*)cfg.presentCallback,cfg.swapChain);
            resource("hudless",cfg.HUDLessColor);
        }
    }
    if(d&&d->type==FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12){
        static std::atomic<unsigned> count{0};auto n=++count;
        if(n<=8||n%120==0){auto& ui=*reinterpret_cast<const ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12*>(d);
            fprintf(stderr,"RIFE_UI_REGISTER context=%p flags=%u\n",c?*c:nullptr,ui.flags);resource("registered_ui",ui.uiResource);}
    }
    if(c&&d&&d->type==FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION){
        auto copy=*reinterpret_cast<const ffxConfigureDescFrameGeneration*>(d);
        if(copy.presentCallback){
            try{copy.presentCallbackUserContext=routeFor(*c,copy.presentCallback,copy.presentCallbackUserContext);copy.presentCallback=present;return configureFn(c,&copy.header);}
            catch(...){return FFX_API_RETURN_ERROR_RUNTIME_ERROR;}
        }
    }
    return configureFn(c,d);
}
extern "C" __declspec(dllexport) ffxReturnCode_t ffxDispatch(ffxContext* c,const ffxDispatchDescHeader* d){
    if(!initialize())return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    if(d&&d->type==FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION){
        static std::atomic<unsigned> count{0};auto n=++count;
        if(n<=8||n%120==0){auto& f=*reinterpret_cast<const ffxDispatchDescFrameGeneration*>(d);
            fprintf(stderr,"RIFE_GENERATE count=%u context=%p frame=%llu command=%p outputs=%u reset=%u transfer=%u rect=%d,%d,%d,%d\n",n,c?*c:nullptr,(unsigned long long)f.frameID,f.commandList,f.numGeneratedFrames,unsigned(f.reset),f.backbufferTransferFunction,f.generationRect.left,f.generationRect.top,f.generationRect.width,f.generationRect.height);
            resource("present",f.presentColor);if(f.numGeneratedFrames)resource("output0",f.outputs[0]);
        }
    }
    return dispatchFn(c,d);
}

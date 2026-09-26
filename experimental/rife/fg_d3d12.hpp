#pragma once
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include "pair_d3d12.hpp"

// Experimental frame-generation callback for the Direct FFX cube. It owns the
// previous HUD-less presentation color and replaces only the generation
// callback. The AMD swapchain still owns pacing and presentation.
namespace rife_d3d12 {
class FrameGeneration {
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12Resource> previous;
    std::unique_ptr<PairTransport> transport;
    UINT width=0,height=0;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    uint64_t previousProviderId=UINT64_MAX;
    uint64_t pairSerial=0;
    unsigned pairs=0,primes=0,missingClean=0;
    std::mutex callbackMutex;

    static D3D12_RESOURCE_STATES state(uint32_t f){unsigned s=0;
        if(f&FFX_API_RESOURCE_STATE_UNORDERED_ACCESS)s|=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        if(f&FFX_API_RESOURCE_STATE_COMPUTE_READ)s|=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if(f&FFX_API_RESOURCE_STATE_PIXEL_READ)s|=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        if(f&FFX_API_RESOURCE_STATE_COPY_SRC)s|=D3D12_RESOURCE_STATE_COPY_SOURCE;
        if(f&FFX_API_RESOURCE_STATE_COPY_DEST)s|=D3D12_RESOURCE_STATE_COPY_DEST;
        if(f&FFX_API_RESOURCE_STATE_RENDER_TARGET)s|=D3D12_RESOURCE_STATE_RENDER_TARGET;
        return D3D12_RESOURCE_STATES(s);
    }
    static void barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
        if(before==after)return;D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);
    }
    void initialize(ID3D12Resource* source){
        auto desc=source->GetDesc();
        need(desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&desc.DepthOrArraySize==1&&desc.MipLevels==1&&desc.SampleDesc.Count==1,"RIFE FG source shape");
        need(desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM||desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM,"RIFE FG source format");
        width=UINT(desc.Width);height=desc.Height;format=desc.Format;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        desc.Flags=D3D12_RESOURCE_FLAG_NONE;
        check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&previous)),"RIFE FG history");
        wchar_t helper[32768]{},native[32768]{};
        DWORD helperLength=GetEnvironmentVariableW(L"METAL_RIFE_HELPER",helper,32768);
        DWORD nativeLength=GetEnvironmentVariableW(L"METAL_RIFE_LIBRARY",native,32768);
        need(helperLength&&helperLength<32768&&nativeLength&&nativeLength<32768,"RIFE explicit helper and native paths");
        char scaleText[8]{};DWORD scaleLength=GetEnvironmentVariableA("RIFE_ISCALE",scaleText,sizeof(scaleText));
        unsigned scale=scaleLength?unsigned(strtoul(scaleText,nullptr,10)):4;
        need(scale==1||scale==2||scale==4||scale==8,"RIFE scale 1, 2, 4 or 8");
        transport=std::make_unique<PairTransport>(device.Get(),width,height,scale,format,helper,native);
        const unsigned internalW=width/scale,internalH=height/scale;
        const unsigned paddedW=(internalW+63u)&~63u,paddedH=(internalH+63u)&~63u;
        printf("RIFE_FG_READY format=%u input_output=%ux%u internal=%ux%u padded=%ux%u scale=%u backend=experimental\n",unsigned(format),width,height,internalW,internalH,paddedW,paddedH,scale);
    }
    void copy(ID3D12GraphicsCommandList* list,ID3D12Resource* source,D3D12_RESOURCE_STATES sourceState,ID3D12Resource* output,D3D12_RESOURCE_STATES outputState){
        barrier(list,source,sourceState,D3D12_RESOURCE_STATE_COPY_SOURCE);barrier(list,output,outputState,D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(output,source);
        barrier(list,output,D3D12_RESOURCE_STATE_COPY_DEST,outputState);barrier(list,source,D3D12_RESOURCE_STATE_COPY_SOURCE,sourceState);
    }
public:
    explicit FrameGeneration(ID3D12Device* value):device(value){need(value!=nullptr,"RIFE FG device");}
    void resetHistory(){
        std::lock_guard<std::mutex> lock(callbackMutex);
        if(transport)transport->finish();
        previousProviderId=UINT64_MAX;
        printf("RIFE_FG_HISTORY_RESET model_retained=%u output=%ux%u\n",unsigned(transport!=nullptr),width,height);
    }
    static ffxReturnCode_t callback(ffxDispatchDescFrameGeneration* params,void* user){
        try{return static_cast<FrameGeneration*>(user)->generate(params);}catch(const std::exception& e){fprintf(stderr,"RIFE_FG_FAIL reason=%s\n",e.what());return FFX_API_RETURN_ERROR_RUNTIME_ERROR;}
    }
    static ffxReturnCode_t callbackWithHudless(ffxDispatchDescFrameGeneration* params,void* user,const FfxApiResource* clean){
        try{return static_cast<FrameGeneration*>(user)->generate(params,clean,true,true);}catch(const std::exception& e){fprintf(stderr,"RIFE_FG_FAIL reason=%s\n",e.what());return FFX_API_RETURN_ERROR_RUNTIME_ERROR;}
    }
    // Native FSR/Streamline inputs already hand UI composition to the game's
    // frame-generation swapchain. Generate only the clean scene in that path;
    // the swapchain adds its registered currentUI exactly once afterwards.
    static ffxReturnCode_t callbackWithNativeHudless(ffxDispatchDescFrameGeneration* params,void* user,const FfxApiResource* clean){
        try{return static_cast<FrameGeneration*>(user)->generate(params,clean,true,false);}catch(const std::exception& e){fprintf(stderr,"RIFE_FG_FAIL reason=%s\n",e.what());return FFX_API_RETURN_ERROR_RUNTIME_ERROR;}
    }
    ffxReturnCode_t generate(ffxDispatchDescFrameGeneration* params,const FfxApiResource* clean=nullptr,bool requireClean=false,bool composeFull=true){
        std::lock_guard<std::mutex> lock(callbackMutex);
        need(params&&params->commandList&&params->presentColor.resource,"RIFE FG callback inputs");
        need(params->numGeneratedFrames==1&&params->outputs[0].resource,"RIFE FG one output");
        auto list=static_cast<ID3D12GraphicsCommandList*>(params->commandList);
        auto full=static_cast<ID3D12Resource*>(params->presentColor.resource);
        auto output=static_cast<ID3D12Resource*>(params->outputs[0].resource);
        const auto fullState=state(params->presentColor.state),outputState=state(params->outputs[0].state);
        const bool validClean=clean&&clean->resource&&clean->resource!=params->presentColor.resource;
        if(requireClean&&!validClean){
            // Do not interpolate a UI-bearing frame before the scene capture is
            // valid. The current real frame is still safe to present.
            copy(list,full,fullState,output,outputState);
            previousProviderId=UINT64_MAX;
            if(++missingClean<=8||missingClean%120==0)printf("RIFE_FG_SKIP reason=missing_hudless frame=%llu count=%u\n",(unsigned long long)params->frameID,missingClean);
            return FFX_API_RETURN_OK;
        }
        auto source=validClean?static_cast<ID3D12Resource*>(clean->resource):full;
        if(!transport)initialize(source);
        const bool recovered=transport->consumeAbandoned();
        need(source->GetDesc().Format==format&&full->GetDesc().Format==format&&output->GetDesc().Format==format,"RIFE FG stable format");
        auto sourceState=validClean?state(clean->state):fullState;
        // The swapchain provider's generation callback is not the application's
        // prepare stream. A positive frame-ID gap can be valid. Only reset or a
        // non-monotonic ID invalidates retained history. PairTransport keeps an
        // adjacent private serial because its ABI requires B == A + 1.
        const bool nonMonotonic=previousProviderId!=UINT64_MAX&&params->frameID<=previousProviderId;
        const bool prime=recovered||params->reset||previousProviderId==UINT64_MAX||nonMonotonic;
        if(prime){
            auto* safe=composeFull?full:source;
            const auto safeState=composeFull?fullState:sourceState;
            copy(list,safe,safeState,output,outputState);
            copy(list,source,sourceState,previous.Get(),D3D12_RESOURCE_STATE_COMMON);
            previousProviderId=params->frameID;++primes;
            if(primes<=8||primes%120==0)printf("RIFE_FG_PRIME provider_frame=%llu reset=%u recovered=%u nonmonotonic=%u count=%u\n",(unsigned long long)params->frameID,unsigned(params->reset),unsigned(recovered),unsigned(nonMonotonic),primes);
            return FFX_API_RETURN_OK;
        }
        const D3D12_RESOURCE_STATES states[]={D3D12_RESOURCE_STATE_COMMON,sourceState,outputState};
        const uint64_t firstSerial=pairSerial,secondSerial=pairSerial+1;
        need(transport->record(list,previous.Get(),source,output,states,firstSerial,secondSerial,
                               validClean&&composeFull?full:nullptr,fullState),"RIFE FG pair record");
        // These commands are forwarded to the continuation list created by
        // PairTransport, so history updates only after native interpolation.
        copy(list,source,sourceState,previous.Get(),D3D12_RESOURCE_STATE_COMMON);
        const uint64_t firstProviderId=previousProviderId;
        previousProviderId=params->frameID;pairSerial=secondSerial;++pairs;
        if(pairs<=4||pairs%120==0)printf("RIFE_FG_PAIR provider_first=%llu provider_second=%llu pair_first=%llu pair_second=%llu count=%u\n",(unsigned long long)firstProviderId,(unsigned long long)params->frameID,(unsigned long long)firstSerial,(unsigned long long)secondSerial,pairs);
        return FFX_API_RETURN_OK;
    }
};
}

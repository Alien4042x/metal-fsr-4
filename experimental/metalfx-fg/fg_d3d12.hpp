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
namespace metalfx_d3d12 {
class FrameGeneration {
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12Resource> previous;
    Microsoft::WRL::ComPtr<ID3D12Resource> depthSource,motionSource;
    std::unique_ptr<PairTransport> transport;
    UINT width=0,height=0;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    uint64_t previousProviderId=UINT64_MAX;
    uint64_t latestPreparedId=UINT64_MAX;
    unsigned geometryMismatch=0;
    uint64_t pairSerial=0;
    float deltaSeconds=1.f/30.f;
    float cameraNear=.1f,cameraFar=100.f,cameraFovRadians=2.f*atanf(1.f/1.8f);
    float motionScaleX=0.f,motionScaleY=0.f;
    bool gameCamera=false;
    D3D12_RESOURCE_STATES depthState=D3D12_RESOURCE_STATE_DEPTH_WRITE;
    D3D12_RESOURCE_STATES motionState=D3D12_RESOURCE_STATE_RENDER_TARGET;
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
        need(desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&desc.DepthOrArraySize==1&&desc.MipLevels==1&&desc.SampleDesc.Count==1,"METALFX FG source shape");
        need(desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM||desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM,"METALFX FG source format");
        width=UINT(desc.Width);height=desc.Height;format=desc.Format;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        desc.Flags=D3D12_RESOURCE_FLAG_NONE;
        check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&previous)),"METALFX FG history");
        need(depthSource&&motionSource,"METALFX geometry inputs");
        const auto zd=depthSource->GetDesc(),md=motionSource->GetDesc();
        need(zd.Width==md.Width&&zd.Height==md.Height&&
             zd.Format==DXGI_FORMAT_R24G8_TYPELESS&&md.Format==DXGI_FORMAT_R16G16_FLOAT,
             "METALFX cube depth/motion format");
        wchar_t helper[32768]{},native[32768]{};
        DWORD helperLength=GetEnvironmentVariableW(L"METAL_FX_PAIR_HELPER",helper,32768);
        DWORD nativeLength=GetEnvironmentVariableW(L"METAL_FX_PAIR_LIBRARY",native,32768);
        need(helperLength&&helperLength<32768&&nativeLength&&nativeLength<32768,"METALFX explicit helper and native paths");
        transport=std::make_unique<PairTransport>(device.Get(),width,height,unsigned(zd.Width),zd.Height,format,helper,native);
        printf("METALFX_FG_READY format=%u output=%ux%u render=%llux%u backend=experimental scheduler=amd_3.1.6\n",
               unsigned(format),width,height,(unsigned long long)zd.Width,zd.Height);
    }
    void copy(ID3D12GraphicsCommandList* list,ID3D12Resource* source,D3D12_RESOURCE_STATES sourceState,ID3D12Resource* output,D3D12_RESOURCE_STATES outputState){
        barrier(list,source,sourceState,D3D12_RESOURCE_STATE_COPY_SOURCE);barrier(list,output,outputState,D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(output,source);
        barrier(list,output,D3D12_RESOURCE_STATE_COPY_DEST,outputState);barrier(list,source,D3D12_RESOURCE_STATE_COPY_SOURCE,sourceState);
    }
public:
    explicit FrameGeneration(ID3D12Device* value):device(value){need(value!=nullptr,"METALFX FG device");}
    void setGeometry(ID3D12Resource* depth,ID3D12Resource* motion,float dt,
                     D3D12_RESOURCE_STATES depthBefore=D3D12_RESOURCE_STATE_DEPTH_WRITE,
                     D3D12_RESOURCE_STATES motionBefore=D3D12_RESOURCE_STATE_RENDER_TARGET){
        std::lock_guard<std::mutex> lock(callbackMutex);
        // FidelityFX Prepare uses milliseconds; MetalFX deltaTime uses seconds.
        depthSource=depth;motionSource=motion;deltaSeconds=dt*.001f;
        depthState=depthBefore;motionState=motionBefore;
    }
    void setGeometry(const FfxApiResource& depth,const FfxApiResource& motion,float dt){
        setGeometry(static_cast<ID3D12Resource*>(depth.resource),
                    static_cast<ID3D12Resource*>(motion.resource),dt,
                    state(depth.state),state(motion.state));
    }
    void setCamera(float nearPlane,float farPlane,float fovRadians,float motionX,float motionY){
        std::lock_guard<std::mutex> lock(callbackMutex);
        cameraNear=nearPlane;cameraFar=farPlane;cameraFovRadians=fovRadians;
        motionScaleX=motionX;motionScaleY=motionY;gameCamera=true;
    }
    void notePreparedFrame(uint64_t frameId){
        std::lock_guard<std::mutex> lock(callbackMutex);
        latestPreparedId=frameId;
    }
    static ffxReturnCode_t callback(ffxDispatchDescFrameGeneration* params,void* user){
        try{return static_cast<FrameGeneration*>(user)->generate(params);}catch(const std::exception& e){fprintf(stderr,"METALFX_FG_FAIL reason=%s\n",e.what());return FFX_API_RETURN_ERROR_RUNTIME_ERROR;}
    }
    static ffxReturnCode_t callbackWithHudless(ffxDispatchDescFrameGeneration* params,void* user,const FfxApiResource* clean){
        try{return static_cast<FrameGeneration*>(user)->generate(params,clean,true);}catch(const std::exception& e){fprintf(stderr,"METALFX_FG_FAIL reason=%s\n",e.what());return FFX_API_RETURN_ERROR_RUNTIME_ERROR;}
    }
    ffxReturnCode_t generate(ffxDispatchDescFrameGeneration* params,const FfxApiResource* clean=nullptr,bool requireClean=false){
        std::lock_guard<std::mutex> lock(callbackMutex);
        need(params&&params->commandList&&params->presentColor.resource,"METALFX FG callback inputs");
        need(params->numGeneratedFrames==1&&params->outputs[0].resource,"METALFX FG one output");
        auto list=static_cast<ID3D12GraphicsCommandList*>(params->commandList);
        auto full=static_cast<ID3D12Resource*>(params->presentColor.resource);
        auto output=static_cast<ID3D12Resource*>(params->outputs[0].resource);
        if(latestPreparedId!=UINT64_MAX&&latestPreparedId!=params->frameID){
            if(++geometryMismatch<=8||geometryMismatch%120==0)
                fprintf(stderr,"METALFX_GEOMETRY_ALIGNMENT callback_frame=%llu latest_prepare=%llu mismatch_count=%u\n",
                        (unsigned long long)params->frameID,
                        (unsigned long long)latestPreparedId,geometryMismatch);
        }
        const auto fullState=state(params->presentColor.state),outputState=state(params->outputs[0].state);
        const bool validClean=clean&&clean->resource&&clean->resource!=params->presentColor.resource;
        if(requireClean&&!validClean){
            // Do not interpolate a UI-bearing frame before the scene capture is
            // valid. The current real frame is still safe to present.
            copy(list,full,fullState,output,outputState);
            previousProviderId=UINT64_MAX;
            if(++missingClean<=8||missingClean%120==0)printf("METALFX_FG_SKIP reason=missing_hudless frame=%llu count=%u\n",(unsigned long long)params->frameID,missingClean);
            return FFX_API_RETURN_OK;
        }
        auto source=validClean?static_cast<ID3D12Resource*>(clean->resource):full;
        if(!transport)initialize(source);
        const bool recovered=transport->consumeAbandoned();
        need(source->GetDesc().Format==format&&full->GetDesc().Format==format&&output->GetDesc().Format==format,"METALFX FG stable format");
        auto sourceState=validClean?state(clean->state):fullState;
        // The swapchain provider's generation callback is not the application's
        // prepare stream. A positive frame-ID gap can be valid. Only reset or a
        // non-monotonic ID invalidates retained history. PairTransport keeps an
        // adjacent private serial because its ABI requires B == A + 1.
        const bool nonMonotonic=previousProviderId!=UINT64_MAX&&params->frameID<=previousProviderId;
        const bool prime=recovered||params->reset||previousProviderId==UINT64_MAX||nonMonotonic;
        if(prime){
            copy(list,full,fullState,output,outputState);
            copy(list,source,sourceState,previous.Get(),D3D12_RESOURCE_STATE_COMMON);
            previousProviderId=params->frameID;++primes;
            if(primes<=8||primes%120==0)printf("METALFX_FG_PRIME provider_frame=%llu reset=%u recovered=%u nonmonotonic=%u count=%u\n",(unsigned long long)params->frameID,unsigned(params->reset),unsigned(recovered),unsigned(nonMonotonic),primes);
            return FFX_API_RETURN_OK;
        }
        need(depthSource&&motionSource,"METALFX missing cube geometry");
        if(gameCamera){
            const auto render=depthSource->GetDesc();
            // FFX scales raw vectors into render pixels. MetalFX samples the
            // upscaled color, so convert that displacement to output pixels.
            transport->setCamera(cameraNear,cameraFar,cameraFovRadians,
                                 motionScaleX*float(width)/float(render.Width),
                                 motionScaleY*float(height)/float(render.Height));
        }
        const D3D12_RESOURCE_STATES states[]={D3D12_RESOURCE_STATE_COMMON,sourceState,outputState,
                                               depthState,motionState};
        const uint64_t firstSerial=pairSerial,secondSerial=pairSerial+1;
        need(transport->record(list,previous.Get(),source,output,depthSource.Get(),motionSource.Get(),
                               states,firstSerial,secondSerial,deltaSeconds,
                               validClean?full:nullptr,fullState),"METALFX FG pair record");
        // These commands are forwarded to the continuation list created by
        // PairTransport, so history updates only after native interpolation.
        copy(list,source,sourceState,previous.Get(),D3D12_RESOURCE_STATE_COMMON);
        const uint64_t firstProviderId=previousProviderId;
        previousProviderId=params->frameID;pairSerial=secondSerial;++pairs;
        if(pairs<=4||pairs%120==0)printf("METALFX_FG_PAIR provider_first=%llu provider_second=%llu pair_first=%llu pair_second=%llu count=%u\n",(unsigned long long)firstProviderId,(unsigned long long)params->frameID,(unsigned long long)firstSerial,(unsigned long long)secondSerial,pairs);
        return FFX_API_RETURN_OK;
    }
};
}

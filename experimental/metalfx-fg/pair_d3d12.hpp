#pragma once
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <functional>
#include <array>
#include <vector>
#include <mutex>
#include <stdexcept>
#include "pair_abi.h"

// Cube-only recording transport. DllNativeSplit synchronizes D3D12 readback,
// native MetalFX interpolation, and the upload into AMD's FG output texture.
namespace metalfx_d3d12 {
using Microsoft::WRL::ComPtr;
inline void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
inline void check(HRESULT hr,const char* why){need(SUCCEEDED(hr),why);}
#include "../../src/bridge/dll_native_split.hpp"
class PairTransport {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Resource> readback[5],upload,retained[6];
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[3]{};
    UINT64 sizes[3]{};
    HMODULE helper=nullptr;
    using Call=int(WINAPI*)(const wchar_t*,MetalFxPairArgs*);
    Call call=nullptr;
    MetalFxPairArgs args{};
    std::unique_ptr<DllNativeSplit> split;
    void** queueTable=nullptr;
    void* originalExecute=nullptr;
    bool ownsQueueObserver=false;
    unsigned rejected=0;
    unsigned completedPairs=0;
    unsigned changedPairs=0;
    double totalMilliseconds=0.0,maxMilliseconds=0.0;
    bool reject(const char* reason,ID3D12GraphicsCommandList* list){
        if(++rejected<=16||rejected%120==0)fprintf(stderr,"METALFX_RECORD_REJECT reason=%s list=%p active_split=%p active_head=%p count=%u\n",reason,static_cast<void*>(list),static_cast<void*>(DllNativeSplit::current),DllNativeSplit::current?static_cast<void*>(DllNativeSplit::current->head):nullptr,rejected);
        return false;
    }
    static inline PairTransport* owner=nullptr;
    void setExecute(void* fn){
        DWORD protection=0,unused=0;
        need(VirtualProtect(queueTable+10,sizeof(void*),PAGE_READWRITE,&protection)!=0,"METALFX queue access");
        queueTable[10]=fn;
        need(VirtualProtect(queueTable+10,sizeof(void*),protection,&unused)!=0,"METALFX queue protection");
    }
    ComPtr<ID3D12Resource> buffer(D3D12_HEAP_TYPE type,UINT64 size){
        D3D12_HEAP_PROPERTIES h{};h.Type=type;h.CreationNodeMask=h.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=size;
        d.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> out;
        check(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&out)),"METALFX staging buffer");return out;
    }
    static void barrier(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
        if(from==to)return;
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};l->ResourceBarrier(1,&b);
    }
public:
    PairTransport(ID3D12Device* d,unsigned w,unsigned h,unsigned rw,unsigned rh,DXGI_FORMAT format,const wchar_t* pe,const wchar_t* native):device(d){
        need(!owner&&!DllNativeSplit::current,"METALFX transport already active");
        need(w>=64&&h>=64&&w<=3840&&h<=2160&&rw>=16&&rh>=16&&rw<=w&&rh<=h,"METALFX extent");
        need(format==DXGI_FORMAT_B8G8R8A8_UNORM||format==DXGI_FORMAT_R8G8B8A8_UNORM,"METALFX format");
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=w;desc.Height=h;
        desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Format=format;
        device->GetCopyableFootprints(&desc,0,1,0,&footprints[0],nullptr,nullptr,&sizes[0]);
        sizes[0]=UINT64(footprints[0].Footprint.RowPitch)*h;
        desc.Width=rw;desc.Height=rh;desc.Format=DXGI_FORMAT_R24G8_TYPELESS;
        device->GetCopyableFootprints(&desc,0,1,0,&footprints[1],nullptr,nullptr,&sizes[1]);
        sizes[1]=UINT64(footprints[1].Footprint.RowPitch)*rh;
        desc.Format=DXGI_FORMAT_R16G16_FLOAT;
        device->GetCopyableFootprints(&desc,0,1,0,&footprints[2],nullptr,nullptr,&sizes[2]);
        sizes[2]=UINT64(footprints[2].Footprint.RowPitch)*rh;
        for(unsigned i=0;i<2;++i)readback[i]=buffer(D3D12_HEAP_TYPE_READBACK,sizes[0]);
        readback[3]=buffer(D3D12_HEAP_TYPE_READBACK,sizes[1]);
        readback[4]=buffer(D3D12_HEAP_TYPE_READBACK,sizes[2]);
        upload=buffer(D3D12_HEAP_TYPE_UPLOAD,sizes[0]);
        helper=LoadLibraryW(pe);need(helper!=nullptr,"METALFX helper load");
        try {
            call=reinterpret_cast<Call>(GetProcAddress(helper,"MetalFxPair"));need(call!=nullptr,"METALFX helper entry");
            args.version=1;args.size=sizeof(args);args.width=w;args.height=h;
            args.renderWidth=rw;args.renderHeight=rh;
            args.format=format==DXGI_FORMAT_R8G8B8A8_UNORM?1:2;
            args.nearPlane=.1f;args.farPlane=100.f;
            args.fieldOfViewDegrees=2.f*atanf(1.f/1.8f)*180.f/3.14159265f;
            args.motionScaleX=float(w);args.motionScaleY=float(h);
            need(call(native,&args)==0&&args.completed,"METALFX native init");
            if(!DllNativeSplit::queueOriginal){
                ComPtr<ID3D12CommandQueue> probe;D3D12_COMMAND_QUEUE_DESC q{};
                check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&probe)),"METALFX queue discovery");
                queueTable=*reinterpret_cast<void***>(probe.Get());originalExecute=queueTable[10];
                DllNativeSplit::queueOriginal=reinterpret_cast<DllNativeSplit::Execute>(originalExecute);
                setExecute(reinterpret_cast<void*>(&DllNativeSplit::execute));ownsQueueObserver=true;
            }
            owner=this;
        }catch(...){
            if(ownsQueueObserver&&queueTable&&originalExecute){setExecute(originalExecute);DllNativeSplit::queueOriginal=nullptr;}
            if(args.context){args.op=2;call(nullptr,&args);}FreeLibrary(helper);helper=nullptr;throw;
        }
    }
    PairTransport(const PairTransport&)=delete;
    PairTransport& operator=(const PairTransport&)=delete;
    void setCamera(float nearPlane,float farPlane,float fieldOfViewRadians,
                   float motionScaleX,float motionScaleY){
        need(std::isfinite(nearPlane)&&nearPlane>0&&std::isfinite(farPlane)&&farPlane>nearPlane&&
             std::isfinite(fieldOfViewRadians)&&fieldOfViewRadians>0&&fieldOfViewRadians<3.14159265f&&
             std::isfinite(motionScaleX)&&std::isfinite(motionScaleY)&&
             motionScaleX!=0&&motionScaleY!=0,"METALFX camera contract");
        args.nearPlane=nearPlane;args.farPlane=farPlane;
        args.fieldOfViewDegrees=fieldOfViewRadians*180.f/3.14159265f;
        args.motionScaleX=motionScaleX;args.motionScaleY=motionScaleY;
    }
    void finish(){
        if(!split)return;
        if(!split->submitted){puts("METALFX_SPLIT_DROP reason=destroyed_before_submit (profile-switch teardown path)");split.reset();for(auto& r:retained)r.Reset();return;}
        if(split->abandoned){split.reset();for(auto& r:retained)r.Reset();return;}
        split->completion();split.reset();for(auto& r:retained)r.Reset();
    }
    bool consumeAbandoned(){
        if(!split||!split->abandoned)return false;
        finish();
        puts("METALFX_SPLIT_RECOVER reason=provider_reset_unsubmitted_list");
        return true;
    }
    ~PairTransport(){
        // Graceful teardown (was std::terminate on any exception): a profile
        // switch can destroy the FG context with a recording pending; the game
        // must stay alive. Each stage is isolated so one failure cannot skip
        // the rest of the cleanup.
        try{finish();}catch(...){fprintf(stderr,"METALFX_TRANSPORT_TEARDOWN_DEGRADED stage=finish (split orphaned, process continues)\n");}
        if(completedPairs)printf("METALFX_D3D12_SUMMARY pairs=%u changed_pairs=%u copied_pairs=%u mean_gpu_ms=%.3f max_gpu_ms=%.3f rejected=%u\n",
                                 completedPairs,changedPairs,completedPairs-changedPairs,
                                 totalMilliseconds/double(completedPairs),maxMilliseconds,rejected);
        try{if(ownsQueueObserver&&queueTable){setExecute(originalExecute);DllNativeSplit::queueOriginal=nullptr;}}catch(...){fprintf(stderr,"METALFX_TRANSPORT_TEARDOWN_DEGRADED stage=queue_restore\n");}
        try{if(args.context){args.op=2;call(nullptr,&args);}if(helper)FreeLibrary(helper);}catch(...){fprintf(stderr,"METALFX_TRANSPORT_TEARDOWN_DEGRADED stage=native_close\n");}
        owner=nullptr;
    }
    bool record(ID3D12GraphicsCommandList* list,ID3D12Resource* a,ID3D12Resource* b,ID3D12Resource* out,
                ID3D12Resource* depth,ID3D12Resource* motion,
                const D3D12_RESOURCE_STATES states[5],uint64_t frameA,uint64_t frameB,float dt,
                ID3D12Resource* full=nullptr,D3D12_RESOURCE_STATES fullState=D3D12_RESOURCE_STATE_COMMON){
        if(!list||!a||!b||!out||!depth||!motion)return reject("null_input",list);
        if(a==b||a==out||b==out||full==a||full==b||full==out)return reject("resource_alias",list);
        if(frameA==UINT64_MAX||frameB!=frameA+1)return reject("non_adjacent_pair_serial",list);
        const auto type=list->GetType();
        if(type!=D3D12_COMMAND_LIST_TYPE_DIRECT&&type!=D3D12_COMMAND_LIST_TYPE_COMPUTE)return reject("command_list_type",list);
        if(DllNativeSplit::current)return reject("active_split",list);
        ID3D12Resource* resources[]={a,b,out,full,depth,motion};
        const auto colorFormat=args.format==1?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_B8G8R8A8_UNORM;
        for(unsigned i=0;i<6;++i){
            auto* r=resources[i];if(!r)continue;
            const auto d=r->GetDesc();ComPtr<ID3D12Device> rd;
            const auto expected=i==4?DXGI_FORMAT_R24G8_TYPELESS:i==5?DXGI_FORMAT_R16G16_FLOAT:colorFormat;
            const unsigned w=i<4?args.width:args.renderWidth,h=i<4?args.height:args.renderHeight;
            if(FAILED(r->GetDevice(IID_PPV_ARGS(&rd)))||rd.Get()!=device.Get()||
               d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.Width!=w||d.Height!=h||
               d.DepthOrArraySize!=1||d.MipLevels!=1||d.SampleDesc.Count!=1||d.Format!=expected)
                return reject("resource_contract",list);
        }
        finish();args.frameA=frameA;args.frameB=frameB;args.deltaSeconds=dt;
        if(full&&!readback[2])readback[2]=buffer(D3D12_HEAP_TYPE_READBACK,sizes[0]);
        for(unsigned i=0;i<6;++i)retained[i]=resources[i];
        // A provider reset may abandon the continuation list. The head list
        // always writes a safe current frame before attempting interpolation.
        barrier(list,out,states[2],D3D12_RESOURCE_STATE_COPY_DEST);
        auto* safe=full?full:b;const auto safeState=full?fullState:states[1];
        barrier(list,safe,safeState,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION safeFrom{},safeTo{};
        safeFrom.pResource=safe;safeFrom.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        safeTo.pResource=out;safeTo.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&safeTo,0,0,0,&safeFrom,nullptr);
        barrier(list,safe,D3D12_RESOURCE_STATE_COPY_SOURCE,safeState);
        barrier(list,out,D3D12_RESOURCE_STATE_COPY_DEST,states[2]);
        const unsigned indexes[]={0,1,3,4};
        for(unsigned i:indexes){
            auto* source=resources[i==3?4:i==4?5:i];
            const auto before=i==3?states[3]:i==4?states[4]:states[i];
            const auto fp=i==3?1u:i==4?2u:0u;
            barrier(list,source,before,D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION from{},to{};
            from.pResource=source;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource=readback[i].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint=footprints[fp];
            list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            barrier(list,source,D3D12_RESOURCE_STATE_COPY_SOURCE,before);
        }
        if(full){
            barrier(list,full,fullState,D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION from{},to{};
            from.pResource=full;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource=readback[2].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint=footprints[0];
            list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            barrier(list,full,D3D12_RESOURCE_STATE_COPY_SOURCE,fullState);
        }
        split=std::make_unique<DllNativeSplit>(device.Get(),list,[this](){
            void* mapped[6]{};D3D12_RANGE empty{};
            for(unsigned i=0;i<5;++i){
                if(i==2&&!retained[3])continue;
                const unsigned slot=i<3?0:i==3?1:2;
                D3D12_RANGE read{0,SIZE_T(sizes[slot])};
                check(readback[i]->Map(0,&read,&mapped[i]),"METALFX readback map");
            }
            check(upload->Map(0,&empty,&mapped[5]),"METALFX upload map");
            args.op=1;args.previous=uintptr_t(mapped[0]);args.current=uintptr_t(mapped[1]);
            args.full=uintptr_t(mapped[2]);args.depth=uintptr_t(mapped[3]);
            args.motion=uintptr_t(mapped[4]);args.output=uintptr_t(mapped[5]);
            args.colorBytes=sizes[0];args.depthBytes=sizes[1];args.motionBytes=sizes[2];
            args.colorPitch=footprints[0].Footprint.RowPitch;args.fullPitch=mapped[2]?args.colorPitch:0;
            args.depthPitch=footprints[1].Footprint.RowPitch;args.motionPitch=footprints[2].Footprint.RowPitch;
            const int rc=call(nullptr,&args);
            for(unsigned i=0;i<5;++i)if(mapped[i])readback[i]->Unmap(0,&empty);
            D3D12_RANGE write{0,SIZE_T(sizes[0])};upload->Unmap(0,&write);
            need(rc==0&&args.completed,"METALFX native interpolation");
            ++completedPairs;totalMilliseconds+=args.gpuMilliseconds;
            changedPairs+=args.changedPixels>0;
            maxMilliseconds=std::max(maxMilliseconds,args.gpuMilliseconds);
            if(completedPairs<=4||completedPairs%120==0)
                printf("METALFX_D3D12_PAIR first=%llu second=%llu gpu_ms=%.3f mean_gpu_ms=%.3f changed_samples=%llu count=%u\n",
                       (unsigned long long)args.frameA,(unsigned long long)args.frameB,
                       args.gpuMilliseconds,totalMilliseconds/completedPairs,
                       (unsigned long long)args.changedPixels,completedPairs);
        });
        barrier(list,out,states[2],D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};
        from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint=footprints[0];
        to.pResource=out;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        barrier(list,out,D3D12_RESOURCE_STATE_COPY_DEST,states[2]);return true;
    }
};
}

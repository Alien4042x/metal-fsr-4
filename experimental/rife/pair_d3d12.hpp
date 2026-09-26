#pragma once
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <functional>
#include <array>
#include <vector>
#include <mutex>
#include <stdexcept>
#include "pair_abi.h"
#include "gpu_ui_compositor.hpp"

// Experimental single-recording transport. Caller must supply a HUD-free pair.
// No DXGI interception, presentation, UI extraction or frame-history ownership.
namespace rife_d3d12 {
using Microsoft::WRL::ComPtr;
inline void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
inline void check(HRESULT hr,const char* why){need(SUCCEEDED(hr),why);}
#include "../../src/bridge/dll_native_split.hpp"
class PairTransport {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Resource> readback[2],upload,retained[4];
    std::unique_ptr<GpuUiCompositor> uiCompositor;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes=0;
    HMODULE helper=nullptr;
    using Call=int(WINAPI*)(const wchar_t*,RifePairArgs*);
    Call call=nullptr;
    RifePairArgs args{};
    std::unique_ptr<DllNativeSplit> split;
    void** queueTable=nullptr;
    void* originalExecute=nullptr;
    bool ownsQueueObserver=false;
    unsigned rejected=0;
    unsigned completedPairs=0;
    double totalMilliseconds=0.0,maxMilliseconds=0.0;
    bool reject(const char* reason,ID3D12GraphicsCommandList* list){
        if(++rejected<=16||rejected%120==0)fprintf(stderr,"RIFE_RECORD_REJECT reason=%s list=%p active_split=%p active_head=%p count=%u\n",reason,static_cast<void*>(list),static_cast<void*>(DllNativeSplit::current),DllNativeSplit::current?static_cast<void*>(DllNativeSplit::current->head):nullptr,rejected);
        return false;
    }
    static inline PairTransport* owner=nullptr;
    void setExecute(void* fn){
        DWORD protection=0,unused=0;
        need(VirtualProtect(queueTable+10,sizeof(void*),PAGE_READWRITE,&protection)!=0,"RIFE queue access");
        queueTable[10]=fn;
        need(VirtualProtect(queueTable+10,sizeof(void*),protection,&unused)!=0,"RIFE queue protection");
    }
    ComPtr<ID3D12Resource> buffer(D3D12_HEAP_TYPE type){
        D3D12_HEAP_PROPERTIES h{};h.Type=type;h.CreationNodeMask=h.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;
        d.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> out;
        check(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&out)),"RIFE staging buffer");return out;
    }
    static void barrier(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
        if(from==to)return;
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};l->ResourceBarrier(1,&b);
    }
public:
    PairTransport(ID3D12Device* d,unsigned w,unsigned h,unsigned scale,DXGI_FORMAT format,const wchar_t* pe,const wchar_t* native):device(d){
        need(!owner&&!DllNativeSplit::current,"RIFE transport already active");
        need(w>=64&&h>=64&&w<=3840&&h<=2160,"RIFE extent");
        need(format==DXGI_FORMAT_B8G8R8A8_UNORM||format==DXGI_FORMAT_R8G8B8A8_UNORM,"RIFE format");
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=w;desc.Height=h;
        desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Format=format;
        device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
        bytes=UINT64(footprint.Footprint.RowPitch)*h;
        readback[0]=buffer(D3D12_HEAP_TYPE_READBACK);readback[1]=buffer(D3D12_HEAP_TYPE_READBACK);upload=buffer(D3D12_HEAP_TYPE_UPLOAD);
        helper=LoadLibraryW(pe);need(helper!=nullptr,"RIFE helper load");
        try {
            call=reinterpret_cast<Call>(GetProcAddress(helper,"MetalRifePair"));need(call!=nullptr,"RIFE helper entry");
            args.version=2;args.size=sizeof(args);
            args.width=w;args.height=h;args.scale=scale;args.format=format==DXGI_FORMAT_R8G8B8A8_UNORM?RIFE_PAIR_RGBA8:RIFE_PAIR_BGRA8;
            need(call(native,&args)==0&&args.completed,"RIFE native init");
            if(!DllNativeSplit::queueOriginal){
                ComPtr<ID3D12CommandQueue> probe;D3D12_COMMAND_QUEUE_DESC q{};
                check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&probe)),"RIFE queue discovery");
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
    void finish(){
        if(!split)return;
        if(!split->submitted){puts("RIFE_SPLIT_DROP reason=destroyed_before_submit (profile-switch teardown path)");split.reset();for(auto& r:retained)r.Reset();return;}
        if(split->abandoned){split.reset();for(auto& r:retained)r.Reset();return;}
        split->completion();split.reset();for(auto& r:retained)r.Reset();
    }
    bool consumeAbandoned(){
        if(!split||!split->abandoned)return false;
        finish();
        puts("RIFE_SPLIT_RECOVER reason=provider_reset_unsubmitted_list");
        return true;
    }
    ~PairTransport(){
        // Graceful teardown (was std::terminate on any exception): a profile
        // switch can destroy the FG context with a recording pending; the game
        // must stay alive. Each stage is isolated so one failure cannot skip
        // the rest of the cleanup.
        try{finish();}catch(...){fprintf(stderr,"RIFE_TRANSPORT_TEARDOWN_DEGRADED stage=finish (split orphaned, process continues)\n");}
        if(completedPairs)printf("RIFE_D3D12_SUMMARY pairs=%u mean_ms=%.3f max_ms=%.3f rejected=%u\n",completedPairs,totalMilliseconds/double(completedPairs),maxMilliseconds,rejected);
        try{if(ownsQueueObserver&&queueTable){setExecute(originalExecute);DllNativeSplit::queueOriginal=nullptr;}}catch(...){fprintf(stderr,"RIFE_TRANSPORT_TEARDOWN_DEGRADED stage=queue_restore\n");}
        try{if(args.context){args.op=2;call(nullptr,&args);}if(helper)FreeLibrary(helper);}catch(...){fprintf(stderr,"RIFE_TRANSPORT_TEARDOWN_DEGRADED stage=native_close\n");}
        owner=nullptr;
    }
    bool record(ID3D12GraphicsCommandList* list,ID3D12Resource* a,ID3D12Resource* b,ID3D12Resource* out,
                const D3D12_RESOURCE_STATES states[3],uint64_t frameA,uint64_t frameB,
                ID3D12Resource* full=nullptr,D3D12_RESOURCE_STATES fullState=D3D12_RESOURCE_STATE_COMMON){
        if(!list||!a||!b||!out)return reject("null_input",list);
        if(a==b||a==out||b==out||full==a||full==b||full==out)return reject("resource_alias",list);
        if(frameA==UINT64_MAX||frameB!=frameA+1)return reject("non_adjacent_pair_serial",list);
        const auto listType=list->GetType();
        if(listType!=D3D12_COMMAND_LIST_TYPE_DIRECT&&listType!=D3D12_COMMAND_LIST_TYPE_COMPUTE)return reject("unsupported_command_list",list);
        if(DllNativeSplit::current)return reject(DllNativeSplit::current->head==list?"active_split_same_list":"active_split_other_list",list);
        ID3D12Resource* resources[]={a,b,out,full};
        for(auto* r:resources){if(!r)continue;auto d=r->GetDesc();ComPtr<ID3D12Device> rd;const auto expected=args.format==RIFE_PAIR_RGBA8?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_B8G8R8A8_UNORM;
            if(FAILED(r->GetDevice(IID_PPV_ARGS(&rd)))||rd.Get()!=device.Get()||d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.Width!=args.width||d.Height!=args.height||d.DepthOrArraySize!=1||d.MipLevels!=1||d.SampleDesc.Count!=1||d.Format!=expected)return reject("resource_contract",list);}
        finish();args.frameA=frameA;args.frameB=frameB;
        if(full&&!uiCompositor)uiCompositor=std::make_unique<GpuUiCompositor>(device.Get(),b->GetDesc(),footprint,bytes);
        for(unsigned i=0;i<4;++i)retained[i]=resources[i];
        // Fail-safe output: stamp the current real frame into `out` on the
        // tracked head list, which always executes. If the provider resets the
        // list before Execute and the continuation tail (native upload copy) is
        // abandoned, the provider still presents `out`; without this copy it
        // would hold a stale buffer (single-frame flicker). The tail's copy
        // overwrites this with the interpolated frame when everything completes.
        barrier(list,out,states[2],D3D12_RESOURCE_STATE_COPY_DEST);
        auto* safe=full?full:b;
        const auto safeState=full?fullState:states[1];
        barrier(list,safe,safeState,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION safeFrom{},safeTo{};
        safeFrom.pResource=safe;safeFrom.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        safeTo.pResource=out;safeTo.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&safeTo,0,0,0,&safeFrom,nullptr);
        barrier(list,safe,D3D12_RESOURCE_STATE_COPY_SOURCE,safeState);
        barrier(list,out,D3D12_RESOURCE_STATE_COPY_DEST,states[2]);
        for(unsigned i=0;i<2;++i){
            barrier(list,resources[i],states[i],D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=resources[i];from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource=readback[i].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
            list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            barrier(list,resources[i],D3D12_RESOURCE_STATE_COPY_SOURCE,states[i]);
        }
        split=std::make_unique<DllNativeSplit>(device.Get(),list,[this](){
            void* mapped[3]{};D3D12_RANGE empty{},read{0,SIZE_T(bytes)};
            check(readback[0]->Map(0,&read,&mapped[0]),"RIFE first map");
            check(readback[1]->Map(0,&read,&mapped[1]),"RIFE second map");
            check(upload->Map(0,&empty,&mapped[2]),"RIFE output map");
            args.op=1;args.inputA=uintptr_t(mapped[0]);args.inputB=uintptr_t(mapped[1]);args.output=uintptr_t(mapped[2]);
            args.bytesA=args.bytesB=args.bytesOut=bytes;args.pitchA=args.pitchB=args.pitchOut=footprint.Footprint.RowPitch;
            args.inputFull=0;args.bytesFull=0;args.pitchFull=0;
            int rc=call(nullptr,&args);
            readback[0]->Unmap(0,&empty);readback[1]->Unmap(0,&empty);
            upload->Unmap(0,&read);
            need(rc==0&&args.completed,"RIFE inference failed before output submission");
            ++completedPairs;totalMilliseconds+=args.milliseconds;maxMilliseconds=std::max(maxMilliseconds,args.milliseconds);
            if(completedPairs<=4||completedPairs%120==0)printf("RIFE_D3D12_PAIR first=%llu second=%llu completed=1 ms=%.3f mean_ms=%.3f max_ms=%.3f count=%u\n",(unsigned long long)args.frameA,(unsigned long long)args.frameB,args.milliseconds,totalMilliseconds/double(completedPairs),maxMilliseconds,completedPairs);
        },false);
        if(full)uiCompositor->record(list,upload.Get(),b,states[1],full,fullState,out,states[2]);
        else{
            barrier(list,out,states[2],D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=footprint;
            to.pResource=out;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            barrier(list,out,D3D12_RESOURCE_STATE_COPY_DEST,states[2]);
        }
        return true;
    }
};
}

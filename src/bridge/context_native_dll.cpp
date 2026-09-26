#include "fsr4_d3d12_context.hpp"
#include "lab_native_frame_callback.hpp"
#include "fsr_d3d12_texture_views.hpp"
#include "../vendor/fsr-sdk-2.3.0/api/include/dx12/ffx_api_dx12.h"
#include <array>
#include <cmath>
#include <stdexcept>
#include "dll_native_transport.hpp"
#include "fsr4_model_presets.hpp"
namespace {
template<class T> struct Com {T* p=nullptr;~Com(){if(p)p->Release();}T** put(){return &p;}};
constexpr uint32_t supportedFlags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE |
    FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION | FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
    FFX_UPSCALE_ENABLE_DEPTH_INFINITE | FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION | FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
bool stateFor(uint32_t f, D3D12_RESOURCE_STATES& state) {
    state = D3D12_RESOURCE_STATE_COMMON;
    constexpr uint32_t known = FFX_API_RESOURCE_STATE_COMMON | FFX_API_RESOURCE_STATE_UNORDERED_ACCESS |
        FFX_API_RESOURCE_STATE_COMPUTE_READ | FFX_API_RESOURCE_STATE_PIXEL_READ | FFX_API_RESOURCE_STATE_COPY_SRC |
        FFX_API_RESOURCE_STATE_COPY_DEST | FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT | FFX_API_RESOURCE_STATE_PRESENT |
        FFX_API_RESOURCE_STATE_RENDER_TARGET | FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT;
    if (!f || (f & ~known)) return false;
    const auto add = [&](uint32_t bit, D3D12_RESOURCE_STATES d) { if (f & bit) state |= d; };
    add(FFX_API_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    add(FFX_API_RESOURCE_STATE_COMPUTE_READ,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    add(FFX_API_RESOURCE_STATE_PIXEL_READ,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    add(FFX_API_RESOURCE_STATE_COPY_SRC,D3D12_RESOURCE_STATE_COPY_SOURCE);
    add(FFX_API_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_DEST);
    add(FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    add(FFX_API_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET);
    add(FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT,D3D12_RESOURCE_STATE_DEPTH_WRITE);
    return true;
}
bool sameDevice(ID3D12Device* expected, ID3D12DeviceChild* child) {
    Com<ID3D12Device> actual;
    return SUCCEEDED(child->GetDevice(IID_PPV_ARGS(actual.put()))) && actual.p==expected;
}
}
struct Fsr4D3D12Context::Impl {
 std::unique_ptr<DllNativeTransport> transport;
 ID3D12Device* device;ffxCreateContextDescUpscale desc;
 uint64_t count=0;unsigned lastW=0,lastH=0,lastRW=0,lastRH=0;
 float previousJitterX=0,previousJitterY=0,previousExposure=0;
 bool resetNeeded=true;
 Impl(ID3D12Device* d,const ffxCreateContextDescUpscale& c):device(d),desc(c){
  if(!d||(c.flags&~supportedFlags)||!c.maxUpscaleSize.width||!c.maxUpscaleSize.height||!c.maxRenderSize.width||!c.maxRenderSize.height||c.maxUpscaleSize.width>3440||c.maxUpscaleSize.height>1440||c.maxRenderSize.width>c.maxUpscaleSize.width||c.maxRenderSize.height>c.maxUpscaleSize.height)throw std::invalid_argument("native full frame context admission");
  device->AddRef();desc.header.pNext=nullptr;transport=std::make_unique<DllNativeTransport>(device,c.maxUpscaleSize.width,c.maxUpscaleSize.height);
 }
 ~Impl(){transport.reset();device->Release();}
 bool dispatch(const ffxDispatchDescUpscale& request,std::string& reason){
  auto reject=[&](const char* message){reason=message;resetNeeded=true;return false;};

        const auto size=request.upscaleSize.width||request.upscaleSize.height?request.upscaleSize:desc.maxUpscaleSize;
        if(request.header.type!=FFX_API_DISPATCH_DESC_TYPE_UPSCALE||request.header.pNext||request.flags||!request.commandList)
            return reject("unsupported dispatch descriptor");
        if(!size.width||!size.height||size.width>desc.maxUpscaleSize.width||size.height>desc.maxUpscaleSize.height||
            !request.renderSize.width||!request.renderSize.height||request.renderSize.width>desc.maxRenderSize.width||request.renderSize.height>desc.maxRenderSize.height||
            request.renderSize.width>size.width||request.renderSize.height>size.height)return reject("unsupported logical extent");
        if((desc.flags&FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION)||fsr4ModelForSize(size.width,size.height,request.renderSize.width,request.renderSize.height)==Fsr4ModelPreset::Unsupported)
            return reject("trained model unavailable for custom/dynamic scale; original provider required");
        if(request.enableSharpening&&(!std::isfinite(request.sharpness)||request.sharpness<0||request.sharpness>1))
            return reject("invalid RCAS sharpness");
        if(!std::isfinite(request.preExposure)||request.preExposure<=0||!std::isfinite(request.jitterOffset.x)||!std::isfinite(request.jitterOffset.y)||
            !std::isfinite(request.motionVectorScale.x)||!std::isfinite(request.motionVectorScale.y))return reject("invalid exposure/motion values");
        auto list=static_cast<ID3D12GraphicsCommandList*>(request.commandList);
        if(!sameDevice(device,list)||(list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT&&list->GetType()!=D3D12_COMMAND_LIST_TYPE_COMPUTE))return reject("foreign device/list");
        const FfxApiResource* external[]={&request.color,&request.depth,&request.motionVectors,&request.exposure,&request.output};
        std::array<ID3D12Resource*,5> resources{};std::array<D3D12_RESOURCE_STATES,5> states{};std::array<DXGI_FORMAT,4> requested{};
        const FsrTextureRole roles[]={FsrTextureRole::Color,FsrTextureRole::Depth,FsrTextureRole::Motion,FsrTextureRole::Exposure};
        for(unsigned i=0;i<5;++i){
            if(i==3&&!external[i]->resource){requested[i]=DXGI_FORMAT_R16_FLOAT;continue;}
            resources[i]=static_cast<ID3D12Resource*>(external[i]->resource);
            if(!resources[i]||!sameDevice(device,resources[i])||!stateFor(external[i]->state,states[i]))return reject("invalid resource/state/device");
            const auto d=resources[i]->GetDesc();const unsigned rw=i==3?1:i==4?size.width:request.renderSize.width,rh=i==3?1:i==4?size.height:request.renderSize.height;
            if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.DepthOrArraySize!=1||d.MipLevels!=1||d.SampleDesc.Count!=1||d.Width<rw||d.Height<rh)
                return reject("unsupported subresource/sample/extent");
            if(i==4){if(d.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT)return reject("unsupported output format");}
            else {if(fsrTextureView(d.Format,roles[i])==DXGI_FORMAT_UNKNOWN)return reject("unsupported typed input view");requested[i]=d.Format;}
            for(unsigned j=0;j<i;++j)if(resources[i]==resources[j])return reject("aliased game resources");
        }
        // Only formats with proven copy/SRV semantics are admitted initially.
        if(requested[0]!=DXGI_FORMAT_R16G16B16A16_FLOAT&&requested[0]!=DXGI_FORMAT_R32G32B32A32_FLOAT&&requested[0]!=DXGI_FORMAT_R11G11B10_FLOAT)return reject("color copy format");
        if(requested[1]!=DXGI_FORMAT_R32_FLOAT&&requested[1]!=DXGI_FORMAT_R24G8_TYPELESS&&requested[1]!=DXGI_FORMAT_R32G8X24_TYPELESS)return reject("depth copy format");
        if(requested[2]!=DXGI_FORMAT_R16G16_FLOAT&&requested[2]!=DXGI_FORMAT_R32G32_FLOAT)return reject("motion copy format");
        if(requested[3]!=DXGI_FORMAT_R16_FLOAT&&requested[3]!=DXGI_FORMAT_R32_FLOAT&&requested[3]!=DXGI_FORMAT_R32G32B32A32_FLOAT)return reject("exposure copy format");
        // Depth/stencil copies require whole subresources with matching extents.
        if(requested[1]==DXGI_FORMAT_R24G8_TYPELESS||requested[1]==DXGI_FORMAT_R32G8X24_TYPELESS){const auto d=resources[1]->GetDesc();
            if(d.Width!=desc.maxRenderSize.width||d.Height!=desc.maxRenderSize.height)return reject("depth copy capacity mismatch");}

  LabNativeFramePacket packet{};auto& a=packet.params;
  a.width=size.width;a.height=size.height;a.renderWidth=request.renderSize.width;a.renderHeight=request.renderSize.height;
  a.reset=request.reset||resetNeeded||lastW!=size.width||lastH!=size.height||lastRW!=request.renderSize.width||lastRH!=request.renderSize.height||previousExposure!=request.preExposure;
  a.inverted=bool(desc.flags&FFX_UPSCALE_ENABLE_DEPTH_INVERTED);a.jittered=bool(desc.flags&FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION);
  a.jx=request.jitterOffset.x;a.jy=request.jitterOffset.y;a.mvx=request.motionVectorScale.x/request.renderSize.width;a.mvy=request.motionVectorScale.y/request.renderSize.height;
  a.cx=(previousJitterX-a.jx)/request.renderSize.width;a.cy=(previousJitterY-a.jy)/request.renderSize.height;a.exposure=request.preExposure;
  a.reserved=request.enableSharpening?1:0;a.padding=request.enableSharpening?std::exp2(2.f*request.sharpness-2.f):0.f;
  for(unsigned i=0;i<5;++i){packet.resources[i]=resources[i];packet.states[i]=states[i];if(i<4)a.format[i]=requested[i];}
  // The Scene callback validates its own resources/spans before submitting.
  // E_INVALIDARG means pre-submit rejection; any execution failure terminates lab.
  const HRESULT hr=transport->record(list,packet)?S_OK:E_INVALIDARG;
  if(hr==E_INVALIDARG)return reject("native transport admission");
  if(FAILED(hr))throw std::runtime_error("native failure after admission");
  ++count;resetNeeded=false;lastW=size.width;lastH=size.height;lastRW=request.renderSize.width;lastRH=request.renderSize.height;
  previousJitterX=a.jx;previousJitterY=a.jy;previousExposure=request.preExposure;return true;
 }
};
Fsr4D3D12Context::Fsr4D3D12Context(ID3D12Device* d,const ffxCreateContextDescUpscale& c,const std::wstring&):impl(std::make_unique<Impl>(d,c)){}
Fsr4D3D12Context::~Fsr4D3D12Context()=default;
bool Fsr4D3D12Context::dispatch(const ffxDispatchDescUpscale& d,std::string& r){return impl->dispatch(d,r);}
void Fsr4D3D12Context::invalidateHistory() noexcept {impl->resetNeeded=true;}
uint64_t Fsr4D3D12Context::allocatedBytes() const noexcept {return 0;} // Metal/transport ledger logged by owner, not D3D provider.
uint64_t Fsr4D3D12Context::dispatchCount() const noexcept {return impl->count;}
bool Fsr4D3D12Context::recordHistoryReadback(ID3D12GraphicsCommandList*,ID3D12Resource*,uint32_t,uint32_t){return false;}
bool Fsr4D3D12Context::recordTensorReadback(ID3D12GraphicsCommandList*,ID3D12Resource*,uint32_t,uint32_t,uint32_t){return false;}

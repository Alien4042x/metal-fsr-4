#pragma once
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace rife_d3d12 {
using Microsoft::WRL::ComPtr;
class GpuUiCompositor {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Resource> generated,composed;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    UINT width=0,height=0;
    UINT64 bytes=0;
    unsigned floor=16;

    static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
    static void check(HRESULT value,const char* message){require(SUCCEEDED(value),message);}
    static void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
        if(before==after)return;
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};
        list->ResourceBarrier(1,&barrier);
    }
    void writeDescriptors(ID3D12Resource* clean,ID3D12Resource* full){
        auto handle=heap->GetCPUDescriptorHandleForHeapStart();
        const UINT step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=format;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(generated.Get(),&srv,handle);handle.ptr+=step;
        device->CreateShaderResourceView(clean,&srv,handle);handle.ptr+=step;
        device->CreateShaderResourceView(full,&srv,handle);handle.ptr+=step;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;uav.Buffer.NumElements=UINT(bytes/4);
        uav.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(composed.Get(),nullptr,&uav,handle);
    }
public:
    GpuUiCompositor(ID3D12Device* value,const D3D12_RESOURCE_DESC& texture,
                    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& placed,UINT64 byteCount)
        :device(value),footprint(placed),format(texture.Format),width(UINT(texture.Width)),height(texture.Height),bytes(byteCount){
        require(device&&width&&height&&bytes,"RIFE GPU UI arguments");
        require(format==DXGI_FORMAT_R8G8B8A8_UNORM||format==DXGI_FORMAT_B8G8R8A8_UNORM,"RIFE GPU UI format");
        if(const char* text=std::getenv("RIFE_UI_DIFFERENCE_FLOOR")){
            char* end=nullptr;const unsigned long parsed=std::strtoul(text,&end,10);
            if(end!=text&&*end=='\0'&&parsed<=255)floor=unsigned(parsed);
        }
        D3D12_HEAP_PROPERTIES properties{};properties.Type=D3D12_HEAP_TYPE_DEFAULT;
        properties.CreationNodeMask=properties.VisibleNodeMask=1;
        auto generatedDesc=texture;generatedDesc.Flags=D3D12_RESOURCE_FLAG_NONE;
        check(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&generatedDesc,
              D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&generated)),"RIFE GPU UI generated texture");
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;
        buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=buffer.SampleDesc.Count=1;
        buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;buffer.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&buffer,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&composed)),"RIFE GPU UI output buffer");

        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[0].NumDescriptors=3;
        ranges[0].BaseShaderRegister=0;ranges[0].OffsetInDescriptorsFromTableStart=0;
        ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;ranges[1].NumDescriptors=1;
        ranges[1].BaseShaderRegister=0;ranges[1].OffsetInDescriptorsFromTableStart=3;
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable={2,ranges};parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants={0,0,5};parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=2;signature.pParameters=parameters;
        ComPtr<ID3DBlob> serialized,errors;
        check(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors),"RIFE GPU UI root serialize");
        check(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root)),"RIFE GPU UI root");
        static const char shader[]=
            "Texture2D<float4> generatedFrame : register(t0);\n"
            "Texture2D<float4> cleanFrame : register(t1);\n"
            "Texture2D<float4> fullFrame : register(t2);\n"
            "RWByteAddressBuffer outputBytes : register(u0);\n"
            "cbuffer C : register(b0) { uint width; uint height; uint pitch; uint bgra; uint floorByte; };\n"
            "float3 linearize(float3 v) { float3 base=max((v+0.055)/1.055,0.0); return lerp(v/12.92, pow(base,2.4), step(0.04045,v)); }\n"
            "[numthreads(8,8,1)] void main(uint3 p : SV_DispatchThreadID) {\n"
            " if(p.x>=width||p.y>=height)return;\n"
            " float4 generated=generatedFrame.Load(int3(p.xy,0));\n"
            " float4 clean=cleanFrame.Load(int3(p.xy,0));\n"
            " float4 full=fullFrame.Load(int3(p.xy,0));\n"
            " float3 byteGap=abs(clean.rgb-full.rgb)*255.0;\n"
            " float factor=0.0;\n"
            " if(any(byteGap>float(floorByte))){\n"
            "  float3 a=linearize(clean.rgb),b=linearize(full.rgb);\n"
            "  float3 hi=max(a,b),lo=min(a,b);\n"
            "  float3 component=min(1.0,(1.0-lo/max(hi,1e-8))*10.0);\n"
            "  factor=max(component.x,max(component.y,component.z));\n"
            " }\n"
            " uint4 c=(uint4)round(saturate(lerp(generated,full,factor))*255.0);\n"
            " uint packed=bgra!=0 ? (c.z|(c.y<<8)|(c.x<<16)|(c.w<<24)) : (c.x|(c.y<<8)|(c.z<<16)|(c.w<<24));\n"
            " outputBytes.Store(p.y*pitch+p.x*4,packed);\n"
            "}\n";
        ComPtr<ID3DBlob> code;errors.Reset();
        HRESULT compiled=D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"main","cs_5_1",
                                    D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                    0,&code,&errors);
        if(errors)fprintf(stderr,"RIFE_GPU_UI_SHADER %.*s\n",int(errors->GetBufferSize()),static_cast<const char*>(errors->GetBufferPointer()));
        check(compiled,"RIFE GPU UI shader compile");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};pipelineDesc.pRootSignature=root.Get();
        pipelineDesc.CS={code->GetBufferPointer(),code->GetBufferSize()};
        check(device->CreateComputePipelineState(&pipelineDesc,IID_PPV_ARGS(&pipeline)),"RIFE GPU UI pipeline");
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};heapDesc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors=4;heapDesc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(device->CreateDescriptorHeap(&heapDesc,IID_PPV_ARGS(&heap)),"RIFE GPU UI descriptors");
        printf("RIFE_GPU_UI_READY output=%ux%u format=%u floor=%u cpu_full_readback=0\n",width,height,unsigned(format),floor);
    }
    void record(ID3D12GraphicsCommandList* list,ID3D12Resource* upload,
                ID3D12Resource* clean,D3D12_RESOURCE_STATES cleanState,
                ID3D12Resource* full,D3D12_RESOURCE_STATES fullState,
                ID3D12Resource* output,D3D12_RESOURCE_STATES outputState){
        require(list&&upload&&clean&&full&&output,"RIFE GPU UI resources");
        writeDescriptors(clean,full);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=upload;
        from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=footprint;
        to.pResource=generated.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        transition(list,generated.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(list,clean,cleanState,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(list,full,fullState,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ID3D12DescriptorHeap* active[]={heap.Get()};list->SetDescriptorHeaps(1,active);
        list->SetComputeRootSignature(root.Get());list->SetPipelineState(pipeline.Get());
        list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
        const UINT constants[]={width,height,footprint.Footprint.RowPitch,
                               format==DXGI_FORMAT_B8G8R8A8_UNORM?1u:0u,floor};
        list->SetComputeRoot32BitConstants(1,5,constants,0);
        list->Dispatch((width+7)/8,(height+7)/8,1);
        D3D12_RESOURCE_BARRIER uav{};uav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;uav.UAV.pResource=composed.Get();
        list->ResourceBarrier(1,&uav);
        transition(list,composed.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(list,output,outputState,D3D12_RESOURCE_STATE_COPY_DEST);
        from={};to={};from.pResource=composed.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint=footprint;to.pResource=output;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        transition(list,output,D3D12_RESOURCE_STATE_COPY_DEST,outputState);
        transition(list,composed.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(list,full,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,fullState);
        transition(list,clean,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,cleanState);
        transition(list,generated.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    }
};
}

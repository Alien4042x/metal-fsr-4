// WineForge-Internal: fsr-lab/direct-ffx-gamepath-cube-v1.
// Derived from the preserved matched scene; no native Metal bridge.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <cstring>
#include <atomic>
#include <thread>
#include <psapi.h>
static UINT W=128,H=96,RW=64,RH=48,ViewW=128,ViewH=96;
static unsigned MaxFrames=8;static double MaxSeconds=20;
static bool Visible=false;static std::atomic<bool> stopRequested{false};


static void check(HRESULT h,const char* s){if(FAILED(h)){printf("FAIL %s hr=%08lx\n",s,(unsigned long)h);throw std::runtime_error(s);}}
static void need(bool b,const char* s){if(!b)throw std::runtime_error(s);}
static LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){
 if(m==WM_CLOSE||(m==WM_KEYDOWN&&a==VK_ESCAPE)){stopRequested=true;return 0;}
 if(m==WM_APP+1){DestroyWindow(w);return 0;}
 if(m==WM_DESTROY){PostQuitMessage(0);return 0;}
 return DefWindowProcW(w,m,a,b);
}
struct V {float x,y,z,r,g,b;};
using Clock=std::chrono::steady_clock;
static double elapsed(Clock::time_point a,Clock::time_point b){return std::chrono::duration<double>(b-a).count();}
#include "scene_ffx_gamepath_provider.hpp"
#include "present.hpp"
struct Scene {
 SceneFrameGeneration fg;
 SceneFFX fsr;
 ID3D12Resource* jitterDepth=nullptr;ID3D12PipelineState* colorOnlyPSO=nullptr;
 IDXGIAdapter3* memoryAdapter=nullptr;
 double encodeMs=0,submitWaitMs=0,presentMs=0,reuseWaitMs=0;
 ID3D12Resource* exposure=nullptr;


 float lastAngle=0;unsigned captureCount=0;
 std::vector<IUnknown*> owned; bool pending=false; HANDLE event=nullptr;
 ID3D12Device* d=nullptr;ID3D12CommandQueue* q=nullptr;ID3D12CommandAllocator* allocator=nullptr;
 ID3D12GraphicsCommandList* list=nullptr;ID3D12Fence* fence=nullptr;UINT64 serial=0;
 IDXGISwapChain3* swap=nullptr;ID3D12Resource *back[2]={},*depth=nullptr,*vb=nullptr,*hud=nullptr,*readback=nullptr;
 ID3D12Resource *color=nullptr,*motion=nullptr,*upscaled=nullptr;
 ID3D12DescriptorHeap *imageSrv=nullptr;
 ID3D12RootSignature* displayRoot=nullptr;ID3D12PipelineState* displayPSO=nullptr;
 struct Capture {ID3D12Resource* source;ID3D12Resource* buffer;D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;UINT64 bytes;UINT width,height,channels;const char* name;};
 std::vector<Capture> captures;
 ID3D12DescriptorHeap *rtv=nullptr,*dsv=nullptr;ID3D12RootSignature* root=nullptr;
 ID3D12PipelineState *cubePSO=nullptr,*linePSO=nullptr;UINT rtvStep=0;
 D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint={};UINT64 captureBytes=0;
 template<class T> T* keep(T* p){owned.push_back(p);return p;}
 ~Scene(){if(pending)ExitProcess(90);fg.close();if(fsr.context){try{fsr.close();}catch(...){ExitProcess(91);}}for(auto it=owned.rbegin();it!=owned.rend();++it)(*it)->Release();if(event)CloseHandle(event);}
 ID3D12Resource* resource(D3D12_HEAP_TYPE heap,D3D12_RESOURCE_DESC desc,D3D12_RESOURCE_STATES state,const D3D12_CLEAR_VALUE* clear=nullptr){
  D3D12_HEAP_PROPERTIES hp={};hp.Type=heap;hp.CreationNodeMask=hp.VisibleNodeMask=1;ID3D12Resource* r=nullptr;
  check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,state,clear,IID_PPV_ARGS(&r)),"resource");return keep(r);
 }
 ID3D12Resource* buffer(UINT64 n,D3D12_HEAP_TYPE heap){D3D12_RESOURCE_DESC x={};x.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
  x.Width=n;x.Height=x.DepthOrArraySize=x.MipLevels=x.SampleDesc.Count=1;x.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return resource(heap,x,heap==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST);
 }
 void barrier(ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){D3D12_RESOURCE_BARRIER b={};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};list->ResourceBarrier(1,&b);}
 void finish(){
  const auto start=Clock::now();check(list->Close(),"close");ID3D12CommandList* lists[]={list};pending=true;q->ExecuteCommandLists(1,lists);
  check(q->Signal(fence,++serial),"signal");check(fence->SetEventOnCompletion(serial,event),"event");
  need(WaitForSingleObject(event,10000)==WAIT_OBJECT_0&&fence->GetCompletedValue()==serial,"GPU completion within 10s");pending=false;
  check(d->GetDeviceRemovedReason(),"device health");submitWaitMs=elapsed(start,Clock::now())*1000;
 }
 void init(HWND window){
  IDXGIFactory4* f=nullptr;check(CreateDXGIFactory2(0,IID_PPV_ARGS(&f)),"factory");keep(f);
  IDXGIAdapter1* adapter=nullptr;check(f->EnumAdapters1(0,&adapter),"adapter");keep(adapter);
  if(SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&memoryAdapter))))keep(memoryAdapter);
  DXGI_ADAPTER_DESC1 ad={};adapter->GetDesc1(&ad);printf("ADAPTER %ls (DXGI description)\n",ad.Description);
  check(D3D12CreateDevice(adapter,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&d)),"device");keep(d);fg.init(d);
  D3D12_COMMAND_QUEUE_DESC qd={};check(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)),"queue");keep(q);
  check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");keep(allocator);
  check(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator,nullptr,IID_PPV_ARGS(&list)),"list");keep(list);check(list->Close(),"initial close");
  check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");keep(fence);event=CreateEventW(nullptr,FALSE,FALSE,nullptr);need(event!=nullptr,"event allocation");
  DXGI_SWAP_CHAIN_DESC1 sd={};sd.Width=W;sd.Height=H;sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.SampleDesc.Count=1;
  sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=2;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
  IDXGISwapChain1* originalSwap=nullptr;
  check(f->CreateSwapChainForHwnd(q,window,&sd,nullptr,nullptr,&originalSwap),"application swap create");
  IDXGISwapChain4* s=nullptr;const auto swapQuery=originalSwap->QueryInterface(IID_PPV_ARGS(&s));originalSwap->Release();check(swapQuery,"application swap4");
  fg.wrapSwap(s,q);keep(s);
  check(s->QueryInterface(IID_PPV_ARGS(&swap)),"swapchain3");keep(swap);f->MakeWindowAssociation(window,DXGI_MWA_NO_ALT_ENTER);
  D3D12_DESCRIPTOR_HEAP_DESC hd={};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=5;
  check(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv)),"RTV heap");keep(rtv);rtvStep=d->GetDescriptorHandleIncrementSize(hd.Type);
  auto handle=rtv->GetCPUDescriptorHandleForHeapStart();for(UINT i=0;i<2;++i){check(swap->GetBuffer(i,IID_PPV_ARGS(&back[i])),"swap buffer");keep(back[i]);d->CreateRenderTargetView(back[i],nullptr,handle);handle.ptr+=rtvStep;}
  hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=2;check(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dsv)),"DSV heap");keep(dsv);
  D3D12_RESOURCE_DESC dd={};dd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;dd.Width=RW;dd.Height=RH;dd.DepthOrArraySize=dd.MipLevels=dd.SampleDesc.Count=1;
  dd.Format=DXGI_FORMAT_R24G8_TYPELESS;dd.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;D3D12_CLEAR_VALUE cv={};cv.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;cv.DepthStencil.Depth=0;
  depth=resource(D3D12_HEAP_TYPE_DEFAULT,dd,D3D12_RESOURCE_STATE_DEPTH_WRITE,&cv);D3D12_DEPTH_STENCIL_VIEW_DESC dv={};dv.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;dv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
  d->CreateDepthStencilView(depth,&dv,dsv->GetCPUDescriptorHandleForHeapStart());
  jitterDepth=resource(D3D12_HEAP_TYPE_DEFAULT,dd,D3D12_RESOURCE_STATE_DEPTH_WRITE,&cv);auto jitterHandle=dsv->GetCPUDescriptorHandleForHeapStart();jitterHandle.ptr+=d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);d->CreateDepthStencilView(jitterDepth,&dv,jitterHandle);
  auto makeTex=[&](UINT w,UINT h,DXGI_FORMAT fmt,D3D12_RESOURCE_FLAGS flags){auto desc=dd;desc.Width=w;desc.Height=h;desc.Format=fmt;desc.Flags=flags;
   return resource(D3D12_HEAP_TYPE_DEFAULT,desc,D3D12_RESOURCE_STATE_RENDER_TARGET);};
  color=makeTex(RW,RH,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  motion=makeTex(RW,RH,DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  upscaled=makeTex(W,H,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  for(auto tex:{color,motion,upscaled}){d->CreateRenderTargetView(tex,nullptr,handle);handle.ptr+=rtvStep;}
  hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=1;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  check(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&imageSrv)),"display SRV heap");keep(imageSrv);
  D3D12_SHADER_RESOURCE_VIEW_DESC srv={};srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
  d->CreateShaderResourceView(upscaled,&srv,imageSrv->GetCPUDescriptorHandleForHeapStart());
  D3D12_ROOT_PARAMETER rp={};rp.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;rp.Constants.Num32BitValues=8;rp.ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;
  D3D12_ROOT_SIGNATURE_DESC rd={};rd.NumParameters=1;rd.pParameters=&rp;rd.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ID3DBlob *blob=nullptr,*errors=nullptr;HRESULT hr=D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors);if(errors)keep(errors);check(hr,"serialize root");keep(blob);
  check(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"root");keep(root);
  auto shader=[&](const char* entry,const char* target){ID3DBlob *code=nullptr,*err=nullptr;
   HRESULT h=D3DCompileFromFile(L"scene_ffx_gamepath.hlsl",nullptr,nullptr,entry,target,D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&err);
   if(err){printf("SHADER %s %.*s\n",entry,int(err->GetBufferSize()),(char*)err->GetBufferPointer());keep(err);}check(h,"compile shader");return keep(code);};
  auto vs=shader("VS","vs_5_0"),ps=shader("PS","ps_5_0");
  D3D12_INPUT_ELEMENT_DESC il[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},{"COLOR",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd={};pd.pRootSignature=root;pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;pd.SampleMask=UINT_MAX;
  auto& blend=pd.BlendState.RenderTarget[0];blend.SrcBlend=blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlend=blend.DestBlendAlpha=D3D12_BLEND_ZERO;
  blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.LogicOp=D3D12_LOGIC_OP_NOOP;
  pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
  pd.DepthStencilState.DepthEnable=TRUE;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_GREATER;
  pd.DepthStencilState.FrontFace.StencilFailOp=pd.DepthStencilState.FrontFace.StencilDepthFailOp=pd.DepthStencilState.FrontFace.StencilPassOp=D3D12_STENCIL_OP_KEEP;
  pd.DepthStencilState.FrontFace.StencilFunc=D3D12_COMPARISON_FUNC_ALWAYS;pd.DepthStencilState.BackFace=pd.DepthStencilState.FrontFace;
  pd.InputLayout={il,2};pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=2;pd.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;pd.RTVFormats[1]=DXGI_FORMAT_R16G16_FLOAT;pd.DSVFormat=DXGI_FORMAT_D24_UNORM_S8_UINT;pd.SampleDesc.Count=1;
  check(d->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&cubePSO)),"cube pipeline");keep(cubePSO);
  auto colorPs=shader("ColorPS","ps_5_0");pd.PS={colorPs->GetBufferPointer(),colorPs->GetBufferSize()};pd.NumRenderTargets=1;pd.RTVFormats[1]=DXGI_FORMAT_UNKNOWN;check(d->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&colorOnlyPSO)),"jitter color pipeline");keep(colorOnlyPSO);
  auto hudPs=shader("HudPS","ps_5_0");pd.PS={hudPs->GetBufferPointer(),hudPs->GetBufferSize()};
  pd.NumRenderTargets=1;pd.RTVFormats[0]=sd.Format;pd.RTVFormats[1]=DXGI_FORMAT_UNKNOWN;pd.DSVFormat=DXGI_FORMAT_UNKNOWN;
  pd.DepthStencilState.DepthEnable=FALSE;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
  check(d->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&linePSO)),"graph pipeline");keep(linePSO);
  D3D12_DESCRIPTOR_RANGE sr={};sr.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;sr.NumDescriptors=1;
  D3D12_ROOT_PARAMETER tr={};tr.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;tr.DescriptorTable={1,&sr};tr.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
  rd.pParameters=&tr;rd.Flags=D3D12_ROOT_SIGNATURE_FLAG_NONE;blob=nullptr;errors=nullptr;
  hr=D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors);if(errors)keep(errors);check(hr,"display root serialize");keep(blob);
  check(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&displayRoot)),"display root");keep(displayRoot);
  auto fullVs=shader("FullVS","vs_5_0"),fullPs=shader("FullPS","ps_5_0");pd.VS={fullVs->GetBufferPointer(),fullVs->GetBufferSize()};pd.PS={fullPs->GetBufferPointer(),fullPs->GetBufferSize()};
  pd.pRootSignature=displayRoot;pd.InputLayout={};pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  check(d->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&displayPSO)),"display pipeline");keep(displayPSO);
  const float xyz[8][3]={{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
  const UINT faces[6][6]={{0,1,2,0,2,3},{4,6,5,4,7,6},{0,4,5,0,5,1},{3,2,6,3,6,7},{0,3,7,0,7,4},{1,5,6,1,6,2}};
  const float colors[6][3]={{.95f,.22f,.18f},{.12f,.73f,.95f},{.8f,.25f,.9f},{.95f,.77f,.15f},{.16f,.8f,.48f},{.33f,.39f,.97f}};
  std::vector<V> vertices;for(UINT face=0;face<6;++face)for(UINT idx:faces[face])vertices.push_back({xyz[idx][0],xyz[idx][1],xyz[idx][2],colors[face][0],colors[face][1],colors[face][2]});
  vb=buffer(vertices.size()*sizeof(V),D3D12_HEAP_TYPE_UPLOAD);void* p=nullptr;D3D12_RANGE empty={};check(vb->Map(0,&empty,&p),"VB map");memcpy(p,vertices.data(),vertices.size()*sizeof(V));vb->Unmap(0,nullptr);
  hud=buffer(512*sizeof(V),D3D12_HEAP_TYPE_UPLOAD);
  auto bd=back[0]->GetDesc();d->GetCopyableFootprints(&bd,0,1,0,&footprint,nullptr,nullptr,&captureBytes);readback=buffer(captureBytes,D3D12_HEAP_TYPE_READBACK);
  auto captureResource=[&](ID3D12Resource* tex,UINT channels,const char* name){auto desc=tex->GetDesc();Capture c={};c.source=tex;c.width=UINT(desc.Width);c.height=desc.Height;c.channels=channels;c.name=name;
   d->GetCopyableFootprints(&desc,0,1,0,&c.fp,nullptr,nullptr,&c.bytes);c.bytes=UINT64(c.fp.Footprint.RowPitch)*c.height;c.buffer=buffer(c.bytes,D3D12_HEAP_TYPE_READBACK);captures.push_back(c);};
  captureResource(color,4,"input-decoded-rgba32f.bin");captureResource(depth,1,"depth-decoded-r32f.bin");captureResource(motion,2,"motion-decoded-rg32f.bin");captureResource(upscaled,4,"output-decoded-rgba32f.bin");
  // Fixed R16F exposure texture, same resource format as the game.
  auto ed=dd;ed.Width=ed.Height=1;ed.Format=DXGI_FORMAT_R16_FLOAT;ed.Flags=D3D12_RESOURCE_FLAG_NONE;
  exposure=resource(D3D12_HEAP_TYPE_DEFAULT,ed,D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT ef{};UINT64 eb=0;d->GetCopyableFootprints(&ed,0,1,0,&ef,nullptr,nullptr,&eb);
  auto eu=buffer(eb,D3D12_HEAP_TYPE_UPLOAD);check(eu->Map(0,&empty,&p),"exposure map");memset(p,0,size_t(eb));
  const uint16_t one=0x3c00;memcpy(static_cast<char*>(p)+ef.Offset,&one,2);eu->Unmap(0,nullptr);
  check(allocator->Reset(),"exposure allocator");check(list->Reset(allocator,nullptr),"exposure list");
  D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=eu;from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=ef;
  to.pResource=exposure;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
  barrier(exposure,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);finish();
  fsr.init(d);
  printf("READY direct_ffx_cube output=%ux%u render=%ux%u provider=%s vsync=0 inflight_limit=1 flags=0x9 color=10 depth=44 motion=34 exposure=54 output_format=10\n",W,H,RW,RH,fsr.label.c_str());
 }
 void frame(float angle,const std::vector<double>& history,bool capture,float dt,bool reset){
  const auto t0=Clock::now();
  fsr.jitter();if(fsr.frames==0)lastAngle=angle;
  std::vector<V> graph;auto line=[&](float x,float y,float xx,float yy,float r,float g,float b){graph.push_back({x,y,0,r,g,b});graph.push_back({xx,yy,0,r,g,b});};
  // 180 wall-frame samples; graph fixed 0..200 ms. Clipping is visualization only, CSV retains all values.
  for(float ms:{0.f,50.f,100.f,200.f})line(.3f,-.9f+ms*.0015f,.94f,-.9f+ms*.0015f,.25f,.3f,.4f);
  for(size_t i=1;i<history.size();++i)line(.3f+.64f*(i-1)/179,-.9f+float(std::min(200.,history[i-1]))*.0015f,.3f+.64f*i/179,-.9f+float(std::min(200.,history[i]))*.0015f,.15f,.95f,.75f);
  void* p=nullptr;D3D12_RANGE empty={};check(hud->Map(0,&empty,&p),"HUD map");memcpy(p,graph.data(),graph.size()*sizeof(V));hud->Unmap(0,nullptr);
  check(allocator->Reset(),"allocator reset");check(list->Reset(allocator,cubePSO),"list reset");UINT i=swap->GetCurrentBackBufferIndex();
  barrier(back[i],D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto rt=rtv->GetCPUDescriptorHandleForHeapStart();rt.ptr+=i*rtvStep;auto ds=dsv->GetCPUDescriptorHandleForHeapStart();
  auto cr=rtv->GetCPUDescriptorHandleForHeapStart();cr.ptr+=2*rtvStep;auto mr=cr;mr.ptr+=rtvStep;auto ur=mr;ur.ptr+=rtvStep;
  const float bg[]={.035f,.05f,.08f,1},zero[]={0,0,0,0},sentinel[]={-9,-9,-9,-9};
  list->ClearRenderTargetView(cr,bg,0,nullptr);list->ClearRenderTargetView(mr,zero,0,nullptr);list->ClearRenderTargetView(ur,sentinel,0,nullptr);
  list->ClearDepthStencilView(ds,D3D12_CLEAR_FLAG_DEPTH,0,0,0,nullptr);D3D12_CPU_DESCRIPTOR_HANDLE mrt[]={cr,mr};list->OMSetRenderTargets(2,mrt,FALSE,&ds);
  D3D12_VIEWPORT vp={0,0,float(RW),float(RH),0,1};D3D12_RECT sc={0,0,LONG(RW),LONG(RH)};list->RSSetViewports(1,&vp);list->RSSetScissorRects(1,&sc);
  list->SetGraphicsRootSignature(root);float settings[]={angle,float(W)/H,0,lastAngle,0,0,float(RW),float(RH)};list->SetGraphicsRoot32BitConstants(0,8,settings,0);
  D3D12_VERTEX_BUFFER_VIEW view={vb->GetGPUVirtualAddress(),36*sizeof(V),sizeof(V)};list->IASetVertexBuffers(0,1,&view);list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(36,1,0,0);
  // Original mesh/camera/fragment shader; separate rasterization preserves unjittered depth/MV.
  auto jitterDS=dsv->GetCPUDescriptorHandleForHeapStart();jitterDS.ptr+=d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  list->ClearDepthStencilView(jitterDS,D3D12_CLEAR_FLAG_DEPTH,0,0,0,nullptr);list->ClearRenderTargetView(cr,bg,0,nullptr);
  list->OMSetRenderTargets(1,&cr,FALSE,&jitterDS);list->SetPipelineState(colorOnlyPSO);settings[4]=fsr.jx;settings[5]=fsr.jy;list->SetGraphicsRoot32BitConstants(0,8,settings,0);list->DrawInstanced(36,1,0,0);
  barrier(color,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);barrier(motion,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  barrier(depth,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);barrier(upscaled,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  fsr.run(list,color,depth,motion,exposure,upscaled,dt,reset);
  fg.run(list,depth,motion,fsr.jx,fsr.jy,dt,reset);
  list->ClearState(nullptr);
  const char eventName[]="continuation regression";
  list->BeginEvent(0,eventName,sizeof(eventName));list->SetMarker(0,eventName,sizeof(eventName));list->EndEvent();
  const FLOAT blend[]={1,1,1,1};list->OMSetBlendFactor(blend);list->OMSetStencilRef(0);
  list->SetComputeRootSignature(nullptr);list->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);

  if(capture){for(auto& c:captures){auto state=c.source==upscaled?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
   barrier(c.source,state,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION dest={},src={};dest.pResource=c.buffer;dest.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dest.PlacedFootprint=c.fp;
   src.pResource=c.source;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&dest,0,0,0,&src,nullptr);barrier(c.source,D3D12_RESOURCE_STATE_COPY_SOURCE,state);}}
  barrier(upscaled,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  // FFX modifies graphics/compute state and descriptor heaps. Restore all display state explicitly.
  vp.Width=float(W);vp.Height=float(H);sc.right=W;sc.bottom=H;list->RSSetViewports(1,&vp);list->RSSetScissorRects(1,&sc);
  list->OMSetRenderTargets(1,&rt,FALSE,nullptr);list->SetPipelineState(displayPSO);list->SetGraphicsRootSignature(displayRoot);
  ID3D12DescriptorHeap* heaps[]={imageSrv};list->SetDescriptorHeaps(1,heaps);list->SetGraphicsRootDescriptorTable(0,imageSrv->GetGPUDescriptorHandleForHeapStart());
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(3,1,0,0);list->SetGraphicsRootSignature(root);
  settings[2]=1;list->SetGraphicsRoot32BitConstants(0,8,settings,0);list->SetPipelineState(linePSO);view={hud->GetGPUVirtualAddress(),UINT(graph.size()*sizeof(V)),sizeof(V)};
  list->IASetVertexBuffers(0,1,&view);list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);/* HUD graph omitted from interpolation input; window title remains. */
  if(capture){barrier(back[i],D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
   D3D12_TEXTURE_COPY_LOCATION a={},b={};a.pResource=readback;a.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;a.PlacedFootprint=footprint;
   b.pResource=back[i];b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&a,0,0,0,&b,nullptr);
   barrier(back[i],D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);
  }else barrier(back[i],D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
  barrier(color,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);barrier(motion,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
  barrier(depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_DEPTH_WRITE);barrier(upscaled,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
  encodeMs=elapsed(t0,Clock::now())*1000;finish();
  // WineForge-Internal: fsr4/preserve-primary-submission-timing-v1.
  const double primarySubmitWaitMs=submitWaitMs;puts("REUSE_BEGIN same_list new_recording no_FSR");
  check(allocator->Reset(),"reuse allocator");check(list->Reset(allocator,nullptr),"reuse list");
  list->SetMarker(0,"reuse",6);finish();reuseWaitMs=submitWaitMs;submitWaitMs=primarySubmitWaitMs;puts("REUSE_COMPLETE original_submission_no_native");
  const auto presentStart=Clock::now();check(fg.presentFrame(0,0),"present");presentMs=elapsed(presentStart,Clock::now())*1000;lastAngle=angle;

 }
 static float decodeHalf(uint16_t v){const int e=(v>>10)&31,m=v&1023;const float sign=v&0x8000?-1.f:1.f;
  if(e==31)return m?NAN:sign*INFINITY;
  return sign*std::ldexp(e?1.f+float(m)/1024.f:float(m)/1024.f,e?e-15:-14);
 }
 void exportResources(const std::wstring& base){
  for(auto& c:captures){void* ptr=nullptr;D3D12_RANGE range={0,SIZE_T(c.bytes)};check(c.buffer->Map(0,&range,&ptr),"typed capture map");
   const bool isDepth=c.source==depth;const auto fmt=c.fp.Footprint.Format;
   printf("CAPTURE_LAYOUT name=%s resource_format=%u footprint_format=%u footprint=%ux%u pitch=%u bytes=%llu\n",c.name,unsigned(c.source->GetDesc().Format),unsigned(fmt),c.fp.Footprint.Width,c.fp.Footprint.Height,c.fp.Footprint.RowPitch,static_cast<unsigned long long>(c.bytes));
   if(isDepth){FILE* raw=_wfopen((base+L"-depth-native-footprint.bin").c_str(),L"wb");need(raw!=nullptr,"raw depth evidence file");
    const bool saved=fwrite(ptr,1,size_t(c.bytes),raw)==size_t(c.bytes);fclose(raw);need(saved,"raw depth evidence saved");}
   need(isDepth?(fmt==DXGI_FORMAT_R24G8_TYPELESS||fmt==DXGI_FORMAT_R24_UNORM_X8_TYPELESS||fmt==DXGI_FORMAT_R32_TYPELESS):
       (fmt==DXGI_FORMAT_R16G16B16A16_FLOAT||fmt==DXGI_FORMAT_R16G16_FLOAT),"known capture footprint format");
   std::wstring name(c.name,c.name+strlen(c.name));FILE* f=_wfopen((base+L"-"+name).c_str(),L"wb");need(f!=nullptr,"typed capture file");
   bool ok=true;size_t moving=0;float backgroundError=0,lo=INFINITY,hi=-INFINITY;std::vector<float> row(size_t(c.width)*c.channels);
   for(UINT y=0;y<c.height;++y){const auto source=static_cast<const unsigned char*>(ptr)+c.fp.Offset+size_t(y)*c.fp.Footprint.RowPitch;
    for(size_t x=0;x<row.size();++x){if(isDepth){uint32_t packed;memcpy(&packed,source+x*4,4);row[x]=float(packed&0xffffffu)/16777215.f;}
      else{uint16_t packed;memcpy(&packed,source+x*2,2);row[x]=decodeHalf(packed);}
      const float v=row[x];if(!std::isfinite(v))ok=false;lo=std::min(lo,v);hi=std::max(hi,v);
      if(c.source==motion&&fabsf(v)>1e-8f)++moving;
      if(c.source==upscaled&&x/c.channels<W/30&&y<H/30&&x%c.channels<3){const float bg[]={.035f,.05f,.08f};backgroundError=std::max(backgroundError,fabsf(v-bg[x%c.channels]));}}
    if(fwrite(row.data(),sizeof(float),row.size(),f)!=row.size())ok=false;
   }
   fclose(f);D3D12_RANGE empty{};c.buffer->Unmap(0,&empty);
   printf("TYPED_CAPTURE name=%s format=%u width=%u height=%u row_pitch=%u min=%g max=%g motion_components=%zu background_error=%g\n",c.name,unsigned(fmt),c.width,c.height,c.fp.Footprint.RowPitch,lo,hi,moving,backgroundError);
   need(ok,"finite typed capture");if(c.source==motion)need(moving>size_t(RW)*RH/200,"moving geometry has motion vectors");
   if(c.source==depth)need(lo>=0&&hi<=1&&hi>lo,"nonempty inverted depth");
   if(c.source==upscaled)need(backgroundError<.015f,"FSR flat background preserved");
  }++captureCount;
 }
 void exportFrame(const std::wstring& path){
  void* p=nullptr;D3D12_RANGE range={0,SIZE_T(captureBytes)};check(readback->Map(0,&range,&p),"capture map");
  FILE* f=_wfopen(path.c_str(),L"wb");need(f!=nullptr,"exclusive capture");size_t colored=0;bool ok=true;
  for(UINT y=0;y<H;++y){auto row=(unsigned char*)p+y*footprint.Footprint.RowPitch;
   for(UINT x=0;x<W;++x){auto px=row+4*x;if(std::max({px[0],px[1],px[2]})>100&&int(std::max({px[0],px[1],px[2]}))-std::min({px[0],px[1],px[2]})>40)++colored;}
   if(fwrite(row,1,W*4,f)!=W*4)ok=false;}
  fclose(f);D3D12_RANGE empty={};readback->Unmap(0,&empty);need(ok,"capture write");need(colored>W*H/50&&colored<W*H/2,"nonblank cube coverage");
  printf("CAPTURE rgba8=%u colored_pixels=%zu\n",W*H*4,colored);
 }
};
// Optional counters are sampled once per second; unavailable is encoded as NA.
struct MemorySample {
 std::string working="NA",privateBytes="NA",local="NA",budget="NA",nonlocal="NA",nonlocalBudget="NA";
 double sampledAt=-1;
 bool update(Scene& scene,double now){if(sampledAt>=0&&now-sampledAt<1)return false;sampledAt=now;
  PROCESS_MEMORY_COUNTERS_EX p{};p.cb=sizeof(p);
  if(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&p),sizeof(p))){working=std::to_string(p.WorkingSetSize);privateBytes=std::to_string(p.PrivateUsage);}
  else{working=privateBytes="NA";}
  DXGI_QUERY_VIDEO_MEMORY_INFO m{};
  if(scene.memoryAdapter&&SUCCEEDED(scene.memoryAdapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&m))){local=std::to_string(m.CurrentUsage);budget=std::to_string(m.Budget);}else{local=budget="NA";}
  if(scene.memoryAdapter&&SUCCEEDED(scene.memoryAdapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,&m))){nonlocal=std::to_string(m.CurrentUsage);nonlocalBudget=std::to_string(m.Budget);}else{nonlocal=nonlocalBudget="NA";}
  return true;
 }
};
static unsigned option(const char* name,unsigned fallback,unsigned lo,unsigned hi){char text[32]{};DWORD n=GetEnvironmentVariableA(name,text,32);if(!n)return fallback;need(n<32,"short numeric option");char* end=nullptr;unsigned long v=strtoul(text,&end,10);need(end&&!*end&&v>=lo&&v<=hi,"bounded numeric option");return unsigned(v);}
static int runScene(HWND window,HWND canvas,HWND label,const std::wstring& base){FILE* csv=nullptr;
 try{
  csv=_wfopen((base+L"\\frames.csv").c_str(),L"wx");need(csv!=nullptr,"exclusive frames CSV");setvbuf(csv,nullptr,_IOLBF,0);
  fprintf(csv,"frame,elapsed_s,wall_frame_ms,encode_ms,submit_wait_ms,present_ms,backend,fsr4_dispatches,original_dispatches,allocated_bytes,context_creates,context_destroys,submitted_fence,completed_fence,outstanding,capture,capture_io_ms,reset,angle,process_working_set,process_private,local_usage,local_budget,nonlocal_usage,nonlocal_budget,memory_sample,reuse_wait_ms\n");
  Scene s;const auto initStart=Clock::now();s.init(canvas);printf("INITIALIZED seconds=%.6f\n",elapsed(initStart,Clock::now()));
  const auto start=Clock::now();auto previous=start;MemorySample memory;std::vector<double> history,measured;unsigned frames=0;uint64_t firstAllocation=0;
  while(frames<MaxFrames&&elapsed(start,Clock::now())<MaxSeconds&&!stopRequested){
   const auto frameStart=Clock::now();const double previousAuxMs=elapsed(previous,frameStart)*1000;
   const bool smallGate=MaxFrames==8;
   const bool reset=frames==0||(smallGate&&frames==6);
   const bool capture=frames==4||(smallGate?frames==6:(s.captureCount==1&&elapsed(start,Clock::now())>=MaxSeconds*.75));
   const float angle=float(frames)*(.65f/60);s.frame(angle,history,capture,16.667f,reset);
   const auto copyStart=Clock::now();if(capture){const auto prefix=base+(frames==4?L"\\moving":smallGate?L"\\reset":L"\\late");s.exportFrame(prefix+L"-scene-rgba8.bin");s.exportResources(prefix);}
   const double copyMs=capture?elapsed(copyStart,Clock::now())*1000:0;
   const auto now=Clock::now();const double t=elapsed(start,now),ms=elapsed(previous,now)*1000;previous=now;
   const auto memoryStart=Clock::now();const bool sampled=memory.update(s,t);const double memoryMs=elapsed(memoryStart,Clock::now())*1000;const auto completed=s.fence->GetCompletedValue();need(completed==s.serial,"no unbounded queue backlog");
   if(s.fsr.ours){if(!frames)firstAllocation=s.fsr.last.allocatedBytes;need(s.fsr.last.allocatedBytes==firstAllocation,"stable port allocation");}
   fprintf(csv,"%u,%.6f,%.6f,%.6f,%.6f,%.6f,%u,%llu,%llu,%s,%u,%u,%llu,%llu,%llu,%u,%.6f,%u,%.9f,%s,%s,%s,%s,%s,%s,%u,%.6f\n",
    frames,t,ms,s.encodeMs,s.submitWaitMs,s.presentMs,s.fsr.last.backend,
    static_cast<unsigned long long>(s.fsr.last.fsr4Dispatches),static_cast<unsigned long long>(s.fsr.last.originalDispatches),s.fsr.ours?std::to_string(s.fsr.last.allocatedBytes).c_str():"NA",s.fsr.creates,s.fsr.destroys,
    static_cast<unsigned long long>(s.serial),static_cast<unsigned long long>(completed),static_cast<unsigned long long>(s.serial-completed),capture,copyMs,reset,angle,
    memory.working.c_str(),memory.privateBytes.c_str(),memory.local.c_str(),memory.budget.c_str(),memory.nonlocal.c_str(),memory.nonlocalBudget.c_str(),sampled,s.reuseWaitMs);
   ++frames;history.push_back(ms);if(history.size()>180)history.erase(history.begin());if(frames>5&&!capture)measured.push_back(ms);
   const auto uiStart=Clock::now();
   if(frames==1||sampled){double total=0;for(auto v:history)total+=v;const double avg=total/history.size();wchar_t text[256];
    swprintf(text,256,L"Direct FFX | %u x %u -> %u x %u | %.1f FPS | %.2f ms | backend %u\nVSync OFF | graph 0..200 ms | one in-flight frame",RW,RH,W,H,1000/avg,avg,s.fsr.last.backend);SetWindowTextW(label,text);
    swprintf(text,256,L"Direct FFX cube | %.1f FPS | backend %u",1000/avg,s.fsr.last.backend);SetWindowTextW(window,text);}
   const double uiMs=elapsed(uiStart,Clock::now())*1000;
   printf("SCENE_AUX_TIMING frame=%u work_ms=%.6f previous_aux_ms=%.6f memory_ms=%.6f ui_ms=%.6f\n",frames-1,elapsed(frameStart,now)*1000-copyMs,previousAuxMs,memoryMs,uiMs);
  }
  const bool cancelled=stopRequested;need(cancelled||(frames>=8&&s.captureCount==2),"completed frames and captures");s.fg.drain();need(s.fg.generatedCallbacks>0,"generated presentation callbacks");s.fsr.verify();s.fsr.close();
  printf("%s direct_ffx_cube frames=%u captures=%u duration_s=%.6f contexts_created=%u contexts_destroyed=%u inflight_limit=1\n",cancelled?"CANCELLED":"PASS",frames,s.captureCount,elapsed(start,Clock::now()),s.fsr.creates,s.fsr.destroys);
  fclose(csv);return cancelled?2:0;
 }catch(const std::exception& e){printf("FAIL direct_ffx_cube %s\n",e.what());if(csv)fclose(csv);return 1;}
}
int wmain(){setvbuf(stdout,nullptr,_IONBF,0);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 try{
  W=option("WF_CUBE_WIDTH",128,32,3440);H=option("WF_CUBE_HEIGHT",96,24,1440);RW=option("WF_CUBE_RENDER_WIDTH",W/2,1,W);RH=option("WF_CUBE_RENDER_HEIGHT",H/2,1,H);
  MaxFrames=option("WF_CUBE_FRAMES",8,8,10000);MaxSeconds=option("WF_CUBE_SECONDS",20,1,60);Visible=option("WF_CUBE_VISIBLE",0,0,1)!=0;
  ViewW=std::min(W,UINT(1200));ViewH=std::max(1u,UINT(uint64_t(H)*ViewW/W));
  wchar_t dir[2048]{};const DWORD n=GetEnvironmentVariableW(L"FSR_LAB_OUTPUT_DIR",dir,2048);need(n&&n<2048,"output directory");
  WNDCLASSW wc{};wc.lpfnWndProc=proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"FFXGamepathCube";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);need(RegisterClassW(&wc)!=0,"window class");
  RECT rect={0,0,LONG(ViewW),LONG(ViewH+54)};const DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;AdjustWindowRect(&rect,style,FALSE);
  HWND window=CreateWindowW(wc.lpszClassName,L"Direct FFX cube | initializing",style|(Visible?WS_VISIBLE:0),CW_USEDEFAULT,CW_USEDEFAULT,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,wc.hInstance,nullptr);need(window,"window");
  HWND label=CreateWindowW(L"STATIC",L"Preparing D3D12 and FFX...",WS_CHILD|WS_VISIBLE,4,2,ViewW-8,50,window,nullptr,wc.hInstance,nullptr);
  HWND canvas=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE,0,54,ViewW,ViewH,window,nullptr,wc.hInstance,nullptr);need(label&&canvas,"child windows");
  int result=1;const std::wstring base=dir;
  std::thread render([&](){result=runScene(window,canvas,label,base);PostMessageW(window,WM_APP+1,0,0);});
  MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}stopRequested=true;render.join();return result;
 }catch(const std::exception& e){printf("FAIL startup %s\n",e.what());return 1;}
}

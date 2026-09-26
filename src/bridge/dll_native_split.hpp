// WineForge-Internal: fsr-lab/deferred-native-copy-list-insertion-v1.
// Splits the application command list around native Metal work and forwards
// subsequent commands to a continuation list. Only one recording is active.
#include <array>
#include <atomic>
#include <functional>
#include <vector>
#include <mutex>
struct DllNativeSplit {
 struct CpuTimes {
  double upstreamSubmit=0,upstreamWait=0,nativeTotal=0,downstreamSubmit=0,completionWait=0;
  static bool enabled(){static bool value=[](){char v[2]{};return GetEnvironmentVariableA("METAL_FSR4_TRACE",v,2)==1&&v[0]=='1';}();return value;}
  static long long now(){LARGE_INTEGER t{};QueryPerformanceCounter(&t);return t.QuadPart;}
  static double ms(long long ticks){static double f=[](){LARGE_INTEGER t{};QueryPerformanceFrequency(&t);return double(t.QuadPart);}();return ticks*1000.0/f;}
  void report(const void* context){
   static std::mutex mutex;std::lock_guard<std::mutex> lock(mutex);
   static unsigned count=0;static double sums[5]{},maxima[5]{};
   double values[]={upstreamSubmit,upstreamWait,nativeTotal,downstreamSubmit,completionWait};
   for(unsigned i=0;i<5;++i){sums[i]+=values[i];maxima[i]=std::max(maxima[i],values[i]);}
   if(++count!=120)return;
   fprintf(stderr,"BRIDGE_CPU_TIMING scope=all_completed_splits last_split=%p calls=%u window_end_qpc_ms=%.6f upstream_submit_mean_ms=%.6f upstream_submit_max_ms=%.6f upstream_wait_mean_ms=%.6f upstream_wait_max_ms=%.6f native_call_mean_ms=%.6f native_call_max_ms=%.6f downstream_submit_mean_ms=%.6f downstream_submit_max_ms=%.6f completion_wait_mean_ms=%.6f completion_wait_max_ms=%.6f\n",context,count,ms(now()),sums[0]/count,maxima[0],sums[1]/count,maxima[1],sums[2]/count,maxima[2],sums[3]/count,maxima[3],sums[4]/count,maxima[4]);
   count=0;for(unsigned i=0;i<5;++i)sums[i]=maxima[i]=0;
  }
 } cpuTimes;

 inline static DllNativeSplit* current=nullptr;
 ID3D12Device* device;ID3D12GraphicsCommandList* head;ID3D12CommandQueue* queue=nullptr;
 ID3D12CommandAllocator* allocator=nullptr;ID3D12GraphicsCommandList* tail=nullptr;ID3D12Fence* fence=nullptr;
 void** originalList;void** originalQueue;
 std::array<void*,81> listTable{};std::array<void*,19> queueTable{};
 std::function<void()> native;bool submitted=false,completed=false,abandoned=false;volatile LONG nativeCalls=0;
 unsigned long long seen[2]{};
 // WineForge-Internal: fsr4/continuation-hudless-capture-v1.
 struct ColorState {ID3D12Resource* resource;D3D12_RESOURCE_STATES state;};
 std::vector<ColorState> uiColors;bool captureUi=false,requireEarlyUi=false;
 inline static std::atomic<unsigned> missingUiBoundaryReports{0};
 inline static std::atomic<unsigned> capturedUiBoundaryReports{0};
 float sceneWidth=0,sceneHeight=0;bool fullViewport=false,fullScissor=false,sceneDrawn=false,uiBoundaryCaptured=false;
 D3D12_CPU_DESCRIPTOR_HANDLE boundRtv{};bool boundRtvValid=false;
 void observeColors(UINT count,const D3D12_RESOURCE_BARRIER* bars){
  if(!captureUi)return;
  for(UINT i=0;i<count;++i){const auto& b=bars[i];if(b.Type!=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION||b.Flags!=D3D12_RESOURCE_BARRIER_FLAG_NONE||b.Transition.Subresource!=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)continue;
   auto r=b.Transition.pResource;auto d=r->GetDesc();if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||(d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&d.Format!=DXGI_FORMAT_B8G8R8A8_UNORM))continue;
   bool found=false;for(auto& c:uiColors)if(c.resource==r){c.state=b.Transition.StateAfter;found=true;break;}if(!found&&uiColors.size()<64)uiColors.push_back({r,b.Transition.StateAfter});
  }
 }
 void captureBeforeClose(){
  if(!captureUi||requireEarlyUi){
   if(captureUi&&requireEarlyUi){const auto missing=++missingUiBoundaryReports;if(missing<=4||missing%120==0)printf("FG_HUDLESS_BOUNDARY missing=first_partial_ui fallback=real_frame count=%u\n",missing);}
   return;
  }
  using Capture=unsigned(WINAPI*)(ID3D12GraphicsCommandList*,ID3D12Resource*,D3D12_RESOURCE_STATES);
  auto fn=reinterpret_cast<Capture>(GetProcAddress(GetModuleHandleW(L"dxgi.dll"),"WfFgCaptureBeforeUi"));
  if(fn)for(const auto& c:uiColors)if(fn(tail,c.resource,c.state))break;
 }
 bool captureAtFirstUi(D3D12_CPU_DESCRIPTOR_HANDLE rtv={}){
  if(!captureUi||!sceneDrawn||uiBoundaryCaptured||!sceneWidth||!sceneHeight)return false;
  bool captured=false;
  if(rtv.ptr){
   using CaptureRtv=unsigned(WINAPI*)(ID3D12GraphicsCommandList*,ID3D12Device*,D3D12_CPU_DESCRIPTOR_HANDLE);
   auto fromRtv=reinterpret_cast<CaptureRtv>(GetProcAddress(GetModuleHandleW(L"dxgi.dll"),"WfFgCaptureBeforeUiRtv"));
   if(fromRtv)captured=fromRtv(tail,device,rtv)!=0;
  }
  using Capture=unsigned(WINAPI*)(ID3D12GraphicsCommandList*,ID3D12Resource*,D3D12_RESOURCE_STATES);
  auto fn=reinterpret_cast<Capture>(GetProcAddress(GetModuleHandleW(L"dxgi.dll"),"WfFgCaptureBeforeUiBoundary"));
  if(!captured&&fn)for(const auto& c:uiColors)if(fn(tail,c.resource,c.state)){captured=true;break;}
  if(captured){captureUi=false;uiColors.clear();uiBoundaryCaptured=true;const auto count=++capturedUiBoundaryReports;if(count<=4||count%120==0)printf("FG_HUDLESS_BOUNDARY detected=first_partial_ui count=%u\n",count);}
  return captured;
 }

 // WineForge-Internal: fsr4/bounded-post-upscale-ui-trace-v1.
 // One explicitly selected recording; read-only metadata, no state changes.
 inline static unsigned recordingCount=0;
 unsigned uiTraceFrame=0,uiTraceEvents=0;
 void traceResource(const char* role,ID3D12Resource* r){
  if(!uiTraceFrame||!r||uiTraceEvents>=512)return;
  ++uiTraceEvents;auto d=r->GetDesc();char name[160]{};UINT bytes=sizeof(name)-1;
  if(FAILED(r->GetPrivateData(WKPDID_D3DDebugObjectName,&bytes,name)))name[0]=0;
  for(auto& c:name)if(c&&static_cast<unsigned char>(c)<32)c='?';name[159]=0;
  printf("UI_TRACE frame=%u event=%u role=%s resource=%p dimension=%u format=%u size=%llux%u flags=%u name=%s\n",uiTraceFrame,uiTraceEvents,role,r,unsigned(d.Dimension),unsigned(d.Format),(unsigned long long)d.Width,d.Height,unsigned(d.Flags),name);
 }
 void traceOperation(unsigned slot,const char* name){
  if(!uiTraceFrame||uiTraceEvents>=512)return;
  printf("UI_TRACE frame=%u event=%u slot=%u operation=%s\n",uiTraceFrame,++uiTraceEvents,slot,name);
 }

 static DllNativeSplit& lookup(ID3D12GraphicsCommandList* list){need(current&&current->head==list,"continuation identity");return *current;}
 void note(unsigned slot,const char* name){traceOperation(slot,name);auto bit=1ull<<(slot%64);if(!(seen[slot/64]&bit)){seen[slot/64]|=bit;printf("CONTINUATION_API slot=%u name=%s\n",slot,name);fflush(stdout);}}
 static HRESULT STDMETHODCALLTYPE forwardClose(ID3D12GraphicsCommandList7 *This){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(9,"Close");s.captureBeforeClose();return s.tail->Close();}
 static HRESULT STDMETHODCALLTYPE forwardReset(ID3D12GraphicsCommandList7 *This, ID3D12CommandAllocator *allocator, ID3D12PipelineState *initial_state){
  auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(10,"Reset");need(!s.submitted,"submitted split reset");
  // A provider may abandon a generated frame while changing quality mode and
  // immediately reuse the same command-list object. Restore the real vtable
  // before forwarding Reset; the owner releases the discarded continuation
  // when its next callback observes `abandoned`.
  *reinterpret_cast<void***>(s.head)=s.originalList;s.abandoned=true;current=nullptr;
  puts("BRIDGE abandoned unsubmitted recording on command-list reset");
  return s.head->Reset(allocator,initial_state);
 }
 static void STDMETHODCALLTYPE forwardClearState(ID3D12GraphicsCommandList7 *This, ID3D12PipelineState *pipeline_state){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(11,"ClearState");s.tail->ClearState(pipeline_state);}
 static void STDMETHODCALLTYPE forwardDrawInstanced(ID3D12GraphicsCommandList7 *This, UINT vertex_count_per_instance, UINT instance_count, UINT start_vertex_location, UINT start_instance_location){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(12,"DrawInstanced");if((!s.fullViewport||!s.fullScissor)&&s.sceneDrawn)s.captureAtFirstUi(s.boundRtvValid?s.boundRtv:D3D12_CPU_DESCRIPTOR_HANDLE{});s.tail->DrawInstanced(vertex_count_per_instance, instance_count, start_vertex_location, start_instance_location);if(s.fullViewport&&s.fullScissor)s.sceneDrawn=true;}
 static void STDMETHODCALLTYPE forwardDrawIndexedInstanced(ID3D12GraphicsCommandList7 *This, UINT index_count_per_instance, UINT instance_count, UINT start_vertex_location, INT base_vertex_location, UINT start_instance_location){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(13,"DrawIndexedInstanced");if((!s.fullViewport||!s.fullScissor)&&s.sceneDrawn)s.captureAtFirstUi(s.boundRtvValid?s.boundRtv:D3D12_CPU_DESCRIPTOR_HANDLE{});s.tail->DrawIndexedInstanced(index_count_per_instance, instance_count, start_vertex_location, base_vertex_location, start_instance_location);if(s.fullViewport&&s.fullScissor)s.sceneDrawn=true;}
 static void STDMETHODCALLTYPE forwardDispatch(ID3D12GraphicsCommandList7 *This, UINT x, UINT u, UINT z){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(14,"Dispatch");s.tail->Dispatch(x, u, z);}
 static void STDMETHODCALLTYPE forwardCopyBufferRegion(ID3D12GraphicsCommandList7 *This, ID3D12Resource *dst_buffer, UINT64 dst_offset, ID3D12Resource *src_buffer, UINT64 src_offset, UINT64 byte_count){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(15,"CopyBufferRegion");s.tail->CopyBufferRegion(dst_buffer, dst_offset, src_buffer, src_offset, byte_count);}
 static void STDMETHODCALLTYPE forwardCopyTextureRegion(ID3D12GraphicsCommandList7 *This, const D3D12_TEXTURE_COPY_LOCATION *dst, UINT dst_x, UINT dst_y, UINT dst_z, const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *src_box){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(16,"CopyTextureRegion");if(dst)s.traceResource("copy_destination",dst->pResource);if(src)s.traceResource("copy_source",src->pResource);s.tail->CopyTextureRegion(dst, dst_x, dst_y, dst_z, src, src_box);}
 static void STDMETHODCALLTYPE forwardCopyResource(ID3D12GraphicsCommandList7 *This, ID3D12Resource *dst_resource, ID3D12Resource *src_resource){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(17,"CopyResource");s.traceResource("copy_destination",dst_resource);s.traceResource("copy_source",src_resource);s.tail->CopyResource(dst_resource, src_resource);}
 static void STDMETHODCALLTYPE forwardCopyTiles(ID3D12GraphicsCommandList7 *This, ID3D12Resource *tiled_resource, const D3D12_TILED_RESOURCE_COORDINATE *tile_region_start_coordinate, const D3D12_TILE_REGION_SIZE *tile_region_size, ID3D12Resource *buffer, UINT64 buffer_offset, D3D12_TILE_COPY_FLAGS flags){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(18,"CopyTiles");s.tail->CopyTiles(tiled_resource, tile_region_start_coordinate, tile_region_size, buffer, buffer_offset, flags);}
 static void STDMETHODCALLTYPE forwardResolveSubresource(ID3D12GraphicsCommandList7 *This, ID3D12Resource *dst_resource, UINT dst_sub_resource, ID3D12Resource *src_resource, UINT src_sub_resource, DXGI_FORMAT format){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(19,"ResolveSubresource");s.tail->ResolveSubresource(dst_resource, dst_sub_resource, src_resource, src_sub_resource, format);}
 static void STDMETHODCALLTYPE forwardIASetPrimitiveTopology(ID3D12GraphicsCommandList7 *This, D3D12_PRIMITIVE_TOPOLOGY primitive_topology){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(20,"IASetPrimitiveTopology");s.tail->IASetPrimitiveTopology(primitive_topology);}
 static void STDMETHODCALLTYPE forwardRSSetViewports(ID3D12GraphicsCommandList7 *This, UINT viewport_count, const D3D12_VIEWPORT *viewports){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(21,"RSSetViewports");bool full=false;if(viewport_count==1&&viewports){const auto& v=viewports[0];if(v.TopLeftX==0&&v.TopLeftY==0&&v.Width>0&&v.Height>0){if(!s.sceneWidth||(v.Width>=s.sceneWidth&&v.Height>=s.sceneHeight)){s.sceneWidth=v.Width;s.sceneHeight=v.Height;full=true;}else full=v.Width==s.sceneWidth&&v.Height==s.sceneHeight;}}s.fullViewport=full;s.tail->RSSetViewports(viewport_count, viewports);}
 static void STDMETHODCALLTYPE forwardRSSetScissorRects(ID3D12GraphicsCommandList7 *This, UINT rect_count, const D3D12_RECT *rects){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(22,"RSSetScissorRects");bool full=false;if(rect_count==1&&rects&&s.sceneWidth&&s.sceneHeight){const auto& r=rects[0];full=r.left<=0&&r.top<=0&&r.right>=LONG(s.sceneWidth)&&r.bottom>=LONG(s.sceneHeight);}s.fullScissor=full;s.tail->RSSetScissorRects(rect_count, rects);}
 static void STDMETHODCALLTYPE forwardOMSetBlendFactor(ID3D12GraphicsCommandList7 *This, const FLOAT blend_factor[4]){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(23,"OMSetBlendFactor");s.tail->OMSetBlendFactor(blend_factor);}
 static void STDMETHODCALLTYPE forwardOMSetStencilRef(ID3D12GraphicsCommandList7 *This, UINT stencil_ref){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(24,"OMSetStencilRef");s.tail->OMSetStencilRef(stencil_ref);}
 static void STDMETHODCALLTYPE forwardSetPipelineState(ID3D12GraphicsCommandList7 *This, ID3D12PipelineState *pipeline_state){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(25,"SetPipelineState");s.tail->SetPipelineState(pipeline_state);}
 static void STDMETHODCALLTYPE forwardResourceBarrier(ID3D12GraphicsCommandList7 *This, UINT barrier_count, const D3D12_RESOURCE_BARRIER *barriers){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(26,"ResourceBarrier");s.observeColors(barrier_count,barriers);if(s.uiTraceFrame)for(UINT i=0;i<barrier_count;++i)if(barriers[i].Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION){const auto& b=barriers[i].Transition;s.traceResource("transition",b.pResource);if(s.uiTraceEvents<512)printf("UI_TRACE states=%u->%u subresource=%u\n",unsigned(b.StateBefore),unsigned(b.StateAfter),b.Subresource);}s.tail->ResourceBarrier(barrier_count, barriers);}
 static void STDMETHODCALLTYPE forwardExecuteBundle(ID3D12GraphicsCommandList7 *This, ID3D12GraphicsCommandList *command_list){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(27,"ExecuteBundle");s.tail->ExecuteBundle(command_list);}
 static void STDMETHODCALLTYPE forwardSetDescriptorHeaps(ID3D12GraphicsCommandList7 *This, UINT heap_count, ID3D12DescriptorHeap *const *heaps){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(28,"SetDescriptorHeaps");s.tail->SetDescriptorHeaps(heap_count, heaps);}
 static void STDMETHODCALLTYPE forwardSetComputeRootSignature(ID3D12GraphicsCommandList7 *This, ID3D12RootSignature *root_signature){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(29,"SetComputeRootSignature");s.tail->SetComputeRootSignature(root_signature);}
 static void STDMETHODCALLTYPE forwardSetGraphicsRootSignature(ID3D12GraphicsCommandList7 *This, ID3D12RootSignature *root_signature){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(30,"SetGraphicsRootSignature");s.tail->SetGraphicsRootSignature(root_signature);}
 static void STDMETHODCALLTYPE forwardSetComputeRootDescriptorTable(ID3D12GraphicsCommandList7 *This, UINT root_parameter_index, D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(31,"SetComputeRootDescriptorTable");s.tail->SetComputeRootDescriptorTable(root_parameter_index, base_descriptor);}
 static void STDMETHODCALLTYPE forwardSetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList7 *This, UINT root_parameter_index, D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(32,"SetGraphicsRootDescriptorTable");s.tail->SetGraphicsRootDescriptorTable(root_parameter_index, base_descriptor);}
 static void STDMETHODCALLTYPE forwardSetComputeRoot32BitConstant(ID3D12GraphicsCommandList7 *This, UINT root_parameter_index, UINT data, UINT dst_offset){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(33,"SetComputeRoot32BitConstant");s.tail->SetComputeRoot32BitConstant(root_parameter_index, data, dst_offset);}
 static void STDMETHODCALLTYPE forwardSetGraphicsRoot32BitConstant(ID3D12GraphicsCommandList7 *This, UINT root_parameter_index, UINT data, UINT dst_offset){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(34,"SetGraphicsRoot32BitConstant");s.tail->SetGraphicsRoot32BitConstant(root_parameter_index, data, dst_offset);}
 static void STDMETHODCALLTYPE forwardSetComputeRoot32BitConstants(ID3D12GraphicsCommandList7 *This, UINT root_parameter_index, UINT constant_count, const void *data, UINT dst_offset){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(35,"SetComputeRoot32BitConstants");s.tail->SetComputeRoot32BitConstants(root_parameter_index, constant_count, data, dst_offset);}
 static void STDMETHODCALLTYPE forwardSetGraphicsRoot32BitConstants(ID3D12GraphicsCommandList7 *This, UINT root_parameter_index, UINT constant_count, const void *data, UINT dst_offset){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(36,"SetGraphicsRoot32BitConstants");s.tail->SetGraphicsRoot32BitConstants(root_parameter_index, constant_count, data, dst_offset);}
 static void STDMETHODCALLTYPE forwardSetComputeRootConstantBufferView(ID3D12GraphicsCommandList7 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(37,"SetComputeRootConstantBufferView");s.tail->SetComputeRootConstantBufferView(root_parameter_index, address);}
 static void STDMETHODCALLTYPE forwardSetGraphicsRootConstantBufferView(ID3D12GraphicsCommandList7 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(38,"SetGraphicsRootConstantBufferView");s.tail->SetGraphicsRootConstantBufferView(root_parameter_index, address);}
 static void STDMETHODCALLTYPE forwardSetComputeRootShaderResourceView(ID3D12GraphicsCommandList7 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(39,"SetComputeRootShaderResourceView");s.tail->SetComputeRootShaderResourceView(root_parameter_index, address);}
 static void STDMETHODCALLTYPE forwardSetGraphicsRootShaderResourceView(ID3D12GraphicsCommandList7 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(40,"SetGraphicsRootShaderResourceView");s.tail->SetGraphicsRootShaderResourceView(root_parameter_index, address);}
 static void STDMETHODCALLTYPE forwardSetComputeRootUnorderedAccessView(ID3D12GraphicsCommandList7 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(41,"SetComputeRootUnorderedAccessView");s.tail->SetComputeRootUnorderedAccessView(root_parameter_index, address);}
 static void STDMETHODCALLTYPE forwardSetGraphicsRootUnorderedAccessView(ID3D12GraphicsCommandList7 *This, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(42,"SetGraphicsRootUnorderedAccessView");s.tail->SetGraphicsRootUnorderedAccessView(root_parameter_index, address);}
 static void STDMETHODCALLTYPE forwardIASetIndexBuffer(ID3D12GraphicsCommandList7 *This, const D3D12_INDEX_BUFFER_VIEW *view){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(43,"IASetIndexBuffer");s.tail->IASetIndexBuffer(view);}
 static void STDMETHODCALLTYPE forwardIASetVertexBuffers(ID3D12GraphicsCommandList7 *This, UINT start_slot, UINT view_count, const D3D12_VERTEX_BUFFER_VIEW *views){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(44,"IASetVertexBuffers");s.tail->IASetVertexBuffers(start_slot, view_count, views);}
 static void STDMETHODCALLTYPE forwardSOSetTargets(ID3D12GraphicsCommandList7 *This, UINT start_slot, UINT view_count, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *views){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(45,"SOSetTargets");s.tail->SOSetTargets(start_slot, view_count, views);}
 static void STDMETHODCALLTYPE forwardOMSetRenderTargets(ID3D12GraphicsCommandList7 *This, UINT render_target_descriptor_count, const D3D12_CPU_DESCRIPTOR_HANDLE *render_target_descriptors, WINBOOL single_descriptor_handle, const D3D12_CPU_DESCRIPTOR_HANDLE *depth_stencil_descriptor){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(46,"OMSetRenderTargets");s.boundRtvValid=render_target_descriptor_count==1&&render_target_descriptors&&render_target_descriptors[0].ptr;s.boundRtv=s.boundRtvValid?render_target_descriptors[0]:D3D12_CPU_DESCRIPTOR_HANDLE{};s.tail->OMSetRenderTargets(render_target_descriptor_count, render_target_descriptors, single_descriptor_handle, depth_stencil_descriptor);}
 static void STDMETHODCALLTYPE forwardClearDepthStencilView(ID3D12GraphicsCommandList7 *This, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil, UINT rect_count, const D3D12_RECT *rects){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(47,"ClearDepthStencilView");s.tail->ClearDepthStencilView(dsv, flags, depth, stencil, rect_count, rects);}
 static void STDMETHODCALLTYPE forwardClearRenderTargetView(ID3D12GraphicsCommandList7 *This, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT rect_count, const D3D12_RECT *rects){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(48,"ClearRenderTargetView");if(rect_count&&rects)s.captureAtFirstUi(rtv);s.tail->ClearRenderTargetView(rtv, color, rect_count, rects);}
 static void STDMETHODCALLTYPE forwardClearUnorderedAccessViewUint(ID3D12GraphicsCommandList7 *This, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, ID3D12Resource *resource, const UINT values[4], UINT rect_count, const D3D12_RECT *rects){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(49,"ClearUnorderedAccessViewUint");s.tail->ClearUnorderedAccessViewUint(gpu_handle, cpu_handle, resource, values, rect_count, rects);}
 static void STDMETHODCALLTYPE forwardClearUnorderedAccessViewFloat(ID3D12GraphicsCommandList7 *This, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, ID3D12Resource *resource, const float values[4], UINT rect_count, const D3D12_RECT *rects){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(50,"ClearUnorderedAccessViewFloat");s.tail->ClearUnorderedAccessViewFloat(gpu_handle, cpu_handle, resource, values, rect_count, rects);}
 static void STDMETHODCALLTYPE forwardDiscardResource(ID3D12GraphicsCommandList7 *This, ID3D12Resource *resource, const D3D12_DISCARD_REGION *region){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(51,"DiscardResource");s.tail->DiscardResource(resource, region);}
 static void STDMETHODCALLTYPE forwardBeginQuery(ID3D12GraphicsCommandList7 *This, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(52,"BeginQuery");s.tail->BeginQuery(heap, type, index);}
 static void STDMETHODCALLTYPE forwardEndQuery(ID3D12GraphicsCommandList7 *This, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(53,"EndQuery");s.tail->EndQuery(heap, type, index);}
 static void STDMETHODCALLTYPE forwardResolveQueryData(ID3D12GraphicsCommandList7 *This, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT start_index, UINT query_count, ID3D12Resource *dst_buffer, UINT64 aligned_dst_buffer_offset){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(54,"ResolveQueryData");s.tail->ResolveQueryData(heap, type, start_index, query_count, dst_buffer, aligned_dst_buffer_offset);}
 static void STDMETHODCALLTYPE forwardSetPredication(ID3D12GraphicsCommandList7 *This, ID3D12Resource *buffer, UINT64 aligned_buffer_offset, D3D12_PREDICATION_OP operation){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(55,"SetPredication");s.tail->SetPredication(buffer, aligned_buffer_offset, operation);}
 static void STDMETHODCALLTYPE forwardSetMarker(ID3D12GraphicsCommandList7 *This, UINT metadata, const void *data, UINT size){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(56,"SetMarker");s.tail->SetMarker(metadata, data, size);}
 static void STDMETHODCALLTYPE forwardBeginEvent(ID3D12GraphicsCommandList7 *This, UINT metadata, const void *data, UINT size){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(57,"BeginEvent");s.tail->BeginEvent(metadata, data, size);}
 static void STDMETHODCALLTYPE forwardEndEvent(ID3D12GraphicsCommandList7 *This){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(58,"EndEvent");s.tail->EndEvent();}
 static void STDMETHODCALLTYPE forwardExecuteIndirect(ID3D12GraphicsCommandList7 *This, ID3D12CommandSignature *command_signature, UINT max_command_count, ID3D12Resource *arg_buffer, UINT64 arg_buffer_offset, ID3D12Resource *count_buffer, UINT64 count_buffer_offset){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(59,"ExecuteIndirect");s.tail->ExecuteIndirect(command_signature, max_command_count, arg_buffer, arg_buffer_offset, count_buffer, count_buffer_offset);}
 static void STDMETHODCALLTYPE forwardAtomicCopyBufferUINT(ID3D12GraphicsCommandList7 *This, ID3D12Resource *dst_buffer, UINT64 dst_offset, ID3D12Resource *src_buffer, UINT64 src_offset, UINT dependent_resource_count, ID3D12Resource *const *dependent_resources, const D3D12_SUBRESOURCE_RANGE_UINT64 *dependent_sub_resource_ranges){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(60,"AtomicCopyBufferUINT");ID3D12GraphicsCommandList1* extended=nullptr;check(s.tail->QueryInterface(IID_PPV_ARGS(&extended)),"continuation interface 1: AtomicCopyBufferUINT");extended->AtomicCopyBufferUINT(dst_buffer, dst_offset, src_buffer, src_offset, dependent_resource_count, dependent_resources, dependent_sub_resource_ranges);extended->Release();}
 static void STDMETHODCALLTYPE forwardAtomicCopyBufferUINT64(ID3D12GraphicsCommandList7 *This, ID3D12Resource *dst_buffer, UINT64 dst_offset, ID3D12Resource *src_buffer, UINT64 src_offset, UINT dependent_resource_count, ID3D12Resource *const *dependent_resources, const D3D12_SUBRESOURCE_RANGE_UINT64 *dependent_sub_resource_ranges){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(61,"AtomicCopyBufferUINT64");ID3D12GraphicsCommandList1* extended=nullptr;check(s.tail->QueryInterface(IID_PPV_ARGS(&extended)),"continuation interface 1: AtomicCopyBufferUINT64");extended->AtomicCopyBufferUINT64(dst_buffer, dst_offset, src_buffer, src_offset, dependent_resource_count, dependent_resources, dependent_sub_resource_ranges);extended->Release();}
 static void STDMETHODCALLTYPE forwardOMSetDepthBounds(ID3D12GraphicsCommandList7 *This, FLOAT min, FLOAT max){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(62,"OMSetDepthBounds");ID3D12GraphicsCommandList1* extended=nullptr;check(s.tail->QueryInterface(IID_PPV_ARGS(&extended)),"continuation interface 1: OMSetDepthBounds");extended->OMSetDepthBounds(min, max);extended->Release();}
 static void STDMETHODCALLTYPE forwardSetSamplePositions(ID3D12GraphicsCommandList7 *This, UINT sample_count, UINT pixel_count, D3D12_SAMPLE_POSITION *sample_positions){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(63,"SetSamplePositions");ID3D12GraphicsCommandList1* extended=nullptr;check(s.tail->QueryInterface(IID_PPV_ARGS(&extended)),"continuation interface 1: SetSamplePositions");extended->SetSamplePositions(sample_count, pixel_count, sample_positions);extended->Release();}
 static void STDMETHODCALLTYPE forwardResolveSubresourceRegion(ID3D12GraphicsCommandList7 *This, ID3D12Resource *dst_resource, UINT dst_sub_resource_idx, UINT dst_x, UINT dst_y, ID3D12Resource *src_resource, UINT src_sub_resource_idx, D3D12_RECT *src_rect, DXGI_FORMAT format, D3D12_RESOLVE_MODE mode){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(64,"ResolveSubresourceRegion");ID3D12GraphicsCommandList1* extended=nullptr;check(s.tail->QueryInterface(IID_PPV_ARGS(&extended)),"continuation interface 1: ResolveSubresourceRegion");extended->ResolveSubresourceRegion(dst_resource, dst_sub_resource_idx, dst_x, dst_y, src_resource, src_sub_resource_idx, src_rect, format, mode);extended->Release();}
 static void STDMETHODCALLTYPE forwardSetViewInstanceMask(ID3D12GraphicsCommandList7 *This, UINT mask){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(65,"SetViewInstanceMask");ID3D12GraphicsCommandList1* extended=nullptr;check(s.tail->QueryInterface(IID_PPV_ARGS(&extended)),"continuation interface 1: SetViewInstanceMask");extended->SetViewInstanceMask(mask);extended->Release();}
 static void STDMETHODCALLTYPE forwardWriteBufferImmediate(ID3D12GraphicsCommandList7 *This, UINT count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *parameters, const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(66,"WriteBufferImmediate");ID3D12GraphicsCommandList2* extended=nullptr;check(s.tail->QueryInterface(IID_PPV_ARGS(&extended)),"continuation interface 2: WriteBufferImmediate");extended->WriteBufferImmediate(count, parameters, modes);extended->Release();}
 static void STDMETHODCALLTYPE forwardSetProtectedResourceSession(ID3D12GraphicsCommandList7 *This, ID3D12ProtectedResourceSession *protected_resource_session){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(67,"SetProtectedResourceSession");ID3D12GraphicsCommandList3* extended=nullptr;check(s.tail->QueryInterface(IID_PPV_ARGS(&extended)),"continuation interface 3: SetProtectedResourceSession");extended->SetProtectedResourceSession(protected_resource_session);extended->Release();}
 static void STDMETHODCALLTYPE forwardBeginRenderPass(ID3D12GraphicsCommandList7 *This, UINT render_targets_count, const D3D12_RENDER_PASS_RENDER_TARGET_DESC *render_targets, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *depth_stencil, D3D12_RENDER_PASS_FLAGS flags){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(68,"BeginRenderPass");ID3D12GraphicsCommandList4* extended=nullptr;check(s.tail->QueryInterface(IID_PPV_ARGS(&extended)),"continuation interface 4: BeginRenderPass");extended->BeginRenderPass(render_targets_count, render_targets, depth_stencil, flags);extended->Release();}
 static void STDMETHODCALLTYPE forwardEndRenderPass(ID3D12GraphicsCommandList7 *This){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(69,"EndRenderPass");ID3D12GraphicsCommandList4* extended=nullptr;check(s.tail->QueryInterface(IID_PPV_ARGS(&extended)),"continuation interface 4: EndRenderPass");extended->EndRenderPass();extended->Release();}
 static void STDMETHODCALLTYPE forwardInitializeMetaCommand(ID3D12GraphicsCommandList7 *This, ID3D12MetaCommand *meta_command, const void *initialization_parameters_data, SIZE_T initialization_parameters_data_size_in_bytes){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(70,"InitializeMetaCommand");ID3D12GraphicsCommandList4* extended=nullptr;check(s.tail->QueryInterface(IID_PPV_ARGS(&extended)),"continuation interface 4: InitializeMetaCommand");extended->InitializeMetaCommand(meta_command, initialization_parameters_data, initialization_parameters_data_size_in_bytes);extended->Release();}
 static void STDMETHODCALLTYPE forwardExecuteMetaCommand(ID3D12GraphicsCommandList7 *This, ID3D12MetaCommand *meta_command, const void *execution_parameters_data, SIZE_T execution_parameters_data_size_in_bytes){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(71,"ExecuteMetaCommand");ID3D12GraphicsCommandList4* extended=nullptr;check(s.tail->QueryInterface(IID_PPV_ARGS(&extended)),"continuation interface 4: ExecuteMetaCommand");extended->ExecuteMetaCommand(meta_command, execution_parameters_data, execution_parameters_data_size_in_bytes);extended->Release();}
 static void STDMETHODCALLTYPE forwardBuildRaytracingAccelerationStructure(ID3D12GraphicsCommandList7 *This, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *desc, UINT postbuild_info_descs_count, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *postbuild_info_descs){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(72,"BuildRaytracingAccelerationStructure");ID3D12GraphicsCommandList4* extended=nullptr;check(s.tail->QueryInterface(IID_PPV_ARGS(&extended)),"continuation interface 4: BuildRaytracingAccelerationStructure");extended->BuildRaytracingAccelerationStructure(desc, postbuild_info_descs_count, postbuild_info_descs);extended->Release();}
 static void STDMETHODCALLTYPE forwardEmitRaytracingAccelerationStructurePostbuildInfo(ID3D12GraphicsCommandList7 *This, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *desc, UINT src_acceleration_structures_count, const D3D12_GPU_VIRTUAL_ADDRESS *src_acceleration_structure_data){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(73,"EmitRaytracingAccelerationStructurePostbuildInfo");ID3D12GraphicsCommandList4* extended=nullptr;check(s.tail->QueryInterface(IID_PPV_ARGS(&extended)),"continuation interface 4: EmitRaytracingAccelerationStructurePostbuildInfo");extended->EmitRaytracingAccelerationStructurePostbuildInfo(desc, src_acceleration_structures_count, src_acceleration_structure_data);extended->Release();}
 static void STDMETHODCALLTYPE forwardCopyRaytracingAccelerationStructure(ID3D12GraphicsCommandList7 *This, D3D12_GPU_VIRTUAL_ADDRESS dst_acceleration_structure_data, D3D12_GPU_VIRTUAL_ADDRESS src_acceleration_structure_data, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(74,"CopyRaytracingAccelerationStructure");ID3D12GraphicsCommandList4* extended=nullptr;check(s.tail->QueryInterface(IID_PPV_ARGS(&extended)),"continuation interface 4: CopyRaytracingAccelerationStructure");extended->CopyRaytracingAccelerationStructure(dst_acceleration_structure_data, src_acceleration_structure_data, mode);extended->Release();}
 static void STDMETHODCALLTYPE forwardSetPipelineState1(ID3D12GraphicsCommandList7 *This, ID3D12StateObject *state_object){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(75,"SetPipelineState1");ID3D12GraphicsCommandList4* extended=nullptr;check(s.tail->QueryInterface(IID_PPV_ARGS(&extended)),"continuation interface 4: SetPipelineState1");extended->SetPipelineState1(state_object);extended->Release();}
 static void STDMETHODCALLTYPE forwardDispatchRays(ID3D12GraphicsCommandList7 *This, const D3D12_DISPATCH_RAYS_DESC *desc){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(76,"DispatchRays");ID3D12GraphicsCommandList4* extended=nullptr;check(s.tail->QueryInterface(IID_PPV_ARGS(&extended)),"continuation interface 4: DispatchRays");extended->DispatchRays(desc);extended->Release();}
 static void STDMETHODCALLTYPE forwardRSSetShadingRate(ID3D12GraphicsCommandList7 *This, D3D12_SHADING_RATE base_shading_rate, const D3D12_SHADING_RATE_COMBINER *combiners){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(77,"RSSetShadingRate");ID3D12GraphicsCommandList5* extended=nullptr;check(s.tail->QueryInterface(IID_PPV_ARGS(&extended)),"continuation interface 5: RSSetShadingRate");extended->RSSetShadingRate(base_shading_rate, combiners);extended->Release();}
 static void STDMETHODCALLTYPE forwardRSSetShadingRateImage(ID3D12GraphicsCommandList7 *This, ID3D12Resource *shading_rate_image){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(78,"RSSetShadingRateImage");ID3D12GraphicsCommandList5* extended=nullptr;check(s.tail->QueryInterface(IID_PPV_ARGS(&extended)),"continuation interface 5: RSSetShadingRateImage");extended->RSSetShadingRateImage(shading_rate_image);extended->Release();}
 static void STDMETHODCALLTYPE forwardDispatchMesh(ID3D12GraphicsCommandList7 *This, UINT thread_group_count_x, UINT thread_group_count_y, UINT thread_group_count_z){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(79,"DispatchMesh");ID3D12GraphicsCommandList6* extended=nullptr;check(s.tail->QueryInterface(IID_PPV_ARGS(&extended)),"continuation interface 6: DispatchMesh");extended->DispatchMesh(thread_group_count_x, thread_group_count_y, thread_group_count_z);extended->Release();}
 static void STDMETHODCALLTYPE forwardBarrier(ID3D12GraphicsCommandList7 *This, UINT32 barrier_groups_count, const D3D12_BARRIER_GROUP *barrier_groups){auto& s=lookup(reinterpret_cast<ID3D12GraphicsCommandList*>(This));s.note(80,"Barrier");ID3D12GraphicsCommandList7* extended=nullptr;check(s.tail->QueryInterface(IID_PPV_ARGS(&extended)),"continuation interface 7: Barrier");extended->Barrier(barrier_groups_count, barrier_groups);extended->Release();}
 using Execute=void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList*const*);
 inline static Execute queueOriginal=nullptr;
 [[noreturn]] static void unsupported(){puts("FAIL unsupported continuation API");ExitProcess(71);}
 static HRESULT STDMETHODCALLTYPE close(ID3D12GraphicsCommandList* list){need(current&&list==current->head,"split close identity");return current->tail->Close();}
 static void STDMETHODCALLTYPE copyBuffer(ID3D12GraphicsCommandList* list,ID3D12Resource* dst,UINT64 off,ID3D12Resource* src,UINT64 srcOff,UINT64 bytes){
  need(current&&list==current->head,"split copy identity");current->tail->CopyBufferRegion(dst,off,src,srcOff,bytes);
 }
 static void STDMETHODCALLTYPE copyTexture(ID3D12GraphicsCommandList* list,const D3D12_TEXTURE_COPY_LOCATION* dst,UINT x,UINT y,UINT z,const D3D12_TEXTURE_COPY_LOCATION* src,const D3D12_BOX* box){
  need(current&&list==current->head,"split texture identity");current->tail->CopyTextureRegion(dst,x,y,z,src,box);
 }
 static void STDMETHODCALLTYPE barriers(ID3D12GraphicsCommandList* list,UINT n,const D3D12_RESOURCE_BARRIER* values){
  need(current&&list==current->head,"split barrier identity");current->tail->ResourceBarrier(n,values);
 }
 // WineForge-Internal: fsr-lab/submission-exception-boundary-v1.
 static void STDMETHODCALLTYPE execute(ID3D12CommandQueue* q,UINT n,ID3D12CommandList*const* lists) noexcept {
  try{executeImpl(q,n,lists);}catch(const std::exception& e){
   fprintf(stderr,"FAIL BRIDGE_SUBMIT reason=%s queue=%p count=%u\n",e.what(),static_cast<void*>(q),n);
   fflush(stderr);fflush(stdout);ExitProcess(86);
  }catch(...){fprintf(stderr,"FAIL BRIDGE_SUBMIT unknown exception count=%u\n",n);fflush(stderr);ExitProcess(86);}
 }
 static void executeImpl(ID3D12CommandQueue* q,UINT n,ID3D12CommandList*const* lists){
  if(!current){queueOriginal(q,n,lists);return;}
  auto& s=*current;bool matched=false;for(UINT i=0;i<n;++i)matched|=lists[i]==s.head;
  if(!matched){queueOriginal(q,n,lists);return;}
  need(!s.submitted,"tracked list submitted once");UINT index=0;while(index<n&&lists[index]!=s.head)++index;need(index<n,"matched index");for(UINT j=index+1;j<n;++j)need(lists[j]!=s.head,"no duplicate tracked list");s.queue=q;q->AddRef();s.submitted=true;

  // Restore physical head interface before giving it to the translation backend.
  *reinterpret_cast<void***>(s.head)=s.originalList;
  auto original=queueOriginal;auto t0=CpuTimes::enabled()?CpuTimes::now():0;original(q,index+1,lists);
  auto t1=t0?CpuTimes::now():0;
  check(q->Signal(s.fence,1),"bridge upstream signal");
  HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);need(event!=nullptr,"bridge event");
   check(s.fence->SetEventOnCompletion(1,event),"bridge upstream event");DWORD wait=WaitForSingleObject(event,10000);CloseHandle(event);
   if(wait!=WAIT_OBJECT_0||s.fence->GetCompletedValue()!=1){
    // Graceful degradation (was ExitProcess(72)): the upstream GPU work did not
    // complete in 10s, so the readback inputs are not valid for native. Orphan
    // the split instead of killing the process: skip native and the tail, keep
    // the provider's remaining lists flowing. The pair transport's fail-safe
    // head copy keeps the presented frame clean; FSR upscale frames may show a
    // stale texture for this pathological frame only.
    puts("BRIDGE_UPSTREAM_TIMEOUT orphaning split (native and tail skipped)");
    s.abandoned=true;if(current==&s)current=nullptr;
    if(index+1<n)original(q,n-index-1,lists+index+1);
    return;
   }
   auto t2=t0?CpuTimes::now():0;
   puts("BRIDGE upstream GPU complete before native");s.native();InterlockedIncrement(&s.nativeCalls);
  auto t3=t0?CpuTimes::now():0;
  ID3D12CommandList* continuation[]={s.tail};original(q,1,continuation);check(q->Signal(s.fence,2),"bridge completion fence");puts("BRIDGE native complete before continuation submission");if(index+1<n)original(q,n-index-1,lists+index+1);
  // WineForge-Internal: fsr-lab/detach-completed-list-generation-v1.
  // Only this recording belongs to the insertion. Caller may reset/reuse the
  // physical list before the next FFX dispatch releases our completion resources.
  if(t0){auto t4=CpuTimes::now();s.cpuTimes.upstreamSubmit=CpuTimes::ms(t1-t0);s.cpuTimes.upstreamWait=CpuTimes::ms(t2-t1);s.cpuTimes.nativeTotal=CpuTimes::ms(t3-t2);s.cpuTimes.downstreamSubmit=CpuTimes::ms(t4-t3);}
  if(current==&s)current=nullptr;
  puts("BRIDGE detached submitted recording; caller list reusable");
 }
 DllNativeSplit(ID3D12Device* d,ID3D12GraphicsCommandList* list,std::function<void()> callback,bool uiCaptureAllowed=true):device(d),head(list),native(callback){
  need(!current,"one split observer");
  char uiOption[2]{};captureUi=uiCaptureAllowed&&GetEnvironmentVariableA("METAL_FG_CAPTURE_HUDLESS",uiOption,2)==1&&uiOption[0]=='1';
  char earlyOption[2]{};requireEarlyUi=GetEnvironmentVariableA("METAL_FG_REQUIRE_EARLY_HUDLESS",earlyOption,2)==1&&earlyOption[0]=='1';
  const unsigned recording=++recordingCount;char traceValue[16]{};
  DWORD traceLength=GetEnvironmentVariableA("METAL_FG_UI_TRACE_FRAME",traceValue,sizeof(traceValue));
  if(traceLength&&traceLength<sizeof(traceValue)&&strtoul(traceValue,nullptr,10)==recording){uiTraceFrame=recording;printf("UI_TRACE begin frame=%u list=%p max_events=512\n",recording,head);}

  check(device->CreateCommandAllocator(head->GetType(),IID_PPV_ARGS(&allocator)),"bridge allocator");
  check(device->CreateCommandList(0,head->GetType(),allocator,nullptr,IID_PPV_ARGS(&tail)),"bridge continuation");
  check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"bridge fence");
  check(head->Close(),"bridge seal upstream");originalList=*reinterpret_cast<void***>(head);
  for(unsigned i=0;i<9;++i)listTable[i]=originalList[i];
  listTable[9]=reinterpret_cast<void*>(&forwardClose);
  listTable[10]=reinterpret_cast<void*>(&forwardReset);
  listTable[11]=reinterpret_cast<void*>(&forwardClearState);
  listTable[12]=reinterpret_cast<void*>(&forwardDrawInstanced);
  listTable[13]=reinterpret_cast<void*>(&forwardDrawIndexedInstanced);
  listTable[14]=reinterpret_cast<void*>(&forwardDispatch);
  listTable[15]=reinterpret_cast<void*>(&forwardCopyBufferRegion);
  listTable[16]=reinterpret_cast<void*>(&forwardCopyTextureRegion);
  listTable[17]=reinterpret_cast<void*>(&forwardCopyResource);
  listTable[18]=reinterpret_cast<void*>(&forwardCopyTiles);
  listTable[19]=reinterpret_cast<void*>(&forwardResolveSubresource);
  listTable[20]=reinterpret_cast<void*>(&forwardIASetPrimitiveTopology);
  listTable[21]=reinterpret_cast<void*>(&forwardRSSetViewports);
  listTable[22]=reinterpret_cast<void*>(&forwardRSSetScissorRects);
  listTable[23]=reinterpret_cast<void*>(&forwardOMSetBlendFactor);
  listTable[24]=reinterpret_cast<void*>(&forwardOMSetStencilRef);
  listTable[25]=reinterpret_cast<void*>(&forwardSetPipelineState);
  listTable[26]=reinterpret_cast<void*>(&forwardResourceBarrier);
  listTable[27]=reinterpret_cast<void*>(&forwardExecuteBundle);
  listTable[28]=reinterpret_cast<void*>(&forwardSetDescriptorHeaps);
  listTable[29]=reinterpret_cast<void*>(&forwardSetComputeRootSignature);
  listTable[30]=reinterpret_cast<void*>(&forwardSetGraphicsRootSignature);
  listTable[31]=reinterpret_cast<void*>(&forwardSetComputeRootDescriptorTable);
  listTable[32]=reinterpret_cast<void*>(&forwardSetGraphicsRootDescriptorTable);
  listTable[33]=reinterpret_cast<void*>(&forwardSetComputeRoot32BitConstant);
  listTable[34]=reinterpret_cast<void*>(&forwardSetGraphicsRoot32BitConstant);
  listTable[35]=reinterpret_cast<void*>(&forwardSetComputeRoot32BitConstants);
  listTable[36]=reinterpret_cast<void*>(&forwardSetGraphicsRoot32BitConstants);
  listTable[37]=reinterpret_cast<void*>(&forwardSetComputeRootConstantBufferView);
  listTable[38]=reinterpret_cast<void*>(&forwardSetGraphicsRootConstantBufferView);
  listTable[39]=reinterpret_cast<void*>(&forwardSetComputeRootShaderResourceView);
  listTable[40]=reinterpret_cast<void*>(&forwardSetGraphicsRootShaderResourceView);
  listTable[41]=reinterpret_cast<void*>(&forwardSetComputeRootUnorderedAccessView);
  listTable[42]=reinterpret_cast<void*>(&forwardSetGraphicsRootUnorderedAccessView);
  listTable[43]=reinterpret_cast<void*>(&forwardIASetIndexBuffer);
  listTable[44]=reinterpret_cast<void*>(&forwardIASetVertexBuffers);
  listTable[45]=reinterpret_cast<void*>(&forwardSOSetTargets);
  listTable[46]=reinterpret_cast<void*>(&forwardOMSetRenderTargets);
  listTable[47]=reinterpret_cast<void*>(&forwardClearDepthStencilView);
  listTable[48]=reinterpret_cast<void*>(&forwardClearRenderTargetView);
  listTable[49]=reinterpret_cast<void*>(&forwardClearUnorderedAccessViewUint);
  listTable[50]=reinterpret_cast<void*>(&forwardClearUnorderedAccessViewFloat);
  listTable[51]=reinterpret_cast<void*>(&forwardDiscardResource);
  listTable[52]=reinterpret_cast<void*>(&forwardBeginQuery);
  listTable[53]=reinterpret_cast<void*>(&forwardEndQuery);
  listTable[54]=reinterpret_cast<void*>(&forwardResolveQueryData);
  listTable[55]=reinterpret_cast<void*>(&forwardSetPredication);
  listTable[56]=reinterpret_cast<void*>(&forwardSetMarker);
  listTable[57]=reinterpret_cast<void*>(&forwardBeginEvent);
  listTable[58]=reinterpret_cast<void*>(&forwardEndEvent);
  listTable[59]=reinterpret_cast<void*>(&forwardExecuteIndirect);
  listTable[60]=reinterpret_cast<void*>(&forwardAtomicCopyBufferUINT);
  listTable[61]=reinterpret_cast<void*>(&forwardAtomicCopyBufferUINT64);
  listTable[62]=reinterpret_cast<void*>(&forwardOMSetDepthBounds);
  listTable[63]=reinterpret_cast<void*>(&forwardSetSamplePositions);
  listTable[64]=reinterpret_cast<void*>(&forwardResolveSubresourceRegion);
  listTable[65]=reinterpret_cast<void*>(&forwardSetViewInstanceMask);
  listTable[66]=reinterpret_cast<void*>(&forwardWriteBufferImmediate);
  listTable[67]=reinterpret_cast<void*>(&forwardSetProtectedResourceSession);
  listTable[68]=reinterpret_cast<void*>(&forwardBeginRenderPass);
  listTable[69]=reinterpret_cast<void*>(&forwardEndRenderPass);
  listTable[70]=reinterpret_cast<void*>(&forwardInitializeMetaCommand);
  listTable[71]=reinterpret_cast<void*>(&forwardExecuteMetaCommand);
  listTable[72]=reinterpret_cast<void*>(&forwardBuildRaytracingAccelerationStructure);
  listTable[73]=reinterpret_cast<void*>(&forwardEmitRaytracingAccelerationStructurePostbuildInfo);
  listTable[74]=reinterpret_cast<void*>(&forwardCopyRaytracingAccelerationStructure);
  listTable[75]=reinterpret_cast<void*>(&forwardSetPipelineState1);
  listTable[76]=reinterpret_cast<void*>(&forwardDispatchRays);
  listTable[77]=reinterpret_cast<void*>(&forwardRSSetShadingRate);
  listTable[78]=reinterpret_cast<void*>(&forwardRSSetShadingRateImage);
  listTable[79]=reinterpret_cast<void*>(&forwardDispatchMesh);
  listTable[80]=reinterpret_cast<void*>(&forwardBarrier);

  head->AddRef();current=this;*reinterpret_cast<void***>(head)=listTable.data();
  puts("BRIDGE recorded insertion; no GPU submission yet");
 }
 void completion(){const UINT64 value=2;auto start=CpuTimes::enabled()?CpuTimes::now():0;if(fence->GetCompletedValue()<value){HANDLE e=CreateEventW(nullptr,FALSE,FALSE,nullptr);need(e!=nullptr,"completion event");check(fence->SetEventOnCompletion(value,e),"completion event registration");DWORD w=WaitForSingleObject(e,10000);CloseHandle(e);if(w!=WAIT_OBJECT_0){puts("BRIDGE_COMPLETION_TIMEOUT orphaning split (was ExitProcess(83))");return;}}need(submitted&&nativeCalls==1,"one successful native insertion");if(start&&!completed){cpuTimes.completionWait=CpuTimes::ms(CpuTimes::now()-start);cpuTimes.report(this);}completed=true;}
 ~DllNativeSplit(){if(submitted&&!completed)puts("BRIDGE_ORPHAN_SPLIT reason=destroyed_before_completion (was ExitProcess(73); D3D12 defers release until GPU idle)");if(current==this){*reinterpret_cast<void***>(head)=originalList;current=nullptr;}if(queue)queue->Release();fence->Release();tail->Release();allocator->Release();head->Release();}
};

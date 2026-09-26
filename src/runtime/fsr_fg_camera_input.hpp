#pragma once
#include <cmath>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-pragmas"
#include "../vendor/fsr-sdk-2.3.0/framegeneration/include/ffx_framegeneration.h"
#pragma clang diagnostic pop

// Normalize application-owned SDK inputs without caching frames or retaining GPU objects.
// Unknown extensions remain unsupported instead of silently losing their meaning.
namespace fsrFgCamera {
enum class Status { camera, missing, invalid, unsupported };
struct Input {
 ffxDispatchDescFrameGenerationPrepareV2 prepare{};
 Status status=Status::unsupported;
 const char* reason="unsupported_prepare_descriptor";
};
inline bool valid(const ffxDispatchDescFrameGenerationPrepareV2& p){
 const float* axes[]={p.cameraUp,p.cameraRight,p.cameraForward};
 for(unsigned i=0;i<3;++i){
  if(!std::isfinite(p.cameraPosition[i]))return false;
  for(const auto* axis:axes)if(!std::isfinite(axis[i]))return false;
 }
 auto dot=[](const float* a,const float* b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
 for(unsigned i=0;i<3;++i){
  if(std::abs(dot(axes[i],axes[i])-1.f)>=.01f)return false;
  for(unsigned j=i+1;j<3;++j)if(std::abs(dot(axes[i],axes[j]))>=.01f)return false;
 }
 return true;
}
inline Input normalize(const ffxDispatchDescHeader* header){
 Input out;
 if(!header)return out;
 bool hasCamera=false;
 ffxApiHeader* backend=nullptr;
 // SDK api/include/dx12/ffx_api_dx12.h: BACKEND_DX12 has sub-ID 0x02.
 // Keep this normalizer platform-independent and preserve the caller-owned
 // terminal descriptor intact for the synchronous dispatch.
 constexpr uint64_t backendDx12=FFX_API_MAKE_BACKEND_SUB_ID(FFX_API_BACKEND_ID_DX12,0x02);
 if(header->type==FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2){
  out.prepare=*reinterpret_cast<const ffxDispatchDescFrameGenerationPrepareV2*>(header);
  hasCamera=true;
 }else if(header->type==FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE){
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  const auto& in=*reinterpret_cast<const ffxDispatchDescFrameGenerationPrepare*>(header);
#pragma clang diagnostic pop
  auto& p=out.prepare;
  p.frameID=in.frameID;p.commandList=in.commandList;p.renderSize=in.renderSize;
  p.jitterOffset=in.jitterOffset;p.motionVectorScale=in.motionVectorScale;
  p.frameTimeDelta=in.frameTimeDelta;p.reset=in.unused_reset;
  p.cameraNear=in.cameraNear;p.cameraFar=in.cameraFar;p.cameraFovAngleVertical=in.cameraFovAngleVertical;
  p.viewSpaceToMetersFactor=in.viewSpaceToMetersFactor;p.depth=in.depth;p.motionVectors=in.motionVectors;
 }else return out;
 const void* visited[8]={header};unsigned count=1;
 auto* node=header->pNext;
 while(node){
  for(unsigned i=0;i<count;++i)if(visited[i]==node){out.reason="cyclic_prepare_chain";return out;}
  if(count==8){out.reason="prepare_chain_too_long";return out;}
  visited[count++]=node;
  if(node->type==backendDx12){
   if(backend||node->pNext){out.reason="nonterminal_or_duplicate_backend";return out;}
   backend=node;node=nullptr;continue;
  }
  if(node->type!=FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO){out.reason="unsupported_prepare_extension";return out;}
  if(hasCamera){out.reason="duplicate_camera_input";return out;}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  const auto& in=*reinterpret_cast<const ffxDispatchDescFrameGenerationPrepareCameraInfo*>(node);
#pragma clang diagnostic pop
  for(unsigned i=0;i<3;++i){out.prepare.cameraPosition[i]=in.cameraPosition[i];out.prepare.cameraUp[i]=in.cameraUp[i];out.prepare.cameraRight[i]=in.cameraRight[i];out.prepare.cameraForward[i]=in.cameraForward[i];}
  hasCamera=true;node=node->pNext;
 }
 out.prepare.header={FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2,backend};
 out.status=!hasCamera?Status::missing:valid(out.prepare)?Status::camera:Status::invalid;
 out.reason=out.status==Status::camera?"camera_available":out.status==Status::missing?"camera_missing":"invalid_camera";
 return out;
}
}

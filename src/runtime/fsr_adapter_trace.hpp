#pragma once
#include <d3d12.h>
// WineForge-Internal: fsr-adapter/bounded-descriptor-trace-v1.
// Metadata only. No GPU resources are mapped, retained, or changed.
static SRWLOCK traceLock=SRWLOCK_INIT;
static HANDLE traceFile=INVALID_HANDLE_VALUE;
static unsigned traceEvents=0;
static unsigned long long dispatchCount=0,lastSignature=0;
static void traceWrite(const char* text){
 if(traceFile==INVALID_HANDLE_VALUE)return;
 DWORD wrote=0,n=DWORD(strlen(text));
 if(!WriteFile(traceFile,text,n,&wrote,nullptr)||wrote!=n){CloseHandle(traceFile);traceFile=INVALID_HANDLE_VALUE;}
}
static void traceInit(const std::wstring& directory,bool fsr4=false){
 wchar_t name[96];swprintf(name,96,L"wf-fsr-trace-%lu.log",GetCurrentProcessId());
 traceFile=CreateFileW((directory+name).c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
 traceWrite(fsr4?"WF_FSR_TRACE version=2 provider=fsr4_with_original_fallback max_events=128\n":"WF_FSR_TRACE version=2 provider=original_passthrough max_events=128\n");
}
static void traceResource(const char* name,const FfxApiResource& r){
 char line[320];const auto& d=r.description;
 snprintf(line,sizeof(line),"RESOURCE name=%s present=%u type=%u format=%u size=%ux%u depth=%u mips=%u flags=0x%x usage=0x%x state=0x%x\n",
  name,unsigned(r.resource!=nullptr),d.type,d.format,d.width,d.height,d.depth,d.mipCount,d.flags,d.usage,r.state);traceWrite(line);
 if(r.resource){const auto actual=static_cast<ID3D12Resource*>(r.resource)->GetDesc();
  snprintf(line,sizeof(line),"D3D12_RESOURCE name=%s dimension=%u format=%u size=%llux%u array_depth=%u mips=%u samples=%u quality=%u flags=0x%x\n",
   name,unsigned(actual.Dimension),unsigned(actual.Format),static_cast<unsigned long long>(actual.Width),actual.Height,unsigned(actual.DepthOrArraySize),unsigned(actual.MipLevels),actual.SampleDesc.Count,actual.SampleDesc.Quality,unsigned(actual.Flags));traceWrite(line);}
}
static void traceCreate(ffxContext* c,const ffxCreateContextDescHeader* d,ffxReturnCode_t rc){
 if(!d||d->type!=FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE)return;
 AcquireSRWLockExclusive(&traceLock);
 if(traceEvents<128){++traceEvents;const auto& x=*reinterpret_cast<const ffxCreateContextDescUpscale*>(d);char line[320];
  snprintf(line,sizeof(line),"CREATE context=%p rc=%u flags=0x%x max_render=%ux%u max_upscale=%ux%u\n",c?*c:nullptr,rc,x.flags,x.maxRenderSize.width,x.maxRenderSize.height,x.maxUpscaleSize.width,x.maxUpscaleSize.height);traceWrite(line);}
 ReleaseSRWLockExclusive(&traceLock);
}
static void traceDispatch(ffxContext* c,const ffxDispatchDescHeader* d){
 if(!d||d->type!=FFX_API_DISPATCH_DESC_TYPE_UPSCALE)return;
 AcquireSRWLockExclusive(&traceLock);
 ++dispatchCount;
 if(traceEvents>=128||traceFile==INVALID_HANDLE_VALUE){ReleaseSRWLockExclusive(&traceLock);return;}
 const auto& x=*reinterpret_cast<const ffxDispatchDescUpscale*>(d);
 unsigned long long sig=1469598103934665603ull;
 auto add=[&](unsigned v){sig=(sig^v)*1099511628211ull;};
 add(x.renderSize.width);add(x.renderSize.height);add(x.upscaleSize.width);add(x.upscaleSize.height);
 const FfxApiResource* resources[]={&x.color,&x.depth,&x.motionVectors,&x.exposure,&x.reactive,&x.transparencyAndComposition,&x.output};
 for(auto r:resources){add(unsigned(r->resource!=nullptr));add(r->description.format);add(r->description.width);add(r->description.height);add(r->description.usage);add(r->state);}
 if(dispatchCount<=2||sig!=lastSignature||dispatchCount%1024==0||x.reset){
  ++traceEvents;lastSignature=sig;char line[640];
  snprintf(line,sizeof(line),"DISPATCH call=%llu context=%p list_present=%u render=%ux%u upscale=%ux%u jitter=%g,%g mv_scale=%g,%g reset=%u pre_exposure=%g frame_ms=%g near=%g far=%g fov=%g sharpen=%u sharpness=%g flags=0x%x\n",
   dispatchCount,c?*c:nullptr,unsigned(x.commandList!=nullptr),x.renderSize.width,x.renderSize.height,x.upscaleSize.width,x.upscaleSize.height,x.jitterOffset.x,x.jitterOffset.y,x.motionVectorScale.x,x.motionVectorScale.y,unsigned(x.reset),x.preExposure,x.frameTimeDelta,x.cameraNear,x.cameraFar,x.cameraFovAngleVertical,unsigned(x.enableSharpening),x.sharpness,x.flags);
  traceWrite(line);const char* names[]={"color","depth","motion","exposure","reactive","composition","output"};
  for(unsigned i=0;i<7;++i)traceResource(names[i],*resources[i]);
  if(traceEvents==128)traceWrite("TRACE_LIMIT_REACHED original_passthrough_continues=1\n");
 }
 ReleaseSRWLockExclusive(&traceLock);
}
static inline void traceRoute(unsigned backend,unsigned long long fsr4Count,unsigned long long originalCount,const char* reason){
 AcquireSRWLockExclusive(&traceLock);static unsigned routes=0;
 if(traceFile!=INVALID_HANDLE_VALUE&&routes<128){++routes;char line[360];
  snprintf(line,sizeof(line),"ROUTE backend=%u fsr4_dispatches=%llu original_dispatches=%llu reason=%s\n",backend,fsr4Count,originalCount,reason);traceWrite(line);}
 ReleaseSRWLockExclusive(&traceLock);
}

// Separate budget: dispatch/resource traces must not hide menu configuration.
static void traceConfigure(ffxContext* c,const ffxConfigureDescHeader* d,
                           ffxReturnCode_t rc,bool tracked,bool disabled){
 AcquireSRWLockExclusive(&traceLock);static unsigned configurations=0;
 if(traceFile!=INVALID_HANDLE_VALUE&&configurations<128){
  ++configurations;char line[320];
  snprintf(line,sizeof(line),"CONFIGURE context=%p type=0x%llx chained=%u rc=%u tracked_upscaler=%u native_disabled=%u\n",
   c?*c:nullptr,static_cast<unsigned long long>(d?d->type:0),unsigned(d&&d->pNext),
   unsigned(rc),unsigned(tracked),unsigned(disabled));traceWrite(line);
  if(configurations==128)traceWrite("CONFIGURE_TRACE_LIMIT_REACHED\n");
 }
 ReleaseSRWLockExclusive(&traceLock);
}

#pragma once
#include <windows.h>
#include <atomic>
#include <cstring>
#include "../../src/vendor/streamline-camera/sl_core_types.h"
#include "../../src/runtime/fsr_fg_camera_input.hpp"

// Observes successful slSetConstants calls through the caller's existing import.
// The original Streamline call and its result remain authoritative.
namespace fsrSlCamera {
using SetConstants=sl::Result(*)(const sl::Constants&,const sl::FrameToken&,const sl::ViewportHandle&);
struct Sample {uint32_t frame=0,viewport=0;bool valid=false;ffxDispatchDescFrameGenerationPrepareV2 camera{};};
using Observer=void(*)(const Sample&) noexcept;
inline std::atomic<SetConstants> original{nullptr};
inline std::atomic<Observer> observer{nullptr};
using GetProcAddressFn=FARPROC(WINAPI*)(HMODULE,LPCSTR);
inline GetProcAddressFn systemGetProcAddress=&::GetProcAddress;
inline sl::Result intercept(const sl::Constants& values,const sl::FrameToken& frame,const sl::ViewportHandle& viewport){
 auto call=original.load();
 const auto result=call(values,frame,viewport);
 auto notify=observer.load();if(!notify)return result;
 Sample sample;sample.frame=uint32_t(frame);sample.viewport=uint32_t(viewport);
 if(result==sl::Result::eOk&&values.structType==sl::Constants::s_structType&&values.structVersion>=1&&values.structVersion<=2){
  auto copy=[](float* target,const sl::float3& value){target[0]=value.x;target[1]=value.y;target[2]=value.z;};
  copy(sample.camera.cameraPosition,values.cameraPos);copy(sample.camera.cameraUp,values.cameraUp);
  copy(sample.camera.cameraRight,values.cameraRight);copy(sample.camera.cameraForward,values.cameraFwd);
  sample.camera.frameID=sample.frame;sample.camera.reset=values.reset==sl::Boolean::eTrue;sample.camera.cameraNear=values.cameraNear;sample.camera.cameraFar=values.cameraFar;
  sample.camera.cameraFovAngleVertical=values.cameraFOV;
  sample.valid=fsrFgCamera::valid(sample.camera);
 }
 notify(sample);return result;
}
// The caller's GetProcAddress import is forwarded unchanged except for the one
// Streamline export. Capturing the system pointer before patching avoids recursion.
inline FARPROC WINAPI resolve(HMODULE module,LPCSTR name){
 auto result=systemGetProcAddress(module,name);const DWORD error=GetLastError();
 if(result&&reinterpret_cast<uintptr_t>(name)>0xffff&&name&&!strcmp(name,"slSetConstants")&&module==GetModuleHandleW(L"sl.interposer.dll")){
  auto fn=reinterpret_cast<SetConstants>(result);SetConstants expected=nullptr;
  if(original.compare_exchange_strong(expected,fn)||expected==fn){
   // GetProcAddress uses an erased function-pointer type on the Windows ABI.
   const SetConstants hook=&intercept;
   static_assert(sizeof(result)==sizeof(hook));
   std::memcpy(&result,&hook,sizeof(result));
  }
 }
 SetLastError(error);return result;
}
inline bool replace(void** slot,void* expected,void* desired){
 DWORD before=0;if(!VirtualProtect(slot,sizeof(*slot),PAGE_READWRITE,&before))return false;
 const bool changed=InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot),desired,expected)==expected;
 DWORD ignored=0;const bool restored=VirtualProtect(slot,sizeof(*slot),before,&ignored)!=FALSE;
 return changed&&restored;
}
// Call only for a known loaded PE module; never searches game memory for matrices.
// Supports direct imports and future GetProcAddress calls through this caller.
// Already cached pointers and custom/delay-load resolvers are not patched.
inline unsigned attach(HMODULE caller,Observer notify){
 if(!caller||!notify)return 0;
 auto dll=GetModuleHandleW(L"sl.interposer.dll");
 auto fn=dll?reinterpret_cast<SetConstants>(systemGetProcAddress(dll,"slSetConstants")):nullptr;
 SetConstants expected=nullptr;if(fn&&!original.compare_exchange_strong(expected,fn)&&expected!=fn)return 0;
 observer.store(notify);
 auto base=reinterpret_cast<unsigned char*>(caller);auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
 if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<0)return 0;
 auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
 if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)return 0;
 const size_t size=nt->OptionalHeader.SizeOfImage;
 auto range=[&](size_t offset,size_t bytes){return offset<size&&bytes<=size-offset;};
 auto directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
 if(!directory.VirtualAddress||!range(directory.VirtualAddress,directory.Size))return 0;
 auto imports=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+directory.VirtualAddress);unsigned hooked=0;
 for(size_t i=0;(i+1)*sizeof(*imports)<=directory.Size&&imports[i].Name;++i){
  const auto& entry=imports[i];if(!range(entry.Name,1)||!memchr(base+entry.Name,0,size-entry.Name))continue;
  const auto* library=reinterpret_cast<char*>(base+entry.Name);
  const bool streamline=!_stricmp(library,"sl.interposer.dll");
  const bool resolver=!_stricmp(library,"kernel32.dll")||!_stricmp(library,"kernelbase.dll");
  if(!streamline&&!resolver)continue;
  if(!entry.OriginalFirstThunk||!entry.FirstThunk)continue;
  for(size_t j=0;;++j){
   size_t lookup=entry.OriginalFirstThunk+j*sizeof(IMAGE_THUNK_DATA64),target=entry.FirstThunk+j*sizeof(IMAGE_THUNK_DATA64);
   if(!range(lookup,sizeof(IMAGE_THUNK_DATA64))||!range(target,sizeof(IMAGE_THUNK_DATA64)))break;
   const auto name=reinterpret_cast<IMAGE_THUNK_DATA64*>(base+lookup)->u1.AddressOfData;if(!name)break;
   if(IMAGE_SNAP_BY_ORDINAL64(name)||!range(size_t(name),sizeof(WORD)+1))continue;
   auto text=reinterpret_cast<char*>(base+name+sizeof(WORD));if(!memchr(text,0,size-size_t(name)-sizeof(WORD)))continue;
   void* before=nullptr;void* after=nullptr;
   if(streamline&&fn&&!strcmp(text,"slSetConstants")){before=reinterpret_cast<void*>(fn);after=reinterpret_cast<void*>(&intercept);}
   else if(resolver&&!strcmp(text,"GetProcAddress")){before=reinterpret_cast<void*>(systemGetProcAddress);after=reinterpret_cast<void*>(&resolve);}
   else continue;
   auto slot=reinterpret_cast<void**>(base+target);if(*slot==after)continue;
   if(replace(slot,before,after))++hooked;
  }
 }
 return hooked;
}
}

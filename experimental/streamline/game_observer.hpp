#pragma once
#include "frame_inputs.hpp"
namespace fsrSlGame {
inline fsrSlCamera::FrameInputs inputs;
inline std::atomic<unsigned> samples{0};
inline void observe(const fsrSlCamera::Sample& sample)noexcept{
 try {inputs.publish(sample);const unsigned count=++samples;
  if(count<=8||count%256==0)fprintf(stderr,"FG_STREAMLINE_CAMERA count=%u frame=%u viewport=%u valid=%u admitted=0 reason=no_resource_identity\n",count,sample.frame,sample.viewport,unsigned(sample.valid));
 }catch(...){fprintf(stderr,"FG_STREAMLINE_CAMERA dropped=1 reason=observer_exception\n");}
}
inline void attachCaller(void* address)noexcept{
 char value[2]{};if(GetEnvironmentVariableA("METAL_FG_STREAMLINE_PROBE",value,2)!=1||value[0]!='1')return;
 try {
  static std::mutex mutex;static HMODULE attached[16]{};static unsigned count=0;
  std::lock_guard<std::mutex> lock(mutex);
  HMODULE caller=nullptr;
  if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(address),&caller))return;
  for(unsigned i=0;i<count;++i)if(attached[i]==caller)return;
  if(count==16)return;
  // Pin the explicit caller for the lifetime of the installed IAT hook.
  if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(address),&caller))return;
  attached[count++]=caller;
  unsigned hooks=fsrSlCamera::attach(caller,observe);
  fprintf(stderr,"FG_STREAMLINE_ATTACH caller=%p hooks=%u interposer_loaded=%u camera_samples=%u mode=observe_only\n",static_cast<void*>(caller),hooks,unsigned(GetModuleHandleW(L"sl.interposer.dll")!=nullptr),samples.load());
 }catch(...){fprintf(stderr,"FG_STREAMLINE_ATTACH failed=1\n");}
}
}

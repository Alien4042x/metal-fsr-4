// WineForge-Internal: fsr4/dynamic-dxgi-entry-admission-v1.
// Forwards DXGI to the configured backend and wraps supported swapchains for FG.
#include <windows.h>
#include <dxgi1_6.h>
#include <mutex>
#include <cstdio>
#include "game_adapter.hpp"
namespace {
HMODULE backend=nullptr;
std::once_flag once;
FARPROC entry(const char* name) noexcept {
 try {std::call_once(once,[]{
  wchar_t path[32768]{};
  DWORD n=GetEnvironmentVariableW(L"METAL_FG_DXGI_BACKEND",path,32768);
  if(!n||n>=32768||!((path[0]&&path[1]==L':'&&(path[2]==L'\\'||path[2]==L'/'))||(path[0]==L'\\'&&path[1]==L'\\')))return;
  HMODULE self=nullptr;
  if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&entry),&self))return;
  HMODULE loaded=LoadLibraryExW(path,nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
  if(loaded==self){if(loaded)FreeLibrary(loaded);fprintf(stderr,"FG_DXGI_BACKEND rejected=self_recursion\n");return;}
  backend=loaded;
  fprintf(stderr,"FG_DXGI_BACKEND loaded=%u error=%lu\n",unsigned(backend!=nullptr),backend?0ul:GetLastError());
 });}catch(...){return nullptr;}
 return backend?GetProcAddress(backend,name):nullptr;
}
}
extern "C" HRESULT WINAPI wfCreateDXGIFactory(REFIID iid,void** out){
 auto fn=reinterpret_cast<HRESULT(WINAPI*)(REFIID,void**)>(entry("CreateDXGIFactory"));
 if(!fn){if(out)*out=nullptr;return DXGI_ERROR_UNSUPPORTED;}
 const auto hr=fn(iid,out);fprintf(stderr,"FG_DXGI_FACTORY version=0 hr=%08lx\n",(unsigned long)hr);if(SUCCEEDED(hr))wfGameFg::wrapFactory(iid,out);return hr;
}
extern "C" HRESULT WINAPI wfCreateDXGIFactory1(REFIID iid,void** out){
 auto fn=reinterpret_cast<HRESULT(WINAPI*)(REFIID,void**)>(entry("CreateDXGIFactory1"));
 if(!fn){if(out)*out=nullptr;return DXGI_ERROR_UNSUPPORTED;}
 const auto hr=fn(iid,out);fprintf(stderr,"FG_DXGI_FACTORY version=1 hr=%08lx\n",(unsigned long)hr);if(SUCCEEDED(hr))wfGameFg::wrapFactory(iid,out);return hr;
}
extern "C" HRESULT WINAPI wfCreateDXGIFactory2(UINT flags,REFIID iid,void** out){
 auto fn=reinterpret_cast<HRESULT(WINAPI*)(UINT,REFIID,void**)>(entry("CreateDXGIFactory2"));
 if(!fn){if(out)*out=nullptr;return DXGI_ERROR_UNSUPPORTED;}
 const auto hr=fn(flags,iid,out);fprintf(stderr,"FG_DXGI_FACTORY version=2 hr=%08lx\n",(unsigned long)hr);if(SUCCEEDED(hr))wfGameFg::wrapFactory(iid,out);return hr;
}
extern "C" HRESULT WINAPI wfDXGIGetDebugInterface1(UINT flags,REFIID iid,void** out){
 auto fn=reinterpret_cast<HRESULT(WINAPI*)(UINT,REFIID,void**)>(entry("DXGIGetDebugInterface1"));
 if(!fn){if(out)*out=nullptr;return DXGI_ERROR_UNSUPPORTED;}return fn(flags,iid,out);
}
extern "C" HRESULT WINAPI wfDXGIDeclareAdapterRemovalSupport(){
 auto fn=reinterpret_cast<HRESULT(WINAPI*)()>(entry("DXGIDeclareAdapterRemovalSupport"));return fn?fn():DXGI_ERROR_UNSUPPORTED;
}
extern "C" HRESULT WINAPI wfDXGID3D10CreateDevice(HMODULE module,IDXGIFactory* factory,IDXGIAdapter* adapter,UINT flags,const void* levels,UINT count,void** out){
 auto fn=reinterpret_cast<HRESULT(WINAPI*)(HMODULE,IDXGIFactory*,IDXGIAdapter*,UINT,const void*,UINT,void**)>(entry("DXGID3D10CreateDevice"));
 if(!fn){if(out)*out=nullptr;return DXGI_ERROR_UNSUPPORTED;}return fn(module,factory,adapter,flags,levels,count,out);
}
extern "C" HRESULT WINAPI wfDXGID3D10RegisterLayers(const void* layers,UINT count){
 auto fn=reinterpret_cast<HRESULT(WINAPI*)(const void*,UINT)>(entry("DXGID3D10RegisterLayers"));return fn?fn(layers,count):DXGI_ERROR_UNSUPPORTED;
}

#include <windows.h>
#include <winternl.h>
#include <cwchar>
#include <cstdio>
#include "fsr4_full_metal_abi.h"
// WineForge-Internal: fsr-lab/temporal-scene-bridge-v1.
// Retain unixlib across frames so pipelines and temporal buffers remain live.
extern "C" __declspec(dllexport) int WINAPI LabFullFrame(const wchar_t* path,FullMetalArgs* a){
 using Query=LONG(NTAPI*)(HANDLE,const void*,int,void*,SIZE_T,SIZE_T*);
 using Call=LONG(WINAPI*)(uint64_t,unsigned,void*);
 static uint64_t handles[2]={};auto nt=GetModuleHandleW(L"ntdll.dll");
 auto query=(Query)GetProcAddress(nt,"NtQueryVirtualMemory");auto call=(Call)GetProcAddress(nt,"__wine_unix_call");
 if(!query||!call||!a||a->version!=1||a->size!=sizeof(*a))return 10;
 if(a->op==0){if(handles[0]||!path)return 11;size_t n=wcslen(path)*sizeof(wchar_t);if(n>65532)return 12;
  UNICODE_STRING name={USHORT(n),USHORT(n+2),const_cast<wchar_t*>(path)};
  LONG rc=query(GetCurrentProcess(),&name,1002,handles,sizeof(handles),nullptr);if(rc||!handles[0]||!handles[1])return 13;
 }
 if(!handles[1])return 14;
 LONG rc=call(handles[1],0,a);
 if(a->pending)return 15;
 if(a->op==2||rc){LONG unload=query(GetCurrentProcess(),&handles[0],1004,nullptr,0,nullptr);handles[0]=handles[1]=0;if(unload)return 16;}
 return rc?17:0;
}

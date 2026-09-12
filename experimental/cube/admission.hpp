#pragma once
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-pragmas"
#include "../../src/vendor/fsr-sdk-2.3.0/framegeneration/include/ffx_framegeneration.h"
#pragma clang diagnostic pop
// WineForge-Internal: fsr4/analytical-framegeneration-admission-v1.
static void frameGenerationAdmission(ID3D12Device* device) {
 HMODULE module=LoadLibraryExW(L"amd_fidelityfx_framegeneration_dx12.dll",nullptr,LOAD_LIBRARY_SEARCH_APPLICATION_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
 if(!module)printf("FG_LOAD_ERROR win32=%lu\n",GetLastError());
 need(module!=nullptr,"frame generation DLL load");
 auto query=reinterpret_cast<PfnFfxQuery>(GetProcAddress(module,"ffxQuery"));
 auto create=reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(module,"ffxCreateContext"));
 auto destroy=reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(module,"ffxDestroyContext"));
 need(query&&create&&destroy,"frame generation exports");
 uint64_t count=0,ids[32]{};const char* names[32]{};
 ffxQueryDescGetVersions versions{};versions.header.type=FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
 versions.createDescType=FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;versions.device=device;versions.outputCount=&count;
 auto rc=query(nullptr,&versions.header);printf("FG_QUERY_COUNT rc=%u count=%llu\n",unsigned(rc),(unsigned long long)count);
 need(rc==0&&count>0&&count<=32,"frame generation provider count");
 versions.versionIds=ids;versions.versionNames=names;need(query(nullptr,&versions.header)==0&&count<=32,"frame generation provider list");
 uint64_t selected=0;unsigned matches=0;
 for(uint64_t i=0;i<count;++i){printf("FG_PROVIDER id=%llu name=%s\n",(unsigned long long)ids[i],names[i]?names[i]:"null");if(names[i]&&strcmp(names[i],"3.1.6")==0){selected=ids[i];++matches;}}
 need(matches==1,"unique analytical3.1.6 provider");
 ffxOverrideVersion version{};version.header.type=FFX_API_DESC_TYPE_OVERRIDE_VERSION;version.versionId=selected;
 ffxCreateContextDescFrameGenerationVersion abi{};abi.header={FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION,&version.header};abi.version=FFX_FRAMEGENERATION_VERSION;
 ffxCreateBackendDX12Desc backend{};backend.header={FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12,&abi.header};backend.device=device;
 ffxCreateContextDescFrameGeneration desc{};desc.header={FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION,&backend.header};
 desc.flags=FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;desc.displaySize={W,H};desc.maxRenderSize={RW,RH};desc.backBufferFormat=FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
 ffxContext context=nullptr;rc=create(&context,&desc.header,nullptr);printf("FG_CREATE rc=%u context=%p\n",unsigned(rc),context);fflush(stdout);
 need(rc==0&&context,"frame generation context");
 ffxQueryGetProviderVersion actual{};actual.header.type=FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
 need(query(&context,&actual.header)==0&&actual.versionId==selected,"actual analytical provider");
 need(destroy(&context,nullptr)==0,"frame generation destroy");FreeLibrary(module);
 printf("FG_ADMISSION_PASS generated_frames=0 actual_version=3.1.6\n");
}

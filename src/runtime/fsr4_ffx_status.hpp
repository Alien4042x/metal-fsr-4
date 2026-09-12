#pragma once
#include <cstdint>
inline constexpr uint64_t wfFsr4VersionId=0x5746465352340002ull;
struct WfFsr4Status {
    uint32_t size=sizeof(WfFsr4Status),backend=0;
    uint64_t fsr4Dispatches=0,originalDispatches=0,allocatedBytes=0;
    char reason[192]{};
};
using PfnWfFsr4Status=uint32_t (*)(void**,WfFsr4Status*);
using PfnWfFsr4HistoryReadback=uint32_t (*)(void**,void*,void*,uint32_t,uint32_t);
using PfnWfFsr4TensorReadback=uint32_t (*)(void**,void*,void*,uint32_t,uint32_t,uint32_t);

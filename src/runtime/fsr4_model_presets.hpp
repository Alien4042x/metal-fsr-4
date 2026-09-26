#pragma once
#include <cstdint>
// WineForge-Internal: fsr4/trained-model-preset-routing-v1.
// Named scale ratios only. Custom/DRS require a separately qualified model.
enum class Fsr4ModelPreset : unsigned { NativeAA, Quality, Balanced, Performance, UltraPerformance, Unsupported };
inline const char* fsr4ModelName(Fsr4ModelPreset preset) {
    constexpr const char* names[]={"native","quality","balanced","performance","ultraperf"};
    return unsigned(preset)<5 ? names[unsigned(preset)] : "unsupported";
}
inline Fsr4ModelPreset fsr4ModelForSize(unsigned w,unsigned h,unsigned rw,unsigned rh) {
    if(!w||!h||!rw||!rh||rw>w||rh>h) return Fsr4ModelPreset::Unsupported;
    constexpr unsigned numerators[]={1,2,10,1,1}, denominators[]={1,3,17,2,3};
    auto matches=[](unsigned output,unsigned input,unsigned n,unsigned d){
        const uint64_t scaled=uint64_t(output)*n;
        return input==scaled/d || input==(scaled+d-1)/d;
    };
    for(unsigned i=0;i<5;++i)
        if(matches(w,rw,numerators[i],denominators[i]) && matches(h,rh,numerators[i],denominators[i]))
            return Fsr4ModelPreset(i);
    return Fsr4ModelPreset::Unsupported;
}
inline float fsr4Pass0Scale(Fsr4ModelPreset preset) {
    constexpr float scales[]={0.014608594588935375f,0.014884727075695992f,0.012475206516683102f,0.014811458997428417f,0.011951033025979996f};
    return unsigned(preset)<5 ? scales[unsigned(preset)] : 0.f;
}

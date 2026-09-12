#pragma once
#include <memory>
#include <string>
#include <d3d12.h>
#include "../vendor/fsr-sdk-2.3.0/upscalers/include/ffx_upscale.h"

// WineForge-Internal: fsr4/context-owned-game-resources-v1.
// Caller must serialize the context's GPU work and complete it before destroy,
// as for the original D3D12 effect resources. No private queue or CPU wait.
class Fsr4D3D12Context {
public:
    Fsr4D3D12Context(ID3D12Device*, const ffxCreateContextDescUpscale&, const std::wstring& compilerPath);
    ~Fsr4D3D12Context();
    Fsr4D3D12Context(const Fsr4D3D12Context&) = delete;
    Fsr4D3D12Context& operator=(const Fsr4D3D12Context&) = delete;
    // false means unsupported BEFORE commands were recorded; use original FSR.
    bool dispatch(const ffxDispatchDescUpscale&, std::string& reason);
    void invalidateHistory() noexcept;
    uint64_t allocatedBytes() const noexcept;
    uint64_t dispatchCount() const noexcept;
#ifdef WF_FSR4_GPU_PROFILE
    bool setProfileHeap(ID3D12QueryHeap*);
    bool setAmplification(unsigned group,unsigned count);
#endif
    // Explicit diagnostic only: append a history copy to caller's READBACK buffer.
    // Normal FFX dispatch never invokes this and still performs no CPU readback.
    bool recordHistoryReadback(ID3D12GraphicsCommandList*, ID3D12Resource*, uint32_t width, uint32_t height);
    bool recordTensorReadback(ID3D12GraphicsCommandList*, ID3D12Resource*, uint32_t index, uint32_t width, uint32_t height);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

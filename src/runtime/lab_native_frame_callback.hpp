#pragma once
#include <d3d12.h>
#include "fsr4_full_metal_abi.h"
struct LabNativeFramePacket {
 Fsr4FrameParams params{};
 ID3D12Resource* resources[5]{};
 D3D12_RESOURCE_STATES states[5]{};
};
// ONLY the Scene owning this command list may register a synchronous callback.
using LabNativeFrameCallback=HRESULT(*)(void*,ID3D12GraphicsCommandList*,const LabNativeFramePacket&);
inline LabNativeFrameCallback labNativeFrameCallback=nullptr;
inline void* labNativeFrameOwner=nullptr;

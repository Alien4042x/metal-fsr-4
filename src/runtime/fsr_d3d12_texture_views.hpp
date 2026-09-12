#pragma once
// WineForge-Internal: fsr4/explicit-dxgi-texture-view-selection-v1.
// FFX format numbers are lossy: inspect the actual D3D12 resource description.
enum class FsrTextureRole {Color,Depth,Motion,Exposure};
static DXGI_FORMAT fsrTextureView(DXGI_FORMAT resource,FsrTextureRole role){
    if(role==FsrTextureRole::Color){switch(resource){
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:case DXGI_FORMAT_R16G16B16A16_FLOAT:return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:case DXGI_FORMAT_R32G32B32A32_FLOAT:return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default:return DXGI_FORMAT_UNKNOWN;}}
    if(role==FsrTextureRole::Motion){switch(resource){
        case DXGI_FORMAT_R16G16_TYPELESS:case DXGI_FORMAT_R16G16_FLOAT:return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R32G32_TYPELESS:case DXGI_FORMAT_R32G32_FLOAT:return DXGI_FORMAT_R32G32_FLOAT;
        default:return DXGI_FORMAT_UNKNOWN;}}
    if(role==FsrTextureRole::Exposure && (resource==DXGI_FORMAT_R16_FLOAT||resource==DXGI_FORMAT_R16_TYPELESS))return DXGI_FORMAT_R16_FLOAT;
    if(resource==DXGI_FORMAT_R32_FLOAT||resource==DXGI_FORMAT_R32_TYPELESS)return DXGI_FORMAT_R32_FLOAT;
    if(role==FsrTextureRole::Depth){switch(resource){
        case DXGI_FORMAT_D32_FLOAT:return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R24G8_TYPELESS:case DXGI_FORMAT_D24_UNORM_S8_UINT:case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        default:return DXGI_FORMAT_UNKNOWN;}}
    return DXGI_FORMAT_UNKNOWN;
}

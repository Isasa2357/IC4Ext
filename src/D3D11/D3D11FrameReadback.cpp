#include "IC4Ext/D3D11/D3D11FrameReadback.hpp"

#include <sstream>
#include <string>

namespace IC4Ext {

namespace {

std::string HrToString(HRESULT hr)
{
    std::ostringstream oss;
    oss << "HRESULT=0x" << std::hex << static_cast<unsigned long>(hr);
    return oss.str();
}

bool DxgiToGpuFrameFormat(DXGI_FORMAT dxgi, GpuFrameFormat& out) noexcept
{
    switch (dxgi) {
    case DXGI_FORMAT_R8_UNORM:
        out = GpuFrameFormat::R8;
        return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        out = GpuFrameFormat::RGBA8;
        return true;
    default:
        return false;
    }
}

std::uint64_t EstimateTextureBytes(const D3D11_TEXTURE2D_DESC& desc) noexcept
{
    const std::uint64_t bpp = desc.Format == DXGI_FORMAT_R8_UNORM ? 1ull :
                              desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? 4ull : 0ull;
    return static_cast<std::uint64_t>(desc.Width) *
           static_cast<std::uint64_t>(desc.Height) *
           static_cast<std::uint64_t>(desc.ArraySize) *
           bpp;
}

bool SameTextureDesc(const D3D11_TEXTURE2D_DESC& a, const D3D11_TEXTURE2D_DESC& b) noexcept
{
    return a.Width == b.Width &&
           a.Height == b.Height &&
           a.MipLevels == b.MipLevels &&
           a.ArraySize == b.ArraySize &&
           a.Format == b.Format &&
           a.SampleDesc.Count == b.SampleDesc.Count &&
           a.SampleDesc.Quality == b.SampleDesc.Quality &&
           a.Usage == b.Usage &&
           a.BindFlags == b.BindFlags &&
           a.CPUAccessFlags == b.CPUAccessFlags &&
           a.MiscFlags == b.MiscFlags;
}

D3D11_TEXTURE2D_DESC MakeStagingDesc(D3D11_TEXTURE2D_DESC desc) noexcept
{
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.Usage = D3D11_USAGE_STAGING;
    return desc;
}

} // namespace

void D3D11FrameReadback::setError(ErrorCode code, const std::string& where, const std::string& message)
{
    lastError_ = MakeError(code, where, message);
}

bool D3D11FrameReadback::initialize(D3D11CoreLib::D3D11Core* core)
{
    lastError_ = NoError();
    resetCache();
    if (!core) {
        setError(ErrorCode::InvalidArgument, "D3D11FrameReadback::initialize", "D3D11Core must be non-null");
        return false;
    }
    core_ = core;
    device_ = core->GetDevice();
    context_ = core->GetImmediateContext();
    if (!device_ || !context_) {
        setError(ErrorCode::InvalidArgument, "D3D11FrameReadback::initialize", "D3D11Core has null device/context");
        return false;
    }
    return true;
}

bool D3D11FrameReadback::initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    lastError_ = NoError();
    resetCache();
    core_ = nullptr;
    if (!device || !context) {
        setError(ErrorCode::InvalidArgument, "D3D11FrameReadback::initialize", "device and context must be non-null");
        return false;
    }
    device_ = device;
    context_ = context;
    return true;
}

void D3D11FrameReadback::resetCache() noexcept
{
    stagingTexture_.Reset();
    stagingDesc_ = {};
    hasStagingDesc_ = false;
    cacheStats_ = {};
}

bool D3D11FrameReadback::validateFrame(const D3D11CameraFrame& frame,
                                       GpuFrameFormat& srcFormat,
                                       D3D11_TEXTURE2D_DESC& desc)
{
    if (!device_ || !context_) {
        setError(ErrorCode::D3D11Error, "D3D11FrameReadback::readback", "readback is not initialized");
        return false;
    }
    if (!frame.texture) {
        setError(ErrorCode::InvalidArgument, "D3D11FrameReadback::readback", "frame texture is null");
        return false;
    }
    desc = {};
    frame.texture->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0) {
        setError(ErrorCode::InvalidArgument, "D3D11FrameReadback::readback", "frame texture has invalid size");
        return false;
    }
    if (desc.ArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1) {
        setError(ErrorCode::UnsupportedFormat, "D3D11FrameReadback::readback", "only single-subresource non-MSAA Texture2D frames are supported");
        return false;
    }
    if (!DxgiToGpuFrameFormat(desc.Format, srcFormat)) {
        setError(ErrorCode::UnsupportedFormat, "D3D11FrameReadback::readback", "only DXGI_FORMAT_R8_UNORM and DXGI_FORMAT_R8G8B8A8_UNORM are supported");
        return false;
    }
    return true;
}

bool D3D11FrameReadback::ensureStagingTexture(const D3D11_TEXTURE2D_DESC& sourceDesc)
{
    const D3D11_TEXTURE2D_DESC desired = MakeStagingDesc(sourceDesc);
    if (stagingTexture_ && hasStagingDesc_ && SameTextureDesc(stagingDesc_, desired)) {
        ++cacheStats_.cacheHits;
        return true;
    }

    ++cacheStats_.cacheMisses;
    stagingTexture_.Reset();
    hasStagingDesc_ = false;

    HRESULT hr = device_->CreateTexture2D(&desired, nullptr, stagingTexture_.GetAddressOf());
    if (FAILED(hr)) {
        setError(ErrorCode::D3D11Error, "D3D11FrameReadback::CreateTexture2D", HrToString(hr));
        return false;
    }

    stagingDesc_ = desired;
    hasStagingDesc_ = true;
    ++cacheStats_.resourceRebuilds;
    cacheStats_.bytesAllocated = EstimateTextureBytes(desired);
    return true;
}

bool D3D11FrameReadback::readback(const D3D11CameraFrame& frame,
                                  CpuFrameFormat dstFormat,
                                  CpuFrame& out,
                                  std::uint32_t waitTimeoutMs)
{
    lastError_ = NoError();

    GpuFrameFormat srcFormat{};
    D3D11_TEXTURE2D_DESC desc{};
    if (!validateFrame(frame, srcFormat, desc)) return false;

    if (frame.ready.isValid() && !frame.ready.wait(waitTimeoutMs)) {
        setError(ErrorCode::Timeout, "D3D11FrameReadback::readback", "timed out waiting for frame ready fence");
        return false;
    }

    ++cacheStats_.readbacks;
    if (!ensureStagingTexture(desc)) return false;

    context_->CopyResource(stagingTexture_.Get(), frame.texture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = context_->Map(stagingTexture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        setError(ErrorCode::D3D11Error, "D3D11FrameReadback::Map", HrToString(hr));
        return false;
    }

    const bool ok = ConvertPackedGpuFrameToCpuFrame(static_cast<const std::uint8_t*>(mapped.pData),
                                                    desc.Width,
                                                    desc.Height,
                                                    mapped.RowPitch,
                                                    srcFormat,
                                                    dstFormat,
                                                    frame.timing,
                                                    out,
                                                    &lastError_);
    context_->Unmap(stagingTexture_.Get(), 0);
    if (ok) {
        out.chunkMetadata = frame.chunkMetadata;
    }
    return ok;
}

} // namespace IC4Ext

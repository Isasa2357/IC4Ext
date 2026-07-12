#include "IC4Ext/D3D11/D3D11FrameReadback.hpp"

#include <sstream>
#include <string>

namespace IC4Ext {
namespace {

std::string HrToString(HRESULT hr)
{
    std::ostringstream stream;
    stream << "HRESULT=0x" << std::hex << static_cast<unsigned long>(hr);
    return stream.str();
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
    const std::uint64_t bytesPerPixel =
        desc.Format == DXGI_FORMAT_R8_UNORM
            ? 1ull
            : desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? 4ull : 0ull;
    return static_cast<std::uint64_t>(desc.Width) * desc.Height *
           desc.ArraySize * bytesPerPixel;
}

bool SameTextureDesc(
    const D3D11_TEXTURE2D_DESC& lhs,
    const D3D11_TEXTURE2D_DESC& rhs) noexcept
{
    return lhs.Width == rhs.Width && lhs.Height == rhs.Height &&
           lhs.MipLevels == rhs.MipLevels && lhs.ArraySize == rhs.ArraySize &&
           lhs.Format == rhs.Format &&
           lhs.SampleDesc.Count == rhs.SampleDesc.Count &&
           lhs.SampleDesc.Quality == rhs.SampleDesc.Quality &&
           lhs.Usage == rhs.Usage && lhs.BindFlags == rhs.BindFlags &&
           lhs.CPUAccessFlags == rhs.CPUAccessFlags &&
           lhs.MiscFlags == rhs.MiscFlags;
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

void D3D11FrameReadback::setError(
    ErrorCode code,
    const std::string& where,
    const std::string& message)
{
    lastError_ = MakeError(code, where, message);
}

bool D3D11FrameReadback::initialize(D3D11CoreLib::D3D11Core* core)
{
    lastError_ = NoError();
    resetCache();
    contextMutex_.reset();
    if (!core) {
        setError(
            ErrorCode::InvalidArgument,
            "D3D11FrameReadback::initialize",
            "D3D11Core must be non-null");
        return false;
    }
    core_ = core;
    device_ = core->GetDevice();
    context_ = core->GetImmediateContext();
    if (!device_ || !context_) {
        setError(
            ErrorCode::InvalidArgument,
            "D3D11FrameReadback::initialize",
            "D3D11Core has null device/context");
        return false;
    }

    contextMutex_ = D3D11::Detail::AcquireImmediateContextMutex(context_.Get());
    Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(context_.As(&multithread)) && multithread) {
        multithread->SetMultithreadProtected(TRUE);
    }
    return contextMutex_ != nullptr;
}

bool D3D11FrameReadback::initialize(
    ID3D11Device* device,
    ID3D11DeviceContext* context)
{
    lastError_ = NoError();
    resetCache();
    contextMutex_.reset();
    core_ = nullptr;
    if (!device || !context) {
        setError(
            ErrorCode::InvalidArgument,
            "D3D11FrameReadback::initialize",
            "device and context must be non-null");
        return false;
    }
    device_ = device;
    context_ = context;
    contextMutex_ = D3D11::Detail::AcquireImmediateContextMutex(context_.Get());

    Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(context_.As(&multithread)) && multithread) {
        multithread->SetMultithreadProtected(TRUE);
    }
    return contextMutex_ != nullptr;
}

void D3D11FrameReadback::resetCache() noexcept
{
    stagingTexture_.Reset();
    stagingDesc_ = {};
    hasStagingDesc_ = false;
    cacheStats_ = {};
}

bool D3D11FrameReadback::validateTexture(
    ID3D11Texture2D* texture,
    const char* where,
    GpuFrameFormat& srcFormat,
    D3D11_TEXTURE2D_DESC& desc)
{
    if (!device_ || !context_) {
        setError(ErrorCode::D3D11Error, where, "readback is not initialized");
        return false;
    }
    if (!texture) {
        setError(ErrorCode::InvalidArgument, where, "frame texture is null");
        return false;
    }

    desc = {};
    texture->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0) {
        setError(ErrorCode::InvalidArgument, where, "frame texture has invalid size");
        return false;
    }
    if (desc.ArraySize != 1 || desc.MipLevels != 1 ||
        desc.SampleDesc.Count != 1) {
        setError(
            ErrorCode::UnsupportedFormat,
            where,
            "only single-subresource non-MSAA Texture2D frames are supported");
        return false;
    }
    if (!DxgiToGpuFrameFormat(desc.Format, srcFormat)) {
        setError(
            ErrorCode::UnsupportedFormat,
            where,
            "only DXGI_FORMAT_R8_UNORM and DXGI_FORMAT_R8G8B8A8_UNORM are supported");
        return false;
    }
    return true;
}

bool D3D11FrameReadback::ensureStagingTexture(
    const D3D11_TEXTURE2D_DESC& sourceDesc)
{
    const D3D11_TEXTURE2D_DESC desired = MakeStagingDesc(sourceDesc);
    if (stagingTexture_ && hasStagingDesc_ &&
        SameTextureDesc(stagingDesc_, desired)) {
        ++cacheStats_.cacheHits;
        return true;
    }

    ++cacheStats_.cacheMisses;
    stagingTexture_.Reset();
    hasStagingDesc_ = false;
    const HRESULT result = device_->CreateTexture2D(
        &desired,
        nullptr,
        stagingTexture_.GetAddressOf());
    if (FAILED(result)) {
        setError(
            ErrorCode::D3D11Error,
            "D3D11FrameReadback::CreateTexture2D",
            HrToString(result));
        return false;
    }
    stagingDesc_ = desired;
    hasStagingDesc_ = true;
    ++cacheStats_.resourceRebuilds;
    cacheStats_.bytesAllocated = EstimateTextureBytes(desired);
    return true;
}

bool D3D11FrameReadback::copyAndConvert(
    ID3D11Texture2D* texture,
    const D3D11_TEXTURE2D_DESC& desc,
    GpuFrameFormat srcFormat,
    CpuFrameFormat dstFormat,
    const FrameTiming& timing,
    const FrameChunkMetadata& chunkMetadata,
    CpuFrame& out)
{
    D3D11::Detail::ImmediateContextSequenceLock contextSequence(contextMutex_);

    ++cacheStats_.readbacks;
    if (!ensureStagingTexture(desc)) return false;

    context_->CopyResource(stagingTexture_.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT result = context_->Map(
        stagingTexture_.Get(),
        0,
        D3D11_MAP_READ,
        0,
        &mapped);
    if (FAILED(result)) {
        setError(
            ErrorCode::D3D11Error,
            "D3D11FrameReadback::Map",
            HrToString(result));
        return false;
    }

    const bool converted = ConvertPackedGpuFrameToCpuFrame(
        static_cast<const std::uint8_t*>(mapped.pData),
        desc.Width,
        desc.Height,
        mapped.RowPitch,
        srcFormat,
        dstFormat,
        timing,
        out,
        &lastError_);
    context_->Unmap(stagingTexture_.Get(), 0);
    if (converted) out.chunkMetadata = chunkMetadata;
    return converted;
}

bool D3D11FrameReadback::readback(
    const D3D11CameraFrame& frame,
    CpuFrameFormat dstFormat,
    CpuFrame& out,
    std::uint32_t waitTimeoutMs)
{
    lastError_ = NoError();
    GpuFrameFormat sourceFormat{};
    D3D11_TEXTURE2D_DESC description{};
    if (!validateTexture(
            frame.texture.Get(),
            "D3D11FrameReadback::readback",
            sourceFormat,
            description)) {
        return false;
    }
    if (frame.ready.isValid() && !frame.ready.wait(waitTimeoutMs)) {
        setError(
            ErrorCode::Timeout,
            "D3D11FrameReadback::readback",
            "timed out waiting for frame ready fence");
        return false;
    }
    return copyAndConvert(
        frame.texture.Get(),
        description,
        sourceFormat,
        dstFormat,
        frame.timing,
        frame.chunkMetadata,
        out);
}

bool D3D11FrameReadback::readback(
    const D3D11::ReadOnlyFrame& frame,
    CpuFrameFormat dstFormat,
    CpuFrame& out,
    std::uint32_t waitTimeoutMs)
{
    lastError_ = NoError();
    GpuFrameFormat sourceFormat{};
    D3D11_TEXTURE2D_DESC description{};
    if (!validateTexture(
            frame.texture(),
            "D3D11FrameReadback::readback(ReadOnlyFrame)",
            sourceFormat,
            description)) {
        return false;
    }
    if (!frame.waitReady(waitTimeoutMs)) {
        setError(
            ErrorCode::Timeout,
            "D3D11FrameReadback::readback(ReadOnlyFrame)",
            "timed out waiting for producer fence");
        return false;
    }
    return copyAndConvert(
        frame.texture(),
        description,
        sourceFormat,
        dstFormat,
        frame.timing(),
        frame.chunkMetadata(),
        out);
}

} // namespace IC4Ext

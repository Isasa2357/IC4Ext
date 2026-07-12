#include "IC4Ext/D3D12/D3D12FrameReadback.hpp"

#include <D3D12Helper/D3D12Core/D3D12Barrier.hpp>

#include <sstream>
#include <stdexcept>
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

bool StateContainsCopySource(D3D12_RESOURCE_STATES state) noexcept
{
    const auto value = static_cast<unsigned int>(state);
    const auto copySource = static_cast<unsigned int>(D3D12_RESOURCE_STATE_COPY_SOURCE);
    return (value & copySource) == copySource;
}

} // namespace

void D3D12FrameReadback::setError(
    ErrorCode code,
    const std::string& where,
    const std::string& message)
{
    lastError_ = MakeError(code, where, message);
}

bool D3D12FrameReadback::initialize(const D3D12BackendContext& backendIn)
{
    lastError_ = NoError();
    resetCache();
    backend_ = backendIn;
    if (!backend_.resolve() || !backend_.corePtr || !backend_.queue || !backend_.device) {
        setError(
            ErrorCode::InvalidArgument,
            "D3D12FrameReadback::initialize",
            "D3D12 backend must be created from D3D12Helper D3D12Core and queue");
        return false;
    }

    core_ = backend_.corePtr;
    queue_ = backend_.queue;
    device_ = backend_.device;

    try {
        commandContext_.Initialize(device_, queue_->GetType());
    } catch (const std::exception& e) {
        setError(ErrorCode::D3D12Error, "D3D12FrameReadback::initialize", e.what());
        return false;
    }
    return true;
}

void D3D12FrameReadback::resetCache() noexcept
{
    readbackBuffer_ = D3D12CoreLib::D3D12ReadbackBuffer{};
    readbackBufferSizeBytes_ = 0;
    cacheStats_ = {};
}

bool D3D12FrameReadback::validateFrame(
    const D3D12CameraFrame& frame,
    GpuFrameFormat& srcFormat,
    D3D12_RESOURCE_DESC& desc)
{
    if (!core_ || !queue_ || !device_) {
        setError(
            ErrorCode::D3D12Error,
            "D3D12FrameReadback::readback",
            "readback is not initialized");
        return false;
    }
    if (!frame.texture) {
        setError(
            ErrorCode::InvalidArgument,
            "D3D12FrameReadback::readback",
            "frame texture is null");
        return false;
    }

    desc = frame.texture->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.Width == 0 || desc.Height == 0) {
        setError(
            ErrorCode::InvalidArgument,
            "D3D12FrameReadback::readback",
            "frame resource must be a non-empty Texture2D");
        return false;
    }
    if (desc.DepthOrArraySize != 1 || desc.MipLevels != 1) {
        setError(
            ErrorCode::UnsupportedFormat,
            "D3D12FrameReadback::readback",
            "only single-subresource Texture2D frames are supported");
        return false;
    }
    if (!DxgiToGpuFrameFormat(desc.Format, srcFormat)) {
        setError(
            ErrorCode::UnsupportedFormat,
            "D3D12FrameReadback::readback",
            "only DXGI_FORMAT_R8_UNORM and DXGI_FORMAT_R8G8B8A8_UNORM are supported");
        return false;
    }
    return true;
}

bool D3D12FrameReadback::validateReadOnlyFrame(
    const D3D12::ReadOnlyFrame& frame,
    GpuFrameFormat& srcFormat,
    D3D12_RESOURCE_DESC& desc)
{
    if (!core_ || !queue_ || !device_) {
        setError(
            ErrorCode::D3D12Error,
            "D3D12FrameReadback::readback(ReadOnlyFrame)",
            "readback is not initialized");
        return false;
    }
    if (!frame || !frame.resource()) {
        setError(
            ErrorCode::InvalidArgument,
            "D3D12FrameReadback::readback(ReadOnlyFrame)",
            "read-only frame resource is null");
        return false;
    }
    if (!StateContainsCopySource(frame.publishedState())) {
        setError(
            ErrorCode::InvalidArgument,
            "D3D12FrameReadback::readback(ReadOnlyFrame)",
            "published read-only state must include D3D12_RESOURCE_STATE_COPY_SOURCE");
        return false;
    }

    desc = frame.resource()->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.Width == 0 || desc.Height == 0 ||
        desc.DepthOrArraySize != 1 || desc.MipLevels != 1) {
        setError(
            ErrorCode::UnsupportedFormat,
            "D3D12FrameReadback::readback(ReadOnlyFrame)",
            "only non-empty single-subresource Texture2D frames are supported");
        return false;
    }
    if (!DxgiToGpuFrameFormat(desc.Format, srcFormat)) {
        setError(
            ErrorCode::UnsupportedFormat,
            "D3D12FrameReadback::readback(ReadOnlyFrame)",
            "only DXGI_FORMAT_R8_UNORM and DXGI_FORMAT_R8G8B8A8_UNORM are supported");
        return false;
    }
    return true;
}

bool D3D12FrameReadback::ensureReadbackBuffer(UINT64 totalBytes)
{
    if (readbackBuffer_.Get() && readbackBufferSizeBytes_ >= totalBytes) {
        ++cacheStats_.cacheHits;
        return true;
    }

    ++cacheStats_.cacheMisses;
    readbackBuffer_ = D3D12CoreLib::D3D12ReadbackBuffer{};
    readbackBufferSizeBytes_ = 0;
    readbackBuffer_.Initialize(device_, totalBytes);
    readbackBufferSizeBytes_ = totalBytes;
    ++cacheStats_.resourceRebuilds;
    cacheStats_.bytesAllocated = totalBytes;
    return true;
}

bool D3D12FrameReadback::copyAndConvert(
    ID3D12Resource* source,
    const D3D12_RESOURCE_DESC& desc,
    GpuFrameFormat srcFormat,
    CpuFrameFormat dstFormat,
    const FrameTiming& timing,
    const FrameChunkMetadata& chunkMetadata,
    CpuFrame& out,
    std::uint32_t waitTimeoutMs,
    D3D12_RESOURCE_STATES sourceState,
    bool transitionSource)
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;
    device_->GetCopyableFootprints(
        &desc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);
    if (totalBytes == 0 || footprint.Footprint.RowPitch == 0) {
        setError(
            ErrorCode::D3D12Error,
            "D3D12FrameReadback::GetCopyableFootprints",
            "invalid readback footprint");
        return false;
    }

    try {
        ++cacheStats_.readbacks;
        if (!ensureReadbackBuffer(totalBytes)) return false;

        commandContext_.Reset();
        ID3D12GraphicsCommandList* commandList = commandContext_.GetCommandList();

        if (transitionSource && sourceState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            const auto toCopy = D3D12CoreLib::MakeTransitionBarrier(
                source,
                sourceState,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
            commandContext_.ResourceBarrier(toCopy);
        }

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readbackBuffer_.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;

        D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
        sourceLocation.pResource = source;
        sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        sourceLocation.SubresourceIndex = 0;

        commandList->CopyTextureRegion(
            &destination, 0, 0, 0, &sourceLocation, nullptr);

        if (transitionSource && sourceState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            const auto back = D3D12CoreLib::MakeTransitionBarrier(
                source,
                D3D12_RESOURCE_STATE_COPY_SOURCE,
                sourceState);
            commandContext_.ResourceBarrier(back);
        }

        commandContext_.Close();
        ID3D12CommandList* lists[] = {commandContext_.GetCommandList()};
        queue_->ExecuteCommandLists(1, lists);

        D3D12ReadyToken copyReady;
        copyReady.value = queue_->Signal();
        copyReady.fence = queue_->Fence().Get();
        if (!copyReady.wait(waitTimeoutMs)) {
            setError(
                ErrorCode::Timeout,
                "D3D12FrameReadback::readback",
                "timed out waiting for readback copy fence");
            return false;
        }

        const void* mapped = readbackBuffer_.Map();
        if (!mapped) {
            setError(
                ErrorCode::D3D12Error,
                "D3D12FrameReadback::Map",
                "mapped pointer is null");
            return false;
        }

        const auto* sourceBytes =
            static_cast<const std::uint8_t*>(mapped) + footprint.Offset;
        const bool converted = ConvertPackedGpuFrameToCpuFrame(
            sourceBytes,
            static_cast<std::uint32_t>(desc.Width),
            static_cast<std::uint32_t>(desc.Height),
            footprint.Footprint.RowPitch,
            srcFormat,
            dstFormat,
            timing,
            out,
            &lastError_);
        readbackBuffer_.Unmap();
        if (converted) out.chunkMetadata = chunkMetadata;
        return converted;
    } catch (const std::exception& e) {
        setError(ErrorCode::D3D12Error, "D3D12FrameReadback::readback", e.what());
        return false;
    }
}

bool D3D12FrameReadback::readback(
    const D3D12CameraFrame& frame,
    CpuFrameFormat dstFormat,
    CpuFrame& out,
    std::uint32_t waitTimeoutMs)
{
    lastError_ = NoError();

    GpuFrameFormat sourceFormat{};
    D3D12_RESOURCE_DESC description{};
    if (!validateFrame(frame, sourceFormat, description)) return false;

    if (frame.ready.isValid() && !frame.ready.wait(waitTimeoutMs)) {
        setError(
            ErrorCode::Timeout,
            "D3D12FrameReadback::readback",
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
        out,
        waitTimeoutMs,
        frame.resourceState,
        true);
}

bool D3D12FrameReadback::readback(
    const D3D12::ReadOnlyFrame& frame,
    CpuFrameFormat dstFormat,
    CpuFrame& out,
    std::uint32_t waitTimeoutMs)
{
    lastError_ = NoError();

    GpuFrameFormat sourceFormat{};
    D3D12_RESOURCE_DESC description{};
    if (!validateReadOnlyFrame(frame, sourceFormat, description)) return false;

    try {
        if (frame.readyToken().isValid()) {
            queue_->GpuWait(
                frame.readyToken().fence.Get(),
                frame.readyToken().value);
        }
    } catch (const std::exception& e) {
        setError(
            ErrorCode::D3D12Error,
            "D3D12FrameReadback::readback(ReadOnlyFrame)::GpuWait",
            e.what());
        return false;
    }

    return copyAndConvert(
        frame.resource(),
        description,
        sourceFormat,
        dstFormat,
        frame.timing(),
        frame.chunkMetadata(),
        out,
        waitTimeoutMs,
        frame.publishedState(),
        false);
}

} // namespace IC4Ext

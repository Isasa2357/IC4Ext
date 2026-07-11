#include "IC4Ext/V2/D3D12/D3D12PooledFrameConverter.hpp"

#include <D3D12Helper/D3D12Framework/D3D12Helpers.hpp>
#include <D3D12Helper/D3D12Framework/D3D12Resource.hpp>

#include <algorithm>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace IC4Ext::V2 {

namespace {

constexpr UINT ThreadGroupSizeX = 16;
constexpr UINT ThreadGroupSizeY = 16;

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment) noexcept
{
    return (value + alignment - 1ull) & ~(alignment - 1ull);
}

DXGI_FORMAT ToDxgiFormat(GpuFrameFormat format) noexcept
{
    switch (format) {
    case GpuFrameFormat::R8:
        return DXGI_FORMAT_R8_UNORM;
    case GpuFrameFormat::RGBA8:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

std::uint32_t BayerPatternCode(CameraPixelFormat format) noexcept
{
    switch (format) {
    case CameraPixelFormat::BayerBG8: return 0;
    case CameraPixelFormat::BayerGB8: return 1;
    case CameraPixelFormat::BayerGR8: return 2;
    case CameraPixelFormat::BayerRG8: return 3;
    default: return 3;
    }
}

D3D12_RESOURCE_BARRIER TransitionBarrier(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) noexcept
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* resource) noexcept
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    return barrier;
}

} // namespace

class D3D12PooledFrameConverter::Impl
{
public:
    IC4Ext::D3D12FrameConverter* converter = nullptr;
    std::unordered_map<const void*, D3D12CoreLib::D3D12Resource> inputBuffers;
    mutable std::mutex mutex;
    ErrorInfo lastError;

    void setError(ErrorCode code, const char* where, const std::string& message)
    {
        lastError = MakeError(code, where, message);
    }
};

D3D12PooledFrameConverter::D3D12PooledFrameConverter()
    : impl_(std::make_unique<Impl>())
{
}

D3D12PooledFrameConverter::~D3D12PooledFrameConverter() = default;
D3D12PooledFrameConverter::D3D12PooledFrameConverter(D3D12PooledFrameConverter&&) noexcept = default;
D3D12PooledFrameConverter& D3D12PooledFrameConverter::operator=(D3D12PooledFrameConverter&&) noexcept = default;

bool D3D12PooledFrameConverter::initialize(IC4Ext::D3D12FrameConverter& converter)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!converter.core_ || !converter.queue_ || !converter.device_ || !converter.fenceManager_) {
        impl_->setError(
            ErrorCode::InvalidArgument,
            "D3D12PooledFrameConverter::initialize",
            "The source D3D12FrameConverter is not initialized");
        return false;
    }
    impl_->converter = &converter;
    impl_->inputBuffers.clear();
    impl_->lastError = NoError();
    return true;
}

bool D3D12PooledFrameConverter::convert(
    const IC4Ext::D3D12CpuFrameView& input,
    const FrameOutputSpec& outputSpec,
    D3D12FrameWriter writer,
    FrameChunkMetadata chunkMetadata,
    D3D12ReadOnlyFrame& outFrame)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    outFrame = {};

    auto* converter = impl_->converter;
    if (!converter) {
        impl_->setError(
            ErrorCode::InvalidArgument,
            "D3D12PooledFrameConverter::convert",
            "The pooled converter is not initialized");
        return false;
    }
    if (!writer) {
        impl_->setError(
            ErrorCode::InvalidArgument,
            "D3D12PooledFrameConverter::convert",
            "The frame-pool writer is invalid");
        return false;
    }

    std::uint64_t neededBytes = 0;
    if (!converter->validateInput(input, outputSpec, neededBytes)) {
        impl_->setError(
            ErrorCode::UnsupportedFormat,
            "D3D12PooledFrameConverter::convert",
            std::string("Unsupported or invalid conversion: ") +
                ToString(input.format.actualInputFormat) + " -> " +
                ToString(outputSpec.outputFormat));
        return false;
    }
    if (!converter->ensurePipelines()) {
        impl_->lastError = converter->lastError_;
        return false;
    }

    const DXGI_FORMAT outputFormat = ToDxgiFormat(outputSpec.outputFormat);
    ID3D12Resource* outputResource = writer.resource();
    if (!outputResource || writer.uavCpuHandle().ptr == 0 || outputFormat == DXGI_FORMAT_UNKNOWN) {
        impl_->setError(
            ErrorCode::InvalidArgument,
            "D3D12PooledFrameConverter::convert",
            "The writer does not expose a UAV-capable output texture");
        return false;
    }

    const D3D12_RESOURCE_DESC outputDesc = outputResource->GetDesc();
    if (outputDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        outputDesc.Width != static_cast<UINT64>(input.format.width) ||
        outputDesc.Height != static_cast<UINT>(input.format.height) ||
        outputDesc.Format != outputFormat) {
        impl_->setError(
            ErrorCode::InvalidArgument,
            "D3D12PooledFrameConverter::convert",
            "The frame-pool texture shape or format does not match the input/output specification");
        return false;
    }

    const std::uint64_t uploadSize = AlignUp(neededBytes, 4);
    auto* slot = converter->acquireSlot(uploadSize);
    if (!slot) {
        impl_->lastError = converter->lastError_;
        return false;
    }

    try {
        auto& inputBuffer = impl_->inputBuffers[slot];
        inputBuffer = D3D12CoreLib::CreateBuffer(
            *converter->core_,
            uploadSize,
            D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_COPY_DEST);

        auto upload = slot->uploadRing.Allocate(uploadSize, 4);
        std::memset(upload.cpuPtr, 0, static_cast<std::size_t>(uploadSize));
        std::memcpy(upload.cpuPtr, input.data, static_cast<std::size_t>(neededBytes));

        auto* commandList = slot->commandContext.GetCommandList();
        commandList->CopyBufferRegion(
            inputBuffer.Get(),
            0,
            upload.resource,
            upload.offset,
            uploadSize);

        auto inputBarrier = TransitionBarrier(
            inputBuffer.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        slot->commandContext.ResourceBarrier(inputBarrier);

        const D3D12_RESOURCE_STATES initialState = writer.initialState();
        const D3D12_RESOURCE_STATES writeState = writer.writeState();
        const D3D12_RESOURCE_STATES publishedState = writer.publishedState();
        if (initialState != writeState) {
            auto outputToWrite = TransitionBarrier(outputResource, initialState, writeState);
            slot->commandContext.ResourceBarrier(outputToWrite);
        }

        const auto inputSrv = slot->descriptorAllocator.Allocate();
        const auto outputUav = slot->descriptorAllocator.Allocate();

        D3D12_SHADER_RESOURCE_VIEW_DESC inputSrvDesc{};
        inputSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        inputSrvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        inputSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        inputSrvDesc.Buffer.FirstElement = 0;
        inputSrvDesc.Buffer.NumElements = static_cast<UINT>(uploadSize / 4u);
        inputSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        converter->device_->CreateShaderResourceView(
            inputBuffer.Get(),
            &inputSrvDesc,
            inputSrv.cpu);

        converter->device_->CopyDescriptorsSimple(
            1,
            outputUav.cpu,
            writer.uavCpuHandle(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        ID3D12DescriptorHeap* heaps[] = {slot->descriptorAllocator.GetHeap()};
        commandList->SetDescriptorHeaps(1, heaps);

        auto& pipeline = outputSpec.outputFormat == GpuFrameFormat::R8
            ? converter->r8Pipeline_
            : converter->rgbaPipeline_;
        commandList->SetComputeRootSignature(pipeline.GetRootSignature());
        commandList->SetPipelineState(pipeline.GetPipelineState());
        commandList->SetComputeRootDescriptorTable(pipeline.SrvTableIndex(), inputSrv.gpu);
        commandList->SetComputeRootDescriptorTable(pipeline.UavTableIndex(), outputUav.gpu);

        IC4Ext::D3D12FrameConverter::ConvertConstants constants;
        constants.width = static_cast<std::uint32_t>(input.format.width);
        constants.height = static_cast<std::uint32_t>(input.format.height);
        constants.inputRowPitchBytes = static_cast<std::uint32_t>(input.format.inputRowPitchBytes);
        constants.inputFormat = static_cast<std::uint32_t>(input.format.actualInputFormat);
        constants.bayerPattern = BayerPatternCode(input.format.actualInputFormat);
        commandList->SetComputeRoot32BitConstants(
            pipeline.RootConstantsIndex(),
            sizeof(constants) / sizeof(std::uint32_t),
            &constants,
            0);

        const UINT groupsX =
            (static_cast<UINT>(input.format.width) + ThreadGroupSizeX - 1u) /
            ThreadGroupSizeX;
        const UINT groupsY =
            (static_cast<UINT>(input.format.height) + ThreadGroupSizeY - 1u) /
            ThreadGroupSizeY;
        commandList->Dispatch(groupsX, groupsY, 1);

        auto uavBarrier = UavBarrier(outputResource);
        slot->commandContext.ResourceBarrier(uavBarrier);
        if (writeState != publishedState) {
            auto outputToPublished = TransitionBarrier(outputResource, writeState, publishedState);
            slot->commandContext.ResourceBarrier(outputToPublished);
        }

        slot->commandContext.Close();
        ID3D12CommandList* lists[] = {slot->commandContext.GetCommandList()};
        converter->queue_->ExecuteCommandLists(1, lists);
        const std::uint64_t fenceValue = converter->queue_->Signal();
        slot->uploadRing.FinishFrame(fenceValue);
        slot->inFlight = converter->fenceManager_->makeToken(fenceValue);
        if (!slot->inFlight.isValid()) {
            impl_->lastError = converter->fenceManager_->lastError();
            return false;
        }

        FrameFormatMetadata outputMetadata = input.format;
        outputMetadata.outputFormat = outputSpec.outputFormat;
        outFrame = writer.publish(
            slot->inFlight,
            input.timing,
            std::move(outputMetadata),
            std::move(chunkMetadata));
        if (!outFrame) {
            impl_->setError(
                ErrorCode::InternalError,
                "D3D12PooledFrameConverter::convert",
                "The frame writer failed to publish after command submission");
            return false;
        }

        impl_->lastError = NoError();
        return true;
    } catch (const std::exception& e) {
        impl_->setError(ErrorCode::D3D12Error, "D3D12PooledFrameConverter::convert", e.what());
        return false;
    }
}

ErrorInfo D3D12PooledFrameConverter::lastError() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->lastError;
}

} // namespace IC4Ext::V2

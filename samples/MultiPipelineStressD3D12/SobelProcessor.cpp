#include "SobelProcessor.hpp"

#include <IC4Ext/D3D12/D3D12ReadyToken.hpp>

#include <D3D12Helper/D3D12Core/D3D12CommandContext.hpp>
#include <D3D12Helper/D3D12Core/D3D12Queue.hpp>
#include <D3D12Helper/D3D12Framework/D3D12ComputePipeline.hpp>
#include <D3D12Helper/D3D12Framework/D3D12DescriptorAllocator.hpp>
#include <D3D12Helper/D3D12Framework/D3D12ShaderCompiler.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <mutex>
#include <string>
#include <utility>

namespace IC4ExtStress {

namespace {

constexpr UINT ThreadGroupSize = 16;
constexpr std::size_t SlotCount = 4;

const char* SobelShader = R"HLSL(
Texture2D<float4> gInput : register(t0);
RWTexture2D<float4> gOutput : register(u0);

cbuffer Parameters : register(b0)
{
    uint gWidth;
    uint gHeight;
    float gStrength;
    uint gReserved;
};

float Luma(int2 position)
{
    position.x = clamp(position.x, 0, int(gWidth) - 1);
    position.y = clamp(position.y, 0, int(gHeight) - 1);
    const float3 rgb = gInput.Load(int3(position, 0)).rgb;
    return dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
}

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= gWidth || dispatchThreadId.y >= gHeight) return;

    const int2 p = int2(dispatchThreadId.xy);
    const float gx =
        -Luma(p + int2(-1, -1)) + Luma(p + int2(1, -1))
        -2.0f * Luma(p + int2(-1, 0)) + 2.0f * Luma(p + int2(1, 0))
        -Luma(p + int2(-1, 1)) + Luma(p + int2(1, 1));
    const float gy =
        -Luma(p + int2(-1, -1)) - 2.0f * Luma(p + int2(0, -1)) - Luma(p + int2(1, -1))
        +Luma(p + int2(-1, 1)) + 2.0f * Luma(p + int2(0, 1)) + Luma(p + int2(1, 1));

    const float edge = saturate(sqrt(gx * gx + gy * gy) * gStrength);
    gOutput[dispatchThreadId.xy] = float4(edge, edge, edge, 1.0f);
}
)HLSL";

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

struct Constants
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float strength = 1.5f;
    std::uint32_t reserved = 0;
};

} // namespace

class SobelProcessor::Impl
{
public:
    struct Slot
    {
        D3D12CoreLib::D3D12CommandContext commandContext;
        D3D12CoreLib::D3D12DescriptorAllocator descriptorAllocator;
        IC4Ext::D3D12ReadyToken inFlight;
        IC4Ext::D3D12::ReadOnlyFrame inputHold;
        IC4Ext::D3D12::ReadOnlyFrame outputHold;
    };

    IC4Ext::D3D12BackendContext producerBackend;
    IC4Ext::D3D12BackendContext processingBackend;
    D3D12CoreLib::D3D12Queue queue;
    D3D12CoreLib::D3D12ComputePipeline pipeline;
    std::array<Slot, SlotCount> slots;
    std::size_t nextSlot = 0;

    IC4Ext::D3D12::FramePool outputPool;
    std::uint32_t poolWidth = 0;
    std::uint32_t poolHeight = 0;

    mutable std::mutex mutex;
    SobelProcessorStats statsValue;
    IC4Ext::ErrorInfo error;
    bool initialized = false;

    void setError(IC4Ext::ErrorCode code, const char* where, std::string message)
    {
        std::lock_guard<std::mutex> lock(mutex);
        error = IC4Ext::MakeError(code, where, std::move(message));
        ++statsValue.failures;
    }

    void clearError()
    {
        std::lock_guard<std::mutex> lock(mutex);
        error = IC4Ext::NoError();
    }

    bool finishSlot(Slot& slot, std::uint32_t timeoutMs)
    {
        if (!slot.inFlight.isValid()) return true;
        if (!slot.inFlight.wait(timeoutMs)) {
            setError(
                IC4Ext::ErrorCode::Timeout,
                "SobelProcessor::finishSlot",
                "timed out waiting for HLSL compute completion");
            return false;
        }

        slot.inFlight = {};
        slot.inputHold = {};
        slot.outputHold = {};
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++statsValue.completedFrames;
        }
        return true;
    }

    bool ensurePool(const IC4Ext::D3D12::ReadOnlyFrame& input)
    {
        const auto description = input.resource()->GetDesc();
        if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            description.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
            description.Width == 0 || description.Height == 0) {
            setError(
                IC4Ext::ErrorCode::UnsupportedFormat,
                "SobelProcessor::ensurePool",
                "Sobel stress processing requires RGBA8 Texture2D input");
            return false;
        }

        const auto width = static_cast<std::uint32_t>(description.Width);
        const auto height = static_cast<std::uint32_t>(description.Height);
        if (outputPool.isInitialized() && width == poolWidth && height == poolHeight) {
            return true;
        }

        if (!flush(10'000)) return false;
        outputPool.reset();

        IC4Ext::D3D12::FramePoolConfig config;
        config.width = width;
        config.height = height;
        config.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        config.resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        config.writeState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        config.publishedState = D3D12_RESOURCE_STATE_GENERIC_READ;
        config.createSrv = true;
        config.createUav = true;
        config.initialCapacity = 8;
        config.maxCapacity = 32;
        config.exhaustionPolicy = IC4Ext::D3D12::FramePoolExhaustionPolicy::WaitWithTimeout;
        config.waitTimeout = std::chrono::milliseconds(1000);

        if (!outputPool.initialize(processingBackend, config)) {
            const auto poolError = outputPool.lastError();
            setError(
                IC4Ext::ErrorCode::D3D12Error,
                "SobelProcessor::ensurePool",
                poolError.where + ": " + poolError.message);
            return false;
        }

        poolWidth = width;
        poolHeight = height;
        return true;
    }

    bool flush(std::uint32_t timeoutMs) noexcept
    {
        bool result = true;
        for (auto& slot : slots) {
            try {
                if (!finishSlot(slot, timeoutMs)) result = false;
            } catch (...) {
                result = false;
            }
        }
        try {
            queue.WaitIdle();
        } catch (...) {
            result = false;
        }
        return result;
    }
};

SobelProcessor::SobelProcessor()
    : impl_(std::make_unique<Impl>())
{
}

SobelProcessor::~SobelProcessor()
{
    if (impl_) impl_->flush(10'000);
}

bool SobelProcessor::initialize(
    const IC4Ext::D3D12BackendContext& producerBackend)
{
    if (!impl_) return false;

    auto resolved = producerBackend;
    if (!resolved.resolve() || !resolved.device || !resolved.corePtr) {
        impl_->setError(
            IC4Ext::ErrorCode::InvalidArgument,
            "SobelProcessor::initialize",
            "producer backend is incomplete");
        return false;
    }

    try {
        impl_->producerBackend = resolved;
        impl_->queue.Initialize(
            resolved.device,
            D3D12_COMMAND_LIST_TYPE_COMPUTE,
            D3D12_COMMAND_QUEUE_PRIORITY_NORMAL);

        impl_->processingBackend = resolved;
        impl_->processingBackend.queue = &impl_->queue;
        impl_->processingBackend.commandQueue = impl_->queue.Get();

        const auto bytecode = D3D12CoreLib::CompileShaderFromSource_D3DCompile(
            SobelShader,
            "main",
            "cs_5_0",
            "MultiPipelineStressSobel.hlsl");
        D3D12CoreLib::ComputePipelineDesc description;
        description.numSrvs = 1;
        description.numUavs = 1;
        description.numRootConstantValues = sizeof(Constants) / sizeof(std::uint32_t);
        impl_->pipeline.InitializeWithTemplate(
            resolved.device,
            bytecode,
            description);

        for (auto& slot : impl_->slots) {
            slot.commandContext.Initialize(
                resolved.device,
                D3D12_COMMAND_LIST_TYPE_COMPUTE);
            slot.descriptorAllocator.Initialize(
                resolved.device,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                2,
                true);
        }
    } catch (const std::exception& exception) {
        impl_->setError(
            IC4Ext::ErrorCode::D3D12Error,
            "SobelProcessor::initialize",
            exception.what());
        return false;
    }

    impl_->initialized = true;
    impl_->clearError();
    return true;
}

bool SobelProcessor::process(
    const IC4Ext::D3D12::ReadOnlyFrame& input)
{
    if (!impl_ || !impl_->initialized || !input) {
        if (impl_) {
            impl_->setError(
                IC4Ext::ErrorCode::InvalidArgument,
                "SobelProcessor::process",
                "processor is not initialized or input is invalid");
        }
        return false;
    }
    if (!impl_->ensurePool(input)) return false;

    auto& slot = impl_->slots[impl_->nextSlot % impl_->slots.size()];
    impl_->nextSlot = (impl_->nextSlot + 1) % impl_->slots.size();
    if (!impl_->finishSlot(slot, 10'000)) return false;

    if (!IC4Ext::D3D12::WaitForReadOnlyFrameReadyOnQueue(
            impl_->queue,
            input)) {
        impl_->setError(
            IC4Ext::ErrorCode::D3D12Error,
            "SobelProcessor::process",
            "failed to enqueue producer-ready wait");
        return false;
    }

    auto writer = impl_->outputPool.acquire();
    if (!writer) {
        const auto poolError = impl_->outputPool.lastError();
        impl_->setError(
            IC4Ext::ErrorCode::D3D12Error,
            "SobelProcessor::process",
            poolError.where + ": " + poolError.message);
        return false;
    }

    try {
        slot.commandContext.Reset();
        slot.descriptorAllocator.Reset();
        auto* commandList = slot.commandContext.GetCommandList();

        const auto initialState = writer.initialState();
        const auto writeState = writer.writeState();
        const auto publishedState = writer.publishedState();
        if (initialState != writeState) {
            const auto barrier = TransitionBarrier(
                writer.resource(),
                initialState,
                writeState);
            slot.commandContext.ResourceBarrier(barrier);
        }

        const auto inputSrv = slot.descriptorAllocator.Allocate();
        const auto outputUav = slot.descriptorAllocator.Allocate();
        impl_->processingBackend.device->CopyDescriptorsSimple(
            1,
            inputSrv.cpu,
            input.srvCpuHandle(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        impl_->processingBackend.device->CopyDescriptorsSimple(
            1,
            outputUav.cpu,
            writer.uavCpuHandle(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        ID3D12DescriptorHeap* heaps[] = {
            slot.descriptorAllocator.GetHeap()};
        commandList->SetDescriptorHeaps(1, heaps);
        impl_->pipeline.Bind(commandList);
        commandList->SetComputeRootDescriptorTable(
            impl_->pipeline.SrvTableIndex(),
            inputSrv.gpu);
        commandList->SetComputeRootDescriptorTable(
            impl_->pipeline.UavTableIndex(),
            outputUav.gpu);

        Constants constants;
        constants.width = impl_->poolWidth;
        constants.height = impl_->poolHeight;
        commandList->SetComputeRoot32BitConstants(
            impl_->pipeline.RootConstantsIndex(),
            sizeof(constants) / sizeof(std::uint32_t),
            &constants,
            0);

        commandList->Dispatch(
            (impl_->poolWidth + ThreadGroupSize - 1u) / ThreadGroupSize,
            (impl_->poolHeight + ThreadGroupSize - 1u) / ThreadGroupSize,
            1);

        slot.commandContext.ResourceBarrier(UavBarrier(writer.resource()));
        if (writeState != publishedState) {
            slot.commandContext.ResourceBarrier(TransitionBarrier(
                writer.resource(),
                writeState,
                publishedState));
        }

        slot.commandContext.Close();
        ID3D12CommandList* lists[] = {slot.commandContext.GetCommandList()};
        impl_->queue.ExecuteCommandLists(1, lists);

        IC4Ext::D3D12ReadyToken completion;
        completion.value = impl_->queue.Signal();
        completion.fence = impl_->queue.Fence().Get();
        if (!completion.isValid()) {
            impl_->setError(
                IC4Ext::ErrorCode::D3D12Error,
                "SobelProcessor::process",
                "compute queue returned an invalid completion token");
            return false;
        }

        auto metadata = input.format();
        metadata.outputFormat = IC4Ext::GpuFrameFormat::RGBA8;
        auto output = writer.publish(
            completion,
            input.timing(),
            std::move(metadata),
            input.chunkMetadata());
        if (!output) {
            completion.wait(10'000);
            impl_->setError(
                IC4Ext::ErrorCode::InternalError,
                "SobelProcessor::process",
                "failed to publish private Sobel output texture");
            return false;
        }

        slot.inputHold = input;
        slot.outputHold = std::move(output);
        slot.inFlight = completion;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            ++impl_->statsValue.submittedFrames;
            impl_->error = IC4Ext::NoError();
        }
        return true;
    } catch (const std::exception& exception) {
        impl_->setError(
            IC4Ext::ErrorCode::D3D12Error,
            "SobelProcessor::process",
            exception.what());
        return false;
    }
}

bool SobelProcessor::flush(std::uint32_t timeoutMs) noexcept
{
    return impl_ && impl_->flush(timeoutMs);
}

SobelProcessorStats SobelProcessor::stats() const noexcept
{
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->statsValue;
}

IC4Ext::ErrorInfo SobelProcessor::lastError() const
{
    if (!impl_) return IC4Ext::NoError();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->error;
}

} // namespace IC4ExtStress

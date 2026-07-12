#include "SobelProcessor.hpp"

#include <IC4Ext/D3D12/D3D12ReadyToken.hpp>

#include <D3D12Helper/D3D12Core/D3D12CommandContext.hpp>
#include <D3D12Helper/D3D12Core/D3D12Queue.hpp>
#include <D3D12Helper/D3D12Framework/D3D12ComputePipeline.hpp>
#include <D3D12Helper/D3D12Framework/D3D12DescriptorAllocator.hpp>
#include <D3D12Helper/D3D12Framework/D3D12ShaderCompiler.hpp>

#include <array>
#include <chrono>
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
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight) return;
    const int2 p = int2(id.xy);
    const float gx =
        -Luma(p + int2(-1, -1)) + Luma(p + int2(1, -1))
        -2.0f * Luma(p + int2(-1, 0)) + 2.0f * Luma(p + int2(1, 0))
        -Luma(p + int2(-1, 1)) + Luma(p + int2(1, 1));
    const float gy =
        -Luma(p + int2(-1, -1)) - 2.0f * Luma(p + int2(0, -1)) - Luma(p + int2(1, -1))
        +Luma(p + int2(-1, 1)) + 2.0f * Luma(p + int2(0, 1)) + Luma(p + int2(1, 1));
    const float edge = saturate(sqrt(gx * gx + gy * gy) * gStrength);
    gOutput[id.xy] = float4(edge, edge, edge, 1.0f);
}
)HLSL";

struct Constants
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float strength = 1.5f;
    std::uint32_t reserved = 0;
};

D3D12_RESOURCE_BARRIER Transition(
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

D3D12_RESOURCE_BARRIER Uav(ID3D12Resource* resource) noexcept
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    return barrier;
}

} // namespace

class SobelProcessor::Impl
{
public:
    struct Slot
    {
        D3D12CoreLib::D3D12CommandContext context;
        D3D12CoreLib::D3D12DescriptorAllocator descriptors;
        IC4Ext::D3D12ReadyToken completion;
        IC4Ext::D3D12::ReadOnlyFrame inputHold;
        IC4Ext::D3D12::ReadOnlyFrame outputHold;
    };

    IC4Ext::D3D12BackendContext backend;
    D3D12CoreLib::D3D12Queue queue;
    D3D12CoreLib::D3D12ComputePipeline pipeline;
    std::array<Slot, SlotCount> slots;
    std::size_t nextSlot = 0;

    IC4Ext::D3D12::FramePool outputPool;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool initialized = false;

    mutable std::mutex mutex;
    SobelProcessorStats stats;
    IC4Ext::ErrorInfo error;

    void fail(IC4Ext::ErrorCode code, const char* where, std::string message)
    {
        std::lock_guard<std::mutex> lock(mutex);
        error = IC4Ext::MakeError(code, where, std::move(message));
        ++stats.failures;
    }

    void clearError()
    {
        std::lock_guard<std::mutex> lock(mutex);
        error = IC4Ext::NoError();
    }

    bool finish(Slot& slot, std::uint32_t timeoutMs)
    {
        if (!slot.completion.isValid()) return true;
        if (!slot.completion.wait(timeoutMs)) {
            fail(IC4Ext::ErrorCode::Timeout,
                 "SobelProcessor::finish",
                 "timed out waiting for the HLSL command slot");
            return false;
        }
        slot.completion = {};
        slot.inputHold = {};
        slot.outputHold = {};
        std::lock_guard<std::mutex> lock(mutex);
        ++stats.completedFrames;
        return true;
    }

    bool flush(std::uint32_t timeoutMs) noexcept
    {
        bool ok = true;
        for (auto& slot : slots) {
            try {
                if (!finish(slot, timeoutMs)) ok = false;
            } catch (...) {
                ok = false;
            }
        }
        try {
            queue.WaitIdle();
        } catch (...) {
            ok = false;
        }
        return ok;
    }

    bool ensureOutputPool(const IC4Ext::D3D12::ReadOnlyFrame& input)
    {
        const auto description = input.resource()->GetDesc();
        if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            description.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
            description.Width == 0 || description.Height == 0) {
            fail(IC4Ext::ErrorCode::UnsupportedFormat,
                 "SobelProcessor::ensureOutputPool",
                 "the HLSL stress workload requires RGBA8 Texture2D input");
            return false;
        }

        const auto nextWidth = static_cast<std::uint32_t>(description.Width);
        const auto nextHeight = static_cast<std::uint32_t>(description.Height);
        if (outputPool.isInitialized() &&
            nextWidth == width && nextHeight == height) {
            return true;
        }

        if (!flush(10'000)) return false;
        outputPool.reset();

        IC4Ext::D3D12::FramePoolConfig config;
        config.width = nextWidth;
        config.height = nextHeight;
        config.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        config.resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        config.writeState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        config.publishedState = D3D12_RESOURCE_STATE_GENERIC_READ;
        config.createSrv = true;
        config.createUav = true;
        config.initialCapacity = 8;
        config.maxCapacity = 32;
        config.exhaustionPolicy =
            IC4Ext::D3D12::FramePoolExhaustionPolicy::WaitWithTimeout;
        config.waitTimeout = std::chrono::milliseconds(1000);

        if (!outputPool.initialize(backend, config)) {
            const auto poolError = outputPool.lastError();
            fail(IC4Ext::ErrorCode::D3D12Error,
                 "SobelProcessor::ensureOutputPool",
                 poolError.where + ": " + poolError.message);
            return false;
        }

        width = nextWidth;
        height = nextHeight;
        return true;
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
        impl_->fail(IC4Ext::ErrorCode::InvalidArgument,
                    "SobelProcessor::initialize",
                    "producer backend is incomplete");
        return false;
    }

    try {
        // A dedicated direct queue is used so the private output can transition
        // to GENERIC_READ, which contains both shader-read and COPY_SOURCE bits.
        impl_->queue.Initialize(
            resolved.device,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            D3D12_COMMAND_QUEUE_PRIORITY_NORMAL);
        impl_->backend = resolved;
        impl_->backend.queue = &impl_->queue;
        impl_->backend.commandQueue = impl_->queue.Get();

        const auto bytecode = D3D12CoreLib::CompileShaderFromSource_D3DCompile(
            SobelShader, "main", "cs_5_0", "MultiPipelineStressSobel.hlsl");
        D3D12CoreLib::ComputePipelineDesc pipelineDescription;
        pipelineDescription.numSrvs = 1;
        pipelineDescription.numUavs = 1;
        pipelineDescription.numRootConstantValues =
            sizeof(Constants) / sizeof(std::uint32_t);
        impl_->pipeline.InitializeWithTemplate(
            resolved.device, bytecode, pipelineDescription);

        for (auto& slot : impl_->slots) {
            slot.context.Initialize(
                resolved.device, D3D12_COMMAND_LIST_TYPE_DIRECT);
            slot.descriptors.Initialize(
                resolved.device,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                2,
                true);
        }
    } catch (const std::exception& exception) {
        impl_->fail(IC4Ext::ErrorCode::D3D12Error,
                    "SobelProcessor::initialize",
                    exception.what());
        return false;
    }

    impl_->initialized = true;
    impl_->clearError();
    return true;
}

bool SobelProcessor::process(const IC4Ext::D3D12::ReadOnlyFrame& input)
{
    if (!impl_ || !impl_->initialized || !input) {
        if (impl_) {
            impl_->fail(IC4Ext::ErrorCode::InvalidArgument,
                        "SobelProcessor::process",
                        "processor is not initialized or input is invalid");
        }
        return false;
    }
    if (!impl_->ensureOutputPool(input)) return false;

    auto& slot = impl_->slots[impl_->nextSlot % impl_->slots.size()];
    impl_->nextSlot = (impl_->nextSlot + 1) % impl_->slots.size();
    if (!impl_->finish(slot, 10'000)) return false;

    if (!IC4Ext::D3D12::WaitForReadOnlyFrameReadyOnQueue(
            impl_->queue, input)) {
        impl_->fail(IC4Ext::ErrorCode::D3D12Error,
                    "SobelProcessor::process",
                    "failed to enqueue the producer-ready wait");
        return false;
    }

    auto writer = impl_->outputPool.acquire();
    if (!writer) {
        const auto poolError = impl_->outputPool.lastError();
        impl_->fail(IC4Ext::ErrorCode::D3D12Error,
                    "SobelProcessor::process",
                    poolError.where + ": " + poolError.message);
        return false;
    }

    try {
        slot.context.Reset();
        slot.descriptors.Reset();
        auto* commandList = slot.context.GetCommandList();

        if (writer.initialState() != writer.writeState()) {
            slot.context.ResourceBarrier(Transition(
                writer.resource(), writer.initialState(), writer.writeState()));
        }

        const auto inputSrv = slot.descriptors.Allocate();
        const auto outputUav = slot.descriptors.Allocate();
        impl_->backend.device->CopyDescriptorsSimple(
            1, inputSrv.cpu, input.srvCpuHandle(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        impl_->backend.device->CopyDescriptorsSimple(
            1, outputUav.cpu, writer.uavCpuHandle(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        ID3D12DescriptorHeap* heaps[] = {slot.descriptors.GetHeap()};
        commandList->SetDescriptorHeaps(1, heaps);
        impl_->pipeline.Bind(commandList);
        commandList->SetComputeRootDescriptorTable(
            impl_->pipeline.SrvTableIndex(), inputSrv.gpu);
        commandList->SetComputeRootDescriptorTable(
            impl_->pipeline.UavTableIndex(), outputUav.gpu);

        Constants constants;
        constants.width = impl_->width;
        constants.height = impl_->height;
        commandList->SetComputeRoot32BitConstants(
            impl_->pipeline.RootConstantsIndex(),
            sizeof(constants) / sizeof(std::uint32_t),
            &constants,
            0);
        commandList->Dispatch(
            (impl_->width + ThreadGroupSize - 1u) / ThreadGroupSize,
            (impl_->height + ThreadGroupSize - 1u) / ThreadGroupSize,
            1);

        slot.context.ResourceBarrier(Uav(writer.resource()));
        if (writer.writeState() != writer.publishedState()) {
            slot.context.ResourceBarrier(Transition(
                writer.resource(), writer.writeState(), writer.publishedState()));
        }

        slot.context.Close();
        ID3D12CommandList* lists[] = {slot.context.GetCommandList()};
        impl_->queue.ExecuteCommandLists(1, lists);

        IC4Ext::D3D12ReadyToken completion;
        completion.value = impl_->queue.Signal();
        completion.fence = impl_->queue.Fence().Get();
        if (!completion.isValid()) {
            impl_->fail(IC4Ext::ErrorCode::D3D12Error,
                        "SobelProcessor::process",
                        "the direct queue returned an invalid completion token");
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
            impl_->fail(IC4Ext::ErrorCode::InternalError,
                        "SobelProcessor::process",
                        "failed to publish the private Sobel output");
            return false;
        }

        slot.inputHold = input;
        slot.outputHold = std::move(output);
        slot.completion = completion;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            ++impl_->stats.submittedFrames;
            impl_->error = IC4Ext::NoError();
        }
        return true;
    } catch (const std::exception& exception) {
        impl_->fail(IC4Ext::ErrorCode::D3D12Error,
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
    return impl_->stats;
}

IC4Ext::ErrorInfo SobelProcessor::lastError() const
{
    if (!impl_) return IC4Ext::NoError();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->error;
}

} // namespace IC4ExtStress

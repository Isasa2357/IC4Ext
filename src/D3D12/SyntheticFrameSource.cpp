#include "IC4Ext/D3D12/SyntheticFrameSource.hpp"

#include <D3D12Helper/D3D12Core/D3D12CommandContext.hpp>
#include <D3D12Helper/D3D12Core/D3D12Queue.hpp>
#include <D3D12Helper/D3D12Framework/D3D12ComputePipeline.hpp>
#include <D3D12Helper/D3D12Framework/D3D12DescriptorAllocator.hpp>
#include <D3D12Helper/D3D12Framework/D3D12ShaderCompiler.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace IC4Ext::D3D12 {
namespace {

using Clock = std::chrono::steady_clock;
constexpr UINT ThreadGroupSize = 16;
constexpr std::size_t CommandSlotCount = 4;

const char* SyntheticFrameShader = R"HLSL(
RWTexture2D<float4> gOutput : register(u0);

cbuffer Parameters : register(b0)
{
    uint gWidth;
    uint gHeight;
    uint gFrameIndex;
    uint gSeedLow;
    uint gSeedHigh;
    uint gPattern;
    uint gReserved0;
    uint gReserved1;
};

uint Hash32(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

float ByteToUnorm(uint value)
{
    return float(value & 255u) / 255.0f;
}

float4 HashNoise(uint2 position)
{
    uint value = position.x * 0x9e3779b9u;
    value ^= position.y * 0x85ebca6bu;
    value ^= gFrameIndex * 0xc2b2ae35u;
    value ^= gSeedLow;
    value = Hash32(value ^ gSeedHigh);
    const uint value2 = Hash32(value ^ 0x27d4eb2fu);
    return float4(
        ByteToUnorm(value),
        ByteToUnorm(value >> 8),
        ByteToUnorm(value2 >> 16),
        1.0f);
}

float4 Gradient(uint2 position)
{
    const float x = gWidth > 1u ? float(position.x) / float(gWidth - 1u) : 0.0f;
    const float y = gHeight > 1u ? float(position.y) / float(gHeight - 1u) : 0.0f;
    return float4(x, y, ByteToUnorm(gFrameIndex), 1.0f);
}

float4 Checkerboard(uint2 position)
{
    const uint checker = ((position.x / 16u) + (position.y / 16u) + gFrameIndex) & 1u;
    const float a = checker != 0u ? 1.0f : 0.08f;
    const float b = checker != 0u ? 0.12f : 0.9f;
    return float4(a, b, ByteToUnorm(gFrameIndex * 13u), 1.0f);
}

float4 FrameCounterBars(uint2 position)
{
    const uint bar = gWidth > 0u ? min(7u, (position.x * 8u) / gWidth) : 0u;
    const uint bit = (gFrameIndex >> bar) & 1u;
    const float intensity = bit != 0u ? 1.0f : 0.05f;
    const float vertical = gHeight > 1u ? float(position.y) / float(gHeight - 1u) : 0.0f;
    return float4(intensity, vertical, 1.0f - intensity, 1.0f);
}

[numthreads(16, 16, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= gWidth || dispatchId.y >= gHeight) return;

    const uint2 position = dispatchId.xy;
    float4 color;
    if (gPattern == 1u) {
        color = Gradient(position);
    } else if (gPattern == 2u) {
        color = Checkerboard(position);
    } else if (gPattern == 3u) {
        color = FrameCounterBars(position);
    } else {
        color = HashNoise(position);
    }
    gOutput[position] = color;
}
)HLSL";

struct ShaderConstants
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t frameIndex = 0;
    std::uint32_t seedLow = 0;
    std::uint32_t seedHigh = 0;
    std::uint32_t pattern = 0;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
};

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

std::uint64_t SaturatingAdd(std::uint64_t lhs, std::uint64_t rhs) noexcept
{
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    return rhs > maximum - lhs ? maximum : lhs + rhs;
}

std::uint64_t SaturatingMultiply(std::uint64_t lhs, std::uint64_t rhs) noexcept
{
    if (lhs == 0 || rhs == 0) return 0;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    return lhs > maximum / rhs ? maximum : lhs * rhs;
}

std::uint64_t ApplySignedOffset(std::uint64_t value, std::int64_t offset) noexcept
{
    if (offset >= 0) return SaturatingAdd(value, static_cast<std::uint64_t>(offset));
    const std::uint64_t magnitude = static_cast<std::uint64_t>(-(offset + 1)) + 1ull;
    return magnitude >= value ? 1ull : value - magnitude;
}

std::uint64_t FrameNumberFor(const SyntheticFrameSourceConfig& config,
                             std::uint64_t frameIndex) noexcept
{
    return SaturatingAdd(config.firstFrameNumber, frameIndex);
}

std::uint64_t DeviceTimestampFor(const SyntheticFrameSourceConfig& config,
                                 std::uint64_t frameIndex,
                                 std::uint64_t periodNs) noexcept
{
    const auto base = SaturatingAdd(
        config.deviceTimestampOriginNs,
        SaturatingMultiply(frameIndex, periodNs));
    return ApplySignedOffset(base, config.deviceTimestampOffsetNs);
}

ErrorInfo TimeoutError(const char* message)
{
    return MakeError(ErrorCode::Timeout, "SyntheticFrameSource::read", message);
}

} // namespace

class SyntheticFrameSource::Impl
{
public:
    struct CommandSlot
    {
        D3D12CoreLib::D3D12CommandContext context;
        D3D12CoreLib::D3D12DescriptorAllocator descriptors;
        ::IC4Ext::D3D12ReadyToken completion;
    };

    D3D12BackendContext backend;
    D3D12CoreLib::D3D12Queue queue;
    D3D12CoreLib::D3D12ComputePipeline pipeline;
    std::array<CommandSlot, CommandSlotCount> slots;
    std::size_t nextSlot = 0;

    FramePool pool;
    SyntheticFrameSourceConfig configValue;
    SyntheticFrameSourceStats statsValue;

    std::uint64_t nextFrameIndex = 0;
    std::uint64_t periodNs = 0;
    Clock::time_point nextDeadline{};

    mutable std::mutex mutex;
    std::atomic<bool> opened{false};
    ErrorInfo error;

    void setError(ErrorInfo value) { error = std::move(value); }
    void setError(ErrorCode code, const char* where, std::string message)
    {
        error = MakeError(code, where, std::move(message));
    }
    void clearError() { error = NoError(); }

    bool finishSlot(CommandSlot& slot, std::uint32_t timeoutMs)
    {
        if (!slot.completion.isValid()) return true;
        if (!slot.completion.wait(timeoutMs)) {
            setError(
                ErrorCode::Timeout,
                "SyntheticFrameSource::finishSlot",
                "Timed out waiting for a synthetic frame command slot");
            return false;
        }
        slot.completion = {};
        return true;
    }

    void advanceDeadline(Clock::time_point completedAt)
    {
        const auto period = std::chrono::nanoseconds(
            static_cast<std::chrono::nanoseconds::rep>(periodNs));
        nextDeadline += period;
        if (nextDeadline < completedAt) {
            nextDeadline = completedAt + period;
        }
    }
};

SyntheticFrameSource::SyntheticFrameSource()
    : impl_(std::make_unique<Impl>())
{
}

SyntheticFrameSource::~SyntheticFrameSource()
{
    close();
}

bool SyntheticFrameSource::initialize(
    D3D12BackendContext backend,
    SyntheticFrameSourceConfig config)
{
    auto next = std::make_unique<Impl>();
    next->configValue = config;

    if (!config.isValid()) {
        next->setError(
            ErrorCode::InvalidArgument,
            "SyntheticFrameSource::initialize",
            "Invalid synthetic frame source configuration");
        close();
        impl_ = std::move(next);
        return false;
    }

    if (!backend.resolve() || !backend.device || !backend.corePtr) {
        next->setError(
            ErrorCode::InvalidArgument,
            "SyntheticFrameSource::initialize",
            "D3D12 backend context is incomplete");
        close();
        impl_ = std::move(next);
        return false;
    }

    try {
        next->queue.Initialize(
            backend.device,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            D3D12_COMMAND_QUEUE_PRIORITY_NORMAL);

        next->backend = std::move(backend);
        next->backend.queue = &next->queue;
        next->backend.commandQueue = next->queue.Get();

        const auto bytecode = D3D12CoreLib::CompileShaderFromSource_D3DCompile(
            SyntheticFrameShader,
            "main",
            "cs_5_0",
            "IC4Ext_SyntheticFrameSource.hlsl");

        D3D12CoreLib::ComputePipelineDesc pipelineDescription;
        // Keep the template root signature indices stable across D3D12Helper
        // versions. The shader does not read t0, but the generated root
        // signature then uses the conventional SRV/UAV/constants slots.
        pipelineDescription.numSrvs = 1;
        pipelineDescription.numUavs = 1;
        pipelineDescription.numRootConstantValues =
            static_cast<UINT>(sizeof(ShaderConstants) / sizeof(std::uint32_t));
        next->pipeline.InitializeWithTemplate(
            next->backend.device,
            bytecode,
            pipelineDescription);

        for (auto& slot : next->slots) {
            slot.context.Initialize(next->backend.device, D3D12_COMMAND_LIST_TYPE_DIRECT);
            slot.descriptors.Initialize(
                next->backend.device,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                1,
                true);
        }

        FramePoolConfig poolConfig;
        poolConfig.width = config.width;
        poolConfig.height = config.height;
        poolConfig.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        poolConfig.resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        poolConfig.writeState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        poolConfig.publishedState = D3D12_RESOURCE_STATE_GENERIC_READ;
        poolConfig.createSrv = true;
        poolConfig.createUav = true;
        poolConfig.initialCapacity = config.initialFramePoolCapacity;
        poolConfig.maxCapacity = config.maxFramePoolCapacity;
        poolConfig.exhaustionPolicy = config.framePoolExhaustionPolicy;
        poolConfig.waitTimeout = config.framePoolWaitTimeout;

        if (!next->pool.initialize(next->backend, poolConfig)) {
            next->setError(next->pool.lastError());
            close();
            impl_ = std::move(next);
            return false;
        }

        next->periodNs = config.framePeriodNs();
        next->nextDeadline = Clock::now();
        next->nextFrameIndex = 0;
        next->statsValue = {};
        next->statsValue.framePool = next->pool.stats();
        next->clearError();
        next->opened.store(true);
    } catch (const std::exception& exception) {
        next->setError(
            ErrorCode::D3D12Error,
            "SyntheticFrameSource::initialize",
            exception.what());
        close();
        impl_ = std::move(next);
        return false;
    }

    close();
    impl_ = std::move(next);
    return true;
}

void SyntheticFrameSource::close() noexcept
{
    if (!impl_) return;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->opened.store(false);

    for (auto& slot : impl_->slots) {
        if (slot.completion.isValid()) {
            slot.completion.wait(impl_->configValue.gpuWaitTimeoutMs);
            slot.completion = {};
        }
    }

    try {
        if (impl_->queue.Get()) {
            impl_->queue.WaitIdle();
        }
    } catch (...) {
    }

    impl_->pool.reset();
}

bool SyntheticFrameSource::isOpened() const noexcept
{
    return impl_ && impl_->opened.load();
}

bool SyntheticFrameSource::read(
    const CameraReadOptions& options,
    D3D12ReadOnlyFrame& outFrame,
    ErrorInfo& outError)
{
    outFrame = {};
    outError = NoError();

    if (!impl_) {
        outError = MakeError(
            ErrorCode::NotOpened,
            "SyntheticFrameSource::read",
            "Synthetic frame source has no implementation state");
        return false;
    }

    std::unique_lock<std::mutex> lock(impl_->mutex);
    if (!impl_->opened.load()) {
        outError = MakeError(
            ErrorCode::NotOpened,
            "SyntheticFrameSource::read",
            "Synthetic frame source is not initialized");
        impl_->setError(outError);
        return false;
    }

    if (impl_->configValue.frameLimit != 0 &&
        impl_->nextFrameIndex >= impl_->configValue.frameLimit) {
        const auto sleepTime = std::chrono::milliseconds(
            std::min<std::uint32_t>(options.timeoutMs, 1u));
        if (sleepTime.count() > 0) {
            lock.unlock();
            std::this_thread::sleep_for(sleepTime);
            lock.lock();
        }
        ++impl_->statsValue.readTimeouts;
        outError = TimeoutError("Synthetic frame limit has been reached");
        impl_->setError(outError);
        return false;
    }

    auto now = Clock::now();
    if (now < impl_->nextDeadline) {
        const auto remaining = impl_->nextDeadline - now;
        const bool infiniteTimeout = options.timeoutMs == INFINITE;
        const auto timeout = std::chrono::milliseconds(options.timeoutMs);

        if (!infiniteTimeout && (options.timeoutMs == 0 || remaining > timeout)) {
            if (options.timeoutMs > 0) {
                lock.unlock();
                std::this_thread::sleep_for(timeout);
                lock.lock();
            }
            ++impl_->statsValue.readTimeouts;
            outError = TimeoutError("Next synthetic frame is not due before the read timeout");
            impl_->setError(outError);
            return false;
        }

        const auto deadline = impl_->nextDeadline;
        lock.unlock();
        std::this_thread::sleep_until(deadline);
        lock.lock();

        if (!impl_->opened.load()) {
            outError = MakeError(
                ErrorCode::NotOpened,
                "SyntheticFrameSource::read",
                "Synthetic frame source was closed while waiting");
            impl_->setError(outError);
            return false;
        }
    }

    const auto scheduledDeadline = impl_->nextDeadline;
    const auto hostTimestamp = Clock::now();
    const auto period = std::chrono::nanoseconds(
        static_cast<std::chrono::nanoseconds::rep>(impl_->periodNs));
    if (hostTimestamp > scheduledDeadline + period) {
        ++impl_->statsValue.lateFrames;
    }

    auto writer = impl_->pool.acquire();
    if (!writer) {
        ++impl_->statsValue.poolAcquireFailures;
        impl_->statsValue.framePool = impl_->pool.stats();
        outError = impl_->pool.lastError();
        if (!outError) {
            outError = MakeError(
                ErrorCode::Timeout,
                "SyntheticFrameSource::read",
                "Synthetic frame pool is exhausted");
        }
        impl_->setError(outError);
        return false;
    }

    auto& slot = impl_->slots[impl_->nextSlot % impl_->slots.size()];
    impl_->nextSlot = (impl_->nextSlot + 1) % impl_->slots.size();
    if (!impl_->finishSlot(slot, impl_->configValue.gpuWaitTimeoutMs)) {
        ++impl_->statsValue.gpuGenerationFailures;
        outError = impl_->error;
        return false;
    }

    ::IC4Ext::D3D12ReadyToken ready;
    bool submitted = false;

    try {
        slot.context.Reset();
        slot.descriptors.Reset();

        ID3D12Resource* outputResource = writer.resource();
        if (!outputResource || writer.uavCpuHandle().ptr == 0) {
            ++impl_->statsValue.gpuGenerationFailures;
            outError = MakeError(
                ErrorCode::InternalError,
                "SyntheticFrameSource::read",
                "FramePool writer does not expose an UAV-capable texture");
            impl_->setError(outError);
            return false;
        }

        if (writer.initialState() != writer.writeState()) {
            slot.context.ResourceBarrier(TransitionBarrier(
                outputResource,
                writer.initialState(),
                writer.writeState()));
        }

        const auto outputUav = slot.descriptors.Allocate();
        impl_->backend.device->CopyDescriptorsSimple(
            1,
            outputUav.cpu,
            writer.uavCpuHandle(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        auto* commandList = slot.context.GetCommandList();
        ID3D12DescriptorHeap* heaps[] = {slot.descriptors.GetHeap()};
        commandList->SetDescriptorHeaps(1, heaps);
        impl_->pipeline.Bind(commandList);
        commandList->SetComputeRootDescriptorTable(
            impl_->pipeline.UavTableIndex(),
            outputUav.gpu);

        ShaderConstants constants;
        constants.width = impl_->configValue.width;
        constants.height = impl_->configValue.height;
        constants.frameIndex = static_cast<std::uint32_t>(impl_->nextFrameIndex);
        constants.seedLow = static_cast<std::uint32_t>(impl_->configValue.seed);
        constants.seedHigh = static_cast<std::uint32_t>(impl_->configValue.seed >> 32u);
        constants.pattern = static_cast<std::uint32_t>(impl_->configValue.pattern);
        commandList->SetComputeRoot32BitConstants(
            impl_->pipeline.RootConstantsIndex(),
            static_cast<UINT>(sizeof(constants) / sizeof(std::uint32_t)),
            &constants,
            0);

        commandList->Dispatch(
            (impl_->configValue.width + ThreadGroupSize - 1u) / ThreadGroupSize,
            (impl_->configValue.height + ThreadGroupSize - 1u) / ThreadGroupSize,
            1);

        slot.context.ResourceBarrier(UavBarrier(outputResource));
        if (writer.writeState() != writer.publishedState()) {
            slot.context.ResourceBarrier(TransitionBarrier(
                outputResource,
                writer.writeState(),
                writer.publishedState()));
        }

        slot.context.Close();
        ID3D12CommandList* commandLists[] = {slot.context.GetCommandList()};
        impl_->queue.ExecuteCommandLists(1, commandLists);
        submitted = true;

        ready.value = impl_->queue.Signal();
        ready.fence = impl_->queue.Fence().Get();
        if (!ready.isValid()) {
            throw std::runtime_error("Synthetic queue returned an invalid ready token");
        }
        slot.completion = ready;

        FrameTiming timing;
        timing.frameNumber = FrameNumberFor(impl_->configValue, impl_->nextFrameIndex);
        timing.deviceTimestampNs = DeviceTimestampFor(
            impl_->configValue,
            impl_->nextFrameIndex,
            impl_->periodNs);
        timing.hostReceivedTime = hostTimestamp;

        FrameFormatMetadata format;
        format.requestedFormat = CameraPixelFormat::BGRa8;
        format.actualInputFormat = CameraPixelFormat::BGRa8;
        format.outputFormat = GpuFrameFormat::RGBA8;
        format.width = static_cast<int>(impl_->configValue.width);
        format.height = static_cast<int>(impl_->configValue.height);
        format.inputRowPitchBytes =
            static_cast<std::size_t>(impl_->configValue.width) * 4u;

        outFrame = writer.publish(ready, timing, format, {});
        if (!outFrame) {
            ready.wait(impl_->configValue.gpuWaitTimeoutMs);
            throw std::runtime_error("FramePool writer failed to publish the synthetic frame");
        }

        ++impl_->nextFrameIndex;
        ++impl_->statsValue.generatedFrames;
        impl_->statsValue.lastFrameNumber = timing.frameNumber;
        impl_->statsValue.lastDeviceTimestampNs = timing.deviceTimestampNs;
        impl_->statsValue.framePool = impl_->pool.stats();
        impl_->advanceDeadline(Clock::now());
        impl_->clearError();
        outError = NoError();
        return true;
    } catch (const std::exception& exception) {
        if (submitted && ready.isValid()) {
            ready.wait(impl_->configValue.gpuWaitTimeoutMs);
        }
        ++impl_->statsValue.gpuGenerationFailures;
        impl_->statsValue.framePool = impl_->pool.stats();
        outError = MakeError(
            ErrorCode::D3D12Error,
            "SyntheticFrameSource::read",
            exception.what());
        impl_->setError(outError);
        return false;
    }
}

SyntheticFrameSourceConfig SyntheticFrameSource::config() const
{
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->configValue;
}

SyntheticFrameSourceStats SyntheticFrameSource::stats() const
{
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto result = impl_->statsValue;
    result.framePool = impl_->pool.stats();
    return result;
}

FramePoolStats SyntheticFrameSource::framePoolStats() const
{
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->pool.stats();
}

ErrorInfo SyntheticFrameSource::lastError() const
{
    if (!impl_) return NoError();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->error;
}

} // namespace IC4Ext::D3D12

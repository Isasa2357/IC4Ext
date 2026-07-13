#include "SobelProcessor.hpp"

#include <IC4Ext/D3D11/D3D11FenceManager.hpp>

#include <D3D11Helper/D3D11Gpu/D3D11BindingSet.hpp>
#include <D3D11Helper/D3D11Gpu/D3D11ComputePipeline.hpp>
#include <D3D11Helper/D3D11Gpu/D3D11ShaderCompiler.hpp>

#include <array>
#include <chrono>
#include <exception>
#include <mutex>
#include <string>
#include <utility>

namespace IC4ExtStressD3D11 {
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

} // namespace

class SobelProcessor::Impl
{
public:
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
        IC4Ext::D3D11ReadyToken completion;
        IC4Ext::D3D11::ReadOnlyFrame inputHold;
        IC4Ext::D3D11::ReadOnlyFrame outputHold;
    };

    IC4Ext::D3D11BackendContext backend;
    D3D11CoreLib::D3D11ComputePipeline pipeline;
    IC4Ext::D3D11FenceManager fence;
    std::array<Slot, SlotCount> slots;
    std::size_t nextSlot = 0;

    IC4Ext::D3D11::FramePool outputPool;
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
            fail(
                IC4Ext::ErrorCode::Timeout,
                "D3D11 SobelProcessor::finish",
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
        return ok;
    }

    bool ensureOutputPool(const IC4Ext::D3D11::ReadOnlyFrame& input)
    {
        if (!input.texture()) {
            fail(
                IC4Ext::ErrorCode::InvalidArgument,
                "D3D11 SobelProcessor::ensureOutputPool",
                "input texture is null");
            return false;
        }

        D3D11_TEXTURE2D_DESC description{};
        input.texture()->GetDesc(&description);
        if (description.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
            description.Width == 0 || description.Height == 0 ||
            description.ArraySize != 1 || description.MipLevels != 1 ||
            description.SampleDesc.Count != 1) {
            fail(
                IC4Ext::ErrorCode::UnsupportedFormat,
                "D3D11 SobelProcessor::ensureOutputPool",
                "the HLSL stress workload requires a non-MSAA RGBA8 Texture2D");
            return false;
        }

        const auto nextWidth = description.Width;
        const auto nextHeight = description.Height;
        if (outputPool.isInitialized() &&
            nextWidth == width && nextHeight == height) {
            return true;
        }

        if (!flush(10'000)) return false;
        outputPool.reset();

        IC4Ext::D3D11::FramePoolConfig config;
        config.width = nextWidth;
        config.height = nextHeight;
        config.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        config.createSrv = true;
        config.createUav = true;
        config.initialCapacity = 8;
        config.maxCapacity = 32;
        config.exhaustionPolicy =
            IC4Ext::D3D11::FramePoolExhaustionPolicy::WaitWithTimeout;
        config.waitTimeout = std::chrono::milliseconds(1000);

        if (!outputPool.initialize(backend, config)) {
            const auto poolError = outputPool.lastError();
            fail(
                IC4Ext::ErrorCode::D3D11Error,
                "D3D11 SobelProcessor::ensureOutputPool",
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
    const IC4Ext::D3D11BackendContext& producerBackend)
{
    if (!impl_) return false;
    auto resolved = producerBackend;
    if (!resolved.resolve() || !resolved.device || !resolved.immediateContext) {
        impl_->fail(
            IC4Ext::ErrorCode::InvalidArgument,
            "D3D11 SobelProcessor::initialize",
            "producer backend is incomplete");
        return false;
    }

    try {
        impl_->backend = std::move(resolved);
        const auto bytecode =
            D3D11CoreLib::CompileShaderFromSource_D3DCompile(
                SobelShader,
                "main",
                "cs_5_0",
                "MultiPipelineStressD3D11_Sobel.hlsl");
        impl_->pipeline.Initialize(impl_->backend.device, bytecode);

        if (!impl_->fence.initialize(
                impl_->backend.device,
                impl_->backend.immediateContext)) {
            const auto fenceError = impl_->fence.lastError();
            impl_->fail(
                IC4Ext::ErrorCode::D3D11Error,
                "D3D11 SobelProcessor::initialize",
                fenceError.where + ": " + fenceError.message);
            return false;
        }

        for (auto& slot : impl_->slots) {
            D3D11_BUFFER_DESC description{};
            description.ByteWidth = sizeof(Constants);
            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            const HRESULT result = impl_->backend.device->CreateBuffer(
                &description,
                nullptr,
                &slot.constants);
            if (FAILED(result)) {
                impl_->fail(
                    IC4Ext::ErrorCode::D3D11Error,
                    "D3D11 SobelProcessor::CreateBuffer",
                    "failed to create a constant buffer");
                return false;
            }
        }
    } catch (const std::exception& exception) {
        impl_->fail(
            IC4Ext::ErrorCode::D3D11Error,
            "D3D11 SobelProcessor::initialize",
            exception.what());
        return false;
    }

    impl_->initialized = true;
    impl_->clearError();
    return true;
}

bool SobelProcessor::process(const IC4Ext::D3D11::ReadOnlyFrame& input)
{
    if (!impl_ || !impl_->initialized || !input || !input.hasSrv()) {
        if (impl_) {
            impl_->fail(
                IC4Ext::ErrorCode::InvalidArgument,
                "D3D11 SobelProcessor::process",
                "processor is not initialized or input is invalid");
        }
        return false;
    }
    if (!impl_->ensureOutputPool(input)) return false;

    auto& slot = impl_->slots[impl_->nextSlot % impl_->slots.size()];
    impl_->nextSlot = (impl_->nextSlot + 1) % impl_->slots.size();
    if (!impl_->finish(slot, 10'000)) return false;
    if (!input.waitReady(10'000)) {
        impl_->fail(
            IC4Ext::ErrorCode::Timeout,
            "D3D11 SobelProcessor::process",
            "timed out waiting for the producer frame");
        return false;
    }

    auto writer = impl_->outputPool.acquire();
    if (!writer) {
        const auto poolError = impl_->outputPool.lastError();
        impl_->fail(
            IC4Ext::ErrorCode::D3D11Error,
            "D3D11 SobelProcessor::process",
            poolError.where + ": " + poolError.message);
        return false;
    }

    try {
        Constants constants;
        constants.width = impl_->width;
        constants.height = impl_->height;
        impl_->backend.immediateContext->UpdateSubresource(
            slot.constants.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);

        {
            D3D11CoreLib::D3D11ComputeBindingSet bindings;
            bindings.SetShaderResource(0, input.srv());
            bindings.SetUnorderedAccess(0, writer.uav());
            bindings.SetConstantBuffer(0, slot.constants.Get());
            D3D11CoreLib::D3D11ScopedComputeBindings scoped(
                impl_->backend.immediateContext,
                bindings);
            impl_->pipeline.Dispatch(
                impl_->backend.immediateContext,
                (impl_->width + ThreadGroupSize - 1u) / ThreadGroupSize,
                (impl_->height + ThreadGroupSize - 1u) / ThreadGroupSize,
                1);
        }

        const auto completion = impl_->fence.signal();
        if (!completion.isValid()) {
            const auto fenceError = impl_->fence.lastError();
            impl_->fail(
                IC4Ext::ErrorCode::D3D11Error,
                "D3D11 SobelProcessor::process",
                fenceError.where + ": " + fenceError.message);
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
            impl_->fail(
                IC4Ext::ErrorCode::InternalError,
                "D3D11 SobelProcessor::process",
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
        impl_->fail(
            IC4Ext::ErrorCode::D3D11Error,
            "D3D11 SobelProcessor::process",
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

} // namespace IC4ExtStressD3D11

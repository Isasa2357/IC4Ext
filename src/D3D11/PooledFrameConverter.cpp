#include "IC4Ext/D3D11/PooledFrameConverter.hpp"

#include "IC4Ext/D3D11/D3D11FenceManager.hpp"

#include <D3D11Helper/D3D11Gpu/D3D11BindingSet.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace IC4Ext::D3D11 {
namespace {

constexpr std::size_t SlotCount = 4;

struct ConvertConstants
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t inputRowPitchBytes = 0;
    std::uint32_t inputPixelFormat = 0;
};

DXGI_FORMAT ToDxgiFormat(GpuFrameFormat format) noexcept
{
    switch (format) {
    case GpuFrameFormat::R8: return DXGI_FORMAT_R8_UNORM;
    case GpuFrameFormat::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

std::string HrText(HRESULT value)
{
    std::ostringstream stream;
    stream << "HRESULT=0x" << std::hex << static_cast<unsigned long>(value);
    return stream.str();
}

} // namespace

class D3D11PooledFrameConverter::Impl
{
public:
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> inputBuffer;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> inputSrv;
        std::uint32_t inputCapacityBytes = 0;
        Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
        ::IC4Ext::D3D11ReadyToken inFlight;
    };

    ::IC4Ext::D3D11FrameConverter* converter = nullptr;
    std::array<Slot, SlotCount> slots;
    std::size_t nextSlot = 0;
    D3D11PooledFrameConverterStats statsValue;
    ErrorInfo error;
    mutable std::mutex mutex;

    void setError(ErrorCode code, const char* where, std::string message)
    {
        error = MakeError(code, where, std::move(message));
    }

    void refreshCacheStats() noexcept
    {
        statsValue.cachedInputBufferCount = 0;
        statsValue.cachedInputBufferBytes = 0;
        for (const auto& slot : slots) {
            if (slot.inputBuffer) {
                ++statsValue.cachedInputBufferCount;
                statsValue.cachedInputBufferBytes += slot.inputCapacityBytes;
            }
        }
    }

    bool waitIdleLocked(std::uint32_t timeoutMs) noexcept
    {
        for (auto& slot : slots) {
            if (!slot.inFlight.isValid()) continue;
            if (!slot.inFlight.wait(timeoutMs)) {
                setError(
                    ErrorCode::Timeout,
                    "D3D11PooledFrameConverter::waitIdle",
                    "Timed out waiting for a submitted converter slot");
                return false;
            }
            slot.inFlight = {};
        }
        error = NoError();
        return true;
    }
};

D3D11PooledFrameConverter::D3D11PooledFrameConverter()
    : impl_(std::make_unique<Impl>())
{
}

D3D11PooledFrameConverter::~D3D11PooledFrameConverter()
{
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->waitIdleLocked(5000);
    }
}

D3D11PooledFrameConverter::D3D11PooledFrameConverter(
    D3D11PooledFrameConverter&&) noexcept = default;
D3D11PooledFrameConverter& D3D11PooledFrameConverter::operator=(
    D3D11PooledFrameConverter&&) noexcept = default;

bool D3D11PooledFrameConverter::initialize(
    ::IC4Ext::D3D11FrameConverter& converter)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!converter.device_ || !converter.context_ || !converter.fenceManager_) {
        impl_->setError(
            ErrorCode::InvalidArgument,
            "D3D11PooledFrameConverter::initialize",
            "Source D3D11FrameConverter is not initialized");
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(converter.context_->QueryInterface(IID_PPV_ARGS(&multithread))) &&
        multithread) {
        multithread->SetMultithreadProtected(TRUE);
    }

    if (!impl_->waitIdleLocked(5000)) return false;
    impl_->converter = &converter;
    impl_->slots = {};
    impl_->nextSlot = 0;
    impl_->statsValue = {};
    impl_->error = NoError();
    return true;
}

bool D3D11PooledFrameConverter::convert(
    const ::IC4Ext::CpuFrameView& input,
    const FrameOutputSpec& outputSpec,
    D3D11FrameWriter writer,
    FrameChunkMetadata chunkMetadata,
    D3D11ReadOnlyFrame& outFrame)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    outFrame = {};

    auto* converter = impl_->converter;
    if (!converter) {
        impl_->setError(
            ErrorCode::InvalidArgument,
            "D3D11PooledFrameConverter::convert",
            "Pooled converter is not initialized");
        return false;
    }
    if (!writer) {
        impl_->setError(
            ErrorCode::InvalidArgument,
            "D3D11PooledFrameConverter::convert",
            "FramePool writer is invalid");
        return false;
    }
    if (!input.data || input.dataSize == 0 || input.format.width <= 0 ||
        input.format.height <= 0 || input.format.inputRowPitchBytes == 0) {
        impl_->setError(
            ErrorCode::InvalidArgument,
            "D3D11PooledFrameConverter::convert",
            "Input frame view is incomplete");
        return false;
    }
    if (input.dataSize > static_cast<std::size_t>(std::numeric_limits<UINT>::max())) {
        impl_->setError(
            ErrorCode::InvalidArgument,
            "D3D11PooledFrameConverter::convert",
            "Input frame is too large for a D3D11 buffer");
        return false;
    }

    const auto inputFormat = input.format.actualInputFormat;
    const auto outputFormat = outputSpec.outputFormat;
    if (!converter->isSupported(inputFormat, outputFormat)) {
        impl_->setError(
            ErrorCode::UnsupportedFormat,
            "D3D11PooledFrameConverter::convert",
            std::string("Unsupported conversion: ") + ToString(inputFormat) +
                " -> " + ToString(outputFormat));
        return false;
    }

    D3D11CoreLib::D3D11ComputePipeline* pipeline = nullptr;
    bool* pipelineReady = nullptr;
    if (inputFormat == CameraPixelFormat::Mono8 &&
        outputFormat == GpuFrameFormat::R8) {
        pipeline = &converter->mono8ToR8_;
        pipelineReady = &converter->mono8ToR8Ready_;
    } else if (inputFormat == CameraPixelFormat::Mono8) {
        pipeline = &converter->mono8ToRgba8_;
        pipelineReady = &converter->mono8ToRgba8Ready_;
    } else if (inputFormat == CameraPixelFormat::BGR8) {
        pipeline = &converter->bgr8ToRgba8_;
        pipelineReady = &converter->bgr8ToRgba8Ready_;
    } else if (inputFormat == CameraPixelFormat::BGRa8) {
        pipeline = &converter->bgra8ToRgba8_;
        pipelineReady = &converter->bgra8ToRgba8Ready_;
    } else {
        pipeline = &converter->bayer8ToRgba8_;
        pipelineReady = &converter->bayer8ToRgba8Ready_;
    }
    if (!pipeline || !pipelineReady ||
        !converter->ensurePipeline(
            inputFormat,
            outputFormat,
            *pipeline,
            *pipelineReady)) {
        impl_->error = converter->lastError_;
        return false;
    }

    ID3D11Texture2D* outputTexture = writer.texture();
    ID3D11UnorderedAccessView* outputUav = writer.uav();
    if (!outputTexture || !outputUav || !writer.srv()) {
        impl_->setError(
            ErrorCode::InvalidArgument,
            "D3D11PooledFrameConverter::convert",
            "FramePool writer must expose Texture2D, SRV and UAV");
        return false;
    }

    D3D11_TEXTURE2D_DESC outputDescription{};
    outputTexture->GetDesc(&outputDescription);
    if (outputDescription.Width != static_cast<UINT>(input.format.width) ||
        outputDescription.Height != static_cast<UINT>(input.format.height) ||
        outputDescription.Format != ToDxgiFormat(outputFormat)) {
        impl_->setError(
            ErrorCode::InvalidArgument,
            "D3D11PooledFrameConverter::convert",
            "FramePool texture shape or format does not match conversion output");
        return false;
    }

    auto& slot = impl_->slots[impl_->nextSlot % impl_->slots.size()];
    impl_->nextSlot = (impl_->nextSlot + 1) % impl_->slots.size();
    if (slot.inFlight.isValid() && !slot.inFlight.wait(5000)) {
        impl_->setError(
            ErrorCode::Timeout,
            "D3D11PooledFrameConverter::convert",
            "Timed out waiting for a reusable converter slot");
        return false;
    }
    slot.inFlight = {};

    const UINT requiredBytes = static_cast<UINT>(input.dataSize);
    if (!slot.inputBuffer || slot.inputCapacityBytes < requiredBytes) {
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = requiredBytes;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
        HRESULT result = converter->device_->CreateBuffer(
            &description,
            nullptr,
            &buffer);
        if (FAILED(result)) {
            impl_->setError(
                ErrorCode::D3D11Error,
                "D3D11PooledFrameConverter::CreateBuffer",
                HrText(result));
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDescription{};
        srvDescription.Format = DXGI_FORMAT_R8_UINT;
        srvDescription.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDescription.Buffer.FirstElement = 0;
        srvDescription.Buffer.NumElements = requiredBytes;

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        result = converter->device_->CreateShaderResourceView(
            buffer.Get(),
            &srvDescription,
            &srv);
        if (FAILED(result)) {
            impl_->setError(
                ErrorCode::D3D11Error,
                "D3D11PooledFrameConverter::CreateShaderResourceView",
                HrText(result));
            return false;
        }
        slot.inputBuffer = std::move(buffer);
        slot.inputSrv = std::move(srv);
        slot.inputCapacityBytes = requiredBytes;
        ++impl_->statsValue.inputBufferAllocations;
        impl_->refreshCacheStats();
    } else {
        ++impl_->statsValue.inputBufferReuses;
    }

    if (!slot.constants) {
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = sizeof(ConvertConstants);
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        HRESULT result = converter->device_->CreateBuffer(
            &description,
            nullptr,
            &slot.constants);
        if (FAILED(result)) {
            impl_->setError(
                ErrorCode::D3D11Error,
                "D3D11PooledFrameConverter::CreateConstantBuffer",
                HrText(result));
            return false;
        }
    }

    D3D11_BOX inputBox{};
    inputBox.left = 0;
    inputBox.right = requiredBytes;
    inputBox.top = 0;
    inputBox.bottom = 1;
    inputBox.front = 0;
    inputBox.back = 1;
    converter->context_->UpdateSubresource(
        slot.inputBuffer.Get(),
        0,
        &inputBox,
        input.data,
        0,
        0);

    ConvertConstants constants;
    constants.width = static_cast<std::uint32_t>(input.format.width);
    constants.height = static_cast<std::uint32_t>(input.format.height);
    constants.inputRowPitchBytes =
        static_cast<std::uint32_t>(input.format.inputRowPitchBytes);
    constants.inputPixelFormat = static_cast<std::uint32_t>(inputFormat);
    converter->context_->UpdateSubresource(
        slot.constants.Get(),
        0,
        nullptr,
        &constants,
        0,
        0);

    try {
        D3D11CoreLib::D3D11ComputeBindingSet bindings;
        bindings.SetShaderResource(0, slot.inputSrv.Get());
        bindings.SetUnorderedAccess(0, outputUav);
        bindings.SetConstantBuffer(0, slot.constants.Get());
        D3D11CoreLib::D3D11ScopedComputeBindings scoped(
            converter->context_,
            bindings);
        pipeline->Dispatch(
            converter->context_,
            (static_cast<UINT>(input.format.width) + 15u) / 16u,
            (static_cast<UINT>(input.format.height) + 15u) / 16u,
            1);
    } catch (const std::exception& exception) {
        impl_->setError(
            ErrorCode::D3D11Error,
            "D3D11PooledFrameConverter::Dispatch",
            exception.what());
        return false;
    }

    slot.inFlight = converter->fenceManager_->signal();
    if (!slot.inFlight.isValid()) {
        impl_->error = converter->fenceManager_->lastError();
        return false;
    }

    FrameFormatMetadata outputMetadata = input.format;
    outputMetadata.outputFormat = outputFormat;
    outFrame = writer.publish(
        slot.inFlight,
        input.timing,
        std::move(outputMetadata),
        std::move(chunkMetadata));
    if (!outFrame) {
        impl_->setError(
            ErrorCode::InternalError,
            "D3D11PooledFrameConverter::convert",
            "Frame writer failed to publish after dispatch");
        return false;
    }

    ++impl_->statsValue.conversions;
    impl_->error = NoError();
    return true;
}

bool D3D11PooledFrameConverter::waitIdle(std::uint32_t timeoutMs) noexcept
{
    if (!impl_) return true;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->waitIdleLocked(timeoutMs);
}

D3D11PooledFrameConverterStats D3D11PooledFrameConverter::stats() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->statsValue;
}

ErrorInfo D3D11PooledFrameConverter::lastError() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->error;
}

} // namespace IC4Ext::D3D11

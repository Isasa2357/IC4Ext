#include "IC4Ext/D3D11/D3D11FrameConverter.hpp"
#include "IC4Ext/D3D11/D3D11FenceManager.hpp"

#include <D3D11Helper/D3D11Gpu/D3D11BindingSet.hpp>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace IC4Ext {
namespace {

std::string HrToString(HRESULT hr)
{
    std::ostringstream stream;
    stream << "HRESULT=0x" << std::hex << static_cast<unsigned long>(hr);
    return stream.str();
}

DXGI_FORMAT ToDxgi(GpuFrameFormat format) noexcept
{
    switch (format) {
    case GpuFrameFormat::R8: return DXGI_FORMAT_R8_UNORM;
    case GpuFrameFormat::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

std::string ShaderBaseName(
    CameraPixelFormat input,
    GpuFrameFormat output)
{
    if (input == CameraPixelFormat::Mono8 && output == GpuFrameFormat::R8) {
        return "IC4Ext_Convert_Mono8_To_R8";
    }
    if (input == CameraPixelFormat::Mono8 && output == GpuFrameFormat::RGBA8) {
        return "IC4Ext_Convert_Mono8_To_RGBA8";
    }
    if (input == CameraPixelFormat::BGR8 && output == GpuFrameFormat::RGBA8) {
        return "IC4Ext_Convert_BGR8_To_RGBA8";
    }
    if (input == CameraPixelFormat::BGRa8 && output == GpuFrameFormat::RGBA8) {
        return "IC4Ext_Convert_BGRa8_To_RGBA8";
    }
    if ((input == CameraPixelFormat::BayerRG8 ||
         input == CameraPixelFormat::BayerGR8 ||
         input == CameraPixelFormat::BayerGB8 ||
         input == CameraPixelFormat::BayerBG8) &&
        output == GpuFrameFormat::RGBA8) {
        return "IC4Ext_Convert_Bayer8_To_RGBA8";
    }
    return {};
}

struct ConvertConstants
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t inputRowPitchBytes = 0;
    std::uint32_t inputPixelFormat = 0;
};

} // namespace

void D3D11FrameConverter::setError(
    ErrorCode code,
    const std::string& where,
    const std::string& message)
{
    lastError_ = MakeError(code, where, message);
}

bool D3D11FrameConverter::initialize(
    D3D11CoreLib::D3D11Core* core,
    D3D11FenceManager* fenceManager,
    const ShaderLoadConfig& shaderConfig)
{
    lastError_ = NoError();
    contextMutex_.reset();
    if (!core || !fenceManager) {
        setError(
            ErrorCode::InvalidArgument,
            "D3D11FrameConverter::initialize",
            "D3D11Core/fenceManager is null");
        return false;
    }

    core_ = core;
    device_ = core_->GetDevice();
    context_ = core_->GetImmediateContext();
    fenceManager_ = fenceManager;
    shaderConfig_ = shaderConfig;
    if (!device_ || !context_) {
        setError(
            ErrorCode::InvalidArgument,
            "D3D11FrameConverter::initialize",
            "D3D11Core has null device/context");
        return false;
    }

    contextMutex_ =
        D3D11::Detail::AcquireImmediateContextMutex(context_);
    Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(context_->QueryInterface(IID_PPV_ARGS(&multithread))) &&
        multithread) {
        multithread->SetMultithreadProtected(TRUE);
    }

    if (shaderConfig_.shaderDirectory.empty()) {
        shaderConfig_.shaderDirectory =
            std::filesystem::current_path() / "shaders" / "d3d11";
    }
    if (shaderConfig_.entryPoint.empty()) shaderConfig_.entryPoint = "main";
    if (shaderConfig_.target.empty()) shaderConfig_.target = "cs_5_0";
    return contextMutex_ != nullptr;
}

bool D3D11FrameConverter::initialize(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    D3D11FenceManager* fenceManager,
    const ShaderLoadConfig& shaderConfig)
{
    lastError_ = NoError();
    contextMutex_.reset();
    if (!device || !context || !fenceManager) {
        setError(
            ErrorCode::InvalidArgument,
            "D3D11FrameConverter::initialize",
            "device/context/fenceManager is null");
        return false;
    }

    core_ = nullptr;
    device_ = device;
    context_ = context;
    fenceManager_ = fenceManager;
    shaderConfig_ = shaderConfig;
    contextMutex_ =
        D3D11::Detail::AcquireImmediateContextMutex(context_);

    Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(context_->QueryInterface(IID_PPV_ARGS(&multithread))) &&
        multithread) {
        multithread->SetMultithreadProtected(TRUE);
    }

    if (shaderConfig_.shaderDirectory.empty()) {
        shaderConfig_.shaderDirectory =
            std::filesystem::current_path() / "shaders" / "d3d11";
    }
    if (shaderConfig_.entryPoint.empty()) shaderConfig_.entryPoint = "main";
    if (shaderConfig_.target.empty()) shaderConfig_.target = "cs_5_0";
    return contextMutex_ != nullptr;
}

bool D3D11FrameConverter::isSupported(
    CameraPixelFormat input,
    GpuFrameFormat output) const noexcept
{
    return IsSupportedConversion(input, output);
}

bool D3D11FrameConverter::loadShaderBytecode(
    const std::string& baseName,
    D3D11CoreLib::ShaderBytecode& outBytecode)
{
    const auto csoPath = shaderConfig_.shaderDirectory / (baseName + ".cso");
    const auto hlslPath = shaderConfig_.shaderDirectory / (baseName + ".hlsl");

    auto loadCso = [&]() -> bool {
        try {
            outBytecode = D3D11CoreLib::LoadShaderBytecodeFromFile(csoPath);
            return !outBytecode.Empty();
        } catch (const std::exception& exception) {
            setError(
                ErrorCode::ShaderError,
                "D3D11FrameConverter::loadShaderBytecode/cso",
                exception.what());
            return false;
        }
    };

    auto loadHlsl = [&]() -> bool {
        if (!std::filesystem::exists(hlslPath)) {
            setError(
                ErrorCode::ShaderError,
                "D3D11FrameConverter::loadShaderBytecode/hlsl",
                "Missing shader " + hlslPath.string());
            return false;
        }
        try {
            D3D11CoreLib::ShaderCompileDesc description;
            description.sourcePath = hlslPath;
            description.entryPoint = shaderConfig_.entryPoint;
            description.target = shaderConfig_.target;
            description.useDxc = false;
            outBytecode = D3D11CoreLib::CompileShaderFromFile(description);
            return !outBytecode.Empty();
        } catch (const std::exception& exception) {
            setError(
                ErrorCode::ShaderError,
                "D3D11FrameConverter::loadShaderBytecode/hlsl",
                exception.what());
            return false;
        }
    };

    if (shaderConfig_.inputKind == ShaderInputKind::CsoFile) return loadCso();
    if (shaderConfig_.inputKind == ShaderInputKind::HlslFile) return loadHlsl();
    if (shaderConfig_.preferCsoWhenBothExist) {
        if (std::filesystem::exists(csoPath) && loadCso()) return true;
        return loadHlsl();
    }
    if (std::filesystem::exists(hlslPath) && loadHlsl()) return true;
    return loadCso();
}

bool D3D11FrameConverter::ensurePipeline(
    CameraPixelFormat input,
    GpuFrameFormat output,
    D3D11CoreLib::D3D11ComputePipeline& pipeline,
    bool& ready)
{
    if (ready) return true;
    if (!device_) {
        setError(
            ErrorCode::InvalidArgument,
            "D3D11FrameConverter::ensurePipeline",
            "converter is not initialized");
        return false;
    }

    const std::string baseName = ShaderBaseName(input, output);
    if (baseName.empty()) {
        setError(
            ErrorCode::UnsupportedFormat,
            "D3D11FrameConverter::ensurePipeline",
            "Unsupported conversion");
        return false;
    }

    D3D11CoreLib::ShaderBytecode bytecode;
    if (!loadShaderBytecode(baseName, bytecode)) return false;
    try {
        pipeline.Initialize(device_, bytecode);
        ready = true;
        return true;
    } catch (const std::exception& exception) {
        setError(
            ErrorCode::ShaderError,
            "D3D11FrameConverter::ensurePipeline",
            exception.what());
        return false;
    }
}

bool D3D11FrameConverter::createRawInputBuffer(
    const CpuFrameView& input,
    D3D11CoreLib::D3D11Resource& buffer,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv)
{
    if (!input.data || input.dataSize == 0) {
        setError(
            ErrorCode::InvalidArgument,
            "D3D11FrameConverter::createRawInputBuffer",
            "input data is null or empty");
        return false;
    }
    if (input.dataSize > static_cast<std::size_t>(
            std::numeric_limits<UINT>::max())) {
        setError(
            ErrorCode::InvalidArgument,
            "D3D11FrameConverter::createRawInputBuffer",
            "input data is too large for a D3D11 buffer");
        return false;
    }

    D3D11_BUFFER_DESC description{};
    description.ByteWidth = static_cast<UINT>(input.dataSize);
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initialData{};
    initialData.pSysMem = input.data;

    Microsoft::WRL::ComPtr<ID3D11Buffer> rawBuffer;
    HRESULT result = device_->CreateBuffer(
        &description,
        &initialData,
        &rawBuffer);
    if (FAILED(result)) {
        setError(
            ErrorCode::D3D11Error,
            "D3D11FrameConverter::createRawInputBuffer/CreateBuffer",
            HrToString(result));
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDescription{};
    srvDescription.Format = DXGI_FORMAT_R8_UINT;
    srvDescription.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDescription.Buffer.FirstElement = 0;
    srvDescription.Buffer.NumElements = static_cast<UINT>(input.dataSize);
    result = device_->CreateShaderResourceView(
        rawBuffer.Get(),
        &srvDescription,
        &srv);
    if (FAILED(result)) {
        setError(
            ErrorCode::D3D11Error,
            "D3D11FrameConverter::createRawInputBuffer/CreateShaderResourceView",
            HrToString(result));
        return false;
    }

    buffer = D3D11CoreLib::D3D11Resource(std::move(rawBuffer));
    return true;
}

bool D3D11FrameConverter::createOutputTexture(
    const CpuFrameView& input,
    const FrameOutputSpec& spec,
    D3D11CoreLib::D3D11Resource& textureResource,
    D3D11CameraFrame& outFrame)
{
    const DXGI_FORMAT format = ToDxgi(spec.outputFormat);
    if (format == DXGI_FORMAT_UNKNOWN ||
        input.format.width <= 0 || input.format.height <= 0) {
        setError(
            ErrorCode::UnsupportedFormat,
            "D3D11FrameConverter::createOutputTexture",
            "Unsupported output format or frame size");
        return false;
    }

    D3D11_TEXTURE2D_DESC description{};
    description.Width = static_cast<UINT>(input.format.width);
    description.Height = static_cast<UINT>(input.format.height);
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if (spec.createSrv) {
        description.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    }

    HRESULT result = device_->CreateTexture2D(
        &description,
        nullptr,
        &outFrame.texture);
    if (FAILED(result)) {
        setError(
            ErrorCode::D3D11Error,
            "D3D11FrameConverter::createOutputTexture/CreateTexture2D",
            HrToString(result));
        return false;
    }

    if (spec.createSrv) {
        result = device_->CreateShaderResourceView(
            outFrame.texture.Get(),
            nullptr,
            &outFrame.srv);
        if (FAILED(result)) {
            setError(
                ErrorCode::D3D11Error,
                "D3D11FrameConverter::createOutputTexture/CreateShaderResourceView",
                HrToString(result));
            return false;
        }
    }

    result = device_->CreateUnorderedAccessView(
        outFrame.texture.Get(),
        nullptr,
        &outFrame.uav);
    if (FAILED(result)) {
        setError(
            ErrorCode::D3D11Error,
            "D3D11FrameConverter::createOutputTexture/CreateUnorderedAccessView",
            HrToString(result));
        return false;
    }

    textureResource = D3D11CoreLib::D3D11Resource(outFrame.texture);
    return true;
}

bool D3D11FrameConverter::createConstantBuffer(
    const CpuFrameView& input,
    D3D11CoreLib::D3D11Resource& buffer)
{
    ConvertConstants constants{};
    constants.width = static_cast<std::uint32_t>(input.format.width);
    constants.height = static_cast<std::uint32_t>(input.format.height);
    constants.inputRowPitchBytes =
        static_cast<std::uint32_t>(input.format.inputRowPitchBytes);
    constants.inputPixelFormat =
        static_cast<std::uint32_t>(input.format.actualInputFormat);

    D3D11_BUFFER_DESC description{};
    description.ByteWidth = sizeof(ConvertConstants);
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    D3D11_SUBRESOURCE_DATA initialData{};
    initialData.pSysMem = &constants;

    Microsoft::WRL::ComPtr<ID3D11Buffer> rawBuffer;
    const HRESULT result = device_->CreateBuffer(
        &description,
        &initialData,
        &rawBuffer);
    if (FAILED(result)) {
        setError(
            ErrorCode::D3D11Error,
            "D3D11FrameConverter::createConstantBuffer",
            HrToString(result));
        return false;
    }
    buffer = D3D11CoreLib::D3D11Resource(std::move(rawBuffer));
    return true;
}

bool D3D11FrameConverter::convert(
    const CpuFrameView& input,
    const FrameOutputSpec& outputSpec,
    D3D11CameraFrame& outFrame)
{
    lastError_ = NoError();
    outFrame = {};

    if (!device_ || !context_ || !contextMutex_ || !fenceManager_) {
        setError(
            ErrorCode::InvalidArgument,
            "D3D11FrameConverter::convert",
            "converter is not initialized");
        return false;
    }
    if (!input.data || input.dataSize == 0 || input.format.width <= 0 ||
        input.format.height <= 0 || input.format.inputRowPitchBytes == 0) {
        setError(
            ErrorCode::InvalidArgument,
            "D3D11FrameConverter::convert",
            "input frame view is incomplete");
        return false;
    }

    const auto inputFormat = input.format.actualInputFormat;
    const auto outputFormat = outputSpec.outputFormat;
    if (!isSupported(inputFormat, outputFormat)) {
        setError(
            ErrorCode::UnsupportedFormat,
            "D3D11FrameConverter::convert",
            std::string("Unsupported conversion: ") + ToString(inputFormat) +
                " -> " + ToString(outputFormat));
        return false;
    }

    D3D11CoreLib::D3D11ComputePipeline* pipeline = nullptr;
    bool* ready = nullptr;
    if (inputFormat == CameraPixelFormat::Mono8 &&
        outputFormat == GpuFrameFormat::R8) {
        pipeline = &mono8ToR8_;
        ready = &mono8ToR8Ready_;
    } else if (inputFormat == CameraPixelFormat::Mono8) {
        pipeline = &mono8ToRgba8_;
        ready = &mono8ToRgba8Ready_;
    } else if (inputFormat == CameraPixelFormat::BGR8) {
        pipeline = &bgr8ToRgba8_;
        ready = &bgr8ToRgba8Ready_;
    } else if (inputFormat == CameraPixelFormat::BGRa8) {
        pipeline = &bgra8ToRgba8_;
        ready = &bgra8ToRgba8Ready_;
    } else {
        pipeline = &bayer8ToRgba8_;
        ready = &bayer8ToRgba8Ready_;
    }

    D3D11::Detail::ImmediateContextSequenceLock contextSequence(contextMutex_);
    if (!pipeline || !ready ||
        !ensurePipeline(inputFormat, outputFormat, *pipeline, *ready)) {
        return false;
    }

    D3D11CoreLib::D3D11Resource rawBuffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> rawSrv;
    D3D11CoreLib::D3D11Resource outputTexture;
    D3D11CoreLib::D3D11Resource constants;
    if (!createRawInputBuffer(input, rawBuffer, rawSrv) ||
        !createOutputTexture(input, outputSpec, outputTexture, outFrame) ||
        !createConstantBuffer(input, constants)) {
        return false;
    }

    D3D11CoreLib::D3D11ComputeBindingSet bindings;
    bindings.SetShaderResource(0, rawSrv.Get());
    bindings.SetUnorderedAccess(0, outFrame.uav.Get());
    bindings.SetConstantBuffer(0, constants.AsBuffer());

    const UINT groupsX =
        (static_cast<UINT>(input.format.width) + 15u) / 16u;
    const UINT groupsY =
        (static_cast<UINT>(input.format.height) + 15u) / 16u;
    try {
        D3D11CoreLib::D3D11ScopedComputeBindings scoped(context_, bindings);
        pipeline->Dispatch(context_, groupsX, groupsY, 1);
    } catch (const std::exception& exception) {
        setError(
            ErrorCode::D3D11Error,
            "D3D11FrameConverter::convert/Dispatch",
            exception.what());
        return false;
    }

    outFrame.ready = fenceManager_->signal();
    if (!outFrame.ready.isValid()) {
        setError(
            ErrorCode::D3D11Error,
            "D3D11FrameConverter::convert/fenceSignal",
            fenceManager_->lastError().message);
        return false;
    }
    outFrame.timing = input.timing;
    outFrame.format = input.format;
    outFrame.format.outputFormat = outputFormat;
    return true;
}

} // namespace IC4Ext

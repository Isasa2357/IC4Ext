#include "IC4Ext/D3D11/D3D11FrameConverter.hpp"
#include "IC4Ext/D3D11/D3D11FenceManager.hpp"

#include <D3D11Helper/D3D11Gpu/D3D11BindingSet.hpp>
#include <D3D11Helper/D3D11Gpu/D3D11Helpers.hpp>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace IC4Ext {

namespace {

DXGI_FORMAT ToDxgi(GpuFrameFormat fmt)
{
    switch (fmt) {
    case GpuFrameFormat::R8: return DXGI_FORMAT_R8_UNORM;
    case GpuFrameFormat::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

std::string ShaderBaseName(CameraPixelFormat input, GpuFrameFormat output)
{
    if (input == CameraPixelFormat::Mono8 && output == GpuFrameFormat::R8) return "IC4Ext_Convert_Mono8_To_R8";
    if (input == CameraPixelFormat::Mono8 && output == GpuFrameFormat::RGBA8) return "IC4Ext_Convert_Mono8_To_RGBA8";
    if (input == CameraPixelFormat::BGR8 && output == GpuFrameFormat::RGBA8) return "IC4Ext_Convert_BGR8_To_RGBA8";
    if (input == CameraPixelFormat::BGRa8 && output == GpuFrameFormat::RGBA8) return "IC4Ext_Convert_BGRa8_To_RGBA8";
    if ((input == CameraPixelFormat::BayerRG8 || input == CameraPixelFormat::BayerGR8 ||
         input == CameraPixelFormat::BayerGB8 || input == CameraPixelFormat::BayerBG8) &&
        output == GpuFrameFormat::RGBA8) return "IC4Ext_Convert_Bayer8_To_RGBA8";
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

void D3D11FrameConverter::setError(ErrorCode code, const std::string& where, const std::string& message)
{
    lastError_ = MakeError(code, where, message);
}

bool D3D11FrameConverter::initialize(D3D11CoreLib::D3D11Core* core,
                                     D3D11FenceManager* fenceManager,
                                     const ShaderLoadConfig& shaderConfig)
{
    lastError_ = NoError();
    if (!core || !fenceManager) {
        setError(ErrorCode::InvalidArgument, "D3D11FrameConverter::initialize", "D3D11Core/fenceManager is null");
        return false;
    }

    core_ = core;
    device_ = core_->GetDevice();
    context_ = core_->GetImmediateContext();
    fenceManager_ = fenceManager;
    shaderConfig_ = shaderConfig;

    if (!device_ || !context_) {
        setError(ErrorCode::InvalidArgument, "D3D11FrameConverter::initialize", "D3D11Core has null device/context");
        return false;
    }

    if (shaderConfig_.shaderDirectory.empty()) {
        shaderConfig_.shaderDirectory = std::filesystem::current_path() / "shaders" / "d3d11";
    }
    if (shaderConfig_.entryPoint.empty()) {
        shaderConfig_.entryPoint = "main";
    }
    if (shaderConfig_.target.empty()) {
        shaderConfig_.target = "cs_5_0";
    }

    UINT support = 0;
    HRESULT hr = device_->CheckFormatSupport(DXGI_FORMAT_R8_UNORM, &support);
    if (FAILED(hr) || (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) == 0) {
        // Mono8->R8 will fail later. Do not make the entire converter unusable because RGBA8 paths are still valid.
    }
    return true;
}

bool D3D11FrameConverter::initialize(ID3D11Device* device,
                                     ID3D11DeviceContext* context,
                                     D3D11FenceManager* fenceManager,
                                     const ShaderLoadConfig& shaderConfig)
{
    (void)device;
    (void)context;
    (void)fenceManager;
    (void)shaderConfig;
    setError(ErrorCode::InvalidArgument,
             "D3D11FrameConverter::initialize",
             "Raw ID3D11Device/ID3D11DeviceContext initialization is intentionally unsupported in the helper-integrated backend. Use initialize(D3D11Core*, ...).");
    return false;
}

bool D3D11FrameConverter::isSupported(CameraPixelFormat input, GpuFrameFormat output) const noexcept
{
    return IsSupportedConversion(input, output);
}

bool D3D11FrameConverter::loadShaderBytecode(const std::string& baseName, D3D11CoreLib::ShaderBytecode& outBytecode)
{
    const auto csoPath = shaderConfig_.shaderDirectory / (baseName + ".cso");
    const auto hlslPath = shaderConfig_.shaderDirectory / (baseName + ".hlsl");

    auto loadCso = [&]() -> bool {
        try {
            outBytecode = D3D11CoreLib::LoadShaderBytecodeFromFile(csoPath);
            return !outBytecode.Empty();
        } catch (const std::exception& e) {
            setError(ErrorCode::ShaderError, "D3D11FrameConverter::loadShaderBytecode / cso", e.what());
            return false;
        }
    };

    auto loadHlsl = [&]() -> bool {
        if (!std::filesystem::exists(hlslPath)) {
            setError(ErrorCode::ShaderError, "D3D11FrameConverter::loadShaderBytecode / hlsl", "Missing shader " + hlslPath.string());
            return false;
        }
        try {
            D3D11CoreLib::ShaderCompileDesc desc;
            desc.sourcePath = hlslPath;
            desc.entryPoint = shaderConfig_.entryPoint;
            desc.target = shaderConfig_.target;
            desc.useDxc = false;
            outBytecode = D3D11CoreLib::CompileShaderFromFile(desc);
            return !outBytecode.Empty();
        } catch (const std::exception& e) {
            setError(ErrorCode::ShaderError, "D3D11FrameConverter::loadShaderBytecode / hlsl", e.what());
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

bool D3D11FrameConverter::ensurePipeline(CameraPixelFormat input,
                                         GpuFrameFormat output,
                                         D3D11CoreLib::D3D11ComputePipeline& pipeline,
                                         bool& ready)
{
    if (ready) return true;
    if (!device_) {
        setError(ErrorCode::InvalidArgument, "D3D11FrameConverter::ensurePipeline", "converter is not initialized");
        return false;
    }

    const std::string baseName = ShaderBaseName(input, output);
    if (baseName.empty()) {
        setError(ErrorCode::UnsupportedFormat, "D3D11FrameConverter::ensurePipeline", "Unsupported conversion");
        return false;
    }

    D3D11CoreLib::ShaderBytecode bytecode;
    if (!loadShaderBytecode(baseName, bytecode)) return false;

    try {
        pipeline.Initialize(device_, bytecode);
        ready = true;
        return true;
    } catch (const std::exception& e) {
        setError(ErrorCode::ShaderError, "D3D11FrameConverter::ensurePipeline", e.what());
        return false;
    }
}

bool D3D11FrameConverter::createRawInputBuffer(const CpuFrameView& input,
                                               D3D11CoreLib::D3D11Resource& buffer,
                                               Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv)
{
    if (!core_ || !input.data || input.dataSize == 0) {
        setError(ErrorCode::InvalidArgument, "D3D11FrameConverter::createRawInputBuffer", "input data is null or empty");
        return false;
    }
    if (input.dataSize > static_cast<std::size_t>(std::numeric_limits<UINT>::max())) {
        setError(ErrorCode::InvalidArgument, "D3D11FrameConverter::createRawInputBuffer", "input data is too large for D3D11 buffer");
        return false;
    }

    try {
        buffer = D3D11CoreLib::CreateBuffer(*core_,
                                            static_cast<UINT>(input.dataSize),
                                            D3D11_USAGE_DEFAULT,
                                            D3D11_BIND_SHADER_RESOURCE,
                                            0,
                                            0,
                                            input.data);
        srv = D3D11CoreLib::CreateBufferSrv(*core_, buffer, 0, static_cast<UINT>(input.dataSize), DXGI_FORMAT_R8_UINT);
        return true;
    } catch (const std::exception& e) {
        setError(ErrorCode::D3D11Error, "D3D11FrameConverter::createRawInputBuffer", e.what());
        return false;
    }
}

bool D3D11FrameConverter::createOutputTexture(const CpuFrameView& input,
                                              const FrameOutputSpec& spec,
                                              D3D11CoreLib::D3D11Resource& textureResource,
                                              D3D11CameraFrame& outFrame)
{
    const DXGI_FORMAT format = ToDxgi(spec.outputFormat);
    if (format == DXGI_FORMAT_UNKNOWN) {
        setError(ErrorCode::UnsupportedFormat, "D3D11FrameConverter::createOutputTexture", "Unsupported output format");
        return false;
    }

    try {
        UINT bindFlags = D3D11_BIND_UNORDERED_ACCESS;
        if (spec.createSrv) bindFlags |= D3D11_BIND_SHADER_RESOURCE;

        textureResource = D3D11CoreLib::CreateTexture2D(*core_,
                                                        static_cast<UINT>(input.format.width),
                                                        static_cast<UINT>(input.format.height),
                                                        format,
                                                        bindFlags);
        outFrame.texture = textureResource.AsTexture2D();
        if (!outFrame.texture) {
            setError(ErrorCode::D3D11Error, "D3D11FrameConverter::createOutputTexture", "created resource is not a Texture2D");
            return false;
        }

        if (spec.createSrv) {
            outFrame.srv = D3D11CoreLib::CreateTexture2DSrv(*core_, textureResource, format);
        }
        // The compute conversion always requires an UAV, even if the public output spec
        // says the caller does not need to keep one afterwards. For simplicity, the
        // created UAV remains attached to the frame, matching previous behavior.
        outFrame.uav = D3D11CoreLib::CreateTexture2DUav(*core_, textureResource, format);
        return true;
    } catch (const std::exception& e) {
        setError(ErrorCode::D3D11Error, "D3D11FrameConverter::createOutputTexture", e.what());
        return false;
    }
}

bool D3D11FrameConverter::createConstantBuffer(const CpuFrameView& input, D3D11CoreLib::D3D11Resource& buffer)
{
    ConvertConstants constants{};
    constants.width = static_cast<std::uint32_t>(input.format.width);
    constants.height = static_cast<std::uint32_t>(input.format.height);
    constants.inputRowPitchBytes = static_cast<std::uint32_t>(input.format.inputRowPitchBytes);
    constants.inputPixelFormat = static_cast<std::uint32_t>(input.format.actualInputFormat);

    try {
        buffer = D3D11CoreLib::CreateConstantBuffer(*core_, sizeof(ConvertConstants), &constants);
        return true;
    } catch (const std::exception& e) {
        setError(ErrorCode::D3D11Error, "D3D11FrameConverter::createConstantBuffer", e.what());
        return false;
    }
}

bool D3D11FrameConverter::convert(const CpuFrameView& input,
                                  const FrameOutputSpec& outputSpec,
                                  D3D11CameraFrame& outFrame)
{
    lastError_ = NoError();
    outFrame = {};

    if (!core_ || !context_ || !fenceManager_) {
        setError(ErrorCode::InvalidArgument, "D3D11FrameConverter::convert", "converter is not initialized with D3D11Core");
        return false;
    }

    const auto inFmt = input.format.actualInputFormat;
    const auto outFmt = outputSpec.outputFormat;
    if (!isSupported(inFmt, outFmt)) {
        setError(ErrorCode::UnsupportedFormat, "D3D11FrameConverter::convert", std::string("Unsupported conversion: ") + ToString(inFmt) + " -> " + ToString(outFmt));
        return false;
    }

    D3D11CoreLib::D3D11ComputePipeline* pipeline = nullptr;
    bool* ready = nullptr;
    if (inFmt == CameraPixelFormat::Mono8 && outFmt == GpuFrameFormat::R8) {
        pipeline = &mono8ToR8_;
        ready = &mono8ToR8Ready_;
    } else if (inFmt == CameraPixelFormat::Mono8 && outFmt == GpuFrameFormat::RGBA8) {
        pipeline = &mono8ToRgba8_;
        ready = &mono8ToRgba8Ready_;
    } else if (inFmt == CameraPixelFormat::BGR8) {
        pipeline = &bgr8ToRgba8_;
        ready = &bgr8ToRgba8Ready_;
    } else if (inFmt == CameraPixelFormat::BGRa8) {
        pipeline = &bgra8ToRgba8_;
        ready = &bgra8ToRgba8Ready_;
    } else {
        pipeline = &bayer8ToRgba8_;
        ready = &bayer8ToRgba8Ready_;
    }

    if (!pipeline || !ready || !ensurePipeline(inFmt, outFmt, *pipeline, *ready)) {
        return false;
    }

    D3D11CoreLib::D3D11Resource rawBuffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> rawSrv;
    D3D11CoreLib::D3D11Resource outputTexture;
    D3D11CoreLib::D3D11Resource constants;

    if (!createRawInputBuffer(input, rawBuffer, rawSrv)) return false;
    if (!createOutputTexture(input, outputSpec, outputTexture, outFrame)) return false;
    if (!createConstantBuffer(input, constants)) return false;

    D3D11CoreLib::D3D11ComputeBindingSet bindings;
    bindings.SetShaderResource(0, rawSrv.Get());
    bindings.SetUnorderedAccess(0, outFrame.uav.Get());
    bindings.SetConstantBuffer(0, constants.AsBuffer());

    const UINT groupsX = (static_cast<UINT>(input.format.width) + 15u) / 16u;
    const UINT groupsY = (static_cast<UINT>(input.format.height) + 15u) / 16u;

    try {
        D3D11CoreLib::D3D11ScopedComputeBindings scoped(context_, bindings);
        pipeline->Dispatch(context_, groupsX, groupsY, 1);
    } catch (const std::exception& e) {
        setError(ErrorCode::D3D11Error, "D3D11FrameConverter::convert / Dispatch", e.what());
        return false;
    }

    outFrame.ready = fenceManager_->signal();
    if (!outFrame.ready.isValid()) {
        setError(ErrorCode::D3D11Error, "D3D11FrameConverter::convert / fence signal", fenceManager_->lastError().message);
        return false;
    }
    outFrame.timing = input.timing;
    outFrame.format = input.format;
    outFrame.format.outputFormat = outFmt;
    return true;
}

} // namespace IC4Ext

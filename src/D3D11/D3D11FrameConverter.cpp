#include "IC4Ext/D3D11/D3D11FrameConverter.hpp"
#include "IC4Ext/D3D11/D3D11FenceManager.hpp"

#include <d3dcompiler.h>
#include <fstream>
#include <sstream>
#include <vector>

namespace IC4Ext {

namespace {
std::string HrToString(HRESULT hr)
{
    std::ostringstream oss;
    oss << "HRESULT=0x" << std::hex << static_cast<unsigned long>(hr);
    return oss.str();
}

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

std::wstring ToWide(const std::filesystem::path& path)
{
    return path.wstring();
}

bool ReadFileBytes(const std::filesystem::path& path, std::vector<std::uint8_t>& out)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    ifs.seekg(0, std::ios::end);
    const auto size = ifs.tellg();
    if (size < 0) return false;
    ifs.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    if (!out.empty()) {
        ifs.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    }
    return static_cast<bool>(ifs) || ifs.eof();
}
}

void D3D11FrameConverter::setError(ErrorCode code, const std::string& where, const std::string& message)
{
    lastError_ = MakeError(code, where, message);
}

bool D3D11FrameConverter::initialize(ID3D11Device* device,
                                     ID3D11DeviceContext* context,
                                     D3D11FenceManager* fenceManager,
                                     const ShaderLoadConfig& shaderConfig)
{
    lastError_ = NoError();
    if (!device || !context || !fenceManager) {
        setError(ErrorCode::InvalidArgument, "D3D11FrameConverter::initialize", "device/context/fenceManager is null");
        return false;
    }
    device_ = device;
    context_ = context;
    fenceManager_ = fenceManager;
    shaderConfig_ = shaderConfig;

    if (shaderConfig_.shaderDirectory.empty()) {
        shaderConfig_.shaderDirectory = std::filesystem::current_path() / "shaders" / "d3d11";
    }

    UINT support = 0;
    HRESULT hr = device_->CheckFormatSupport(DXGI_FORMAT_R8_UNORM, &support);
    if (FAILED(hr) || (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) == 0) {
        // Mono8->R8 will fail later. Do not make the entire converter unusable because RGBA8 paths are still valid.
    }
    return true;
}

bool D3D11FrameConverter::isSupported(CameraPixelFormat input, GpuFrameFormat output) const noexcept
{
    return IsSupportedConversion(input, output);
}

bool D3D11FrameConverter::ensureShader(CameraPixelFormat input, GpuFrameFormat output, Microsoft::WRL::ComPtr<ID3D11ComputeShader>& shader)
{
    if (shader) return true;
    const std::string baseName = ShaderBaseName(input, output);
    if (baseName.empty()) {
        setError(ErrorCode::UnsupportedFormat, "D3D11FrameConverter::ensureShader", "Unsupported conversion");
        return false;
    }
    return loadComputeShader(baseName, shader);
}

bool D3D11FrameConverter::loadComputeShader(const std::string& baseName, Microsoft::WRL::ComPtr<ID3D11ComputeShader>& outShader)
{
    auto loadCso = [&]() -> bool {
        const auto path = shaderConfig_.shaderDirectory / (baseName + ".cso");
        std::vector<std::uint8_t> bytecode;
        if (!ReadFileBytes(path, bytecode)) {
            setError(ErrorCode::ShaderError, "D3D11FrameConverter::loadComputeShader / cso", "Failed to read " + path.string());
            return false;
        }
        HRESULT hr = device_->CreateComputeShader(bytecode.data(), bytecode.size(), nullptr, &outShader);
        if (FAILED(hr)) {
            setError(ErrorCode::ShaderError, "D3D11FrameConverter::loadComputeShader / CreateComputeShader", "CreateComputeShader failed for " + path.string() + ". " + HrToString(hr));
            return false;
        }
        return true;
    };

    auto loadHlsl = [&]() -> bool {
        const auto path = shaderConfig_.shaderDirectory / (baseName + ".hlsl");
        if (!std::filesystem::exists(path)) {
            setError(ErrorCode::ShaderError, "D3D11FrameConverter::loadComputeShader / hlsl", "Missing shader " + path.string());
            return false;
        }
        Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        HRESULT hr = D3DCompileFromFile(ToWide(path).c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                        shaderConfig_.entryPoint.c_str(), shaderConfig_.target.c_str(),
                                        flags, 0, &bytecode, &errors);
        if (FAILED(hr)) {
            std::string msg = "D3DCompileFromFile failed for " + path.string() + ". " + HrToString(hr);
            if (errors) {
                msg += "\n";
                msg.append(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
            }
            setError(ErrorCode::ShaderError, "D3D11FrameConverter::loadComputeShader / D3DCompileFromFile", msg);
            return false;
        }
        hr = device_->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &outShader);
        if (FAILED(hr)) {
            setError(ErrorCode::ShaderError, "D3D11FrameConverter::loadComputeShader / CreateComputeShader", "CreateComputeShader failed. " + HrToString(hr));
            return false;
        }
        return true;
    };

    if (shaderConfig_.inputKind == ShaderInputKind::CsoFile) return loadCso();
    if (shaderConfig_.inputKind == ShaderInputKind::HlslFile) return loadHlsl();

    if (shaderConfig_.preferCsoWhenBothExist) {
        const auto cso = shaderConfig_.shaderDirectory / (baseName + ".cso");
        if (std::filesystem::exists(cso) && loadCso()) return true;
        return loadHlsl();
    }

    const auto hlsl = shaderConfig_.shaderDirectory / (baseName + ".hlsl");
    if (std::filesystem::exists(hlsl) && loadHlsl()) return true;
    return loadCso();
}

bool D3D11FrameConverter::createRawInputBuffer(const CpuFrameView& input, Microsoft::WRL::ComPtr<ID3D11Buffer>& buffer, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv)
{
    if (!input.data || input.dataSize == 0) {
        setError(ErrorCode::InvalidArgument, "D3D11FrameConverter::createRawInputBuffer", "input data is null or empty");
        return false;
    }

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = static_cast<UINT>(input.dataSize);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    desc.StructureByteStride = 0;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = input.data;

    HRESULT hr = device_->CreateBuffer(&desc, &init, &buffer);
    if (FAILED(hr)) {
        setError(ErrorCode::D3D11Error, "D3D11FrameConverter::createRawInputBuffer / CreateBuffer", HrToString(hr));
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8_UINT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = static_cast<UINT>(input.dataSize);

    hr = device_->CreateShaderResourceView(buffer.Get(), &srvDesc, &srv);
    if (FAILED(hr)) {
        setError(ErrorCode::D3D11Error, "D3D11FrameConverter::createRawInputBuffer / CreateShaderResourceView", HrToString(hr));
        return false;
    }
    return true;
}

bool D3D11FrameConverter::createOutputTexture(const CpuFrameView& input, const FrameOutputSpec& spec, D3D11CameraFrame& outFrame)
{
    const DXGI_FORMAT format = ToDxgi(spec.outputFormat);
    if (format == DXGI_FORMAT_UNKNOWN) {
        setError(ErrorCode::UnsupportedFormat, "D3D11FrameConverter::createOutputTexture", "Unsupported output format");
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(input.format.width);
    desc.Height = static_cast<UINT>(input.format.height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = 0;
    if (spec.createSrv) desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS; // compute conversion requires UAV

    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &outFrame.texture);
    if (FAILED(hr)) {
        setError(ErrorCode::D3D11Error, "D3D11FrameConverter::createOutputTexture / CreateTexture2D", HrToString(hr));
        return false;
    }

    if (spec.createSrv) {
        hr = device_->CreateShaderResourceView(outFrame.texture.Get(), nullptr, &outFrame.srv);
        if (FAILED(hr)) {
            setError(ErrorCode::D3D11Error, "D3D11FrameConverter::createOutputTexture / CreateShaderResourceView", HrToString(hr));
            return false;
        }
    }

    // The compute conversion always requires an UAV, even if the public output spec
    // says the caller does not need to keep one afterwards. For simplicity, the
    // initial implementation keeps the created UAV on the frame.
    hr = device_->CreateUnorderedAccessView(outFrame.texture.Get(), nullptr, &outFrame.uav);
    if (FAILED(hr)) {
        setError(ErrorCode::D3D11Error, "D3D11FrameConverter::createOutputTexture / CreateUnorderedAccessView", HrToString(hr));
        return false;
    }
    return true;
}

bool D3D11FrameConverter::createConstantBuffer(const CpuFrameView& input, Microsoft::WRL::ComPtr<ID3D11Buffer>& buffer)
{
    ConvertConstants constants{};
    constants.width = static_cast<std::uint32_t>(input.format.width);
    constants.height = static_cast<std::uint32_t>(input.format.height);
    constants.inputRowPitchBytes = static_cast<std::uint32_t>(input.format.inputRowPitchBytes);
    constants.inputPixelFormat = static_cast<std::uint32_t>(input.format.actualInputFormat);

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = sizeof(ConvertConstants);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = &constants;

    HRESULT hr = device_->CreateBuffer(&desc, &init, &buffer);
    if (FAILED(hr)) {
        setError(ErrorCode::D3D11Error, "D3D11FrameConverter::createConstantBuffer", HrToString(hr));
        return false;
    }
    return true;
}

bool D3D11FrameConverter::convert(const CpuFrameView& input,
                                  const FrameOutputSpec& outputSpec,
                                  D3D11CameraFrame& outFrame)
{
    lastError_ = NoError();
    outFrame = {};

    const auto inFmt = input.format.actualInputFormat;
    const auto outFmt = outputSpec.outputFormat;
    if (!isSupported(inFmt, outFmt)) {
        setError(ErrorCode::UnsupportedFormat, "D3D11FrameConverter::convert", std::string("Unsupported conversion: ") + ToString(inFmt) + " -> " + ToString(outFmt));
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
    if (inFmt == CameraPixelFormat::Mono8 && outFmt == GpuFrameFormat::R8) shader = mono8ToR8_;
    else if (inFmt == CameraPixelFormat::Mono8 && outFmt == GpuFrameFormat::RGBA8) shader = mono8ToRgba8_;
    else if (inFmt == CameraPixelFormat::BGR8) shader = bgr8ToRgba8_;
    else if (inFmt == CameraPixelFormat::BGRa8) shader = bgra8ToRgba8_;
    else shader = bayer8ToRgba8_;

    if (!ensureShader(inFmt, outFmt, shader)) {
        return false;
    }
    if (inFmt == CameraPixelFormat::Mono8 && outFmt == GpuFrameFormat::R8) mono8ToR8_ = shader;
    else if (inFmt == CameraPixelFormat::Mono8 && outFmt == GpuFrameFormat::RGBA8) mono8ToRgba8_ = shader;
    else if (inFmt == CameraPixelFormat::BGR8) bgr8ToRgba8_ = shader;
    else if (inFmt == CameraPixelFormat::BGRa8) bgra8ToRgba8_ = shader;
    else bayer8ToRgba8_ = shader;

    Microsoft::WRL::ComPtr<ID3D11Buffer> rawBuffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> rawSrv;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants;

    if (!createRawInputBuffer(input, rawBuffer, rawSrv)) return false;
    if (!createOutputTexture(input, outputSpec, outFrame)) return false;
    if (!createConstantBuffer(input, constants)) return false;

    ID3D11ShaderResourceView* srvs[] = { rawSrv.Get() };
    ID3D11UnorderedAccessView* uavs[] = { outFrame.uav.Get() };
    ID3D11Buffer* cbs[] = { constants.Get() };

    context_->CSSetShader(shader.Get(), nullptr, 0);
    context_->CSSetShaderResources(0, 1, srvs);
    context_->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    context_->CSSetConstantBuffers(0, 1, cbs);

    const UINT groupsX = (static_cast<UINT>(input.format.width) + 15u) / 16u;
    const UINT groupsY = (static_cast<UINT>(input.format.height) + 15u) / 16u;
    context_->Dispatch(groupsX, groupsY, 1);

    ID3D11ShaderResourceView* nullSrv[] = { nullptr };
    ID3D11UnorderedAccessView* nullUav[] = { nullptr };
    ID3D11Buffer* nullCb[] = { nullptr };
    context_->CSSetShaderResources(0, 1, nullSrv);
    context_->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
    context_->CSSetConstantBuffers(0, 1, nullCb);
    context_->CSSetShader(nullptr, nullptr, 0);

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

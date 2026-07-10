#include "IC4Ext/D3D12/D3D12FrameConverter.hpp"

#include <D3D12Helper/D3D12Framework/D3D12Helpers.hpp>
#include <D3D12Helper/D3D12Framework/D3D12ShaderCompiler.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace IC4Ext {

namespace {

constexpr UINT kThreadGroupSizeX = 16;
constexpr UINT kThreadGroupSizeY = 16;
const char* kEmbeddedRgbaShader = R"IC4EXT_SHADER(
ByteAddressBuffer gInput : register(t0);
RWTexture2D<float4> gOutput : register(u0);

cbuffer Params : register(b0)
{
    uint gWidth;
    uint gHeight;
    uint gInputRowPitchBytes;
    uint gInputFormat;   // CameraPixelFormat enum value
    uint gBayerPattern;  // 0:BGGR, 1:GBRG, 2:GRBG, 3:RGGB
    uint gReserved0;
    uint gReserved1;
    uint gReserved2;
};

static const uint FMT_MONO8    = 1u;
static const uint FMT_BAYERRG8 = 2u;
static const uint FMT_BAYERGR8 = 3u;
static const uint FMT_BAYERGB8 = 4u;
static const uint FMT_BAYERBG8 = 5u;
static const uint FMT_BGR8     = 6u;
static const uint FMT_BGRA8    = 7u;

uint BytesPerPixel()
{
    if (gInputFormat == FMT_BGR8) return 3u;
    if (gInputFormat == FMT_BGRA8) return 4u;
    return 1u;
}

uint LoadByteAtAddress(uint byteAddress)
{
    const uint aligned = byteAddress & ~3u;
    const uint shift = (byteAddress & 3u) * 8u;
    return (gInput.Load(aligned) >> shift) & 0xffu;
}

uint LoadByte(uint x, uint y, uint component)
{
    x = min(x, gWidth - 1u);
    y = min(y, gHeight - 1u);
    return LoadByteAtAddress(y * gInputRowPitchBytes + x * BytesPerPixel() + component);
}

uint BayerColorAt(uint x, uint y)
{
    const uint px = x & 1u;
    const uint py = y & 1u;

    if (gBayerPattern == 0u) {        // BGGR: B G / G R
        if (py == 0u) return (px == 0u) ? 2u : 1u;
        else          return (px == 0u) ? 1u : 0u;
    }
    else if (gBayerPattern == 1u) {   // GBRG: G B / R G
        if (py == 0u) return (px == 0u) ? 1u : 2u;
        else          return (px == 0u) ? 0u : 1u;
    }
    else if (gBayerPattern == 2u) {   // GRBG: G R / B G
        if (py == 0u) return (px == 0u) ? 1u : 0u;
        else          return (px == 0u) ? 2u : 1u;
    }
    else {                            // RGGB: R G / G B
        if (py == 0u) return (px == 0u) ? 0u : 1u;
        else          return (px == 0u) ? 1u : 2u;
    }
}

float SampleBayer(int x, int y)
{
    x = clamp(x, 0, int(gWidth) - 1);
    y = clamp(y, 0, int(gHeight) - 1);
    return float(LoadByte(uint(x), uint(y), 0u)) / 255.0f;
}

float4 ConvertBayer(uint xU, uint yU)
{
    const int x = int(xU);
    const int y = int(yU);
    const uint c = BayerColorAt(xU, yU);

    const float center = SampleBayer(x, y);
    float r;
    float g;
    float b;

    if (c == 0u) { // R
        r = center;
        g = 0.25f * (SampleBayer(x - 1, y) + SampleBayer(x + 1, y)
                   + SampleBayer(x, y - 1) + SampleBayer(x, y + 1));
        b = 0.25f * (SampleBayer(x - 1, y - 1) + SampleBayer(x + 1, y - 1)
                   + SampleBayer(x - 1, y + 1) + SampleBayer(x + 1, y + 1));
    }
    else if (c == 2u) { // B
        b = center;
        g = 0.25f * (SampleBayer(x - 1, y) + SampleBayer(x + 1, y)
                   + SampleBayer(x, y - 1) + SampleBayer(x, y + 1));
        r = 0.25f * (SampleBayer(x - 1, y - 1) + SampleBayer(x + 1, y - 1)
                   + SampleBayer(x - 1, y + 1) + SampleBayer(x + 1, y + 1));
    }
    else { // G
        g = center;
        const uint leftColor  = BayerColorAt(uint(max(x - 1, 0)), yU);
        const uint rightColor = BayerColorAt(uint(min(x + 1, int(gWidth) - 1)), yU);
        if (leftColor == 0u || rightColor == 0u) {
            r = 0.5f * (SampleBayer(x - 1, y) + SampleBayer(x + 1, y));
            b = 0.5f * (SampleBayer(x, y - 1) + SampleBayer(x, y + 1));
        }
        else {
            b = 0.5f * (SampleBayer(x - 1, y) + SampleBayer(x + 1, y));
            r = 0.5f * (SampleBayer(x, y - 1) + SampleBayer(x, y + 1));
        }
    }

    return float4(r, g, b, 1.0f);
}

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gWidth || tid.y >= gHeight) return;

    float4 color = float4(0.0f, 0.0f, 0.0f, 1.0f);

    if (gInputFormat == FMT_MONO8) {
        const float v = float(LoadByte(tid.x, tid.y, 0u)) / 255.0f;
        color = float4(v, v, v, 1.0f);
    }
    else if (gInputFormat == FMT_BGR8) {
        const float b = float(LoadByte(tid.x, tid.y, 0u)) / 255.0f;
        const float g = float(LoadByte(tid.x, tid.y, 1u)) / 255.0f;
        const float r = float(LoadByte(tid.x, tid.y, 2u)) / 255.0f;
        color = float4(r, g, b, 1.0f);
    }
    else if (gInputFormat == FMT_BGRA8) {
        const float b = float(LoadByte(tid.x, tid.y, 0u)) / 255.0f;
        const float g = float(LoadByte(tid.x, tid.y, 1u)) / 255.0f;
        const float r = float(LoadByte(tid.x, tid.y, 2u)) / 255.0f;
        const float a = float(LoadByte(tid.x, tid.y, 3u)) / 255.0f;
        color = float4(r, g, b, a);
    }
    else {
        color = ConvertBayer(tid.x, tid.y);
    }

    gOutput[tid.xy] = color;
}

)IC4EXT_SHADER";

const char* kEmbeddedR8Shader = R"IC4EXT_SHADER(
ByteAddressBuffer gInput : register(t0);
RWTexture2D<float> gOutput : register(u0);

cbuffer Params : register(b0)
{
    uint gWidth;
    uint gHeight;
    uint gInputRowPitchBytes;
    uint gInputFormat;
    uint gBayerPattern;
    uint gReserved0;
    uint gReserved1;
    uint gReserved2;
};

uint LoadByteAtAddress(uint byteAddress)
{
    const uint aligned = byteAddress & ~3u;
    const uint shift = (byteAddress & 3u) * 8u;
    return (gInput.Load(aligned) >> shift) & 0xffu;
}

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gWidth || tid.y >= gHeight) return;
    const uint byteAddress = tid.y * gInputRowPitchBytes + tid.x;
    gOutput[tid.xy] = float(LoadByteAtAddress(byteAddress)) / 255.0f;
}

)IC4EXT_SHADER";

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

std::string ShaderBaseName(GpuFrameFormat outputFormat)
{
    switch (outputFormat) {
    case GpuFrameFormat::R8: return "IC4Ext_D3D12_Convert_To_R8";
    case GpuFrameFormat::RGBA8: return "IC4Ext_D3D12_Convert_To_RGBA8";
    default: return {};
    }
}

const char* EmbeddedShaderText(GpuFrameFormat outputFormat)
{
    switch (outputFormat) {
    case GpuFrameFormat::R8: return kEmbeddedR8Shader;
    case GpuFrameFormat::RGBA8: return kEmbeddedRgbaShader;
    default: return nullptr;
    }
}

std::uint32_t BayerPatternCode(CameraPixelFormat fmt)
{
    switch (fmt) {
    case CameraPixelFormat::BayerBG8: return 0; // BGGR
    case CameraPixelFormat::BayerGB8: return 1; // GBRG
    case CameraPixelFormat::BayerGR8: return 2; // GRBG
    case CameraPixelFormat::BayerRG8: return 3; // RGGB
    default: return 3;
    }
}

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment)
{
    return (value + alignment - 1ull) & ~(alignment - 1ull);
}

D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* resource,
                                         D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    return barrier;
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

bool ReadFileText(const std::filesystem::path& path, std::string& out)
{
    std::vector<std::uint8_t> bytes;
    if (!ReadFileBytes(path, bytes)) return false;
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

} // namespace

void D3D12FrameConverter::setError(ErrorCode code, const std::string& where, const std::string& message)
{
    lastError_ = MakeError(code, where, message);
}

bool D3D12FrameConverter::initialize(const D3D12BackendContext& backendIn,
                                     D3D12FenceManager* fenceManager,
                                     const ShaderLoadConfig& shaderConfig)
{
    lastError_ = NoError();
    backend_ = backendIn;
    if (!backend_.resolve() || !backend_.corePtr || !backend_.queue || !fenceManager) {
        setError(ErrorCode::InvalidArgument, "D3D12FrameConverter::initialize", "D3D12 backend must be created from D3D12Helper D3D12Core and queue");
        return false;
    }

    core_ = backend_.corePtr;
    queue_ = backend_.queue;
    device_ = backend_.device;
    fenceManager_ = fenceManager;
    shaderConfig_ = shaderConfig;
    if (shaderConfig_.shaderDirectory.empty()) {
        shaderConfig_.shaderDirectory = std::filesystem::current_path() / "shaders" / "d3d12";
    }
    if (shaderConfig_.target.empty()) {
        shaderConfig_.target = "cs_5_0";
    }
    if (shaderConfig_.entryPoint.empty()) {
        shaderConfig_.entryPoint = "main";
    }

    for (auto& slot : slots_) {
        slot.commandContext.Initialize(device_, queue_->GetType());
        slot.uploadRing.Initialize(device_, uploadRingSizeBytes_);
        slot.descriptorAllocator.Initialize(device_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, true);
        slot.initialized = true;
    }
    nextSlot_ = 0;
    return ensurePipelines();
}

bool D3D12FrameConverter::initialize(ID3D12Device* device,
                                     ID3D12CommandQueue* queue,
                                     D3D12FenceManager* fenceManager,
                                     const ShaderLoadConfig& shaderConfig)
{
    (void)device;
    (void)queue;
    (void)fenceManager;
    (void)shaderConfig;
    setError(ErrorCode::InvalidArgument,
             "D3D12FrameConverter::initialize",
             "Raw ID3D12Device/ID3D12CommandQueue initialization is intentionally unsupported in the helper-integrated backend. Use D3D12BackendContext::FromCore(...).");
    return false;
}

bool D3D12FrameConverter::isSupported(CameraPixelFormat input, GpuFrameFormat output) const noexcept
{
    return IsSupportedConversion(input, output);
}

bool D3D12FrameConverter::compileShaderText(const std::string& sourceText,
                                            const std::filesystem::path& displayPath,
                                            D3D12CoreLib::ShaderBytecode& bytecode)
{
    try {
        bytecode = D3D12CoreLib::CompileShaderFromSource_D3DCompile(sourceText,
                                                                    shaderConfig_.entryPoint,
                                                                    shaderConfig_.target,
                                                                    displayPath.string());
        return !bytecode.Empty();
    } catch (const std::exception& e) {
        setError(ErrorCode::ShaderError, "D3D12FrameConverter::compileShaderText", e.what());
        return false;
    }
}

bool D3D12FrameConverter::readShaderFile(GpuFrameFormat outputFormat, std::string& sourceText, std::vector<std::uint8_t>& csoBytes)
{
    const std::string baseName = ShaderBaseName(outputFormat);
    if (baseName.empty()) return false;

    if (shaderConfig_.inputKind == ShaderInputKind::CsoFile ||
        (shaderConfig_.inputKind == ShaderInputKind::Auto && shaderConfig_.preferCsoWhenBothExist)) {
        const auto cso = shaderConfig_.shaderDirectory / (baseName + ".cso");
        if (std::filesystem::exists(cso) && ReadFileBytes(cso, csoBytes)) return true;
    }

    if (shaderConfig_.inputKind == ShaderInputKind::HlslFile || shaderConfig_.inputKind == ShaderInputKind::Auto) {
        const auto hlsl = shaderConfig_.shaderDirectory / (baseName + ".hlsl");
        if (std::filesystem::exists(hlsl) && ReadFileText(hlsl, sourceText)) return true;
    }

    return false;
}

bool D3D12FrameConverter::loadShaderBytecode(GpuFrameFormat outputFormat, D3D12CoreLib::ShaderBytecode& bytecode)
{
    std::string sourceText;
    std::vector<std::uint8_t> csoBytes;
    if (readShaderFile(outputFormat, sourceText, csoBytes)) {
        if (!csoBytes.empty()) {
            bytecode = D3D12CoreLib::ShaderBytecode(std::move(csoBytes));
            return true;
        }
        return compileShaderText(sourceText, shaderConfig_.shaderDirectory / (ShaderBaseName(outputFormat) + ".hlsl"), bytecode);
    }

    const char* embedded = EmbeddedShaderText(outputFormat);
    if (!embedded) {
        setError(ErrorCode::ShaderError, "D3D12FrameConverter::loadShaderBytecode", "No shader for output format");
        return false;
    }
    return compileShaderText(embedded, std::filesystem::path("<embedded>") / (ShaderBaseName(outputFormat) + ".hlsl"), bytecode);
}

bool D3D12FrameConverter::createPipeline(GpuFrameFormat outputFormat)
{
    bool& ready = (outputFormat == GpuFrameFormat::R8) ? r8PipelineReady_ : rgbaPipelineReady_;
    if (ready) return true;

    D3D12CoreLib::ShaderBytecode bytecode;
    if (!loadShaderBytecode(outputFormat, bytecode)) return false;

    try {
        D3D12CoreLib::ComputePipelineDesc desc;
        desc.numSrvs = 1;
        desc.numUavs = 1;
        desc.numRootConstantValues = sizeof(ConvertConstants) / sizeof(std::uint32_t);
        auto& pipeline = (outputFormat == GpuFrameFormat::R8) ? r8Pipeline_ : rgbaPipeline_;
        pipeline.InitializeWithTemplate(device_, bytecode, desc);
        ready = true;
        return true;
    } catch (const std::exception& e) {
        setError(ErrorCode::ShaderError, "D3D12FrameConverter::createPipeline", e.what());
        return false;
    }
}

bool D3D12FrameConverter::ensurePipelines()
{
    return createPipeline(GpuFrameFormat::RGBA8) && createPipeline(GpuFrameFormat::R8);
}

bool D3D12FrameConverter::validateInput(const D3D12CpuFrameView& input,
                                        const FrameOutputSpec& outputSpec,
                                        std::uint64_t& neededBytes) const
{
    if (!input.data || input.format.width <= 0 || input.format.height <= 0 || input.format.inputRowPitchBytes == 0) return false;
    neededBytes = static_cast<std::uint64_t>(input.format.inputRowPitchBytes) * static_cast<std::uint64_t>(input.format.height);
    if (input.dataSize < neededBytes) return false;
    if (!IsSupportedConversion(input.format.actualInputFormat, outputSpec.outputFormat)) return false;
    if (outputSpec.outputFormat == GpuFrameFormat::R8 && input.format.actualInputFormat != CameraPixelFormat::Mono8) return false;
    return true;
}

D3D12FrameConverter::FrameSlot* D3D12FrameConverter::acquireSlot(std::uint64_t uploadSizeBytes)
{
    FrameSlot& slot = slots_[nextSlot_ % slots_.size()];
    nextSlot_ = (nextSlot_ + 1) % slots_.size();

    if (slot.inFlight.isValid()) {
        slot.inFlight.wait(INFINITE);
        slot.inFlight = {};
    }
    if (uploadSizeBytes > slot.uploadRing.GetRingSize()) {
        setError(ErrorCode::InvalidArgument, "D3D12FrameConverter::acquireSlot", "Input frame is larger than D3D12 upload ring size");
        return nullptr;
    }
    slot.uploadRing.ReclaimCompleted(queue_->Fence());
    slot.descriptorAllocator.Reset();
    slot.commandContext.Reset();
    return &slot;
}

bool D3D12FrameConverter::createOutputSrv(D3D12CameraFrame& frame)
{
    try {
        frame.srvHeapHelper.Initialize(device_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true);
        frame.srvHeap = frame.srvHeapHelper.Get();
        auto handle = frame.srvHeapHelper.GetHandle(0);
        D3D12CoreLib::CreateTexture2DSrv(*core_, frame.textureResource, handle.cpu, frame.dxgiFormat);
        frame.srvCpuHandle = handle.cpu;
        frame.srvGpuHandle = handle.gpu;
        return true;
    } catch (const std::exception& e) {
        setError(ErrorCode::D3D12Error, "D3D12FrameConverter::createOutputSrv", e.what());
        return false;
    }
}

bool D3D12FrameConverter::recordConvert(FrameSlot& slot,
                                        const D3D12CpuFrameView& input,
                                        std::uint64_t neededBytes,
                                        const FrameOutputSpec& outputSpec,
                                        D3D12CameraFrame& outFrame)
{
    const DXGI_FORMAT outFormat = ToDxgi(outputSpec.outputFormat);
    const std::uint64_t uploadSize = AlignUp(neededBytes, 4);

    try {
        outFrame.inputBufferResource = D3D12CoreLib::CreateBuffer(*core_, uploadSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        outFrame.inputBufferKeepAlive = outFrame.inputBufferResource.Get();
        outFrame.textureResource = D3D12CoreLib::CreateTexture2D(*core_, static_cast<UINT>(input.format.width), static_cast<UINT>(input.format.height),
                                                                 outFormat, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        outFrame.texture = outFrame.textureResource.Get();
        outFrame.dxgiFormat = outFormat;
        outFrame.resourceState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        auto upload = slot.uploadRing.Allocate(uploadSize, 4);
        std::memset(upload.cpuPtr, 0, static_cast<std::size_t>(uploadSize));
        std::memcpy(upload.cpuPtr, input.data, static_cast<std::size_t>(neededBytes));

        auto* cmd = slot.commandContext.GetCommandList();
        cmd->CopyBufferRegion(outFrame.inputBufferResource.Get(), 0, upload.resource, upload.offset, uploadSize);
        auto inputBarrier = TransitionBarrier(outFrame.inputBufferResource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        slot.commandContext.ResourceBarrier(inputBarrier);

        auto inputSrv = slot.descriptorAllocator.Allocate();
        auto outputUav = slot.descriptorAllocator.Allocate();

        D3D12_SHADER_RESOURCE_VIEW_DESC inputSrvDesc = {};
        inputSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        inputSrvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        inputSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        inputSrvDesc.Buffer.FirstElement = 0;
        inputSrvDesc.Buffer.NumElements = static_cast<UINT>(uploadSize / 4u);
        inputSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device_->CreateShaderResourceView(outFrame.inputBufferResource.Get(), &inputSrvDesc, inputSrv.cpu);
        D3D12CoreLib::CreateTexture2DUav(*core_, outFrame.textureResource, outputUav.cpu, outFormat);

        ID3D12DescriptorHeap* heaps[] = { slot.descriptorAllocator.GetHeap() };
        cmd->SetDescriptorHeaps(1, heaps);

        auto& pipeline = (outputSpec.outputFormat == GpuFrameFormat::R8) ? r8Pipeline_ : rgbaPipeline_;
        cmd->SetComputeRootSignature(pipeline.GetRootSignature());
        cmd->SetPipelineState(pipeline.GetPipelineState());
        cmd->SetComputeRootDescriptorTable(pipeline.SrvTableIndex(), inputSrv.gpu);
        cmd->SetComputeRootDescriptorTable(pipeline.UavTableIndex(), outputUav.gpu);

        ConvertConstants constants;
        constants.width = static_cast<std::uint32_t>(input.format.width);
        constants.height = static_cast<std::uint32_t>(input.format.height);
        constants.inputRowPitchBytes = static_cast<std::uint32_t>(input.format.inputRowPitchBytes);
        constants.inputFormat = static_cast<std::uint32_t>(input.format.actualInputFormat);
        constants.bayerPattern = BayerPatternCode(input.format.actualInputFormat);
        cmd->SetComputeRoot32BitConstants(pipeline.RootConstantsIndex(), sizeof(ConvertConstants) / sizeof(std::uint32_t), &constants, 0);

        const UINT groupsX = (static_cast<UINT>(input.format.width) + kThreadGroupSizeX - 1u) / kThreadGroupSizeX;
        const UINT groupsY = (static_cast<UINT>(input.format.height) + kThreadGroupSizeY - 1u) / kThreadGroupSizeY;
        cmd->Dispatch(groupsX, groupsY, 1);

        auto uavBarrier = UavBarrier(outFrame.textureResource.Get());
        slot.commandContext.ResourceBarrier(uavBarrier);
        const auto finalState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        auto outputBarrier = TransitionBarrier(outFrame.textureResource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, finalState);
        slot.commandContext.ResourceBarrier(outputBarrier);
        outFrame.textureResource.SetState(finalState);
        outFrame.resourceState = finalState;

        slot.commandContext.Close();
        ID3D12CommandList* lists[] = { slot.commandContext.GetCommandList() };
        queue_->ExecuteCommandLists(1, lists);
        const std::uint64_t fenceValue = queue_->Signal();
        slot.uploadRing.FinishFrame(fenceValue);
        slot.inFlight = fenceManager_->makeToken(fenceValue);
        outFrame.ready = slot.inFlight;
        return true;
    } catch (const std::exception& e) {
        setError(ErrorCode::D3D12Error, "D3D12FrameConverter::recordConvert", e.what());
        return false;
    }
}

bool D3D12FrameConverter::convert(const D3D12CpuFrameView& input,
                                  const FrameOutputSpec& outputSpec,
                                  D3D12CameraFrame& outFrame)
{
    lastError_ = NoError();
    outFrame = {};

    std::uint64_t neededBytes = 0;
    if (!validateInput(input, outputSpec, neededBytes)) {
        setError(ErrorCode::UnsupportedFormat,
                 "D3D12FrameConverter::convert",
                 std::string("Unsupported or invalid conversion: ") + ToString(input.format.actualInputFormat) + " -> " + ToString(outputSpec.outputFormat));
        return false;
    }
    if (!ensurePipelines()) return false;

    FrameSlot* slot = acquireSlot(AlignUp(neededBytes, 4));
    if (!slot) return false;

    if (!recordConvert(*slot, input, neededBytes, outputSpec, outFrame)) return false;
    if (outputSpec.createSrv && !createOutputSrv(outFrame)) return false;

    outFrame.timing = input.timing;
    outFrame.format = input.format;
    outFrame.format.outputFormat = outputSpec.outputFormat;
    return true;
}

} // namespace IC4Ext

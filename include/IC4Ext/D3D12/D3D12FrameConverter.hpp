#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/CoreTypes.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/D3D12/D3D12BackendContext.hpp"
#include "IC4Ext/D3D12/D3D12CameraFrame.hpp"
#include "IC4Ext/D3D12/D3D12FenceManager.hpp"

#include <D3D12Helper/D3D12Core/D3D12CommandContext.hpp>
#include <D3D12Helper/D3D12Framework/D3D12ComputePipeline.hpp>
#include <D3D12Helper/D3D12Framework/D3D12DescriptorAllocator.hpp>
#include <D3D12Helper/D3D12Framework/D3D12UploadRing.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace IC4Ext {

struct D3D12CpuFrameView
{
    const std::uint8_t* data = nullptr;
    std::size_t dataSize = 0;
    FrameTiming timing;
    FrameFormatMetadata format;
};

class D3D12FrameConverter
{
public:
    bool initialize(const D3D12BackendContext& backend,
                    D3D12FenceManager* fenceManager,
                    const ShaderLoadConfig& shaderConfig = {});

    bool initialize(ID3D12Device* device,
                    ID3D12CommandQueue* queue,
                    D3D12FenceManager* fenceManager,
                    const ShaderLoadConfig& shaderConfig = {});

    bool isSupported(CameraPixelFormat input, GpuFrameFormat output) const noexcept;

    bool convert(const D3D12CpuFrameView& input,
                 const FrameOutputSpec& outputSpec,
                 D3D12CameraFrame& outFrame);

    const ErrorInfo& lastError() const noexcept { return lastError_; }

private:
    struct ConvertConstants
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t inputRowPitchBytes = 0;
        std::uint32_t inputFormat = 0;
        std::uint32_t bayerPattern = 0;
        std::uint32_t reserved0 = 0;
        std::uint32_t reserved1 = 0;
        std::uint32_t reserved2 = 0;
    };

    struct FrameSlot
    {
        D3D12CoreLib::D3D12CommandContext commandContext;
        D3D12CoreLib::D3D12UploadRing uploadRing;
        D3D12CoreLib::D3D12DescriptorAllocator descriptorAllocator;
        D3D12ReadyToken inFlight;
        bool initialized = false;
    };

    bool ensurePipelines();
    bool createPipeline(GpuFrameFormat outputFormat);
    bool loadShaderBytecode(GpuFrameFormat outputFormat, D3D12CoreLib::ShaderBytecode& bytecode);
    bool compileShaderText(const std::string& sourceText, const std::filesystem::path& displayPath, D3D12CoreLib::ShaderBytecode& bytecode);
    bool readShaderFile(GpuFrameFormat outputFormat, std::string& sourceText, std::vector<std::uint8_t>& csoBytes);
    bool createOutputSrv(D3D12CameraFrame& frame);
    bool validateInput(const D3D12CpuFrameView& input, const FrameOutputSpec& outputSpec, std::uint64_t& neededBytes) const;
    FrameSlot* acquireSlot(std::uint64_t uploadSizeBytes);
    bool recordConvert(FrameSlot& slot,
                       const D3D12CpuFrameView& input,
                       std::uint64_t neededBytes,
                       const FrameOutputSpec& outputSpec,
                       D3D12CameraFrame& outFrame);

    void setError(ErrorCode code, const std::string& where, const std::string& message);

    D3D12BackendContext backend_;
    D3D12CoreLib::D3D12Core* core_ = nullptr;
    D3D12CoreLib::D3D12Queue* queue_ = nullptr;
    ID3D12Device* device_ = nullptr;
    D3D12FenceManager* fenceManager_ = nullptr;
    ShaderLoadConfig shaderConfig_;

    D3D12CoreLib::D3D12ComputePipeline rgbaPipeline_;
    D3D12CoreLib::D3D12ComputePipeline r8Pipeline_;
    bool rgbaPipelineReady_ = false;
    bool r8PipelineReady_ = false;

    static constexpr std::size_t kFrameSlotCount = 4;
    std::array<FrameSlot, kFrameSlotCount> slots_{};
    std::size_t nextSlot_ = 0;
    std::uint64_t uploadRingSizeBytes_ = 64ull * 1024ull * 1024ull;

    ErrorInfo lastError_;
};

} // namespace IC4Ext

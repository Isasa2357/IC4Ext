#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/CoreTypes.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/D3D11/D3D11CameraFrame.hpp"

#include <D3D11Helper/D3D11Core/D3D11Core.hpp>
#include <D3D11Helper/D3D11Gpu/D3D11ComputePipeline.hpp>
#include <D3D11Helper/D3D11Gpu/D3D11Resource.hpp>
#include <D3D11Helper/D3D11Gpu/D3D11ShaderCompiler.hpp>

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace IC4Ext::D3D11 {
class D3D11PooledFrameConverter;
}

namespace IC4Ext {

class D3D11FenceManager;

struct CpuFrameView
{
    const std::uint8_t* data = nullptr;
    std::size_t dataSize = 0;
    FrameTiming timing;
    FrameFormatMetadata format;
};

class D3D11FrameConverter
{
public:
    bool initialize(D3D11CoreLib::D3D11Core* core,
                    D3D11FenceManager* fenceManager,
                    const ShaderLoadConfig& shaderConfig);

    bool initialize(ID3D11Device* device,
                    ID3D11DeviceContext* context,
                    D3D11FenceManager* fenceManager,
                    const ShaderLoadConfig& shaderConfig);

    bool isSupported(CameraPixelFormat input, GpuFrameFormat output) const noexcept;

    bool convert(const CpuFrameView& input,
                 const FrameOutputSpec& outputSpec,
                 D3D11CameraFrame& outFrame);

    const ErrorInfo& lastError() const noexcept { return lastError_; }

private:
    bool ensurePipeline(CameraPixelFormat input,
                        GpuFrameFormat output,
                        D3D11CoreLib::D3D11ComputePipeline& pipeline,
                        bool& ready);
    bool loadShaderBytecode(const std::string& baseName,
                            D3D11CoreLib::ShaderBytecode& outBytecode);
    bool createRawInputBuffer(
        const CpuFrameView& input,
        D3D11CoreLib::D3D11Resource& buffer,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv);
    bool createOutputTexture(const CpuFrameView& input,
                             const FrameOutputSpec& spec,
                             D3D11CoreLib::D3D11Resource& textureResource,
                             D3D11CameraFrame& outFrame);
    bool createConstantBuffer(const CpuFrameView& input,
                              D3D11CoreLib::D3D11Resource& buffer);
    void setError(ErrorCode code,
                  const std::string& where,
                  const std::string& message);

    D3D11CoreLib::D3D11Core* core_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    D3D11FenceManager* fenceManager_ = nullptr;
    ShaderLoadConfig shaderConfig_;
    ErrorInfo lastError_;

    D3D11CoreLib::D3D11ComputePipeline mono8ToR8_;
    D3D11CoreLib::D3D11ComputePipeline mono8ToRgba8_;
    D3D11CoreLib::D3D11ComputePipeline bgr8ToRgba8_;
    D3D11CoreLib::D3D11ComputePipeline bgra8ToRgba8_;
    D3D11CoreLib::D3D11ComputePipeline bayer8ToRgba8_;

    bool mono8ToR8Ready_ = false;
    bool mono8ToRgba8Ready_ = false;
    bool bgr8ToRgba8Ready_ = false;
    bool bgra8ToRgba8Ready_ = false;
    bool bayer8ToRgba8Ready_ = false;

    friend class D3D11::D3D11PooledFrameConverter;
};

} // namespace IC4Ext

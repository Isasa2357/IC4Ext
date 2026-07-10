#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/CoreTypes.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/D3D11/D3D11CameraFrame.hpp"

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
    struct ShaderEntry;

    bool ensureShader(CameraPixelFormat input, GpuFrameFormat output, Microsoft::WRL::ComPtr<ID3D11ComputeShader>& shader);
    bool loadComputeShader(const std::string& baseName, Microsoft::WRL::ComPtr<ID3D11ComputeShader>& outShader);
    bool createRawInputBuffer(const CpuFrameView& input, Microsoft::WRL::ComPtr<ID3D11Buffer>& buffer, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv);
    bool createOutputTexture(const CpuFrameView& input, const FrameOutputSpec& spec, D3D11CameraFrame& outFrame);
    bool createConstantBuffer(const CpuFrameView& input, Microsoft::WRL::ComPtr<ID3D11Buffer>& buffer);
    void setError(ErrorCode code, const std::string& where, const std::string& message);

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    D3D11FenceManager* fenceManager_ = nullptr;
    ShaderLoadConfig shaderConfig_;
    ErrorInfo lastError_;

    Microsoft::WRL::ComPtr<ID3D11ComputeShader> mono8ToR8_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> mono8ToRgba8_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> bgr8ToRgba8_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> bgra8ToRgba8_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> bayer8ToRgba8_;
};

} // namespace IC4Ext

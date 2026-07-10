#include "IC4Ext/D3D11/D3D11FrameReadback.hpp"

#include <sstream>
#include <string>

namespace IC4Ext {

namespace {

std::string HrToString(HRESULT hr)
{
    std::ostringstream oss;
    oss << "HRESULT=0x" << std::hex << static_cast<unsigned long>(hr);
    return oss.str();
}

bool DxgiToGpuFrameFormat(DXGI_FORMAT dxgi, GpuFrameFormat& out) noexcept
{
    switch (dxgi) {
    case DXGI_FORMAT_R8_UNORM:
        out = GpuFrameFormat::R8;
        return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        out = GpuFrameFormat::RGBA8;
        return true;
    default:
        return false;
    }
}

} // namespace

void D3D11FrameReadback::setError(ErrorCode code, const std::string& where, const std::string& message)
{
    lastError_ = MakeError(code, where, message);
}

bool D3D11FrameReadback::initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    lastError_ = NoError();
    if (!device || !context) {
        setError(ErrorCode::InvalidArgument, "D3D11FrameReadback::initialize", "device and context must be non-null");
        return false;
    }
    device_ = device;
    context_ = context;
    return true;
}

bool D3D11FrameReadback::validateFrame(const D3D11CameraFrame& frame, GpuFrameFormat& srcFormat)
{
    if (!device_ || !context_) {
        setError(ErrorCode::D3D11Error, "D3D11FrameReadback::readback", "readback is not initialized");
        return false;
    }
    if (!frame.texture) {
        setError(ErrorCode::InvalidArgument, "D3D11FrameReadback::readback", "frame texture is null");
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    frame.texture->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0) {
        setError(ErrorCode::InvalidArgument, "D3D11FrameReadback::readback", "frame texture has invalid size");
        return false;
    }
    if (!DxgiToGpuFrameFormat(desc.Format, srcFormat)) {
        setError(ErrorCode::UnsupportedFormat, "D3D11FrameReadback::readback", "only DXGI_FORMAT_R8_UNORM and DXGI_FORMAT_R8G8B8A8_UNORM are supported");
        return false;
    }
    return true;
}

bool D3D11FrameReadback::readback(const D3D11CameraFrame& frame,
                                  CpuFrameFormat dstFormat,
                                  CpuFrame& out,
                                  std::uint32_t waitTimeoutMs)
{
    lastError_ = NoError();

    GpuFrameFormat srcFormat{};
    if (!validateFrame(frame, srcFormat)) return false;

    if (frame.ready.isValid() && !frame.ready.wait(waitTimeoutMs)) {
        setError(ErrorCode::Timeout, "D3D11FrameReadback::readback", "timed out waiting for frame ready fence");
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    frame.texture->GetDesc(&desc);
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.Usage = D3D11_USAGE_STAGING;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, staging.GetAddressOf());
    if (FAILED(hr)) {
        setError(ErrorCode::D3D11Error, "D3D11FrameReadback::CreateTexture2D", HrToString(hr));
        return false;
    }

    context_->CopyResource(staging.Get(), frame.texture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        setError(ErrorCode::D3D11Error, "D3D11FrameReadback::Map", HrToString(hr));
        return false;
    }

    const bool ok = ConvertPackedGpuFrameToCpuFrame(static_cast<const std::uint8_t*>(mapped.pData),
                                                    desc.Width,
                                                    desc.Height,
                                                    mapped.RowPitch,
                                                    srcFormat,
                                                    dstFormat,
                                                    frame.timing,
                                                    out,
                                                    &lastError_);
    context_->Unmap(staging.Get(), 0);
    return ok;
}

} // namespace IC4Ext

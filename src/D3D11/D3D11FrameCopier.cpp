#include "IC4Ext/D3D11/D3D11FrameCopier.hpp"
#include "IC4Ext/D3D11/D3D11FenceManager.hpp"

#include <D3D11Helper/D3D11Gpu/D3D11Copy.hpp>
#include <D3D11Helper/D3D11Gpu/D3D11Helpers.hpp>

#include <exception>
#include <sstream>

namespace IC4Ext {

namespace {
std::string HrToString(HRESULT hr)
{
    std::ostringstream oss;
    oss << "HRESULT=0x" << std::hex << static_cast<unsigned long>(hr);
    return oss.str();
}
}

void D3D11FrameCopier::setError(ErrorCode code, const std::string& where, const std::string& message)
{
    lastError_ = MakeError(code, where, message);
}

bool D3D11FrameCopier::initialize(D3D11CoreLib::D3D11Core* core, D3D11FenceManager* fenceManager)
{
    lastError_ = NoError();
    if (!core || !fenceManager) {
        setError(ErrorCode::InvalidArgument, "D3D11FrameCopier::initialize", "D3D11Core/fenceManager is null");
        return false;
    }
    core_ = core;
    device_ = core_->GetDevice();
    context_ = core_->GetImmediateContext();
    fenceManager_ = fenceManager;
    if (!device_ || !context_) {
        setError(ErrorCode::InvalidArgument, "D3D11FrameCopier::initialize", "D3D11Core has null device/context");
        return false;
    }
    return true;
}

bool D3D11FrameCopier::initialize(ID3D11Device* device, ID3D11DeviceContext* context, D3D11FenceManager* fenceManager)
{
    lastError_ = NoError();
    if (!device || !context || !fenceManager) {
        setError(ErrorCode::InvalidArgument, "D3D11FrameCopier::initialize", "device/context/fenceManager is null");
        return false;
    }
    core_ = nullptr;
    device_ = device;
    context_ = context;
    fenceManager_ = fenceManager;
    return true;
}

bool D3D11FrameCopier::copyFrame(const D3D11CameraFrame& src, D3D11CameraFrame& dst)
{
    lastError_ = NoError();
    dst = {};
    if (!src.texture) {
        setError(ErrorCode::InvalidArgument, "D3D11FrameCopier::copyFrame", "src.texture is null");
        return false;
    }
    if (!device_ || !context_ || !fenceManager_) {
        setError(ErrorCode::InvalidArgument, "D3D11FrameCopier::copyFrame", "copier is not initialized");
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    src.texture->GetDesc(&desc);

    if (core_) {
        try {
            UINT bindFlags = desc.BindFlags;
            if (src.srv) bindFlags |= D3D11_BIND_SHADER_RESOURCE;
            if (src.uav) bindFlags |= D3D11_BIND_UNORDERED_ACCESS;

            auto dstResource = D3D11CoreLib::CreateTexture2D(*core_,
                                                             desc.Width,
                                                             desc.Height,
                                                             desc.Format,
                                                             bindFlags,
                                                             desc.Usage,
                                                             desc.MiscFlags,
                                                             desc.ArraySize,
                                                             desc.MipLevels);
            dst.texture = dstResource.AsTexture2D();
            if (src.srv) {
                dst.srv = D3D11CoreLib::CreateTexture2DSrv(*core_, dstResource, desc.Format);
            }
            if (src.uav) {
                dst.uav = D3D11CoreLib::CreateTexture2DUav(*core_, dstResource, desc.Format);
            }
            D3D11CoreLib::CopyTexture2D(context_, dst.texture.Get(), src.texture.Get());
        } catch (const std::exception& e) {
            setError(ErrorCode::D3D11Error, "D3D11FrameCopier::copyFrame / D3D11Helper", e.what());
            return false;
        }
    } else {
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &dst.texture);
        if (FAILED(hr)) {
            setError(ErrorCode::D3D11Error, "D3D11FrameCopier::copyFrame / CreateTexture2D", HrToString(hr));
            return false;
        }

        if (src.srv) {
            hr = device_->CreateShaderResourceView(dst.texture.Get(), nullptr, &dst.srv);
            if (FAILED(hr)) {
                setError(ErrorCode::D3D11Error, "D3D11FrameCopier::copyFrame / CreateShaderResourceView", HrToString(hr));
                return false;
            }
        }
        if (src.uav) {
            hr = device_->CreateUnorderedAccessView(dst.texture.Get(), nullptr, &dst.uav);
            if (FAILED(hr)) {
                setError(ErrorCode::D3D11Error, "D3D11FrameCopier::copyFrame / CreateUnorderedAccessView", HrToString(hr));
                return false;
            }
        }
        try {
            D3D11CoreLib::CopyTexture2D(context_, dst.texture.Get(), src.texture.Get());
        } catch (const std::exception& e) {
            setError(ErrorCode::D3D11Error, "D3D11FrameCopier::copyFrame / D3D11Helper::CopyTexture2D", e.what());
            return false;
        }
    }

    dst.ready = fenceManager_->signal();
    if (!dst.ready.isValid()) {
        setError(ErrorCode::D3D11Error, "D3D11FrameCopier::copyFrame / signal", fenceManager_->lastError().message);
        return false;
    }
    dst.timing = src.timing;
    dst.format = src.format;
    return true;
}

} // namespace IC4Ext

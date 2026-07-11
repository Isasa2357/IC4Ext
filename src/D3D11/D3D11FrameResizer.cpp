#include "D3D11FrameResizer.hpp"

#include <D3D11Helper/D3D11Gpu/D3D11Helpers.hpp>
#include <D3D11Helper/D3D11Gpu/D3D11ResourceView.hpp>

#include <exception>

namespace IC4Ext {

void D3D11FrameResizer::setError(ErrorCode code, const char* where, const std::string& message)
{
    lastError_ = MakeError(code, where, message);
}

bool D3D11FrameResizer::initialize(D3D11CoreLib::D3D11Core* core,
                                   D3D11FenceManager* fenceManager,
                                   const std::filesystem::path& shaderDirectory)
{
    lastError_ = NoError();
    if (!core || !fenceManager) {
        setError(ErrorCode::InvalidArgument,
                 "D3D11FrameResizer::initialize",
                 "D3D11Core/fence manager is null");
        return false;
    }
    try {
        processingContext_.Initialize(*core, shaderDirectory);
        resizer_.Initialize(processingContext_);
        core_ = core;
        fenceManager_ = fenceManager;
        initialized_ = true;
        return true;
    } catch (const std::exception& e) {
        setError(ErrorCode::D3D11Error, "D3D11FrameResizer::initialize", e.what());
        return false;
    }
}

bool D3D11FrameResizer::resizeFrame(const D3D11CameraFrame& src,
                                    const CameraOutputResizeOptions& options,
                                    D3D11CameraFrame& dst)
{
    lastError_ = NoError();
    dst = {};
    if (!initialized_ || !core_ || !fenceManager_) {
        setError(ErrorCode::InvalidArgument,
                 "D3D11FrameResizer::resizeFrame",
                 "resizer is not initialized");
        return false;
    }
    if (!options.enabled()) {
        setError(ErrorCode::InvalidArgument,
                 "D3D11FrameResizer::resizeFrame",
                 "resize dimensions are disabled");
        return false;
    }
    if (!src.texture) {
        setError(ErrorCode::InvalidArgument,
                 "D3D11FrameResizer::resizeFrame",
                 "source texture is null");
        return false;
    }
    if (src.format.outputFormat != GpuFrameFormat::RGBA8) {
        setError(ErrorCode::UnsupportedFormat,
                 "D3D11FrameResizer::resizeFrame",
                 "output-queue resize currently supports RGBA8 frames only");
        return false;
    }
    if (src.ready.isValid() && !src.ready.wait(INFINITE)) {
        setError(ErrorCode::D3D11Error,
                 "D3D11FrameResizer::resizeFrame",
                 "source ready fence wait failed");
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc{};
    src.texture->GetDesc(&srcDesc);
    if (srcDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        setError(ErrorCode::UnsupportedFormat,
                 "D3D11FrameResizer::resizeFrame",
                 "only DXGI_FORMAT_R8G8B8A8_UNORM is supported");
        return false;
    }

    try {
        auto outputResource = resizer_.CreateOutputTexture(
            *core_, options.width, options.height, srcDesc.Format);
        dst.texture = outputResource.AsTexture2D();
        dst.srv = D3D11CoreLib::CreateTexture2DSrv(*core_, outputResource, srcDesc.Format);
        dst.uav = D3D11CoreLib::CreateTexture2DUav(*core_, outputResource, srcDesc.Format);
        dst.processingSourceKeepAlive = src.texture;

        D3D11CoreLib::Processing::ResizeDesc desc{};
        desc.filter = options.filter == CameraOutputResizeFilter::Point
            ? D3D11CoreLib::Processing::ProcessingFilter::Point
            : D3D11CoreLib::Processing::ProcessingFilter::Linear;

        resizer_.DispatchResizeView(
            core_->GetImmediateContext(),
            D3D11CoreLib::D3D11ResourceView(src.texture.Get()),
            D3D11CoreLib::D3D11ResourceView(dst.texture.Get()),
            desc);

        dst.ready = fenceManager_->signal();
        if (!dst.ready.isValid()) {
            setError(ErrorCode::D3D11Error,
                     "D3D11FrameResizer::resizeFrame / signal",
                     fenceManager_->lastError().message);
            return false;
        }

        dst.timing = src.timing;
        dst.format = src.format;
        dst.format.width = static_cast<int>(options.width);
        dst.format.height = static_cast<int>(options.height);
        dst.format.inputRowPitchBytes = 0;
        dst.chunkMetadata = src.chunkMetadata;
        return true;
    } catch (const std::exception& e) {
        setError(ErrorCode::D3D11Error, "D3D11FrameResizer::resizeFrame", e.what());
        return false;
    }
}

} // namespace IC4Ext

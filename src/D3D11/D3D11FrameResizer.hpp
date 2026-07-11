#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/D3D11/D3D11CameraFrame.hpp"
#include "IC4Ext/D3D11/D3D11FenceManager.hpp"

#include <D3D11Helper/D3D11Core/D3D11Core.hpp>
#include <D3D11Helper/D3D11Processing/D3D11ProcessingContext.hpp>
#include <D3D11Helper/D3D11Processing/D3D11Resize.hpp>

#include <filesystem>

namespace IC4Ext {

class D3D11FrameResizer
{
public:
    bool initialize(D3D11CoreLib::D3D11Core* core,
                    D3D11FenceManager* fenceManager,
                    const std::filesystem::path& shaderDirectory);

    bool resizeFrame(D3D11CameraFrame& src,
                     const CameraOutputResizeOptions& options,
                     D3D11CameraFrame& dst);

    const ErrorInfo& lastError() const noexcept { return lastError_; }

private:
    void setError(ErrorCode code, const char* where, const std::string& message);

    D3D11CoreLib::D3D11Core* core_ = nullptr;
    D3D11FenceManager* fenceManager_ = nullptr;
    D3D11CoreLib::Processing::D3D11ProcessingContext processingContext_;
    D3D11CoreLib::Processing::D3D11Resizer resizer_;
    bool initialized_ = false;
    ErrorInfo lastError_;
};

} // namespace IC4Ext

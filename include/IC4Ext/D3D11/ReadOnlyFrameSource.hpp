#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/Error.hpp"

namespace IC4Ext::D3D11 {

class D3D11ReadOnlyFrame;

class ReadOnlyFrameSource
{
public:
    virtual ~ReadOnlyFrameSource() = default;
    virtual bool isOpened() const noexcept = 0;
    virtual bool read(
        const CameraReadOptions& options,
        D3D11ReadOnlyFrame& outFrame,
        ErrorInfo& outError) = 0;
    virtual ErrorInfo lastError() const = 0;
};

} // namespace IC4Ext::D3D11

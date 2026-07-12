#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/Error.hpp"

namespace IC4Ext::D3D12 {

class D3D12ReadOnlyFrame;

// Injectable source used by CameraCaptureThread. The production constructor
// continues to own CameraCapture; this interface allows camera-free sources and
// other immutable-frame producers to use the same capture-thread/sync path.
class ReadOnlyFrameSource
{
public:
    virtual ~ReadOnlyFrameSource() = default;

    virtual bool isOpened() const noexcept = 0;

    // Returns true and assigns outFrame on success. On failure, outError must
    // describe the failure (Timeout is treated as a non-fatal read timeout by
    // CameraCaptureThread).
    virtual bool read(const CameraReadOptions& options,
                      D3D12ReadOnlyFrame& outFrame,
                      ErrorInfo& outError) = 0;

    virtual ErrorInfo lastError() const = 0;
};

} // namespace IC4Ext::D3D12

#include "IC4Ext/D3D11/D3D11CameraCapture.hpp"
#include "IC4Ext/D3D11/D3D11CameraCaptureThread.hpp"
#include "IC4Ext/D3D11/D3D11DummyCameraCapture.hpp"

namespace IC4Ext {

namespace {

std::string EffectiveSoftwareTriggerCommand(const std::string& commandName)
{
    return commandName.empty() ? std::string("TriggerSoftware") : commandName;
}

} // namespace

bool D3D11CameraCapture::softwareTrigger(const std::string& commandName)
{
    return setIC4Property(EffectiveSoftwareTriggerCommand(commandName), std::string("execute"));
}

bool D3D11DummyCameraCapture::softwareTrigger(const std::string& commandName)
{
    return setIC4Property(EffectiveSoftwareTriggerCommand(commandName), std::string("execute"));
}

bool D3D11CameraCaptureThread::softwareTrigger(const std::string& commandName)
{
    std::lock_guard<std::mutex> sourceLock(sourceMutex_);
    if (!source_ || !source_->isOpened()) {
        setError(ErrorCode::NotOpened,
                 "D3D11CameraCaptureThread::softwareTrigger",
                 "Source is not opened");
        return false;
    }

    const bool ok = source_->softwareTrigger(EffectiveSoftwareTriggerCommand(commandName));
    lastError_ = ok ? NoError() : source_->lastError();
    return ok;
}

} // namespace IC4Ext

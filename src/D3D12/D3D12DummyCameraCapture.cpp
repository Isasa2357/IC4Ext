#include "IC4Ext/D3D12/D3D12DummyCameraCapture.hpp"

#include <chrono>
#include <optional>
#include <utility>

namespace IC4Ext {

D3D12DummyCameraCapture::D3D12DummyCameraCapture(std::uint32_t cameraIndex,
                                   std::shared_ptr<D3D12FrameQueue> frameQueue,
                                   std::weak_ptr<ID3D12CameraControlSink> controlSink)
    : cameraIndex_(cameraIndex), frameQueue_(std::move(frameQueue)), controlSink_(std::move(controlSink))
{
    if (!frameQueue_) {
        opened_.store(false);
        lastError_ = MakeError(ErrorCode::InvalidArgument, "D3D12DummyCameraCapture::D3D12DummyCameraCapture", "frameQueue is null");
    }
}

void D3D12DummyCameraCapture::close() noexcept
{
    opened_.store(false);
}

void D3D12DummyCameraCapture::setError(ErrorCode code, const std::string& where, const std::string& message)
{
    lastError_ = MakeError(code, where, message);
}

D3D12ReadResult D3D12DummyCameraCapture::read(ReadMode mode)
{
    return read(CameraReadOptions{mode, 1000});
}

D3D12ReadResult D3D12DummyCameraCapture::read(const CameraReadOptions& options)
{
    D3D12ReadResult result;
    if (!opened_.load()) {
        result.error = MakeError(ErrorCode::NotOpened, "D3D12DummyCameraCapture::read", "DummyCameraCapture is closed");
        lastError_ = result.error;
        return result;
    }
    if (!frameQueue_) {
        result.error = MakeError(ErrorCode::InvalidArgument, "D3D12DummyCameraCapture::read", "frameQueue is null");
        lastError_ = result.error;
        return result;
    }

    const auto timeout = std::chrono::milliseconds(options.timeoutMs);
    std::optional<D3D12CameraFrame> item;
    if (options.mode == ReadMode::LatestFrame) {
        item = frameQueue_->waitPopLatestFor(timeout);
    } else {
        item = frameQueue_->waitPopFor(timeout);
    }

    if (!item) {
        result.error = MakeError(ErrorCode::Timeout, "D3D12DummyCameraCapture::read", "Read timed out");
        lastError_ = result.error;
        return result;
    }

    result.frame = std::move(*item);
    result.ok = true;
    lastError_ = NoError();
    return result;
}

bool D3D12DummyCameraCapture::sendControlCommand(const CameraControlCommand& command)
{
    auto sink = controlSink_.lock();
    if (!sink) {
        setError(ErrorCode::NotOpened, "D3D12DummyCameraCapture::sendControlCommand", "ControlSink is expired");
        return false;
    }
    const bool ok = sink->submitControlCommand(command);
    lastError_ = ok ? NoError() : sink->lastError();
    return ok;
}

bool D3D12DummyCameraCapture::applyIC4StateJson(const std::filesystem::path& jsonPath,
                                         std::size_t deviceIndex,
                                         bool strict,
                                         bool applyNestedSelectorStates)
{
    return sendControlCommand(CameraControlCommand::ApplyJson(jsonPath, deviceIndex, strict, applyNestedSelectorStates));
}

bool D3D12DummyCameraCapture::setIC4Property(const std::string& propertyName, bool value)
{
    return sendControlCommand(CameraControlCommand::PropertyBool(propertyName, value));
}

bool D3D12DummyCameraCapture::setIC4Property(const std::string& propertyName, int value)
{
    return setIC4Property(propertyName, static_cast<std::int64_t>(value));
}

bool D3D12DummyCameraCapture::setIC4Property(const std::string& propertyName, std::int64_t value)
{
    return sendControlCommand(CameraControlCommand::PropertyInt64(propertyName, value));
}

bool D3D12DummyCameraCapture::setIC4Property(const std::string& propertyName, double value)
{
    return sendControlCommand(CameraControlCommand::PropertyDouble(propertyName, value));
}

bool D3D12DummyCameraCapture::setIC4Property(const std::string& propertyName, const char* value)
{
    return setIC4Property(propertyName, std::string(value ? value : ""));
}

bool D3D12DummyCameraCapture::setIC4Property(const std::string& propertyName, const std::string& value)
{
    return sendControlCommand(CameraControlCommand::PropertyString(propertyName, value));
}

bool D3D12DummyCameraCapture::setFrameRate(double fps)
{
    return sendControlCommand(CameraControlCommand::FrameRate(fps));
}

bool D3D12DummyCameraCapture::setExposureAuto(const std::string& mode)
{
    return sendControlCommand(CameraControlCommand::ExposureAuto(mode));
}

bool D3D12DummyCameraCapture::setExposureTime(double exposureTimeUs)
{
    return sendControlCommand(CameraControlCommand::ExposureTime(exposureTimeUs));
}

bool D3D12DummyCameraCapture::setGainAuto(const std::string& mode)
{
    return sendControlCommand(CameraControlCommand::GainAuto(mode));
}

bool D3D12DummyCameraCapture::setGain(double gain)
{
    return sendControlCommand(CameraControlCommand::Gain(gain));
}

bool D3D12DummyCameraCapture::setGamma(double gamma)
{
    return sendControlCommand(CameraControlCommand::Gamma(gamma));
}

bool D3D12DummyCameraCapture::setOffset(int offsetX, int offsetY)
{
    return sendControlCommand(CameraControlCommand::Offset(offsetX, offsetY));
}

bool D3D12DummyCameraCapture::setRoi(int width, int height, int offsetX, int offsetY)
{
    return sendControlCommand(CameraControlCommand::Roi(width, height, offsetX, offsetY));
}

bool D3D12DummyCameraCapture::setPixelFormat(CameraPixelFormat fmt)
{
    return sendControlCommand(CameraControlCommand::PixelFormat(fmt));
}

} // namespace IC4Ext

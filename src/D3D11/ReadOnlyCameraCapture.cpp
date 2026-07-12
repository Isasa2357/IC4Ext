#include "IC4Ext/D3D11/CameraCapture.hpp"

#include "IC4Ext/D3D11/D3D11CameraCapture.hpp"
#include "IC4Ext/D3D11/D3D11FenceManager.hpp"

#include <deque>
#include <mutex>
#include <utility>

namespace IC4Ext::D3D11 {

class D3D11ReadOnlyCameraCapture::Impl
{
public:
    struct RetainedSourceFrame
    {
        ::IC4Ext::D3D11CameraFrame frame;
        ::IC4Ext::D3D11ReadyToken copyCompletion;
    };

    D3D11BackendContext backend;
    D3D11CameraCaptureOptions options;
    std::unique_ptr<::IC4Ext::D3D11CameraCapture> capture;
    std::unique_ptr<::IC4Ext::D3D11FenceManager> copyFence;
    std::unique_ptr<D3D11FramePool> pool;
    std::uint32_t poolWidth = 0;
    std::uint32_t poolHeight = 0;
    DXGI_FORMAT poolFormat = DXGI_FORMAT_UNKNOWN;
    std::deque<RetainedSourceFrame> retainedSources;

    mutable std::mutex mutex;
    ErrorInfo error;

    void setError(ErrorInfo value) { error = std::move(value); }
    void setError(ErrorCode code, const char* where, std::string message)
    {
        error = MakeError(code, where, std::move(message));
    }
    void clearError() { error = NoError(); }

    void collectCompletedCopies()
    {
        while (!retainedSources.empty()) {
            const auto& token = retainedSources.front().copyCompletion;
            if (token.isValid() && !token.isReady()) break;
            retainedSources.pop_front();
        }
    }

    void waitAndClearCopies() noexcept
    {
        for (auto& retained : retainedSources) {
            if (retained.copyCompletion.isValid()) {
                retained.copyCompletion.wait(5000);
            }
        }
        retainedSources.clear();
    }

    bool ensurePool(const D3D11_TEXTURE2D_DESC& description)
    {
        if (pool && poolWidth == description.Width &&
            poolHeight == description.Height && poolFormat == description.Format) {
            return true;
        }

        auto nextPool = std::make_unique<D3D11FramePool>();
        D3D11FramePoolConfig config;
        config.width = description.Width;
        config.height = description.Height;
        config.format = description.Format;
        config.createSrv = true;
        config.createUav = true;
        config.initialCapacity = options.initialFramePoolCapacity;
        config.maxCapacity = options.maxFramePoolCapacity;
        config.exhaustionPolicy = options.framePoolExhaustionPolicy;
        config.waitTimeout = options.framePoolWaitTimeout;
        if (!nextPool->initialize(backend, config)) {
            setError(nextPool->lastError());
            return false;
        }

        poolWidth = description.Width;
        poolHeight = description.Height;
        poolFormat = description.Format;
        pool = std::move(nextPool);
        return true;
    }
};

D3D11ReadOnlyCameraCapture::D3D11ReadOnlyCameraCapture()
    : impl_(std::make_unique<Impl>())
{
}

D3D11ReadOnlyCameraCapture::~D3D11ReadOnlyCameraCapture()
{
    close();
}

D3D11ReadOnlyCameraCapture::D3D11ReadOnlyCameraCapture(
    D3D11ReadOnlyCameraCapture&& other) noexcept
{
    moveFrom(std::move(other));
}

D3D11ReadOnlyCameraCapture& D3D11ReadOnlyCameraCapture::operator=(
    D3D11ReadOnlyCameraCapture&& other) noexcept
{
    if (this != &other) {
        close();
        moveFrom(std::move(other));
    }
    return *this;
}

void D3D11ReadOnlyCameraCapture::moveFrom(
    D3D11ReadOnlyCameraCapture&& other) noexcept
{
    impl_ = std::move(other.impl_);
    opened_.store(other.opened_.load());
    other.opened_.store(false);
    if (!other.impl_) other.impl_ = std::make_unique<Impl>();
}

bool D3D11ReadOnlyCameraCapture::open(
    const IC4DeviceSelector& selector,
    const CameraCaptureConfig& config,
    D3D11BackendContext backend,
    D3D11CameraCaptureOptions options)
{
    close();
    impl_ = std::make_unique<Impl>();
    impl_->options = options;

    if (!options.isValid()) {
        impl_->setError(
            ErrorCode::InvalidArgument,
            "D3D11ReadOnlyCameraCapture::open",
            "Invalid frame pool options");
        return false;
    }
    if (!backend.resolve() || !backend.corePtr) {
        impl_->setError(
            ErrorCode::InvalidArgument,
            "D3D11ReadOnlyCameraCapture::open",
            "D3D11 backend context is incomplete");
        return false;
    }
    impl_->backend = std::move(backend);

    impl_->copyFence = std::make_unique<::IC4Ext::D3D11FenceManager>();
    if (!impl_->copyFence->initialize(
            impl_->backend.device,
            impl_->backend.immediateContext)) {
        impl_->setError(impl_->copyFence->lastError());
        return false;
    }

    impl_->capture = std::make_unique<::IC4Ext::D3D11CameraCapture>();
    if (!impl_->capture->open(selector, config, impl_->backend.corePtr)) {
        impl_->setError(impl_->capture->lastError());
        return false;
    }

    impl_->clearError();
    opened_.store(true);
    return true;
}

void D3D11ReadOnlyCameraCapture::close() noexcept
{
    opened_.store(false);
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->waitAndClearCopies();
    if (impl_->capture) impl_->capture->close();
    impl_->pool.reset();
    impl_->copyFence.reset();
    impl_->capture.reset();
}

bool D3D11ReadOnlyCameraCapture::startAcquisition()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->capture) return false;
    const bool result = impl_->capture->startAcquisition();
    impl_->setError(result ? NoError() : impl_->capture->lastError());
    return result;
}

bool D3D11ReadOnlyCameraCapture::stopAcquisition()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->capture) return false;
    const bool result = impl_->capture->stopAcquisition();
    impl_->setError(result ? NoError() : impl_->capture->lastError());
    return result;
}

bool D3D11ReadOnlyCameraCapture::isStreaming() const noexcept
{
    return impl_ && impl_->capture && impl_->capture->isStreaming();
}

bool D3D11ReadOnlyCameraCapture::isAcquisitionActive() const noexcept
{
    return impl_ && impl_->capture && impl_->capture->isAcquisitionActive();
}

D3D11ReadOnlyReadResult D3D11ReadOnlyCameraCapture::read(ReadMode mode)
{
    return read(CameraReadOptions{mode, 1000});
}

D3D11ReadOnlyReadResult D3D11ReadOnlyCameraCapture::read(
    const CameraReadOptions& options)
{
    D3D11ReadOnlyReadResult result;
    if (!opened_.load() || !impl_) {
        result.error = MakeError(
            ErrorCode::NotOpened,
            "D3D11ReadOnlyCameraCapture::read",
            "Capture is not opened");
        return result;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->collectCompletedCopies();
    auto sourceResult = impl_->capture->read(options);
    if (!sourceResult) {
        result.error = sourceResult.error;
        impl_->setError(result.error);
        return result;
    }
    if (!sourceResult.frame.texture) {
        result.error = MakeError(
            ErrorCode::InternalError,
            "D3D11ReadOnlyCameraCapture::read",
            "Legacy capture returned a null texture");
        impl_->setError(result.error);
        return result;
    }

    D3D11_TEXTURE2D_DESC description{};
    sourceResult.frame.texture->GetDesc(&description);
    if (!impl_->ensurePool(description)) {
        result.error = impl_->error;
        return result;
    }

    auto writer = impl_->pool->acquire();
    if (!writer) {
        result.error = impl_->pool->lastError();
        impl_->setError(result.error);
        return result;
    }

    impl_->backend.immediateContext->CopyResource(
        writer.texture(),
        sourceResult.frame.texture.Get());
    const auto copyReady = impl_->copyFence->signal();
    if (!copyReady.isValid()) {
        result.error = impl_->copyFence->lastError();
        impl_->setError(result.error);
        return result;
    }

    result.frame = writer.publish(
        copyReady,
        sourceResult.frame.timing,
        sourceResult.frame.format,
        sourceResult.frame.chunkMetadata);
    if (!result.frame) {
        result.error = MakeError(
            ErrorCode::InternalError,
            "D3D11ReadOnlyCameraCapture::read",
            "FramePool writer failed to publish copied camera frame");
        impl_->setError(result.error);
        return result;
    }

    impl_->retainedSources.push_back(Impl::RetainedSourceFrame{
        std::move(sourceResult.frame),
        copyReady});
    impl_->clearError();
    result.ok = true;
    return result;
}

bool D3D11ReadOnlyCameraCapture::applyIC4StateJson(
    const std::filesystem::path& path,
    std::size_t deviceIndex,
    bool strict,
    bool nested)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->capture) return false;
    const bool ok = impl_->capture->applyIC4StateJson(
        path,
        deviceIndex,
        strict,
        nested);
    impl_->setError(ok ? NoError() : impl_->capture->lastError());
    return ok;
}

#define IC4EXT_D3D11_FORWARD_PROPERTY(TYPE) \
bool D3D11ReadOnlyCameraCapture::setIC4Property( \
    const std::string& name, TYPE value) \
{ \
    std::lock_guard<std::mutex> lock(impl_->mutex); \
    if (!impl_->capture) return false; \
    const bool ok = impl_->capture->setIC4Property(name, value); \
    impl_->setError(ok ? NoError() : impl_->capture->lastError()); \
    return ok; \
}

IC4EXT_D3D11_FORWARD_PROPERTY(bool)
IC4EXT_D3D11_FORWARD_PROPERTY(int)
IC4EXT_D3D11_FORWARD_PROPERTY(std::int64_t)
IC4EXT_D3D11_FORWARD_PROPERTY(double)
IC4EXT_D3D11_FORWARD_PROPERTY(const char*)
IC4EXT_D3D11_FORWARD_PROPERTY(const std::string&)
#undef IC4EXT_D3D11_FORWARD_PROPERTY

#define IC4EXT_D3D11_FORWARD_CONTROL(NAME, TYPE) \
bool D3D11ReadOnlyCameraCapture::NAME(TYPE value) \
{ \
    std::lock_guard<std::mutex> lock(impl_->mutex); \
    if (!impl_->capture) return false; \
    const bool ok = impl_->capture->NAME(value); \
    impl_->setError(ok ? NoError() : impl_->capture->lastError()); \
    return ok; \
}

IC4EXT_D3D11_FORWARD_CONTROL(setFrameRate, double)
IC4EXT_D3D11_FORWARD_CONTROL(setExposureAuto, const std::string&)
IC4EXT_D3D11_FORWARD_CONTROL(setExposureTime, double)
IC4EXT_D3D11_FORWARD_CONTROL(setGainAuto, const std::string&)
IC4EXT_D3D11_FORWARD_CONTROL(setGain, double)
IC4EXT_D3D11_FORWARD_CONTROL(setGamma, double)
IC4EXT_D3D11_FORWARD_CONTROL(setPixelFormat, CameraPixelFormat)
#undef IC4EXT_D3D11_FORWARD_CONTROL

bool D3D11ReadOnlyCameraCapture::setOffset(int x, int y)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->capture) return false;
    const bool ok = impl_->capture->setOffset(x, y);
    impl_->setError(ok ? NoError() : impl_->capture->lastError());
    return ok;
}

bool D3D11ReadOnlyCameraCapture::setRoi(int width, int height, int x, int y)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->capture) return false;
    const bool ok = impl_->capture->setRoi(width, height, x, y);
    impl_->setError(ok ? NoError() : impl_->capture->lastError());
    return ok;
}

bool D3D11ReadOnlyCameraCapture::softwareTrigger(const std::string& commandName)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->capture) return false;
    const bool ok = impl_->capture->softwareTrigger(commandName);
    impl_->setError(ok ? NoError() : impl_->capture->lastError());
    return ok;
}

CameraCaptureStats D3D11ReadOnlyCameraCapture::stats() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->capture ? impl_->capture->stats() : CameraCaptureStats{};
}

CameraPerformanceSnapshot D3D11ReadOnlyCameraCapture::performance()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->capture
        ? impl_->capture->performance()
        : CameraPerformanceSnapshot{};
}

D3D11FramePoolStats D3D11ReadOnlyCameraCapture::framePoolStats() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->pool ? impl_->pool->stats() : D3D11FramePoolStats{};
}

ErrorInfo D3D11ReadOnlyCameraCapture::lastError() const
{
    if (!impl_) return NoError();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->error) return impl_->error;
    return impl_->capture ? impl_->capture->lastError() : NoError();
}

} // namespace IC4Ext::D3D11

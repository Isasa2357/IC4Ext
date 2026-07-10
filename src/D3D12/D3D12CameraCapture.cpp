#include "IC4Ext/D3D12/D3D12CameraCapture.hpp"
#include "IC4Ext/D3D12/D3D12FenceManager.hpp"
#include "IC4Ext/D3D12/D3D12FrameConverter.hpp"
#include "IC4Ext/Core/IC4ChunkMetadata.hpp"
#include "../Core/IC4PerformanceUtil.hpp"

#include <ic4/ic4.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>

namespace IC4Ext {

namespace {

class IC4LibraryContext
{
public:
    IC4LibraryContext()
    {
        ic4::InitLibraryConfig cfg;
        cfg.defaultErrorHandlerBehavior = ic4::ErrorHandlerBehavior::Ignore;
        initialized_ = ic4::initLibrary(cfg);
    }
    ~IC4LibraryContext()
    {
        if (initialized_) {
            ic4::exitLibrary();
        }
    }
    bool initialized() const noexcept { return initialized_; }
private:
    bool initialized_ = false;
};

std::shared_ptr<IC4LibraryContext> SharedIC4Context()
{
    static std::weak_ptr<IC4LibraryContext> weak;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    auto ctx = weak.lock();
    if (!ctx) {
        ctx = std::make_shared<IC4LibraryContext>();
        weak = ctx;
    }
    return ctx;
}

std::string IC4ErrorMessage(const ic4::Error& err)
{
    std::ostringstream oss;
    oss << "IC4 error code=" << static_cast<int>(err.code()) << ": " << err.message();
    return oss.str();
}

ic4::PixelFormat ToIC4PixelFormat(CameraPixelFormat fmt)
{
    switch (fmt) {
    case CameraPixelFormat::Mono8: return ic4::PixelFormat::Mono8;
    case CameraPixelFormat::BayerRG8: return ic4::PixelFormat::BayerRG8;
    case CameraPixelFormat::BayerGR8: return ic4::PixelFormat::BayerGR8;
    case CameraPixelFormat::BayerGB8: return ic4::PixelFormat::BayerGB8;
    case CameraPixelFormat::BayerBG8: return ic4::PixelFormat::BayerBG8;
    case CameraPixelFormat::BGR8: return ic4::PixelFormat::BGR8;
    case CameraPixelFormat::BGRa8: return ic4::PixelFormat::BGRa8;
    default: return ic4::PixelFormat::Unspecified;
    }
}

CameraPixelFormat FromIC4PixelFormat(ic4::PixelFormat fmt, CameraPixelFormat fallback)
{
    switch (fmt) {
    case ic4::PixelFormat::Mono8: return CameraPixelFormat::Mono8;
    case ic4::PixelFormat::BayerRG8: return CameraPixelFormat::BayerRG8;
    case ic4::PixelFormat::BayerGR8: return CameraPixelFormat::BayerGR8;
    case ic4::PixelFormat::BayerGB8: return CameraPixelFormat::BayerGB8;
    case ic4::PixelFormat::BayerBG8: return CameraPixelFormat::BayerBG8;
    case ic4::PixelFormat::BGR8: return CameraPixelFormat::BGR8;
    case ic4::PixelFormat::BGRa8: return CameraPixelFormat::BGRa8;
    default: return fallback;
    }
}

using Json = nlohmann::json;

std::string JsonErrorMessage(const std::exception& ex)
{
    return std::string("JSON parse/apply error: ") + ex.what();
}

std::optional<Json> LoadJsonFile(const std::filesystem::path& path, ErrorInfo& outError)
{
    try {
        std::ifstream ifs(path);
        if (!ifs) {
            outError = MakeError(ErrorCode::InvalidArgument, "LoadJsonFile", "Could not open JSON file: " + path.string());
            return std::nullopt;
        }
        Json root;
        ifs >> root;
        return root;
    } catch (const std::exception& ex) {
        outError = MakeError(ErrorCode::InvalidArgument, "LoadJsonFile", JsonErrorMessage(ex));
        return std::nullopt;
    }
}

const Json* FindIC4StateObject(const Json& root, std::size_t deviceIndex, ErrorInfo& outError)
{
    if (!root.is_object() || !root.contains("devices") || !root["devices"].is_array()) {
        outError = MakeError(ErrorCode::InvalidArgument, "FindIC4StateObject", "JSON does not contain devices[]");
        return nullptr;
    }
    const auto& devices = root["devices"];
    if (deviceIndex >= devices.size()) {
        outError = MakeError(ErrorCode::InvalidArgument, "FindIC4StateObject", "deviceIndex is out of range in JSON devices[]");
        return nullptr;
    }
    const auto& device = devices.at(deviceIndex);
    if (!device.is_object() || !device.contains("state") || !device["state"].is_object()) {
        outError = MakeError(ErrorCode::InvalidArgument, "FindIC4StateObject", "JSON device entry does not contain state object");
        return nullptr;
    }
    return &device["state"];
}

bool TryGetJsonInt(const Json& state, const char* name, int& out)
{
    if (!state.is_object() || !state.contains(name)) return false;
    const auto& v = state.at(name);
    if (v.is_number_integer() || v.is_number_unsigned()) {
        out = v.get<int>();
        return true;
    }
    return false;
}

bool TryGetJsonDouble(const Json& state, const char* name, double& out)
{
    if (!state.is_object() || !state.contains(name)) return false;
    const auto& v = state.at(name);
    if (v.is_number()) {
        out = v.get<double>();
        return true;
    }
    return false;
}

bool TryGetJsonPixelFormat(const Json& state, CameraPixelFormat& out)
{
    if (!state.is_object() || !state.contains("PixelFormat") || !state.at("PixelFormat").is_string()) return false;
    return ParseCameraPixelFormat(state.at("PixelFormat").get<std::string>(), out);
}

CameraCaptureConfig BuildEffectiveConfigFromJson(CameraCaptureConfig config, ErrorInfo& outError)
{
    if (!config.ic4StateJson.enabled()) return config;
    auto root = LoadJsonFile(config.ic4StateJson.path, outError);
    if (!root) return config;
    const Json* state = FindIC4StateObject(*root, config.ic4StateJson.deviceIndex, outError);
    if (!state) return config;

    if (!config.streamRequest.forceRequestedFormat) {
        CameraPixelFormat jsonFmt{};
        if (TryGetJsonPixelFormat(*state, jsonFmt)) {
            config.streamRequest.requestedFormat = jsonFmt;
        }
    }

    int jsonInt = 0;
    if (config.streamRequest.width <= 0 && TryGetJsonInt(*state, "Width", jsonInt)) config.streamRequest.width = jsonInt;
    if (config.streamRequest.height <= 0 && TryGetJsonInt(*state, "Height", jsonInt)) config.streamRequest.height = jsonInt;

    double jsonDouble = 0.0;
    if (config.streamRequest.fps <= 0.0 && TryGetJsonDouble(*state, "AcquisitionFrameRate", jsonDouble)) {
        config.streamRequest.fps = jsonDouble;
    }
    return config;
}

bool SetPropertyFromJsonScalar(ic4::PropertyMap& props,
                               const std::string& propertyName,
                               const Json& value,
                               bool strict,
                               ErrorInfo& outError)
{
    ic4::Error err;
    bool ok = true;
    try {
        if (value.is_boolean()) ok = props.setValue(propertyName, value.get<bool>(), err);
        else if (value.is_number_integer()) ok = props.setValue(propertyName, value.get<std::int64_t>(), err);
        else if (value.is_number_unsigned()) ok = props.setValue(propertyName, static_cast<std::int64_t>(value.get<std::uint64_t>()), err);
        else if (value.is_number_float()) ok = props.setValue(propertyName, value.get<double>(), err);
        else if (value.is_string()) ok = props.setValue(propertyName, value.get<std::string>(), err);
        else return true;
    } catch (const std::exception& ex) {
        ok = false;
        outError = MakeError(ErrorCode::IC4Error, "SetPropertyFromJsonScalar", JsonErrorMessage(ex));
    }

    if (!ok) {
        outError = MakeError(ErrorCode::IC4Error,
                             "SetPropertyFromJsonScalar / " + propertyName,
                             err.isError() ? IC4ErrorMessage(err) : "IC4 property setValue returned false");
        return !strict;
    }
    return true;
}

bool ApplyJsonStateObject(ic4::PropertyMap& props,
                          const Json& state,
                          bool strict,
                          bool applyNestedSelectorStates,
                          ErrorInfo& outError)
{
    if (!state.is_object()) {
        outError = MakeError(ErrorCode::InvalidArgument, "ApplyJsonStateObject", "state is not a JSON object");
        return false;
    }

    for (const auto& item : state.items()) {
        const std::string& propertyName = item.key();
        const Json& value = item.value();

        if (value.is_object()) {
            if (!applyNestedSelectorStates) continue;

            const bool hasSelectedValue = value.contains("(Value)") && value.at("(Value)").is_string();
            std::string selectedValue = hasSelectedValue ? value.at("(Value)").get<std::string>() : std::string{};

            for (const auto& selectorEntry : value.items()) {
                if (selectorEntry.key() == "(Value)" || !selectorEntry.value().is_object()) continue;

                ic4::Error selectorErr;
                if (!props.setValue(propertyName, selectorEntry.key(), selectorErr)) {
                    outError = MakeError(ErrorCode::IC4Error,
                                         "ApplyJsonStateObject / selector " + propertyName,
                                         selectorErr.isError() ? IC4ErrorMessage(selectorErr) : "Failed to select nested selector entry");
                    if (strict) return false;
                    continue;
                }

                for (const auto& nestedProp : selectorEntry.value().items()) {
                    if (nestedProp.value().is_object()) continue;
                    if (!SetPropertyFromJsonScalar(props, nestedProp.key(), nestedProp.value(), strict, outError) && strict) return false;
                }
            }

            if (hasSelectedValue) {
                ic4::Error restoreErr;
                if (!props.setValue(propertyName, selectedValue, restoreErr)) {
                    outError = MakeError(ErrorCode::IC4Error,
                                         "ApplyJsonStateObject / restore selector " + propertyName,
                                         restoreErr.isError() ? IC4ErrorMessage(restoreErr) : "Failed to restore nested selector value");
                    if (strict) return false;
                }
            }
            continue;
        }

        if (!SetPropertyFromJsonScalar(props, propertyName, value, strict, outError) && strict) return false;
    }
    return true;
}

std::optional<ic4::DeviceInfo> ResolveDevice(const IC4DeviceSelector& selector, ErrorInfo& outError)
{
    ic4::Error err;
    auto devices = ic4::DeviceEnum::enumDevices(err);
    if (err.isError()) {
        outError = MakeError(ErrorCode::IC4Error, "ResolveDevice / enumDevices", IC4ErrorMessage(err));
        return std::nullopt;
    }
    if (devices.empty()) {
        outError = MakeError(ErrorCode::IC4Error, "ResolveDevice", "No IC4 camera devices were found");
        return std::nullopt;
    }

    if (!selector.serial.empty()) {
        for (const auto& d : devices) {
            ic4::Error e;
            if (d.serial(e) == selector.serial && !e.isError()) return d;
        }
        outError = MakeError(ErrorCode::IC4Error, "ResolveDevice / serial", "No device with serial " + selector.serial);
        return std::nullopt;
    }
    if (!selector.uniqueName.empty()) {
        for (const auto& d : devices) {
            ic4::Error e;
            if (d.uniqueName(e) == selector.uniqueName && !e.isError()) return d;
        }
        outError = MakeError(ErrorCode::IC4Error, "ResolveDevice / uniqueName", "No device with uniqueName " + selector.uniqueName);
        return std::nullopt;
    }
    if (selector.deviceIndex >= 0) {
        const auto idx = static_cast<std::size_t>(selector.deviceIndex);
        if (idx >= devices.size()) {
            outError = MakeError(ErrorCode::IC4Error, "ResolveDevice / deviceIndex", "deviceIndex is out of range");
            return std::nullopt;
        }
        return devices[idx];
    }
    return devices.front();
}

struct PendingIC4Frame
{
    std::shared_ptr<ic4::ImageBuffer> buffer;
    FrameTiming timing;
    FrameFormatMetadata format;
    FrameChunkMetadata chunkMetadata;
};

} // namespace

class D3D12CameraCapture::Impl
{
public:
    class Listener : public ic4::QueueSinkListener
    {
    public:
        explicit Listener(Impl* owner) : owner_(owner) {}
        bool sinkConnected(ic4::QueueSink& sink, const ic4::ImageType& imageType, size_t minBuffersRequired) override
        {
            (void)sink;
            (void)minBuffersRequired;
            return owner_ ? owner_->onSinkConnected(imageType) : false;
        }
        void sinkDisconnected(ic4::QueueSink& sink) override { (void)sink; }
        void framesQueued(ic4::QueueSink& sink) override { if (owner_) owner_->onFramesQueued(sink); }
    private:
        Impl* owner_ = nullptr;
    };

    IC4DeviceSelector selector;
    CameraCaptureConfig config;
    D3D12BackendContext backend;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;

    std::shared_ptr<IC4LibraryContext> ic4Context;
    std::unique_ptr<ic4::Grabber> grabber;
    std::shared_ptr<ic4::QueueSink> queueSink;
    std::shared_ptr<Listener> listener;

    std::unique_ptr<D3D12FenceManager> fenceManager;
    std::unique_ptr<D3D12FrameConverter> converter;

    mutable std::mutex pendingMutex;
    std::condition_variable pendingCv;
    std::deque<PendingIC4Frame> pendingFrames;

    mutable std::mutex statsMutex;
    mutable std::mutex controlMutex;
    CameraCaptureStats stats;
    Internal::FrameTimingPerformanceTracker timingTracker;
    ErrorInfo lastError;
    bool connected = false;

    void setError(ErrorCode code, const std::string& where, const std::string& message)
    {
        lastError = MakeError(code, where, message);
    }

    CameraCaptureStats getStats() const
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        return stats;
    }

    void incrementReceived() { std::lock_guard<std::mutex> lock(statsMutex); ++stats.receivedBuffers; }
    void incrementDropped(std::uint64_t n = 1) { std::lock_guard<std::mutex> lock(statsMutex); stats.droppedPendingBuffers += n; }
    void incrementReadFrames() { std::lock_guard<std::mutex> lock(statsMutex); ++stats.readFrames; }
    void incrementReadTimeouts() { std::lock_guard<std::mutex> lock(statsMutex); ++stats.readTimeouts; }
    void incrementConversionFailures() { std::lock_guard<std::mutex> lock(statsMutex); ++stats.conversionFailures; }

    bool onSinkConnected(const ic4::ImageType& imageType)
    {
        const auto actual = FromIC4PixelFormat(imageType.pixel_format(), config.streamRequest.requestedFormat);
        if (actual != config.streamRequest.requestedFormat) {
            setError(ErrorCode::IC4Error, "D3D12CameraCapture::sinkConnected", std::string("Negotiated pixel format does not match requested format. requested=") + ToString(config.streamRequest.requestedFormat));
            return false;
        }
        connected = true;
        return true;
    }

    void onFramesQueued(ic4::QueueSink& sink)
    {
        for (;;) {
            ic4::Error cancelErr;
            if (sink.isCancelRequested(cancelErr)) break;
            ic4::Error err;
            auto buffer = sink.popOutputBuffer(err);
            if (!buffer) break;

            PendingIC4Frame pending;
            pending.buffer = std::move(buffer);
            pending.timing.hostReceivedTime = std::chrono::steady_clock::now();

            ic4::Error metaErr;
            auto md = pending.buffer->metaData(metaErr);
            if (!metaErr.isError()) {
                pending.timing.frameNumber = md.device_frame_number;
                pending.timing.deviceTimestampNs = md.device_timestamp_ns;
            }
            timingTracker.update(pending.timing);

            {
                std::lock_guard<std::mutex> controlLock(controlMutex);
                pending.chunkMetadata = Internal::ReadChunkMetadata(grabber.get(), pending.buffer);
            }

            ic4::Error typeErr;
            const auto& imageType = pending.buffer->imageType(typeErr);
            pending.format.requestedFormat = config.streamRequest.requestedFormat;
            pending.format.actualInputFormat = typeErr.isError() ? config.streamRequest.requestedFormat : FromIC4PixelFormat(imageType.pixel_format(), config.streamRequest.requestedFormat);
            pending.format.outputFormat = config.outputSpec.outputFormat;
            pending.format.width = typeErr.isError() ? config.streamRequest.width : static_cast<int>(imageType.width());
            pending.format.height = typeErr.isError() ? config.streamRequest.height : static_cast<int>(imageType.height());

            ic4::Error pitchErr;
            const auto pitch = pending.buffer->pitch(pitchErr);
            pending.format.inputRowPitchBytes = pitchErr.isError() ? 0u : static_cast<std::size_t>(std::max<ptrdiff_t>(0, pitch));

            {
                std::lock_guard<std::mutex> lock(pendingMutex);
                if (config.queuePolicy == FrameQueuePolicy::LatestOnly) {
                    const auto dropped = pendingFrames.size();
                    pendingFrames.clear();
                    if (dropped) incrementDropped(static_cast<std::uint64_t>(dropped));
                    pendingFrames.push_back(std::move(pending));
                } else {
                    pendingFrames.push_back(std::move(pending));
                    if (config.maxPendingBuffers > 0) {
                        while (pendingFrames.size() > config.maxPendingBuffers) {
                            pendingFrames.pop_front();
                            incrementDropped();
                        }
                    }
                }
            }
            incrementReceived();
            pendingCv.notify_one();
        }
    }

    CameraPerformanceSnapshot performanceSnapshot()
    {
        CameraPerformanceSnapshot snapshot;
        snapshot.sampledTime = std::chrono::steady_clock::now();
        snapshot.captureStats = getStats();
        snapshot.timing = timingTracker.snapshot();
        if (grabber && (*grabber)) {
            snapshot.streamStatistics = Internal::ReadStreamStatistics(grabber.get());
            std::lock_guard<std::mutex> lock(controlMutex);
            snapshot.temperatures = Internal::ReadDeviceTemperatures(grabber.get());
        }
        return snapshot;
    }

    std::optional<ic4::PropertyMap> getPropertyMap(const char* where)
    {
        if (!grabber || !(*grabber)) { setError(ErrorCode::IC4Error, where, "Grabber is not initialized"); return std::nullopt; }
        ic4::Error err;
        ic4::PropertyMap props = grabber->devicePropertyMap(err);
        if (err.isError() || !props) { setError(ErrorCode::IC4Error, where, IC4ErrorMessage(err)); return std::nullopt; }
        return props;
    }

    bool applyJsonStateConfig()
    {
        if (!config.ic4StateJson.enabled()) return true;
        ErrorInfo parseError;
        auto root = LoadJsonFile(config.ic4StateJson.path, parseError);
        if (!root) { lastError = parseError; return false; }
        const Json* state = FindIC4StateObject(*root, config.ic4StateJson.deviceIndex, parseError);
        if (!state) { lastError = parseError; return false; }
        auto props = getPropertyMap("D3D12CameraCapture::applyJsonStateConfig / devicePropertyMap");
        if (!props) return false;
        ErrorInfo applyError;
        const bool ok = ApplyJsonStateObject(*props, *state, config.ic4StateJson.strict, config.ic4StateJson.applyNestedSelectorStates, applyError);
        if (applyError) lastError = applyError;
        return ok;
    }

    template <typename T>
    bool setPropertyValue(const std::string& propertyName, const T& value, const char* where)
    {
        std::lock_guard<std::mutex> lock(controlMutex);
        auto props = getPropertyMap(where);
        if (!props) return false;
        ic4::Error err;
        if (!props->setValue(propertyName, value, err)) { setError(ErrorCode::IC4Error, where, IC4ErrorMessage(err)); return false; }
        return true;
    }

    bool applyExplicitStreamRequestProperties()
    {
        auto props = getPropertyMap("D3D12CameraCapture::open / devicePropertyMap");
        if (!props) return false;
        ic4::Error err;
        const auto ic4Fmt = ToIC4PixelFormat(config.streamRequest.requestedFormat);
        if (ic4Fmt == ic4::PixelFormat::Unspecified) { setError(ErrorCode::UnsupportedFormat, "D3D12CameraCapture::open / PixelFormat", "Unsupported requestedFormat"); return false; }
        if (!props->setValue(ic4::PropId::PixelFormat, ic4Fmt, err)) { setError(ErrorCode::IC4Error, "D3D12CameraCapture::open / PixelFormat", IC4ErrorMessage(err)); return false; }
        if (config.streamRequest.offsetX || config.streamRequest.offsetY) {
            if (!props->setValue(ic4::PropId::OffsetAutoCenter, "Off", err)) { setError(ErrorCode::IC4Error, "D3D12CameraCapture::open / OffsetAutoCenter", IC4ErrorMessage(err)); return false; }
        }
        if (config.streamRequest.width > 0 && !props->setValue(ic4::PropId::Width, config.streamRequest.width, err)) { setError(ErrorCode::IC4Error, "D3D12CameraCapture::open / Width", IC4ErrorMessage(err)); return false; }
        if (config.streamRequest.height > 0 && !props->setValue(ic4::PropId::Height, config.streamRequest.height, err)) { setError(ErrorCode::IC4Error, "D3D12CameraCapture::open / Height", IC4ErrorMessage(err)); return false; }
        if (config.streamRequest.offsetX && !props->setValue(ic4::PropId::OffsetX, *config.streamRequest.offsetX, err)) { setError(ErrorCode::IC4Error, "D3D12CameraCapture::open / OffsetX", IC4ErrorMessage(err)); return false; }
        if (config.streamRequest.offsetY && !props->setValue(ic4::PropId::OffsetY, *config.streamRequest.offsetY, err)) { setError(ErrorCode::IC4Error, "D3D12CameraCapture::open / OffsetY", IC4ErrorMessage(err)); return false; }
        if (config.streamRequest.fps > 0.0 && !props->setValue(ic4::PropId::AcquisitionFrameRate, config.streamRequest.fps, err)) { setError(ErrorCode::IC4Error, "D3D12CameraCapture::open / AcquisitionFrameRate", IC4ErrorMessage(err)); return false; }
        return true;
    }

    bool applyPropertyOverrides()
    {
        if (config.propertyOverrides.empty()) return true;
        auto props = getPropertyMap("D3D12CameraCapture::open / propertyOverrides");
        if (!props) return false;
        for (const auto& ov : config.propertyOverrides) {
            ic4::Error err;
            bool ok = false;
            std::visit([&](const auto& v) { ok = props->setValue(ov.propertyName, v, err); }, ov.value);
            if (!ok) { setError(ErrorCode::IC4Error, "D3D12CameraCapture::open / propertyOverride " + ov.propertyName, err.isError() ? IC4ErrorMessage(err) : "IC4 property setValue returned false"); return false; }
        }
        return true;
    }

    bool configureDeviceProperties()
    {
        std::lock_guard<std::mutex> lock(controlMutex);
        if (!applyJsonStateConfig()) return false;
        if (!applyExplicitStreamRequestProperties()) return false;
        return applyPropertyOverrides();
    }
};

D3D12CameraCapture::D3D12CameraCapture()
    : impl_(std::make_unique<Impl>())
{
}

D3D12CameraCapture::~D3D12CameraCapture()
{
    close();
}

D3D12CameraCapture::D3D12CameraCapture(D3D12CameraCapture&& other) noexcept
{
    moveFrom(std::move(other));
}

D3D12CameraCapture& D3D12CameraCapture::operator=(D3D12CameraCapture&& other) noexcept
{
    if (this != &other) {
        close();
        moveFrom(std::move(other));
    }
    return *this;
}

void D3D12CameraCapture::moveFrom(D3D12CameraCapture&& other) noexcept
{
    impl_ = std::move(other.impl_);
    opened_.store(other.opened_.load());
    other.opened_.store(false);
    if (!other.impl_) other.impl_ = std::make_unique<Impl>();
}

void D3D12CameraCapture::setError(ErrorCode code, const std::string& where, const std::string& message)
{
    lastError_ = MakeError(code, where, message);
    if (impl_) impl_->lastError = lastError_;
}

bool D3D12CameraCapture::open(const IC4DeviceSelector& selector,
                              const CameraCaptureConfig& config,
                              const D3D12BackendContext& backend)
{
    close();
    impl_ = std::make_unique<Impl>();
    impl_->selector = selector;
    lastError_ = NoError();

    ErrorInfo effectiveConfigError;
    CameraCaptureConfig effectiveConfig = BuildEffectiveConfigFromJson(config, effectiveConfigError);
    if (effectiveConfigError) { lastError_ = effectiveConfigError; impl_->lastError = effectiveConfigError; return false; }

    impl_->config = effectiveConfig;
    impl_->backend = backend;

    if (!impl_->backend.resolve() || !impl_->backend.corePtr || !impl_->backend.queue) {
        setError(ErrorCode::InvalidArgument, "D3D12CameraCapture::open", "D3D12 backend must be created from D3D12Helper D3D12Core. Use D3D12BackendContext::FromCore(...).");
        return false;
    }
    if (!IsSupportedConversion(impl_->config.streamRequest.requestedFormat, impl_->config.outputSpec.outputFormat)) { setError(ErrorCode::UnsupportedFormat, "D3D12CameraCapture::open", std::string("Unsupported conversion: ") + ToString(impl_->config.streamRequest.requestedFormat) + " -> " + ToString(impl_->config.outputSpec.outputFormat)); return false; }

    impl_->device = impl_->backend.device;
    impl_->queue = impl_->backend.commandQueue;
    if (!impl_->device || !impl_->queue) { setError(ErrorCode::D3D12Error, "D3D12CameraCapture::open", "D3D12 backend has null resolved device/queue"); return false; }

    impl_->fenceManager = std::make_unique<D3D12FenceManager>();
    if (!impl_->fenceManager->initialize(impl_->backend)) { setError(ErrorCode::D3D12Error, "D3D12CameraCapture::open / D3D12FenceManager", impl_->fenceManager->lastError().message); return false; }

    impl_->converter = std::make_unique<D3D12FrameConverter>();
    if (!impl_->converter->initialize(impl_->backend, impl_->fenceManager.get(), impl_->config.shaderConfig)) { setError(ErrorCode::D3D12Error, "D3D12CameraCapture::open / D3D12FrameConverter", impl_->converter->lastError().message); return false; }

    impl_->ic4Context = SharedIC4Context();
    if (!impl_->ic4Context->initialized()) { setError(ErrorCode::IC4Error, "D3D12CameraCapture::open / initLibrary", "ic4::initLibrary failed"); return false; }

    ErrorInfo resolveError;
    auto dev = ResolveDevice(selector, resolveError);
    if (!dev) { lastError_ = resolveError; impl_->lastError = resolveError; return false; }

    ic4::Error err;
    impl_->grabber = std::make_unique<ic4::Grabber>(err);
    if (err.isError() || !impl_->grabber || !(*impl_->grabber)) { setError(ErrorCode::IC4Error, "D3D12CameraCapture::open / Grabber", IC4ErrorMessage(err)); return false; }
    if (!impl_->grabber->deviceOpen(*dev, err)) { setError(ErrorCode::IC4Error, "D3D12CameraCapture::open / deviceOpen", IC4ErrorMessage(err)); return false; }

    if (!impl_->configureDeviceProperties()) { lastError_ = impl_->lastError; close(); return false; }

    impl_->listener = std::make_shared<Impl::Listener>(impl_.get());
    ic4::QueueSink::Config sinkConfig;
    sinkConfig.acceptedPixelFormats.push_back(ToIC4PixelFormat(impl_->config.streamRequest.requestedFormat));
    sinkConfig.maxOutputBuffers = (impl_->config.queuePolicy == FrameQueuePolicy::LatestOnly) ? 2u : 0u;
    impl_->queueSink = ic4::QueueSink::create(impl_->listener, sinkConfig, err);
    if (err.isError() || !impl_->queueSink) { setError(ErrorCode::IC4Error, "D3D12CameraCapture::open / QueueSink::create", IC4ErrorMessage(err)); close(); return false; }

    if (!impl_->grabber->streamSetup(impl_->queueSink, ic4::StreamSetupOption::AcquisitionStart, err)) { setError(ErrorCode::IC4Error, "D3D12CameraCapture::open / streamSetup", impl_->lastError ? impl_->lastError.message : IC4ErrorMessage(err)); close(); return false; }

    opened_.store(true);
    return true;
}

void D3D12CameraCapture::close() noexcept
{
    opened_.store(false);
    if (!impl_) return;
    try {
        ic4::Error err;
        if (impl_->grabber && (*impl_->grabber) && impl_->grabber->isAcquisitionActive()) impl_->grabber->acquisitionStop(err);
        if (impl_->grabber && (*impl_->grabber) && impl_->grabber->isStreaming()) impl_->grabber->streamStop(err);
        if (impl_->grabber && (*impl_->grabber) && impl_->grabber->isDeviceOpen()) impl_->grabber->deviceClose(err);
    } catch (...) {
    }
    {
        std::lock_guard<std::mutex> lock(impl_->pendingMutex);
        impl_->pendingFrames.clear();
    }
    impl_->pendingCv.notify_all();
    impl_->queueSink.reset();
    impl_->listener.reset();
    impl_->converter.reset();
    impl_->fenceManager.reset();
    impl_->timingTracker.reset();
    impl_->device = nullptr;
    impl_->queue = nullptr;
}

D3D12ReadResult D3D12CameraCapture::read(ReadMode mode)
{
    return read(CameraReadOptions{mode, 1000});
}

D3D12ReadResult D3D12CameraCapture::read(const CameraReadOptions& options)
{
    D3D12ReadResult result;
    if (!opened_.load() || !impl_) { result.error = MakeError(ErrorCode::NotOpened, "D3D12CameraCapture::read", "Capture is not opened"); lastError_ = result.error; return result; }

    PendingIC4Frame pending;
    {
        std::unique_lock<std::mutex> lock(impl_->pendingMutex);
        const bool hasFrame = impl_->pendingCv.wait_for(lock, std::chrono::milliseconds(options.timeoutMs), [&] { return !impl_->pendingFrames.empty() || !opened_.load(); });
        if (!hasFrame || impl_->pendingFrames.empty()) { impl_->incrementReadTimeouts(); result.error = MakeError(ErrorCode::Timeout, "D3D12CameraCapture::read", "Read timed out"); lastError_ = result.error; return result; }
        if (options.mode == ReadMode::LatestFrame) {
            if (impl_->pendingFrames.size() > 1) impl_->incrementDropped(static_cast<std::uint64_t>(impl_->pendingFrames.size() - 1));
            pending = std::move(impl_->pendingFrames.back());
            impl_->pendingFrames.clear();
        } else {
            pending = std::move(impl_->pendingFrames.front());
            impl_->pendingFrames.pop_front();
        }
    }

    ic4::Error ptrErr;
    const auto* data = static_cast<const std::uint8_t*>(pending.buffer->ptr(ptrErr));
    if (ptrErr.isError() || !data) { result.error = MakeError(ErrorCode::IC4Error, "D3D12CameraCapture::read / ImageBuffer::ptr", IC4ErrorMessage(ptrErr)); lastError_ = result.error; return result; }

    ic4::Error sizeErr;
    const auto dataSize = pending.buffer->bufferSize(sizeErr);
    if (sizeErr.isError() || dataSize == 0) { result.error = MakeError(ErrorCode::IC4Error, "D3D12CameraCapture::read / ImageBuffer::bufferSize", IC4ErrorMessage(sizeErr)); lastError_ = result.error; return result; }

    D3D12CpuFrameView view;
    view.data = data;
    view.dataSize = dataSize;
    view.timing = pending.timing;
    view.format = pending.format;

    if (!impl_->converter->convert(view, impl_->config.outputSpec, result.frame)) {
        impl_->incrementConversionFailures();
        result.error = impl_->converter->lastError();
        lastError_ = result.error;
        return result;
    }
    result.frame.chunkMetadata = pending.chunkMetadata;

    impl_->incrementReadFrames();
    result.ok = true;
    return result;
}

bool D3D12CameraCapture::applyIC4StateJson(const std::filesystem::path& jsonPath,
                                           std::size_t deviceIndex,
                                           bool strict,
                                           bool applyNestedSelectorStates)
{
    if (!opened_.load() || !impl_) { setError(ErrorCode::NotOpened, "D3D12CameraCapture::applyIC4StateJson", "Capture is not opened"); return false; }
    std::lock_guard<std::mutex> lock(impl_->controlMutex);
    CameraCaptureConfig updated = impl_->config;
    updated.ic4StateJson.path = jsonPath;
    updated.ic4StateJson.deviceIndex = deviceIndex;
    updated.ic4StateJson.strict = strict;
    updated.ic4StateJson.applyNestedSelectorStates = applyNestedSelectorStates;
    ErrorInfo effectiveError;
    updated = BuildEffectiveConfigFromJson(updated, effectiveError);
    if (effectiveError) { lastError_ = effectiveError; impl_->lastError = effectiveError; return false; }
    impl_->config = updated;
    impl_->lastError = NoError();
    if (!impl_->applyJsonStateConfig()) { lastError_ = impl_->lastError; return false; }
    lastError_ = impl_->lastError;
    return true;
}

bool D3D12CameraCapture::setIC4Property(const std::string& propertyName, bool value)
{
    if (!opened_.load() || !impl_) { setError(ErrorCode::NotOpened, "D3D12CameraCapture::setIC4Property", "Capture is not opened"); return false; }
    const bool ok = impl_->setPropertyValue(propertyName, value, "D3D12CameraCapture::setIC4Property(bool)");
    lastError_ = ok ? NoError() : impl_->lastError;
    return ok;
}

bool D3D12CameraCapture::setIC4Property(const std::string& propertyName, int value) { return setIC4Property(propertyName, static_cast<std::int64_t>(value)); }

bool D3D12CameraCapture::setIC4Property(const std::string& propertyName, std::int64_t value)
{
    if (!opened_.load() || !impl_) { setError(ErrorCode::NotOpened, "D3D12CameraCapture::setIC4Property", "Capture is not opened"); return false; }
    const bool ok = impl_->setPropertyValue(propertyName, value, "D3D12CameraCapture::setIC4Property(int64)");
    lastError_ = ok ? NoError() : impl_->lastError;
    return ok;
}

bool D3D12CameraCapture::setIC4Property(const std::string& propertyName, double value)
{
    if (!opened_.load() || !impl_) { setError(ErrorCode::NotOpened, "D3D12CameraCapture::setIC4Property", "Capture is not opened"); return false; }
    const bool ok = impl_->setPropertyValue(propertyName, value, "D3D12CameraCapture::setIC4Property(double)");
    lastError_ = ok ? NoError() : impl_->lastError;
    return ok;
}

bool D3D12CameraCapture::setIC4Property(const std::string& propertyName, const char* value) { return setIC4Property(propertyName, std::string(value ? value : "")); }

bool D3D12CameraCapture::setIC4Property(const std::string& propertyName, const std::string& value)
{
    if (!opened_.load() || !impl_) { setError(ErrorCode::NotOpened, "D3D12CameraCapture::setIC4Property", "Capture is not opened"); return false; }
    const bool ok = impl_->setPropertyValue(propertyName, value, "D3D12CameraCapture::setIC4Property(string)");
    lastError_ = ok ? NoError() : impl_->lastError;
    return ok;
}

bool D3D12CameraCapture::setFrameRate(double fps)
{
    const bool ok = setIC4Property("AcquisitionFrameRate", fps);
    if (ok && impl_) impl_->config.streamRequest.fps = fps;
    return ok;
}

bool D3D12CameraCapture::setExposureAuto(const std::string& mode) { return setIC4Property("ExposureAuto", mode); }
bool D3D12CameraCapture::setExposureTime(double exposureTimeUs) { return setIC4Property("ExposureTime", exposureTimeUs); }
bool D3D12CameraCapture::setGainAuto(const std::string& mode) { return setIC4Property("GainAuto", mode); }
bool D3D12CameraCapture::setGain(double gain) { return setIC4Property("Gain", gain); }
bool D3D12CameraCapture::setGamma(double gamma) { return setIC4Property("Gamma", gamma); }

bool D3D12CameraCapture::setOffset(int offsetX, int offsetY)
{
    if (!setIC4Property("OffsetAutoCenter", std::string("Off"))) return false;
    if (!setIC4Property("OffsetX", offsetX)) return false;
    if (!setIC4Property("OffsetY", offsetY)) return false;
    if (impl_) { impl_->config.streamRequest.offsetX = offsetX; impl_->config.streamRequest.offsetY = offsetY; }
    return true;
}

bool D3D12CameraCapture::setRoi(int width, int height, int offsetX, int offsetY)
{
    if (!setIC4Property("OffsetAutoCenter", std::string("Off"))) return false;
    if (!setIC4Property("Width", width)) return false;
    if (!setIC4Property("Height", height)) return false;
    if (!setIC4Property("OffsetX", offsetX)) return false;
    if (!setIC4Property("OffsetY", offsetY)) return false;
    if (impl_) { impl_->config.streamRequest.width = width; impl_->config.streamRequest.height = height; impl_->config.streamRequest.offsetX = offsetX; impl_->config.streamRequest.offsetY = offsetY; }
    return true;
}

bool D3D12CameraCapture::setPixelFormat(CameraPixelFormat fmt)
{
    if (!IsSupportedConversion(fmt, impl_ ? impl_->config.outputSpec.outputFormat : GpuFrameFormat::RGBA8)) { setError(ErrorCode::UnsupportedFormat, "D3D12CameraCapture::setPixelFormat", std::string("Unsupported conversion: ") + ToString(fmt)); return false; }
    const bool ok = setIC4Property("PixelFormat", std::string(ToString(fmt)));
    if (ok && impl_) impl_->config.streamRequest.requestedFormat = fmt;
    return ok;
}

CameraCaptureStats D3D12CameraCapture::stats() const
{
    if (!impl_) return {};
    return impl_->getStats();
}

CameraPerformanceSnapshot D3D12CameraCapture::performance()
{
    if (!impl_) return {};
    return impl_->performanceSnapshot();
}

} // namespace IC4Ext

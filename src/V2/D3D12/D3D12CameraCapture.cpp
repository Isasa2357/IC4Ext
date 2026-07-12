#include "IC4Ext/V2/D3D12/D3D12CameraCapture.hpp"

#include "IC4Ext/Core/IC4ChunkMetadata.hpp"
#include "IC4Ext/D3D12/D3D12FenceManager.hpp"
#include "IC4Ext/D3D12/D3D12FrameConverter.hpp"
#include "IC4Ext/V2/D3D12/D3D12PooledFrameConverter.hpp"
#include "../../Core/IC4PerformanceUtil.hpp"

#include <ic4/ic4.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>

namespace IC4Ext::V2 {
namespace {

class IC4LibraryContext
{
public:
    IC4LibraryContext()
    {
        ic4::InitLibraryConfig config;
        config.defaultErrorHandlerBehavior = ic4::ErrorHandlerBehavior::Ignore;
        initialized_ = ic4::initLibrary(config);
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
    static std::mutex mutex;
    static std::weak_ptr<IC4LibraryContext> weak;

    std::lock_guard<std::mutex> lock(mutex);
    auto context = weak.lock();
    if (!context) {
        context = std::make_shared<IC4LibraryContext>();
        weak = context;
    }
    return context;
}

std::string IC4ErrorMessage(const ic4::Error& error)
{
    std::ostringstream stream;
    stream << "IC4 error code=" << static_cast<int>(error.code())
           << ": " << error.message();
    return stream.str();
}

ic4::PixelFormat ToIC4PixelFormat(CameraPixelFormat format)
{
    switch (format) {
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

CameraPixelFormat FromIC4PixelFormat(ic4::PixelFormat format,
                                      CameraPixelFormat fallback)
{
    switch (format) {
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

DXGI_FORMAT ToDxgiFormat(GpuFrameFormat format) noexcept
{
    switch (format) {
    case GpuFrameFormat::R8: return DXGI_FORMAT_R8_UNORM;
    case GpuFrameFormat::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

using Json = nlohmann::json;

std::optional<Json> LoadJsonFile(const std::filesystem::path& path,
                                 ErrorInfo& outError)
{
    try {
        std::ifstream stream(path);
        if (!stream) {
            outError = MakeError(
                ErrorCode::InvalidArgument,
                "V2::LoadJsonFile",
                "Could not open JSON file: " + path.string());
            return std::nullopt;
        }

        Json root;
        stream >> root;
        return root;
    } catch (const std::exception& exception) {
        outError = MakeError(
            ErrorCode::InvalidArgument,
            "V2::LoadJsonFile",
            std::string("JSON parse error: ") + exception.what());
        return std::nullopt;
    }
}

const Json* FindIC4StateObject(const Json& root,
                               std::size_t deviceIndex,
                               ErrorInfo& outError)
{
    if (!root.is_object() || !root.contains("devices") ||
        !root.at("devices").is_array()) {
        outError = MakeError(
            ErrorCode::InvalidArgument,
            "V2::FindIC4StateObject",
            "JSON does not contain devices[]");
        return nullptr;
    }

    const auto& devices = root.at("devices");
    if (deviceIndex >= devices.size()) {
        outError = MakeError(
            ErrorCode::InvalidArgument,
            "V2::FindIC4StateObject",
            "deviceIndex is out of range in JSON devices[]");
        return nullptr;
    }

    const auto& device = devices.at(deviceIndex);
    if (!device.is_object() || !device.contains("state") ||
        !device.at("state").is_object()) {
        outError = MakeError(
            ErrorCode::InvalidArgument,
            "V2::FindIC4StateObject",
            "JSON device entry does not contain a state object");
        return nullptr;
    }
    return &device.at("state");
}

bool TryGetJsonInt(const Json& state, const char* name, int& out)
{
    if (!state.is_object() || !state.contains(name)) return false;
    const auto& value = state.at(name);
    if (!value.is_number_integer() && !value.is_number_unsigned()) return false;
    out = value.get<int>();
    return true;
}

bool TryGetJsonDouble(const Json& state, const char* name, double& out)
{
    if (!state.is_object() || !state.contains(name)) return false;
    const auto& value = state.at(name);
    if (!value.is_number()) return false;
    out = value.get<double>();
    return true;
}

bool TryGetJsonPixelFormat(const Json& state, CameraPixelFormat& out)
{
    if (!state.is_object() || !state.contains("PixelFormat") ||
        !state.at("PixelFormat").is_string()) {
        return false;
    }
    return ParseCameraPixelFormat(state.at("PixelFormat").get<std::string>(), out);
}

CameraCaptureConfig BuildEffectiveConfigFromJson(CameraCaptureConfig config,
                                                  ErrorInfo& outError)
{
    if (!config.ic4StateJson.enabled()) return config;

    auto root = LoadJsonFile(config.ic4StateJson.path, outError);
    if (!root) return config;

    const Json* state = FindIC4StateObject(
        *root,
        config.ic4StateJson.deviceIndex,
        outError);
    if (!state) return config;

    if (!config.streamRequest.forceRequestedFormat) {
        CameraPixelFormat format{};
        if (TryGetJsonPixelFormat(*state, format)) {
            config.streamRequest.requestedFormat = format;
        }
    }

    int integerValue = 0;
    if (config.streamRequest.width <= 0 &&
        TryGetJsonInt(*state, "Width", integerValue)) {
        config.streamRequest.width = integerValue;
    }
    if (config.streamRequest.height <= 0 &&
        TryGetJsonInt(*state, "Height", integerValue)) {
        config.streamRequest.height = integerValue;
    }

    double doubleValue = 0.0;
    if (config.streamRequest.fps <= 0.0 &&
        TryGetJsonDouble(*state, "AcquisitionFrameRate", doubleValue)) {
        config.streamRequest.fps = doubleValue;
    }
    return config;
}

bool SetPropertyFromJsonScalar(ic4::PropertyMap& properties,
                               const std::string& propertyName,
                               const Json& value,
                               bool strict,
                               ErrorInfo& outError)
{
    ic4::Error error;
    bool ok = true;
    try {
        if (value.is_boolean()) {
            ok = properties.setValue(propertyName, value.get<bool>(), error);
        } else if (value.is_number_integer()) {
            ok = properties.setValue(propertyName, value.get<std::int64_t>(), error);
        } else if (value.is_number_unsigned()) {
            ok = properties.setValue(
                propertyName,
                static_cast<std::int64_t>(value.get<std::uint64_t>()),
                error);
        } else if (value.is_number_float()) {
            ok = properties.setValue(propertyName, value.get<double>(), error);
        } else if (value.is_string()) {
            ok = properties.setValue(propertyName, value.get<std::string>(), error);
        } else {
            return true;
        }
    } catch (const std::exception& exception) {
        ok = false;
        outError = MakeError(
            ErrorCode::IC4Error,
            "V2::SetPropertyFromJsonScalar",
            exception.what());
    }

    if (!ok) {
        outError = MakeError(
            ErrorCode::IC4Error,
            "V2::SetPropertyFromJsonScalar / " + propertyName,
            error.isError() ? IC4ErrorMessage(error)
                            : "IC4 property setValue returned false");
        return !strict;
    }
    return true;
}

bool ApplyJsonStateObject(ic4::PropertyMap& properties,
                          const Json& state,
                          bool strict,
                          bool applyNestedSelectorStates,
                          ErrorInfo& outError)
{
    if (!state.is_object()) {
        outError = MakeError(
            ErrorCode::InvalidArgument,
            "V2::ApplyJsonStateObject",
            "state is not a JSON object");
        return false;
    }

    for (const auto& item : state.items()) {
        const std::string& propertyName = item.key();
        const Json& value = item.value();

        if (value.is_object()) {
            if (!applyNestedSelectorStates) continue;

            const bool hasSelectedValue =
                value.contains("(Value)") && value.at("(Value)").is_string();
            const std::string selectedValue = hasSelectedValue
                ? value.at("(Value)").get<std::string>()
                : std::string{};

            for (const auto& selectorEntry : value.items()) {
                if (selectorEntry.key() == "(Value)" ||
                    !selectorEntry.value().is_object()) {
                    continue;
                }

                ic4::Error selectorError;
                if (!properties.setValue(
                        propertyName,
                        selectorEntry.key(),
                        selectorError)) {
                    outError = MakeError(
                        ErrorCode::IC4Error,
                        "V2::ApplyJsonStateObject / selector " + propertyName,
                        selectorError.isError()
                            ? IC4ErrorMessage(selectorError)
                            : "Failed to select nested selector entry");
                    if (strict) return false;
                    continue;
                }

                for (const auto& nested : selectorEntry.value().items()) {
                    if (nested.value().is_object()) continue;
                    if (!SetPropertyFromJsonScalar(
                            properties,
                            nested.key(),
                            nested.value(),
                            strict,
                            outError) && strict) {
                        return false;
                    }
                }
            }

            if (hasSelectedValue) {
                ic4::Error restoreError;
                if (!properties.setValue(
                        propertyName,
                        selectedValue,
                        restoreError)) {
                    outError = MakeError(
                        ErrorCode::IC4Error,
                        "V2::ApplyJsonStateObject / restore selector " + propertyName,
                        restoreError.isError()
                            ? IC4ErrorMessage(restoreError)
                            : "Failed to restore nested selector value");
                    if (strict) return false;
                }
            }
            continue;
        }

        if (!SetPropertyFromJsonScalar(
                properties,
                propertyName,
                value,
                strict,
                outError) && strict) {
            return false;
        }
    }
    return true;
}

std::optional<ic4::DeviceInfo> ResolveDevice(const IC4DeviceSelector& selector,
                                             ErrorInfo& outError)
{
    ic4::Error error;
    auto devices = ic4::DeviceEnum::enumDevices(error);
    if (error.isError()) {
        outError = MakeError(
            ErrorCode::IC4Error,
            "V2::ResolveDevice / enumDevices",
            IC4ErrorMessage(error));
        return std::nullopt;
    }
    if (devices.empty()) {
        outError = MakeError(
            ErrorCode::IC4Error,
            "V2::ResolveDevice",
            "No IC4 camera devices were found");
        return std::nullopt;
    }

    if (!selector.serial.empty()) {
        for (const auto& device : devices) {
            ic4::Error deviceError;
            if (device.serial(deviceError) == selector.serial &&
                !deviceError.isError()) {
                return device;
            }
        }
        outError = MakeError(
            ErrorCode::IC4Error,
            "V2::ResolveDevice / serial",
            "No device with serial " + selector.serial);
        return std::nullopt;
    }

    if (!selector.uniqueName.empty()) {
        for (const auto& device : devices) {
            ic4::Error deviceError;
            if (device.uniqueName(deviceError) == selector.uniqueName &&
                !deviceError.isError()) {
                return device;
            }
        }
        outError = MakeError(
            ErrorCode::IC4Error,
            "V2::ResolveDevice / uniqueName",
            "No device with uniqueName " + selector.uniqueName);
        return std::nullopt;
    }

    if (selector.deviceIndex >= 0) {
        const auto index = static_cast<std::size_t>(selector.deviceIndex);
        if (index >= devices.size()) {
            outError = MakeError(
                ErrorCode::IC4Error,
                "V2::ResolveDevice / deviceIndex",
                "deviceIndex is out of range");
            return std::nullopt;
        }
        return devices[index];
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
    class Listener final : public ic4::QueueSinkListener
    {
    public:
        explicit Listener(Impl* owner) : owner_(owner) {}

        bool sinkConnected(ic4::QueueSink& sink,
                           const ic4::ImageType& imageType,
                           size_t minBuffersRequired) override
        {
            (void)sink;
            (void)minBuffersRequired;
            return owner_ ? owner_->onSinkConnected(imageType) : false;
        }

        void sinkDisconnected(ic4::QueueSink& sink) override
        {
            (void)sink;
        }

        void framesQueued(ic4::QueueSink& sink) override
        {
            if (owner_) owner_->onFramesQueued(sink);
        }

    private:
        Impl* owner_ = nullptr;
    };

    IC4DeviceSelector selector;
    CameraCaptureConfig config;
    D3D12CameraCaptureOptions options;
    D3D12BackendContext backend;

    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* commandQueue = nullptr;

    std::shared_ptr<IC4LibraryContext> ic4Context;
    std::unique_ptr<ic4::Grabber> grabber;
    std::shared_ptr<ic4::QueueSink> queueSink;
    std::shared_ptr<Listener> listener;

    std::unique_ptr<IC4Ext::D3D12FenceManager> fenceManager;
    std::unique_ptr<IC4Ext::D3D12FrameConverter> converter;
    std::unique_ptr<D3D12PooledFrameConverter> pooledConverter;
    std::unique_ptr<D3D12FramePool> framePool;

    mutable std::mutex pendingMutex;
    std::condition_variable pendingCv;
    std::deque<PendingIC4Frame> pendingFrames;

    mutable std::mutex statsMutex;
    mutable std::mutex errorMutex;
    mutable std::mutex controlMutex;
    mutable std::mutex lifecycleMutex;
    mutable std::mutex conversionMutex;

    CameraCaptureStats captureStats;
    Internal::FrameTimingPerformanceTracker timingTracker;
    ErrorInfo error;
    bool connected = false;

    void setError(ErrorCode code,
                  const std::string& where,
                  const std::string& message)
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        error = MakeError(code, where, message);
    }

    void setError(ErrorInfo value)
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        error = std::move(value);
    }

    void clearError()
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        error = NoError();
    }

    ErrorInfo getError() const
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        return error;
    }

    CameraCaptureStats getStats() const
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        return captureStats;
    }

    void incrementReceived()
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        ++captureStats.receivedBuffers;
    }

    void incrementDropped(std::uint64_t count = 1)
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        captureStats.droppedPendingBuffers += count;
    }

    void incrementReadFrames()
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        ++captureStats.readFrames;
    }

    void incrementReadTimeouts()
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        ++captureStats.readTimeouts;
    }

    void incrementConversionFailures()
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        ++captureStats.conversionFailures;
    }

    bool onSinkConnected(const ic4::ImageType& imageType)
    {
        const auto actual = FromIC4PixelFormat(
            imageType.pixel_format(),
            config.streamRequest.requestedFormat);
        if (actual != config.streamRequest.requestedFormat) {
            setError(
                ErrorCode::IC4Error,
                "V2::D3D12CameraCapture::sinkConnected",
                std::string("Negotiated pixel format does not match requested format. requested=") +
                    ToString(config.streamRequest.requestedFormat));
            return false;
        }
        connected = true;
        return true;
    }

    void onFramesQueued(ic4::QueueSink& sink)
    {
        for (;;) {
            ic4::Error cancelError;
            if (sink.isCancelRequested(cancelError)) break;

            ic4::Error errorValue;
            auto buffer = sink.popOutputBuffer(errorValue);
            if (!buffer) break;

            PendingIC4Frame pending;
            pending.buffer = std::move(buffer);
            pending.timing.hostReceivedTime = std::chrono::steady_clock::now();

            ic4::Error metadataError;
            const auto metadata = pending.buffer->metaData(metadataError);
            if (!metadataError.isError()) {
                pending.timing.frameNumber = metadata.device_frame_number;
                pending.timing.deviceTimestampNs = metadata.device_timestamp_ns;
            }
            timingTracker.update(pending.timing);

            {
                std::lock_guard<std::mutex> controlLock(controlMutex);
                pending.chunkMetadata = Internal::ReadChunkMetadata(
                    grabber.get(),
                    pending.buffer);
            }

            ic4::Error imageTypeError;
            const auto& imageType = pending.buffer->imageType(imageTypeError);
            pending.format.requestedFormat = config.streamRequest.requestedFormat;
            pending.format.actualInputFormat = imageTypeError.isError()
                ? config.streamRequest.requestedFormat
                : FromIC4PixelFormat(
                    imageType.pixel_format(),
                    config.streamRequest.requestedFormat);
            pending.format.outputFormat = config.outputSpec.outputFormat;
            pending.format.width = imageTypeError.isError()
                ? config.streamRequest.width
                : static_cast<int>(imageType.width());
            pending.format.height = imageTypeError.isError()
                ? config.streamRequest.height
                : static_cast<int>(imageType.height());

            ic4::Error pitchError;
            const auto pitch = pending.buffer->pitch(pitchError);
            pending.format.inputRowPitchBytes = pitchError.isError()
                ? 0u
                : static_cast<std::size_t>(std::max<ptrdiff_t>(0, pitch));

            {
                std::lock_guard<std::mutex> lock(pendingMutex);
                if (config.queuePolicy == FrameQueuePolicy::LatestOnly) {
                    const auto dropped = pendingFrames.size();
                    pendingFrames.clear();
                    if (dropped != 0) {
                        incrementDropped(static_cast<std::uint64_t>(dropped));
                    }
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

    std::optional<ic4::PropertyMap> getPropertyMap(const char* where)
    {
        if (!grabber || !(*grabber)) {
            setError(ErrorCode::IC4Error, where, "Grabber is not initialized");
            return std::nullopt;
        }

        ic4::Error propertyError;
        auto properties = grabber->devicePropertyMap(propertyError);
        if (propertyError.isError() || !properties) {
            setError(ErrorCode::IC4Error, where, IC4ErrorMessage(propertyError));
            return std::nullopt;
        }
        return properties;
    }

    bool applyJsonStateConfig()
    {
        if (!config.ic4StateJson.enabled()) return true;

        ErrorInfo parseError;
        auto root = LoadJsonFile(config.ic4StateJson.path, parseError);
        if (!root) {
            setError(parseError);
            return false;
        }

        const Json* state = FindIC4StateObject(
            *root,
            config.ic4StateJson.deviceIndex,
            parseError);
        if (!state) {
            setError(parseError);
            return false;
        }

        auto properties = getPropertyMap(
            "V2::D3D12CameraCapture::applyJsonStateConfig / devicePropertyMap");
        if (!properties) return false;

        ErrorInfo applyError;
        const bool ok = ApplyJsonStateObject(
            *properties,
            *state,
            config.ic4StateJson.strict,
            config.ic4StateJson.applyNestedSelectorStates,
            applyError);
        if (applyError) setError(applyError);
        return ok;
    }

    bool applyExplicitStreamRequestProperties()
    {
        auto properties = getPropertyMap(
            "V2::D3D12CameraCapture::open / devicePropertyMap");
        if (!properties) return false;

        ic4::Error propertyError;
        const auto ic4Format = ToIC4PixelFormat(
            config.streamRequest.requestedFormat);
        if (ic4Format == ic4::PixelFormat::Unspecified) {
            setError(
                ErrorCode::UnsupportedFormat,
                "V2::D3D12CameraCapture::open / PixelFormat",
                "Unsupported requestedFormat");
            return false;
        }

        if (!properties->setValue(ic4::PropId::PixelFormat, ic4Format, propertyError)) {
            setError(
                ErrorCode::IC4Error,
                "V2::D3D12CameraCapture::open / PixelFormat",
                IC4ErrorMessage(propertyError));
            return false;
        }

        if (config.streamRequest.offsetX || config.streamRequest.offsetY) {
            if (!properties->setValue(
                    ic4::PropId::OffsetAutoCenter,
                    "Off",
                    propertyError)) {
                setError(
                    ErrorCode::IC4Error,
                    "V2::D3D12CameraCapture::open / OffsetAutoCenter",
                    IC4ErrorMessage(propertyError));
                return false;
            }
        }

        if (config.streamRequest.width > 0 &&
            !properties->setValue(
                ic4::PropId::Width,
                config.streamRequest.width,
                propertyError)) {
            setError(
                ErrorCode::IC4Error,
                "V2::D3D12CameraCapture::open / Width",
                IC4ErrorMessage(propertyError));
            return false;
        }

        if (config.streamRequest.height > 0 &&
            !properties->setValue(
                ic4::PropId::Height,
                config.streamRequest.height,
                propertyError)) {
            setError(
                ErrorCode::IC4Error,
                "V2::D3D12CameraCapture::open / Height",
                IC4ErrorMessage(propertyError));
            return false;
        }

        if (config.streamRequest.offsetX &&
            !properties->setValue(
                ic4::PropId::OffsetX,
                *config.streamRequest.offsetX,
                propertyError)) {
            setError(
                ErrorCode::IC4Error,
                "V2::D3D12CameraCapture::open / OffsetX",
                IC4ErrorMessage(propertyError));
            return false;
        }

        if (config.streamRequest.offsetY &&
            !properties->setValue(
                ic4::PropId::OffsetY,
                *config.streamRequest.offsetY,
                propertyError)) {
            setError(
                ErrorCode::IC4Error,
                "V2::D3D12CameraCapture::open / OffsetY",
                IC4ErrorMessage(propertyError));
            return false;
        }

        if (config.streamRequest.fps > 0.0 &&
            !properties->setValue(
                ic4::PropId::AcquisitionFrameRate,
                config.streamRequest.fps,
                propertyError)) {
            setError(
                ErrorCode::IC4Error,
                "V2::D3D12CameraCapture::open / AcquisitionFrameRate",
                IC4ErrorMessage(propertyError));
            return false;
        }
        return true;
    }

    bool applyPropertyOverrides()
    {
        if (config.propertyOverrides.empty()) return true;

        auto properties = getPropertyMap(
            "V2::D3D12CameraCapture::open / propertyOverrides");
        if (!properties) return false;

        for (const auto& overrideValue : config.propertyOverrides) {
            ic4::Error propertyError;
            bool ok = false;
            std::visit(
                [&](const auto& value) {
                    ok = properties->setValue(
                        overrideValue.propertyName,
                        value,
                        propertyError);
                },
                overrideValue.value);

            if (!ok) {
                setError(
                    ErrorCode::IC4Error,
                    "V2::D3D12CameraCapture::open / propertyOverride " +
                        overrideValue.propertyName,
                    propertyError.isError()
                        ? IC4ErrorMessage(propertyError)
                        : "IC4 property setValue returned false");
                return false;
            }
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

    template <typename T>
    bool setPropertyValue(const std::string& propertyName,
                          const T& value,
                          const char* where)
    {
        std::lock_guard<std::mutex> lock(controlMutex);
        auto properties = getPropertyMap(where);
        if (!properties) return false;

        ic4::Error propertyError;
        if (!properties->setValue(propertyName, value, propertyError)) {
            setError(ErrorCode::IC4Error, where, IC4ErrorMessage(propertyError));
            return false;
        }
        clearError();
        return true;
    }

    bool ensureFramePool(const FrameFormatMetadata& format)
    {
        const DXGI_FORMAT outputFormat = ToDxgiFormat(config.outputSpec.outputFormat);
        if (format.width <= 0 || format.height <= 0 ||
            outputFormat == DXGI_FORMAT_UNKNOWN) {
            setError(
                ErrorCode::InvalidArgument,
                "V2::D3D12CameraCapture::ensureFramePool",
                "Invalid negotiated frame shape or output format");
            return false;
        }

        if (framePool && framePool->isInitialized()) {
            const auto& current = framePool->config();
            if (current.width == static_cast<std::uint32_t>(format.width) &&
                current.height == static_cast<std::uint32_t>(format.height) &&
                current.format == outputFormat) {
                return true;
            }
        }

        D3D12FramePoolConfig poolConfig;
        poolConfig.width = static_cast<std::uint32_t>(format.width);
        poolConfig.height = static_cast<std::uint32_t>(format.height);
        poolConfig.format = outputFormat;
        poolConfig.resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        poolConfig.writeState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        poolConfig.publishedState = D3D12_RESOURCE_STATE_GENERIC_READ;
        poolConfig.createSrv = true;
        poolConfig.createUav = true;
        poolConfig.initialCapacity = options.initialFramePoolCapacity;
        poolConfig.maxCapacity = options.maxFramePoolCapacity;
        poolConfig.exhaustionPolicy = options.framePoolExhaustionPolicy;
        poolConfig.waitTimeout = options.framePoolWaitTimeout;

        auto nextPool = std::make_unique<D3D12FramePool>();
        if (!nextPool->initialize(backend, poolConfig)) {
            setError(nextPool->lastError());
            return false;
        }

        // Existing published frames remain valid because each frame retains the
        // old pool state independently. Only future acquisitions use nextPool.
        framePool = std::move(nextPool);
        return true;
    }

    bool buildCpuView(PendingIC4Frame& pending,
                      IC4Ext::D3D12CpuFrameView& view,
                      ErrorInfo& outError)
    {
        ic4::Error pointerError;
        const auto* data = static_cast<const std::uint8_t*>(
            pending.buffer->ptr(pointerError));
        if (pointerError.isError() || !data) {
            outError = MakeError(
                ErrorCode::IC4Error,
                "V2::D3D12CameraCapture::read / ImageBuffer::ptr",
                IC4ErrorMessage(pointerError));
            return false;
        }

        ic4::Error sizeError;
        const auto dataSize = pending.buffer->bufferSize(sizeError);
        if (sizeError.isError() || dataSize == 0) {
            outError = MakeError(
                ErrorCode::IC4Error,
                "V2::D3D12CameraCapture::read / ImageBuffer::bufferSize",
                IC4ErrorMessage(sizeError));
            return false;
        }

        view.data = data;
        view.dataSize = dataSize;
        view.timing = pending.timing;
        view.format = pending.format;
        return true;
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

D3D12CameraCapture& D3D12CameraCapture::operator=(
    D3D12CameraCapture&& other) noexcept
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

bool D3D12CameraCapture::open(const IC4DeviceSelector& selector,
                              const CameraCaptureConfig& config,
                              D3D12BackendContext backend,
                              D3D12CameraCaptureOptions options)
{
    close();
    impl_ = std::make_unique<Impl>();
    impl_->selector = selector;
    impl_->options = options;

    if (!options.isValid()) {
        impl_->setError(
            ErrorCode::InvalidArgument,
            "V2::D3D12CameraCapture::open",
            "Invalid frame-pool options");
        return false;
    }

    ErrorInfo effectiveError;
    auto effectiveConfig = BuildEffectiveConfigFromJson(config, effectiveError);
    if (effectiveError) {
        impl_->setError(effectiveError);
        return false;
    }

    // Read-only v2 publication always requires an SRV, and the current camera
    // conversion path writes through a UAV.
    effectiveConfig.outputSpec.createSrv = true;
    effectiveConfig.outputSpec.createUav = true;
    impl_->config = std::move(effectiveConfig);
    impl_->backend = std::move(backend);

    if (!impl_->backend.resolve() || !impl_->backend.corePtr ||
        !impl_->backend.queue) {
        impl_->setError(
            ErrorCode::InvalidArgument,
            "V2::D3D12CameraCapture::open",
            "D3D12 backend must be created from D3D12Helper D3D12Core");
        return false;
    }

    if (!IsSupportedConversion(
            impl_->config.streamRequest.requestedFormat,
            impl_->config.outputSpec.outputFormat)) {
        impl_->setError(
            ErrorCode::UnsupportedFormat,
            "V2::D3D12CameraCapture::open",
            std::string("Unsupported conversion: ") +
                ToString(impl_->config.streamRequest.requestedFormat) +
                " -> " + ToString(impl_->config.outputSpec.outputFormat));
        return false;
    }

    impl_->device = impl_->backend.device;
    impl_->commandQueue = impl_->backend.commandQueue;
    if (!impl_->device || !impl_->commandQueue) {
        impl_->setError(
            ErrorCode::D3D12Error,
            "V2::D3D12CameraCapture::open",
            "Resolved D3D12 device or command queue is null");
        return false;
    }

    impl_->fenceManager = std::make_unique<IC4Ext::D3D12FenceManager>();
    if (!impl_->fenceManager->initialize(impl_->backend)) {
        impl_->setError(impl_->fenceManager->lastError());
        return false;
    }

    impl_->converter = std::make_unique<IC4Ext::D3D12FrameConverter>();
    if (!impl_->converter->initialize(
            impl_->backend,
            impl_->fenceManager.get(),
            impl_->config.shaderConfig)) {
        impl_->setError(impl_->converter->lastError());
        return false;
    }

    impl_->pooledConverter = std::make_unique<D3D12PooledFrameConverter>();
    if (!impl_->pooledConverter->initialize(*impl_->converter)) {
        impl_->setError(impl_->pooledConverter->lastError());
        return false;
    }

    impl_->ic4Context = SharedIC4Context();
    if (!impl_->ic4Context->initialized()) {
        impl_->setError(
            ErrorCode::IC4Error,
            "V2::D3D12CameraCapture::open / initLibrary",
            "ic4::initLibrary failed");
        return false;
    }

    ErrorInfo resolveError;
    auto deviceInfo = ResolveDevice(selector, resolveError);
    if (!deviceInfo) {
        impl_->setError(resolveError);
        return false;
    }

    ic4::Error ic4Error;
    impl_->grabber = std::make_unique<ic4::Grabber>(ic4Error);
    if (ic4Error.isError() || !impl_->grabber || !(*impl_->grabber)) {
        impl_->setError(
            ErrorCode::IC4Error,
            "V2::D3D12CameraCapture::open / Grabber",
            IC4ErrorMessage(ic4Error));
        return false;
    }

    if (!impl_->grabber->deviceOpen(*deviceInfo, ic4Error)) {
        impl_->setError(
            ErrorCode::IC4Error,
            "V2::D3D12CameraCapture::open / deviceOpen",
            IC4ErrorMessage(ic4Error));
        return false;
    }

    if (!impl_->configureDeviceProperties()) {
        close();
        return false;
    }

    impl_->listener = std::make_shared<Impl::Listener>(impl_.get());
    ic4::QueueSink::Config sinkConfig;
    sinkConfig.acceptedPixelFormats.push_back(
        ToIC4PixelFormat(impl_->config.streamRequest.requestedFormat));
    sinkConfig.maxOutputBuffers =
        impl_->config.queuePolicy == FrameQueuePolicy::LatestOnly ? 2u : 0u;

    impl_->queueSink = ic4::QueueSink::create(
        impl_->listener,
        sinkConfig,
        ic4Error);
    if (ic4Error.isError() || !impl_->queueSink) {
        impl_->setError(
            ErrorCode::IC4Error,
            "V2::D3D12CameraCapture::open / QueueSink::create",
            IC4ErrorMessage(ic4Error));
        close();
        return false;
    }

    const auto setupOption =
        impl_->config.acquisitionStartMode == AcquisitionStartMode::Deferred
            ? ic4::StreamSetupOption::DeferAcquisitionStart
            : ic4::StreamSetupOption::AcquisitionStart;

    if (!impl_->grabber->streamSetup(
            impl_->queueSink,
            setupOption,
            ic4Error)) {
        const auto existingError = impl_->getError();
        impl_->setError(
            ErrorCode::IC4Error,
            "V2::D3D12CameraCapture::open / streamSetup",
            existingError ? existingError.message : IC4ErrorMessage(ic4Error));
        close();
        return false;
    }

    opened_.store(true);
    impl_->clearError();
    return true;
}

void D3D12CameraCapture::close() noexcept
{
    opened_.store(false);
    if (!impl_) return;

    try {
        std::lock_guard<std::mutex> lifecycleLock(impl_->lifecycleMutex);
        ic4::Error error;
        if (impl_->grabber && (*impl_->grabber) &&
            impl_->grabber->isAcquisitionActive()) {
            impl_->grabber->acquisitionStop(error);
        }
        if (impl_->grabber && (*impl_->grabber) &&
            impl_->grabber->isStreaming()) {
            impl_->grabber->streamStop(error);
        }
        if (impl_->grabber && (*impl_->grabber) &&
            impl_->grabber->isDeviceOpen()) {
            impl_->grabber->deviceClose(error);
        }
    } catch (...) {
    }

    {
        std::lock_guard<std::mutex> lock(impl_->pendingMutex);
        impl_->pendingFrames.clear();
    }
    impl_->pendingCv.notify_all();

    try {
        if (impl_->backend.queue) {
            impl_->backend.queue->WaitIdle();
        }
    } catch (...) {
    }

    impl_->queueSink.reset();
    impl_->listener.reset();
    impl_->framePool.reset();
    impl_->pooledConverter.reset();
    impl_->converter.reset();
    impl_->fenceManager.reset();
    impl_->grabber.reset();
    impl_->ic4Context.reset();
    impl_->timingTracker.reset();
    impl_->device = nullptr;
    impl_->commandQueue = nullptr;
    impl_->connected = false;
}

bool D3D12CameraCapture::startAcquisition()
{
    if (!opened_.load() || !impl_) {
        if (impl_) {
            impl_->setError(
                ErrorCode::NotOpened,
                "V2::D3D12CameraCapture::startAcquisition",
                "Capture is not opened");
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(impl_->lifecycleMutex);
    if (!impl_->grabber || !(*impl_->grabber) ||
        !impl_->grabber->isStreaming()) {
        impl_->setError(
            ErrorCode::IC4Error,
            "V2::D3D12CameraCapture::startAcquisition",
            "IC4 stream is not configured");
        return false;
    }
    if (impl_->grabber->isAcquisitionActive()) {
        impl_->clearError();
        return true;
    }

    ic4::Error error;
    if (!impl_->grabber->acquisitionStart(error)) {
        impl_->setError(
            ErrorCode::IC4Error,
            "V2::D3D12CameraCapture::startAcquisition",
            IC4ErrorMessage(error));
        return false;
    }
    impl_->clearError();
    return true;
}

bool D3D12CameraCapture::stopAcquisition()
{
    if (!opened_.load() || !impl_) {
        if (impl_) {
            impl_->setError(
                ErrorCode::NotOpened,
                "V2::D3D12CameraCapture::stopAcquisition",
                "Capture is not opened");
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(impl_->lifecycleMutex);
    if (!impl_->grabber || !(*impl_->grabber) ||
        !impl_->grabber->isStreaming()) {
        impl_->setError(
            ErrorCode::IC4Error,
            "V2::D3D12CameraCapture::stopAcquisition",
            "IC4 stream is not configured");
        return false;
    }
    if (!impl_->grabber->isAcquisitionActive()) {
        impl_->clearError();
        return true;
    }

    ic4::Error error;
    if (!impl_->grabber->acquisitionStop(error)) {
        impl_->setError(
            ErrorCode::IC4Error,
            "V2::D3D12CameraCapture::stopAcquisition",
            IC4ErrorMessage(error));
        return false;
    }
    impl_->clearError();
    return true;
}

bool D3D12CameraCapture::isStreaming() const noexcept
{
    if (!opened_.load() || !impl_) return false;
    try {
        std::lock_guard<std::mutex> lock(impl_->lifecycleMutex);
        return impl_->grabber && (*impl_->grabber) &&
               impl_->grabber->isStreaming();
    } catch (...) {
        return false;
    }
}

bool D3D12CameraCapture::isAcquisitionActive() const noexcept
{
    if (!opened_.load() || !impl_) return false;
    try {
        std::lock_guard<std::mutex> lock(impl_->lifecycleMutex);
        return impl_->grabber && (*impl_->grabber) &&
               impl_->grabber->isAcquisitionActive();
    } catch (...) {
        return false;
    }
}

D3D12ReadOnlyReadResult D3D12CameraCapture::read(ReadMode mode)
{
    return read(CameraReadOptions{mode, 1000});
}

D3D12ReadOnlyReadResult D3D12CameraCapture::read(
    const CameraReadOptions& options)
{
    D3D12ReadOnlyReadResult result;
    if (!opened_.load() || !impl_) {
        result.error = MakeError(
            ErrorCode::NotOpened,
            "V2::D3D12CameraCapture::read",
            "Capture is not opened");
        if (impl_) impl_->setError(result.error);
        return result;
    }

    PendingIC4Frame pending;
    {
        std::unique_lock<std::mutex> lock(impl_->pendingMutex);
        const bool hasFrame = impl_->pendingCv.wait_for(
            lock,
            std::chrono::milliseconds(options.timeoutMs),
            [&] {
                return !impl_->pendingFrames.empty() || !opened_.load();
            });

        if (!hasFrame || impl_->pendingFrames.empty()) {
            impl_->incrementReadTimeouts();
            result.error = MakeError(
                ErrorCode::Timeout,
                "V2::D3D12CameraCapture::read",
                "Read timed out");
            impl_->setError(result.error);
            return result;
        }

        if (options.mode == ReadMode::LatestFrame) {
            if (impl_->pendingFrames.size() > 1) {
                impl_->incrementDropped(
                    static_cast<std::uint64_t>(impl_->pendingFrames.size() - 1));
            }
            pending = std::move(impl_->pendingFrames.back());
            impl_->pendingFrames.clear();
        } else {
            pending = std::move(impl_->pendingFrames.front());
            impl_->pendingFrames.pop_front();
        }
    }

    IC4Ext::D3D12CpuFrameView view;
    if (!impl_->buildCpuView(pending, view, result.error)) {
        impl_->setError(result.error);
        return result;
    }

    {
        // The v1 converter slots and UploadRings are shared by the pooled
        // adapter. Serializing this section also makes read() safe against an
        // accidental second reader on the same capture object.
        std::lock_guard<std::mutex> conversionLock(impl_->conversionMutex);

        if (!impl_->ensureFramePool(pending.format)) {
            impl_->incrementConversionFailures();
            result.error = impl_->getError();
            return result;
        }

        auto writer = impl_->framePool->acquire();
        if (!writer) {
            impl_->incrementConversionFailures();
            result.error = impl_->framePool->lastError();
            if (!result.error) {
                result.error = MakeError(
                    ErrorCode::InternalError,
                    "V2::D3D12CameraCapture::read / framePool.acquire",
                    "Frame pool returned an invalid writer");
            }
            impl_->setError(result.error);
            return result;
        }

        if (!impl_->pooledConverter->convert(
                view,
                impl_->config.outputSpec,
                std::move(writer),
                std::move(pending.chunkMetadata),
                result.frame)) {
            impl_->incrementConversionFailures();
            result.error = impl_->pooledConverter->lastError();
            impl_->setError(result.error);
            return result;
        }
    }

    impl_->incrementReadFrames();
    impl_->clearError();
    result.ok = true;
    return result;
}

bool D3D12CameraCapture::applyIC4StateJson(
    const std::filesystem::path& jsonPath,
    std::size_t deviceIndex,
    bool strict,
    bool applyNestedSelectorStates)
{
    if (!opened_.load() || !impl_) {
        if (impl_) {
            impl_->setError(
                ErrorCode::NotOpened,
                "V2::D3D12CameraCapture::applyIC4StateJson",
                "Capture is not opened");
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(impl_->controlMutex);
    CameraCaptureConfig updated = impl_->config;
    updated.ic4StateJson.path = jsonPath;
    updated.ic4StateJson.deviceIndex = deviceIndex;
    updated.ic4StateJson.strict = strict;
    updated.ic4StateJson.applyNestedSelectorStates = applyNestedSelectorStates;

    ErrorInfo effectiveError;
    updated = BuildEffectiveConfigFromJson(updated, effectiveError);
    if (effectiveError) {
        impl_->setError(effectiveError);
        return false;
    }

    impl_->config = std::move(updated);
    if (!impl_->applyJsonStateConfig()) return false;
    impl_->clearError();
    return true;
}

bool D3D12CameraCapture::setIC4Property(
    const std::string& propertyName,
    bool value)
{
    if (!opened_.load() || !impl_) return false;
    return impl_->setPropertyValue(
        propertyName,
        value,
        "V2::D3D12CameraCapture::setIC4Property(bool)");
}

bool D3D12CameraCapture::setIC4Property(
    const std::string& propertyName,
    int value)
{
    return setIC4Property(propertyName, static_cast<std::int64_t>(value));
}

bool D3D12CameraCapture::setIC4Property(
    const std::string& propertyName,
    std::int64_t value)
{
    if (!opened_.load() || !impl_) return false;
    return impl_->setPropertyValue(
        propertyName,
        value,
        "V2::D3D12CameraCapture::setIC4Property(int64)");
}

bool D3D12CameraCapture::setIC4Property(
    const std::string& propertyName,
    double value)
{
    if (!opened_.load() || !impl_) return false;
    return impl_->setPropertyValue(
        propertyName,
        value,
        "V2::D3D12CameraCapture::setIC4Property(double)");
}

bool D3D12CameraCapture::setIC4Property(
    const std::string& propertyName,
    const char* value)
{
    return setIC4Property(propertyName, std::string(value ? value : ""));
}

bool D3D12CameraCapture::setIC4Property(
    const std::string& propertyName,
    const std::string& value)
{
    if (!opened_.load() || !impl_) return false;
    return impl_->setPropertyValue(
        propertyName,
        value,
        "V2::D3D12CameraCapture::setIC4Property(string)");
}

bool D3D12CameraCapture::setFrameRate(double fps)
{
    return setIC4Property("AcquisitionFrameRate", fps);
}

bool D3D12CameraCapture::setExposureAuto(const std::string& mode)
{
    return setIC4Property("ExposureAuto", mode);
}

bool D3D12CameraCapture::setExposureTime(double exposureTimeUs)
{
    return setIC4Property("ExposureTime", exposureTimeUs);
}

bool D3D12CameraCapture::setGainAuto(const std::string& mode)
{
    return setIC4Property("GainAuto", mode);
}

bool D3D12CameraCapture::setGain(double gain)
{
    return setIC4Property("Gain", gain);
}

bool D3D12CameraCapture::setGamma(double gamma)
{
    return setIC4Property("Gamma", gamma);
}

bool D3D12CameraCapture::setOffset(int offsetX, int offsetY)
{
    if (!opened_.load() || !impl_) return false;

    std::lock_guard<std::mutex> lock(impl_->controlMutex);
    auto properties = impl_->getPropertyMap(
        "V2::D3D12CameraCapture::setOffset");
    if (!properties) return false;

    ic4::Error error;
    if (!properties->setValue(ic4::PropId::OffsetAutoCenter, "Off", error) ||
        !properties->setValue(ic4::PropId::OffsetX, offsetX, error) ||
        !properties->setValue(ic4::PropId::OffsetY, offsetY, error)) {
        impl_->setError(
            ErrorCode::IC4Error,
            "V2::D3D12CameraCapture::setOffset",
            IC4ErrorMessage(error));
        return false;
    }

    impl_->config.streamRequest.offsetX = offsetX;
    impl_->config.streamRequest.offsetY = offsetY;
    impl_->clearError();
    return true;
}

bool D3D12CameraCapture::setRoi(
    int width,
    int height,
    int offsetX,
    int offsetY)
{
    if (!opened_.load() || !impl_) return false;

    std::lock_guard<std::mutex> lock(impl_->controlMutex);
    auto properties = impl_->getPropertyMap(
        "V2::D3D12CameraCapture::setRoi");
    if (!properties) return false;

    ic4::Error error;
    if (!properties->setValue(ic4::PropId::OffsetAutoCenter, "Off", error) ||
        !properties->setValue(ic4::PropId::Width, width, error) ||
        !properties->setValue(ic4::PropId::Height, height, error) ||
        !properties->setValue(ic4::PropId::OffsetX, offsetX, error) ||
        !properties->setValue(ic4::PropId::OffsetY, offsetY, error)) {
        impl_->setError(
            ErrorCode::IC4Error,
            "V2::D3D12CameraCapture::setRoi",
            IC4ErrorMessage(error));
        return false;
    }

    impl_->config.streamRequest.width = width;
    impl_->config.streamRequest.height = height;
    impl_->config.streamRequest.offsetX = offsetX;
    impl_->config.streamRequest.offsetY = offsetY;
    impl_->clearError();
    return true;
}

bool D3D12CameraCapture::setPixelFormat(CameraPixelFormat format)
{
    if (!opened_.load() || !impl_) return false;

    const auto ic4Format = ToIC4PixelFormat(format);
    if (ic4Format == ic4::PixelFormat::Unspecified) {
        impl_->setError(
            ErrorCode::UnsupportedFormat,
            "V2::D3D12CameraCapture::setPixelFormat",
            "Unsupported input pixel format");
        return false;
    }

    std::lock_guard<std::mutex> lock(impl_->controlMutex);
    auto properties = impl_->getPropertyMap(
        "V2::D3D12CameraCapture::setPixelFormat");
    if (!properties) return false;

    ic4::Error error;
    if (!properties->setValue(ic4::PropId::PixelFormat, ic4Format, error)) {
        impl_->setError(
            ErrorCode::IC4Error,
            "V2::D3D12CameraCapture::setPixelFormat",
            IC4ErrorMessage(error));
        return false;
    }

    impl_->config.streamRequest.requestedFormat = format;
    impl_->clearError();
    return true;
}

bool D3D12CameraCapture::softwareTrigger(const std::string& commandName)
{
    return setIC4Property(
        commandName.empty() ? std::string("TriggerSoftware") : commandName,
        std::string("execute"));
}

CameraCaptureStats D3D12CameraCapture::stats() const
{
    return impl_ ? impl_->getStats() : CameraCaptureStats{};
}

CameraPerformanceSnapshot D3D12CameraCapture::performance()
{
    return impl_ ? impl_->performanceSnapshot() : CameraPerformanceSnapshot{};
}

D3D12FramePoolStats D3D12CameraCapture::framePoolStats() const
{
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->conversionMutex);
    return impl_->framePool ? impl_->framePool->stats() : D3D12FramePoolStats{};
}

ErrorInfo D3D12CameraCapture::lastError() const
{
    return impl_ ? impl_->getError() : NoError();
}

} // namespace IC4Ext::V2

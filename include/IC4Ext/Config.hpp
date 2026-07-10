#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace IC4Ext {

enum class CameraPixelFormat : std::uint32_t
{
    Mono8 = 1,
    BayerRG8,
    BayerGR8,
    BayerGB8,
    BayerBG8,
    BGR8,
    BGRa8,
};

enum class GpuFrameFormat : std::uint32_t
{
    R8 = 1,
    RGBA8,
};

enum class ReadMode : std::uint32_t
{
    LatestFrame = 0,
    NextFrame = 1,
};

enum class FrameQueuePolicy : std::uint32_t
{
    LatestOnly = 0,
    PreserveFrames = 1,
};

enum class ShaderInputKind : std::uint32_t
{
    Auto = 0,
    HlslFile = 1,
    CsoFile = 2,
};

enum class CameraSyncMode : std::uint32_t
{
    None = 0,
    HardwareTrigger = 1,
    SoftwareTrigger = 2,
};

struct IC4StateJsonConfig
{
    // IC Capture 4 state JSON exported by the official application.
    // The implementation reads devices[deviceIndex].state using nlohmann/json.
    std::filesystem::path path;
    std::size_t deviceIndex = 0;

    // false: unsupported or locked properties are skipped and remembered as lastError only if all critical setup fails.
    // true : the first property application failure makes open()/applyIC4StateJson() fail.
    bool strict = false;

    // IC Capture 4 stores selector-dependent properties as nested objects such as
    // BalanceRatioSelector.{Red,Green,Blue}. When true, IC4Ext switches the selector
    // and applies direct scalar values inside each selected entry. Arrays/register blobs
    // such as LUTValueAll are skipped.
    bool applyNestedSelectorStates = true;

    bool enabled() const noexcept { return !path.empty(); }
};

struct CameraStreamRequest
{
    int width = 0;
    int height = 0;
    double fps = 0.0;

    // Used when no IC4 state JSON is supplied. If a JSON state is supplied, its PixelFormat
    // becomes the effective input format unless forceRequestedFormat is true.
    CameraPixelFormat requestedFormat = CameraPixelFormat::BayerRG8;
    bool forceRequestedFormat = false;

    // Optional ROI offsets. IC Capture 4 exported JSON normally does not contain OffsetX/Y,
    // so these explicit values are applied after the JSON state and after Width/Height.
    std::optional<int> offsetX;
    std::optional<int> offsetY;
};

struct FrameOutputSpec
{
    GpuFrameFormat outputFormat = GpuFrameFormat::RGBA8;
    bool createSrv = true;
    bool createUav = true;
};

struct ShaderLoadConfig
{
    ShaderInputKind inputKind = ShaderInputKind::Auto;
    std::filesystem::path shaderDirectory;
    std::string entryPoint = "main";
    std::string target = "cs_5_0";
    bool preferCsoWhenBothExist = true;
};

struct CameraSyncConfig
{
    CameraSyncMode mode = CameraSyncMode::None;

    // GenICam-style trigger properties. Values are strings because concrete line
    // names and enum entries can vary by camera model.
    std::string triggerSelector = "FrameStart";
    std::string triggerSource;
    std::string triggerActivation = "RisingEdge";
    std::string triggerModeOnValue = "On";
    std::string triggerModeOffValue = "Off";

    bool applyTriggerActivation = true;
    bool setExposureAutoOff = true;
    std::string exposureAutoOffValue = "Off";
    double exposureTimeUs = 0.0;

    // Command property used by softwareTrigger(). IC4 command properties can be
    // executed by setting a string value such as "execute".
    std::string softwareTriggerCommand = "TriggerSoftware";
};

using IC4PropertyValue = std::variant<bool, std::int64_t, double, std::string>;

struct IC4PropertyOverride
{
    std::string propertyName;
    IC4PropertyValue value;
};

struct CameraCaptureConfig
{
    IC4StateJsonConfig ic4StateJson;
    CameraStreamRequest streamRequest;
    FrameOutputSpec outputSpec;
    FrameQueuePolicy queuePolicy = FrameQueuePolicy::LatestOnly;
    std::size_t maxPendingBuffers = 1;
    ShaderLoadConfig shaderConfig;

    // Applied after JSON state and stream/ROI settings. This is mainly used by
    // D3D11CameraCaptureThread setters called before open(). It is also the path
    // used by ConfigureHardwareTriggerSync()/ConfigureSoftwareTriggerSync().
    std::vector<IC4PropertyOverride> propertyOverrides;
};

struct CameraReadOptions
{
    ReadMode mode = ReadMode::LatestFrame;
    std::uint32_t timeoutMs = 1000;
};

struct CameraThreadOptions
{
    std::uint32_t readTimeoutMs = 1000;
    bool copyPerOutputQueue = true;
    bool stopOnReadError = false;
};

enum class FrameSyncPolicy : std::uint32_t
{
    PassThroughSingleCamera = 0,
    TimestampNearest = 1,
    FrameNumberExact = 2,
};

struct FrameSyncOptions
{
    FrameSyncPolicy policy = FrameSyncPolicy::PassThroughSingleCamera;
    std::vector<std::uint32_t> cameraIndices = {0};
    std::uint64_t maxTimestampDiffNs = 1'000'000;
    std::size_t maxBufferedFramesPerCamera = 8;
};

const char* ToString(CameraPixelFormat fmt) noexcept;
const char* ToString(GpuFrameFormat fmt) noexcept;
const char* ToString(CameraSyncMode mode) noexcept;
std::size_t BytesPerPixel(CameraPixelFormat fmt) noexcept;
bool IsSupportedConversion(CameraPixelFormat input, GpuFrameFormat output) noexcept;

bool ParseCameraPixelFormat(const std::string& text, CameraPixelFormat& out) noexcept;

CameraSyncConfig MakeNoSyncConfig(std::string triggerSelector = "FrameStart");
CameraSyncConfig MakeHardwareTriggerSyncConfig(std::string triggerSource = "Line1",
                                               std::string triggerSelector = "FrameStart",
                                               std::string triggerActivation = "RisingEdge");
CameraSyncConfig MakeSoftwareTriggerSyncConfig(std::string triggerSelector = "FrameStart",
                                               std::string softwareTriggerCommand = "TriggerSoftware");

// Materializes sync configuration into CameraCaptureConfig::propertyOverrides.
// Call this before open()/D3D*CameraCaptureThread::start(). Existing overrides
// for the same property names are replaced.
void ConfigureCameraSync(CameraCaptureConfig& config, const CameraSyncConfig& sync);
void ConfigureNoSync(CameraCaptureConfig& config, std::string triggerSelector = "FrameStart");
void ConfigureHardwareTriggerSync(CameraCaptureConfig& config,
                                  std::string triggerSource = "Line1",
                                  std::string triggerSelector = "FrameStart",
                                  std::string triggerActivation = "RisingEdge");
void ConfigureSoftwareTriggerSync(CameraCaptureConfig& config,
                                  std::string triggerSelector = "FrameStart",
                                  std::string softwareTriggerCommand = "TriggerSoftware");

} // namespace IC4Ext

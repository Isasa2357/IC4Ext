#include "IC4Ext/Config.hpp"

#include <utility>

namespace IC4Ext {

namespace {

void AddOrReplaceOverride(CameraCaptureConfig& config, std::string name, IC4PropertyValue value)
{
    for (auto& ov : config.propertyOverrides) {
        if (ov.propertyName == name) {
            ov.value = std::move(value);
            return;
        }
    }
    config.propertyOverrides.push_back(IC4PropertyOverride{std::move(name), std::move(value)});
}

void ApplyExposureSyncOverrides(CameraCaptureConfig& config, const CameraSyncConfig& sync)
{
    if (sync.setExposureAutoOff && !sync.exposureAutoOffValue.empty()) {
        AddOrReplaceOverride(config, "ExposureAuto", sync.exposureAutoOffValue);
    }
    if (sync.exposureTimeUs > 0.0) {
        AddOrReplaceOverride(config, "ExposureTime", sync.exposureTimeUs);
    }
}

} // namespace

const char* ToString(CameraPixelFormat fmt) noexcept
{
    switch (fmt) {
    case CameraPixelFormat::Mono8: return "Mono8";
    case CameraPixelFormat::BayerRG8: return "BayerRG8";
    case CameraPixelFormat::BayerGR8: return "BayerGR8";
    case CameraPixelFormat::BayerGB8: return "BayerGB8";
    case CameraPixelFormat::BayerBG8: return "BayerBG8";
    case CameraPixelFormat::BGR8: return "BGR8";
    case CameraPixelFormat::BGRa8: return "BGRa8";
    default: return "Unknown";
    }
}

const char* ToString(GpuFrameFormat fmt) noexcept
{
    switch (fmt) {
    case GpuFrameFormat::R8: return "R8";
    case GpuFrameFormat::RGBA8: return "RGBA8";
    default: return "Unknown";
    }
}

const char* ToString(CameraSyncMode mode) noexcept
{
    switch (mode) {
    case CameraSyncMode::None: return "None";
    case CameraSyncMode::HardwareTrigger: return "HardwareTrigger";
    case CameraSyncMode::SoftwareTrigger: return "SoftwareTrigger";
    default: return "Unknown";
    }
}

std::size_t BytesPerPixel(CameraPixelFormat fmt) noexcept
{
    switch (fmt) {
    case CameraPixelFormat::Mono8:
    case CameraPixelFormat::BayerRG8:
    case CameraPixelFormat::BayerGR8:
    case CameraPixelFormat::BayerGB8:
    case CameraPixelFormat::BayerBG8:
        return 1;
    case CameraPixelFormat::BGR8:
        return 3;
    case CameraPixelFormat::BGRa8:
        return 4;
    default:
        return 0;
    }
}

bool IsSupportedConversion(CameraPixelFormat input, GpuFrameFormat output) noexcept
{
    if (input == CameraPixelFormat::Mono8) {
        return output == GpuFrameFormat::R8 || output == GpuFrameFormat::RGBA8;
    }
    if (input == CameraPixelFormat::BGR8 || input == CameraPixelFormat::BGRa8) {
        return output == GpuFrameFormat::RGBA8;
    }
    if (input == CameraPixelFormat::BayerRG8 || input == CameraPixelFormat::BayerGR8 ||
        input == CameraPixelFormat::BayerGB8 || input == CameraPixelFormat::BayerBG8) {
        return output == GpuFrameFormat::RGBA8;
    }
    return false;
}

bool ParseCameraPixelFormat(const std::string& text, CameraPixelFormat& out) noexcept
{
    if (text == "Mono8") { out = CameraPixelFormat::Mono8; return true; }
    if (text == "BayerRG8") { out = CameraPixelFormat::BayerRG8; return true; }
    if (text == "BayerGR8") { out = CameraPixelFormat::BayerGR8; return true; }
    if (text == "BayerGB8") { out = CameraPixelFormat::BayerGB8; return true; }
    if (text == "BayerBG8") { out = CameraPixelFormat::BayerBG8; return true; }
    if (text == "BGR8") { out = CameraPixelFormat::BGR8; return true; }
    if (text == "BGRa8" || text == "BGRA8") { out = CameraPixelFormat::BGRa8; return true; }
    return false;
}

CameraSyncConfig MakeNoSyncConfig(std::string triggerSelector)
{
    CameraSyncConfig sync;
    sync.mode = CameraSyncMode::None;
    sync.triggerSelector = std::move(triggerSelector);
    return sync;
}

CameraSyncConfig MakeHardwareTriggerSyncConfig(std::string triggerSource,
                                               std::string triggerSelector,
                                               std::string triggerActivation)
{
    CameraSyncConfig sync;
    sync.mode = CameraSyncMode::HardwareTrigger;
    sync.triggerSource = std::move(triggerSource);
    sync.triggerSelector = std::move(triggerSelector);
    sync.triggerActivation = std::move(triggerActivation);
    return sync;
}

CameraSyncConfig MakeSoftwareTriggerSyncConfig(std::string triggerSelector,
                                               std::string softwareTriggerCommand)
{
    CameraSyncConfig sync;
    sync.mode = CameraSyncMode::SoftwareTrigger;
    sync.triggerSelector = std::move(triggerSelector);
    sync.triggerSource = "Software";
    sync.applyTriggerActivation = false;
    sync.softwareTriggerCommand = std::move(softwareTriggerCommand);
    return sync;
}

void ConfigureCameraSync(CameraCaptureConfig& config, const CameraSyncConfig& sync)
{
    if (!sync.triggerSelector.empty()) {
        AddOrReplaceOverride(config, "TriggerSelector", sync.triggerSelector);
    }

    switch (sync.mode) {
    case CameraSyncMode::None:
        AddOrReplaceOverride(config, "TriggerMode", sync.triggerModeOffValue.empty() ? std::string("Off") : sync.triggerModeOffValue);
        break;

    case CameraSyncMode::HardwareTrigger:
        AddOrReplaceOverride(config, "TriggerMode", sync.triggerModeOnValue.empty() ? std::string("On") : sync.triggerModeOnValue);
        if (!sync.triggerSource.empty()) {
            AddOrReplaceOverride(config, "TriggerSource", sync.triggerSource);
        }
        if (sync.applyTriggerActivation && !sync.triggerActivation.empty()) {
            AddOrReplaceOverride(config, "TriggerActivation", sync.triggerActivation);
        }
        ApplyExposureSyncOverrides(config, sync);
        break;

    case CameraSyncMode::SoftwareTrigger:
        AddOrReplaceOverride(config, "TriggerMode", sync.triggerModeOnValue.empty() ? std::string("On") : sync.triggerModeOnValue);
        AddOrReplaceOverride(config, "TriggerSource", sync.triggerSource.empty() ? std::string("Software") : sync.triggerSource);
        if (sync.applyTriggerActivation && !sync.triggerActivation.empty()) {
            AddOrReplaceOverride(config, "TriggerActivation", sync.triggerActivation);
        }
        ApplyExposureSyncOverrides(config, sync);
        break;
    }
}

void ConfigureNoSync(CameraCaptureConfig& config, std::string triggerSelector)
{
    ConfigureCameraSync(config, MakeNoSyncConfig(std::move(triggerSelector)));
}

void ConfigureHardwareTriggerSync(CameraCaptureConfig& config,
                                  std::string triggerSource,
                                  std::string triggerSelector,
                                  std::string triggerActivation)
{
    ConfigureCameraSync(config, MakeHardwareTriggerSyncConfig(std::move(triggerSource),
                                                             std::move(triggerSelector),
                                                             std::move(triggerActivation)));
}

void ConfigureSoftwareTriggerSync(CameraCaptureConfig& config,
                                  std::string triggerSelector,
                                  std::string softwareTriggerCommand)
{
    ConfigureCameraSync(config, MakeSoftwareTriggerSyncConfig(std::move(triggerSelector),
                                                             std::move(softwareTriggerCommand)));
}

} // namespace IC4Ext

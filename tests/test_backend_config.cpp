#include <IC4Ext/IC4Ext.hpp>
#include <cassert>
#include <iostream>
#include <string>
#include <variant>

namespace {

const IC4Ext::IC4PropertyOverride* FindOverride(const IC4Ext::CameraCaptureConfig& config,
                                                 const std::string& name)
{
    for (const auto& overrideValue : config.propertyOverrides) {
        if (overrideValue.propertyName == name) return &overrideValue;
    }
    return nullptr;
}

std::string GetStringOverride(const IC4Ext::CameraCaptureConfig& config,
                              const std::string& name)
{
    const auto* overrideValue = FindOverride(config, name);
    assert(overrideValue);
    return std::get<std::string>(overrideValue->value);
}

} // namespace

int main()
{
#if !IC4EXT_ENABLE_D3D11 && !IC4EXT_ENABLE_D3D12
#error "IC4Ext test target has no backend enabled"
#endif

    static_assert(IC4EXT_VERSION_MAJOR == 1);
    static_assert(IC4EXT_VERSION_MINOR == 0);
    static_assert(IC4EXT_VERSION_PATCH == 1);
    assert(std::string(IC4EXT_VERSION_STRING) == "1.0.1");
    assert(std::string(IC4Ext::VersionString) == "1.0.1");

    IC4Ext::CameraCaptureConfig lifecycleDefaults;
    assert(lifecycleDefaults.acquisitionStartMode ==
           IC4Ext::AcquisitionStartMode::Immediate);
    lifecycleDefaults.acquisitionStartMode = IC4Ext::AcquisitionStartMode::Deferred;
    assert(lifecycleDefaults.acquisitionStartMode ==
           IC4Ext::AcquisitionStartMode::Deferred);

    IC4Ext::CameraPerformanceSnapshot performanceDefaults;
    assert(!performanceDefaults.streamStatistics.hasValue);
    assert(!performanceDefaults.timing.hasDeviceInterval);
    assert(!performanceDefaults.timing.hasHostInterval);
    assert(performanceDefaults.temperatures.empty());

    IC4Ext::CameraCaptureConfig hardwareSyncConfig;
    IC4Ext::ConfigureHardwareTriggerSync(hardwareSyncConfig,
                                         "Line1",
                                         "FrameStart",
                                         "RisingEdge");
    assert(IC4Ext::ToString(IC4Ext::CameraSyncMode::HardwareTrigger) ==
           std::string("HardwareTrigger"));
    assert(GetStringOverride(hardwareSyncConfig, "TriggerSelector") == "FrameStart");
    assert(GetStringOverride(hardwareSyncConfig, "TriggerMode") == "On");
    assert(GetStringOverride(hardwareSyncConfig, "TriggerSource") == "Line1");
    assert(GetStringOverride(hardwareSyncConfig, "TriggerActivation") == "RisingEdge");
    assert(GetStringOverride(hardwareSyncConfig, "ExposureAuto") == "Off");

    IC4Ext::CameraCaptureConfig softwareSyncConfig;
    IC4Ext::ConfigureSoftwareTriggerSync(softwareSyncConfig);
    assert(IC4Ext::ToString(IC4Ext::CameraSyncMode::SoftwareTrigger) ==
           std::string("SoftwareTrigger"));
    assert(GetStringOverride(softwareSyncConfig, "TriggerSelector") == "FrameStart");
    assert(GetStringOverride(softwareSyncConfig, "TriggerMode") == "On");
    assert(GetStringOverride(softwareSyncConfig, "TriggerSource") == "Software");

    IC4Ext::CameraCaptureConfig noSyncConfig;
    IC4Ext::ConfigureNoSync(noSyncConfig);
    assert(IC4Ext::ToString(IC4Ext::CameraSyncMode::None) == std::string("None"));
    assert(GetStringOverride(noSyncConfig, "TriggerMode") == "Off");

#if IC4EXT_ENABLE_D3D11
    IC4Ext::D3D11ReadyToken d3d11Invalid;
    assert(!d3d11Invalid.isValid());

    IC4Ext::D3D11CameraCapture d3d11Capture;
    auto d3d11Performance = d3d11Capture.performance();
    assert(!d3d11Performance.streamStatistics.hasValue);
    assert(d3d11Performance.captureStats.receivedBuffers == 0);
#endif

#if IC4EXT_ENABLE_D3D12
    IC4Ext::D3D12ReadyToken d3d12Invalid;
    assert(!d3d12Invalid.isValid());

    IC4Ext::D3D12CameraCapture d3d12Capture;
    auto d3d12Performance = d3d12Capture.performance();
    assert(!d3d12Performance.streamStatistics.hasValue);
    assert(d3d12Performance.captureStats.receivedBuffers == 0);
#endif

    IC4Ext::CameraControlCommand command =
        IC4Ext::CameraControlCommand::ExposureTime(1234.0);
    assert(command.type == IC4Ext::CameraControlCommandType::SetExposureTime);
    assert(command.doubleValue == 1234.0);

    std::cout << "test_backend_config passed version=" << IC4EXT_VERSION_STRING
              << " D3D11=" << IC4EXT_ENABLE_D3D11
              << " D3D12=" << IC4EXT_ENABLE_D3D12 << "\n";
    return 0;
}

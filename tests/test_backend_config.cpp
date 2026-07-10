#include <IC4Ext/IC4Ext.hpp>
#include <cassert>
#include <iostream>
#include <string>
#include <variant>

namespace {

const IC4Ext::IC4PropertyOverride* FindOverride(const IC4Ext::CameraCaptureConfig& config,
                                                 const std::string& name)
{
    for (const auto& ov : config.propertyOverrides) {
        if (ov.propertyName == name) return &ov;
    }
    return nullptr;
}

std::string GetStringOverride(const IC4Ext::CameraCaptureConfig& config,
                              const std::string& name)
{
    const auto* ov = FindOverride(config, name);
    assert(ov);
    return std::get<std::string>(ov->value);
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

    IC4Ext::CameraPerformanceSnapshot perfDefaults;
    assert(!perfDefaults.streamStatistics.hasValue);
    assert(!perfDefaults.timing.hasDeviceInterval);
    assert(!perfDefaults.timing.hasHostInterval);
    assert(perfDefaults.temperatures.empty());

    IC4Ext::CameraCaptureConfig hwSyncConfig;
    IC4Ext::ConfigureHardwareTriggerSync(hwSyncConfig, "Line1", "FrameStart", "RisingEdge");
    assert(IC4Ext::ToString(IC4Ext::CameraSyncMode::HardwareTrigger) == std::string("HardwareTrigger"));
    assert(GetStringOverride(hwSyncConfig, "TriggerSelector") == "FrameStart");
    assert(GetStringOverride(hwSyncConfig, "TriggerMode") == "On");
    assert(GetStringOverride(hwSyncConfig, "TriggerSource") == "Line1");
    assert(GetStringOverride(hwSyncConfig, "TriggerActivation") == "RisingEdge");
    assert(GetStringOverride(hwSyncConfig, "ExposureAuto") == "Off");

    IC4Ext::CameraCaptureConfig swSyncConfig;
    IC4Ext::ConfigureSoftwareTriggerSync(swSyncConfig);
    assert(IC4Ext::ToString(IC4Ext::CameraSyncMode::SoftwareTrigger) == std::string("SoftwareTrigger"));
    assert(GetStringOverride(swSyncConfig, "TriggerSelector") == "FrameStart");
    assert(GetStringOverride(swSyncConfig, "TriggerMode") == "On");
    assert(GetStringOverride(swSyncConfig, "TriggerSource") == "Software");

    IC4Ext::CameraCaptureConfig noSyncConfig;
    IC4Ext::ConfigureNoSync(noSyncConfig);
    assert(IC4Ext::ToString(IC4Ext::CameraSyncMode::None) == std::string("None"));
    assert(GetStringOverride(noSyncConfig, "TriggerMode") == "Off");

#if IC4EXT_ENABLE_D3D11
    IC4Ext::D3D11ReadyToken d3d11Invalid;
    assert(!d3d11Invalid.isValid());

    IC4Ext::D3D11CameraCapture d3d11Capture;
    auto d3d11Perf = d3d11Capture.performance();
    assert(!d3d11Perf.streamStatistics.hasValue);
    assert(d3d11Perf.captureStats.receivedBuffers == 0);
#endif

#if IC4EXT_ENABLE_D3D12
    IC4Ext::D3D12ReadyToken d3d12Invalid;
    assert(!d3d12Invalid.isValid());

    IC4Ext::D3D12CameraCapture d3d12Capture;
    auto d3d12Perf = d3d12Capture.performance();
    assert(!d3d12Perf.streamStatistics.hasValue);
    assert(d3d12Perf.captureStats.receivedBuffers == 0);
#endif

    IC4Ext::CameraControlCommand cmd = IC4Ext::CameraControlCommand::ExposureTime(1234.0);
    assert(cmd.type == IC4Ext::CameraControlCommandType::SetExposureTime);
    assert(cmd.doubleValue == 1234.0);

    std::cout << "test_backend_config passed version=" << IC4EXT_VERSION_STRING
              << " D3D11=" << IC4EXT_ENABLE_D3D11
              << " D3D12=" << IC4EXT_ENABLE_D3D12 << "\n";
    return 0;
}

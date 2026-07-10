#include <IC4Ext/IC4Ext.hpp>
#include <cassert>
#include <iostream>

int main()
{
#if !IC4EXT_ENABLE_D3D11 && !IC4EXT_ENABLE_D3D12
#error "IC4Ext test target has no backend enabled"
#endif

    IC4Ext::CameraPerformanceSnapshot perfDefaults;
    assert(!perfDefaults.streamStatistics.hasValue);
    assert(!perfDefaults.timing.hasDeviceInterval);
    assert(!perfDefaults.timing.hasHostInterval);
    assert(perfDefaults.temperatures.empty());

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

    std::cout << "test_backend_config passed D3D11=" << IC4EXT_ENABLE_D3D11
              << " D3D12=" << IC4EXT_ENABLE_D3D12 << "\n";
    return 0;
}

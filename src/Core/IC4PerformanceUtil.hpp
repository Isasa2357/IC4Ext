#pragma once

#include "IC4Ext/Core/CoreTypes.hpp"

#include <ic4/ic4.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace IC4Ext {
namespace Internal {

inline std::uint64_t AbsDiffU64(std::uint64_t a, std::uint64_t b) noexcept
{
    return (a >= b) ? (a - b) : (b - a);
}

class FrameTimingPerformanceTracker
{
public:
    void reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        metrics_ = {};
        hasPrevFrameNumber_ = false;
        prevFrameNumber_ = 0;
        hasPrevDeviceTimestamp_ = false;
        prevDeviceTimestampNs_ = 0;
        hasPrevDeviceInterval_ = false;
        prevDeviceIntervalNs_ = 0;
        hasPrevHostTime_ = false;
        prevHostTime_ = {};
        hasPrevHostInterval_ = false;
        prevHostIntervalNs_ = 0;
    }

    void update(const FrameTiming& timing)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (timing.frameNumber != 0) {
            if (hasPrevFrameNumber_ && timing.frameNumber > prevFrameNumber_) {
                const std::uint64_t gap = timing.frameNumber - prevFrameNumber_;
                metrics_.hasFrameNumberGap = true;
                metrics_.frameNumberGap = gap;
                metrics_.estimatedDroppedFrames = (gap > 1) ? (gap - 1) : 0;
                metrics_.accumulatedEstimatedDroppedFrames += metrics_.estimatedDroppedFrames;
            }
            prevFrameNumber_ = timing.frameNumber;
            hasPrevFrameNumber_ = true;
        }

        if (timing.deviceTimestampNs != 0) {
            if (hasPrevDeviceTimestamp_ && timing.deviceTimestampNs > prevDeviceTimestampNs_) {
                const std::uint64_t interval = timing.deviceTimestampNs - prevDeviceTimestampNs_;
                metrics_.hasDeviceInterval = true;
                metrics_.deviceFrameIntervalNs = interval;
                metrics_.deviceFps = interval > 0 ? 1000000000.0 / static_cast<double>(interval) : 0.0;
                if (hasPrevDeviceInterval_) {
                    metrics_.hasDeviceJitter = true;
                    metrics_.deviceJitterNs = static_cast<double>(AbsDiffU64(interval, prevDeviceIntervalNs_));
                }
                prevDeviceIntervalNs_ = interval;
                hasPrevDeviceInterval_ = true;
            }
            prevDeviceTimestampNs_ = timing.deviceTimestampNs;
            hasPrevDeviceTimestamp_ = true;
        }

        if (timing.hostReceivedTime != std::chrono::steady_clock::time_point{}) {
            if (hasPrevHostTime_ && timing.hostReceivedTime > prevHostTime_) {
                const auto intervalNsSigned = std::chrono::duration_cast<std::chrono::nanoseconds>(timing.hostReceivedTime - prevHostTime_).count();
                if (intervalNsSigned > 0) {
                    const auto interval = static_cast<std::uint64_t>(intervalNsSigned);
                    metrics_.hasHostInterval = true;
                    metrics_.hostReceiveIntervalNs = interval;
                    metrics_.hostReceiveFps = 1000000000.0 / static_cast<double>(interval);
                    if (hasPrevHostInterval_) {
                        metrics_.hasHostJitter = true;
                        metrics_.hostJitterNs = static_cast<double>(AbsDiffU64(interval, prevHostIntervalNs_));
                    }
                    prevHostIntervalNs_ = interval;
                    hasPrevHostInterval_ = true;
                }
            }
            prevHostTime_ = timing.hostReceivedTime;
            hasPrevHostTime_ = true;
        }
    }

    CameraTimingPerformance snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return metrics_;
    }

private:
    mutable std::mutex mutex_;
    CameraTimingPerformance metrics_{};

    bool hasPrevFrameNumber_ = false;
    std::uint64_t prevFrameNumber_ = 0;

    bool hasPrevDeviceTimestamp_ = false;
    std::uint64_t prevDeviceTimestampNs_ = 0;
    bool hasPrevDeviceInterval_ = false;
    std::uint64_t prevDeviceIntervalNs_ = 0;

    bool hasPrevHostTime_ = false;
    std::chrono::steady_clock::time_point prevHostTime_{};
    bool hasPrevHostInterval_ = false;
    std::uint64_t prevHostIntervalNs_ = 0;
};

inline IC4StreamStatistics ReadStreamStatistics(ic4::Grabber* grabber) noexcept
{
    IC4StreamStatistics out;
    if (!grabber || !(*grabber)) {
        return out;
    }

    try {
        ic4::Error err;
        const auto stats = grabber->streamStatistics(err);
        if (err.isError()) {
            return out;
        }

        out.hasValue = true;
        out.deviceDelivered = stats.device_delivered;
        out.deviceTransmissionError = stats.device_transmission_error;
        out.deviceTransformUnderrun = stats.device_transform_underrun;
        out.deviceUnderrun = stats.device_underrun;
        out.transformDelivered = stats.transform_delivered;
        out.transformUnderrun = stats.transform_underrun;
        out.sinkDelivered = stats.sink_delivered;
        out.sinkUnderrun = stats.sink_underrun;
        out.sinkIgnored = stats.sink_ignored;
    } catch (...) {
        return IC4StreamStatistics{};
    }
    return out;
}

inline void TryAppendTemperature(ic4::PropertyMap& props,
                                 const std::string& selector,
                                 std::vector<CameraTemperatureReading>& readings) noexcept
{
    try {
        ic4::Error err;
        const double celsius = props.getValueDouble(ic4::PropId::DeviceTemperature, err);
        if (!err.isError()) {
            CameraTemperatureReading reading;
            reading.hasValue = true;
            reading.selector = selector;
            reading.celsius = celsius;
            readings.push_back(std::move(reading));
        }
    } catch (...) {
    }
}

inline std::vector<CameraTemperatureReading> ReadDeviceTemperatures(ic4::Grabber* grabber) noexcept
{
    std::vector<CameraTemperatureReading> readings;
    if (!grabber || !(*grabber)) {
        return readings;
    }

    try {
        ic4::Error propErr;
        ic4::PropertyMap props = grabber->devicePropertyMap(propErr);
        if (propErr.isError() || !props) {
            return readings;
        }

        ic4::Error selectorErr;
        auto selector = props.find(ic4::PropId::DeviceTemperatureSelector, selectorErr);
        if (!selectorErr.isError() && selector) {
            ic4::Error originalErr;
            const std::string original = selector.getValue(originalErr);

            ic4::Error entriesErr;
            auto entries = selector.entries(entriesErr);
            if (!entriesErr.isError()) {
                for (const auto& entry : entries) {
                    ic4::Error nameErr;
                    const std::string name = entry.name(nameErr);
                    if (nameErr.isError() || name.empty()) {
                        continue;
                    }
                    ic4::Error setErr;
                    if (!selector.setValue(name, setErr) || setErr.isError()) {
                        continue;
                    }
                    TryAppendTemperature(props, name, readings);
                }
            }

            if (!original.empty()) {
                ic4::Error restoreErr;
                selector.setValue(original, restoreErr);
            }
            return readings;
        }

        TryAppendTemperature(props, std::string{}, readings);
    } catch (...) {
        return {};
    }

    return readings;
}

} // namespace Internal
} // namespace IC4Ext

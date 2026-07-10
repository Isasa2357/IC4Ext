#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/Error.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace IC4Ext {

enum class CameraControlCommandType : std::uint32_t
{
    ApplyIC4StateJson = 1,
    SetFrameRate,
    SetExposureAuto,
    SetExposureTime,
    SetGainAuto,
    SetGain,
    SetGamma,
    SetOffset,
    SetRoi,
    SetPixelFormat,
    SetPropertyBool,
    SetPropertyInt64,
    SetPropertyDouble,
    SetPropertyString,
};

struct CameraControlCommand
{
    CameraControlCommandType type = CameraControlCommandType::SetPropertyString;

    std::string propertyName;
    std::string stringValue;
    bool boolValue = false;
    std::int64_t int64Value = 0;
    double doubleValue = 0.0;

    int width = 0;
    int height = 0;
    int offsetX = 0;
    int offsetY = 0;

    CameraPixelFormat pixelFormat = CameraPixelFormat::BayerRG8;

    std::filesystem::path jsonPath;
    std::size_t jsonDeviceIndex = 0;
    bool jsonStrict = false;
    bool jsonApplyNestedSelectorStates = true;

    static CameraControlCommand ApplyJson(std::filesystem::path path,
                                          std::size_t deviceIndex = 0,
                                          bool strict = false,
                                          bool applyNestedSelectorStates = true)
    {
        CameraControlCommand cmd;
        cmd.type = CameraControlCommandType::ApplyIC4StateJson;
        cmd.jsonPath = std::move(path);
        cmd.jsonDeviceIndex = deviceIndex;
        cmd.jsonStrict = strict;
        cmd.jsonApplyNestedSelectorStates = applyNestedSelectorStates;
        return cmd;
    }

    static CameraControlCommand PropertyBool(std::string name, bool value)
    {
        CameraControlCommand cmd;
        cmd.type = CameraControlCommandType::SetPropertyBool;
        cmd.propertyName = std::move(name);
        cmd.boolValue = value;
        return cmd;
    }

    static CameraControlCommand PropertyInt64(std::string name, std::int64_t value)
    {
        CameraControlCommand cmd;
        cmd.type = CameraControlCommandType::SetPropertyInt64;
        cmd.propertyName = std::move(name);
        cmd.int64Value = value;
        return cmd;
    }

    static CameraControlCommand PropertyDouble(std::string name, double value)
    {
        CameraControlCommand cmd;
        cmd.type = CameraControlCommandType::SetPropertyDouble;
        cmd.propertyName = std::move(name);
        cmd.doubleValue = value;
        return cmd;
    }

    static CameraControlCommand PropertyString(std::string name, std::string value)
    {
        CameraControlCommand cmd;
        cmd.type = CameraControlCommandType::SetPropertyString;
        cmd.propertyName = std::move(name);
        cmd.stringValue = std::move(value);
        return cmd;
    }

    static CameraControlCommand FrameRate(double fps)
    {
        CameraControlCommand cmd;
        cmd.type = CameraControlCommandType::SetFrameRate;
        cmd.doubleValue = fps;
        return cmd;
    }

    static CameraControlCommand ExposureAuto(std::string mode)
    {
        CameraControlCommand cmd;
        cmd.type = CameraControlCommandType::SetExposureAuto;
        cmd.stringValue = std::move(mode);
        return cmd;
    }

    static CameraControlCommand ExposureTime(double exposureTimeUs)
    {
        CameraControlCommand cmd;
        cmd.type = CameraControlCommandType::SetExposureTime;
        cmd.doubleValue = exposureTimeUs;
        return cmd;
    }

    static CameraControlCommand GainAuto(std::string mode)
    {
        CameraControlCommand cmd;
        cmd.type = CameraControlCommandType::SetGainAuto;
        cmd.stringValue = std::move(mode);
        return cmd;
    }

    static CameraControlCommand Gain(double gain)
    {
        CameraControlCommand cmd;
        cmd.type = CameraControlCommandType::SetGain;
        cmd.doubleValue = gain;
        return cmd;
    }

    static CameraControlCommand Gamma(double gamma)
    {
        CameraControlCommand cmd;
        cmd.type = CameraControlCommandType::SetGamma;
        cmd.doubleValue = gamma;
        return cmd;
    }

    static CameraControlCommand Offset(int offsetX, int offsetY)
    {
        CameraControlCommand cmd;
        cmd.type = CameraControlCommandType::SetOffset;
        cmd.offsetX = offsetX;
        cmd.offsetY = offsetY;
        return cmd;
    }

    static CameraControlCommand Roi(int width, int height, int offsetX, int offsetY)
    {
        CameraControlCommand cmd;
        cmd.type = CameraControlCommandType::SetRoi;
        cmd.width = width;
        cmd.height = height;
        cmd.offsetX = offsetX;
        cmd.offsetY = offsetY;
        return cmd;
    }

    static CameraControlCommand PixelFormat(CameraPixelFormat fmt)
    {
        CameraControlCommand cmd;
        cmd.type = CameraControlCommandType::SetPixelFormat;
        cmd.pixelFormat = fmt;
        return cmd;
    }
};

class ICameraControlSink
{
public:
    virtual ~ICameraControlSink() = default;

    // Synchronous command application. Implementations return the result of the
    // underlying real camera operation. DummyCameraCapture forwards its setters here.
    virtual bool submitControlCommand(const CameraControlCommand& command) = 0;

    virtual const ErrorInfo& lastError() const noexcept = 0;
};

} // namespace IC4Ext

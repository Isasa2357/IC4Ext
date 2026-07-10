#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/CameraControl.hpp"
#include "IC4Ext/Core/Error.hpp"
#include "IC4Ext/D3D11/D3D11CameraFrame.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace IC4Ext {

using D3D11CameraControlCommand = CameraControlCommand;
using ID3D11CameraControlSink = ICameraControlSink;

class ID3D11Camera
{
public:
    virtual ~ID3D11Camera() = default;

    virtual bool isOpened() const noexcept = 0;

    virtual ReadResult read(ReadMode mode = ReadMode::LatestFrame) = 0;
    virtual ReadResult read(const CameraReadOptions& options) = 0;

    virtual bool applyIC4StateJson(const std::filesystem::path& jsonPath,
                                   std::size_t deviceIndex = 0,
                                   bool strict = false,
                                   bool applyNestedSelectorStates = true) = 0;

    virtual bool setIC4Property(const std::string& propertyName, bool value) = 0;
    virtual bool setIC4Property(const std::string& propertyName, int value) = 0;
    virtual bool setIC4Property(const std::string& propertyName, std::int64_t value) = 0;
    virtual bool setIC4Property(const std::string& propertyName, double value) = 0;
    virtual bool setIC4Property(const std::string& propertyName, const char* value) = 0;
    virtual bool setIC4Property(const std::string& propertyName, const std::string& value) = 0;

    virtual bool setFrameRate(double fps) = 0;
    virtual bool setExposureAuto(const std::string& mode) = 0;
    virtual bool setExposureTime(double exposureTimeUs) = 0;
    virtual bool setGainAuto(const std::string& mode) = 0;
    virtual bool setGain(double gain) = 0;
    virtual bool setGamma(double gamma) = 0;
    virtual bool setOffset(int offsetX, int offsetY) = 0;
    virtual bool setRoi(int width, int height, int offsetX, int offsetY) = 0;
    virtual bool setPixelFormat(CameraPixelFormat fmt) = 0;

    virtual const ErrorInfo& lastError() const noexcept = 0;
};

} // namespace IC4Ext

#pragma once

#include <IC4Ext/IC4Ext.hpp>
#include <ic4/ic4.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace IC4ExtTest {

inline const char* Env(const char* name)
{
    return std::getenv(name);
}

inline const char* Env(const std::string& name)
{
    return std::getenv(name.c_str());
}

inline int EnvInt(const char* name, int fallback)
{
    if (const char* value = Env(name)) return std::atoi(value);
    return fallback;
}

inline double EnvDouble(const char* name, double fallback)
{
    if (const char* value = Env(name)) return std::atof(value);
    return fallback;
}

inline unsigned CameraCooldownMs()
{
    const int value = EnvInt("IC4EXT_TEST_CAMERA_COOLDOWN_MS", 2000);
    return value > 0 ? static_cast<unsigned>(value) : 0u;
}

inline void SleepAfterCameraAccess()
{
    const unsigned ms = CameraCooldownMs();
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

class CameraAccessCooldown final
{
public:
    ~CameraAccessCooldown()
    {
        SleepAfterCameraAccess();
    }
};

inline std::size_t CountIC4Devices()
{
    ic4::InitLibraryConfig cfg;
    cfg.defaultErrorHandlerBehavior = ic4::ErrorHandlerBehavior::Ignore;

    const bool initialized = ic4::initLibrary(cfg);
    if (!initialized) {
        std::cerr << "IC4 library initialization failed while enumerating cameras\n";
        return 0;
    }

    std::size_t count = 0;

    // All IC4 wrapper objects must be destroyed before exitLibrary(). In particular,
    // DeviceInfo and Error can retain library-owned state whose destructors require
    // the IC4 runtime to remain initialized.
    {
        ic4::Error err;
        auto devices = ic4::DeviceEnum::enumDevices(err);
        if (err.isError()) {
            std::cerr << "IC4 device enumeration failed: " << err.message() << "\n";
        } else {
            count = devices.size();
        }
    }

    ic4::exitLibrary();
    return count;
}

inline bool RequireCameraCount(std::size_t required)
{
    const std::size_t actual = CountIC4Devices();
    if (actual < required) {
        std::cerr << "Skipping camera test: requires " << required
                  << " IC4 camera(s), found " << actual << "\n";
        return false;
    }
    return true;
}

inline std::filesystem::path CameraJsonForIndex(int cameraIndex)
{
    const std::string indexed =
        std::string("IC4EXT_TEST_IC4_JSON_") + std::to_string(cameraIndex);
    if (const char* value = Env(indexed)) return value;
    if (const char* value = Env("IC4EXT_TEST_IC4_JSON")) return value;
    return {};
}

inline std::size_t CameraJsonDeviceIndex(int cameraIndex)
{
    const std::string indexed =
        std::string("IC4EXT_TEST_IC4_JSON_DEVICE_INDEX_") + std::to_string(cameraIndex);
    if (const char* value = Env(indexed)) {
        return static_cast<std::size_t>(std::max(0, std::atoi(value)));
    }
    if (const char* value = Env("IC4EXT_TEST_IC4_JSON_DEVICE_INDEX")) {
        return static_cast<std::size_t>(std::max(0, std::atoi(value)));
    }

    // A shared IC Capture state file commonly contains a single devices[0].state
    // entry that is intentionally applied to every identical camera.
    return 0;
}

inline IC4Ext::CameraCaptureConfig MakeCameraConfig(const char* shaderBackend,
                                                     int cameraIndex)
{
    IC4Ext::CameraCaptureConfig config;

    const auto jsonPath = CameraJsonForIndex(cameraIndex);
    if (!jsonPath.empty()) {
        config.ic4StateJson.path = jsonPath;
        config.ic4StateJson.deviceIndex = CameraJsonDeviceIndex(cameraIndex);
    } else {
        IC4Ext::CameraPixelFormat fmt = IC4Ext::CameraPixelFormat::BGR8;
        if (const char* text = Env("IC4EXT_TEST_FORMAT")) {
            IC4Ext::ParseCameraPixelFormat(text, fmt);
        }
        config.streamRequest.requestedFormat = fmt;
        config.streamRequest.forceRequestedFormat = true;
    }

    const int width = EnvInt("IC4EXT_TEST_WIDTH", 0);
    const int height = EnvInt("IC4EXT_TEST_HEIGHT", 0);
    const double fps = EnvDouble("IC4EXT_TEST_FPS", 0.0);
    if (width > 0) config.streamRequest.width = width;
    if (height > 0) config.streamRequest.height = height;
    if (fps > 0.0) config.streamRequest.fps = fps;

    if (const char* ox = Env("IC4EXT_TEST_OFFSET_X")) {
        config.streamRequest.offsetX = std::atoi(ox);
    }
    if (const char* oy = Env("IC4EXT_TEST_OFFSET_Y")) {
        config.streamRequest.offsetY = std::atoi(oy);
    }

    config.outputSpec.outputFormat = IC4Ext::GpuFrameFormat::RGBA8;
    config.shaderConfig.shaderDirectory =
        std::filesystem::current_path() / "shaders" / shaderBackend;
    return config;
}

inline IC4Ext::CameraReadOptions ReadOptions(unsigned timeoutMs = 3000)
{
    IC4Ext::CameraReadOptions options;
    options.mode = IC4Ext::ReadMode::NextFrame;
    options.timeoutMs = timeoutMs;
    return options;
}

} // namespace IC4ExtTest

#include <IC4Ext/IC4Ext.hpp>
#include <D3D11Helper/D3D11Core/D3D11Core.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

namespace {

IC4Ext::CameraPixelFormat ParseCameraFormat(const std::string& s)
{
    if (s == "Mono8") return IC4Ext::CameraPixelFormat::Mono8;
    if (s == "BayerRG8") return IC4Ext::CameraPixelFormat::BayerRG8;
    if (s == "BayerGR8") return IC4Ext::CameraPixelFormat::BayerGR8;
    if (s == "BayerGB8") return IC4Ext::CameraPixelFormat::BayerGB8;
    if (s == "BayerBG8") return IC4Ext::CameraPixelFormat::BayerBG8;
    if (s == "BGR8") return IC4Ext::CameraPixelFormat::BGR8;
    if (s == "BGRa8") return IC4Ext::CameraPixelFormat::BGRa8;
    return IC4Ext::CameraPixelFormat::BGR8;
}

IC4Ext::GpuFrameFormat ParseOutputFormat(const std::string& s)
{
    if (s == "R8") return IC4Ext::GpuFrameFormat::R8;
    return IC4Ext::GpuFrameFormat::RGBA8;
}

const char* ArgValue(int argc, char** argv, const char* name)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == name) return argv[i + 1];
    }
    return nullptr;
}

} // namespace

int main(int argc, char** argv)
{
    const int deviceIndex = ArgValue(argc, argv, "--device-index") ? std::atoi(ArgValue(argc, argv, "--device-index")) : 0;
    const int width = ArgValue(argc, argv, "--width") ? std::atoi(ArgValue(argc, argv, "--width")) : 0;
    const int height = ArgValue(argc, argv, "--height") ? std::atoi(ArgValue(argc, argv, "--height")) : 0;
    const int offsetX = ArgValue(argc, argv, "--offset-x") ? std::atoi(ArgValue(argc, argv, "--offset-x")) : -1;
    const int offsetY = ArgValue(argc, argv, "--offset-y") ? std::atoi(ArgValue(argc, argv, "--offset-y")) : -1;
    const double fps = ArgValue(argc, argv, "--fps") ? std::atof(ArgValue(argc, argv, "--fps")) : 0.0;
    const int maxFrames = ArgValue(argc, argv, "--frames") ? std::atoi(ArgValue(argc, argv, "--frames")) : 300;
    const char* jsonPath = ArgValue(argc, argv, "--ic4-json");
    const bool forceFormat = ArgValue(argc, argv, "--force-format") != nullptr;
    const auto cameraFormat = ParseCameraFormat(ArgValue(argc, argv, "--format") ? ArgValue(argc, argv, "--format") : "BGR8");
    const auto outputFormat = ParseOutputFormat(ArgValue(argc, argv, "--output") ? ArgValue(argc, argv, "--output") : "RGBA8");

    auto core = D3D11CoreLib::D3D11Core::CreateShared();

    IC4Ext::IC4DeviceSelector selector;
    selector.deviceIndex = deviceIndex;

    IC4Ext::CameraCaptureConfig config;
    if (jsonPath) {
        config.ic4StateJson.path = jsonPath;
        config.ic4StateJson.deviceIndex = 0;
        config.ic4StateJson.strict = false;
    }
    config.streamRequest.width = width;
    config.streamRequest.height = height;
    config.streamRequest.fps = fps;
    config.streamRequest.requestedFormat = cameraFormat;
    config.streamRequest.forceRequestedFormat = forceFormat;
    if (offsetX >= 0) config.streamRequest.offsetX = offsetX;
    if (offsetY >= 0) config.streamRequest.offsetY = offsetY;
    config.outputSpec.outputFormat = outputFormat;
    config.queuePolicy = IC4Ext::FrameQueuePolicy::LatestOnly;
    config.maxPendingBuffers = 1;
    config.shaderConfig.shaderDirectory = std::filesystem::current_path() / "shaders" / "d3d11";
    config.shaderConfig.inputKind = IC4Ext::ShaderInputKind::Auto;

    IC4Ext::D3D11CameraCapture capture;
    if (!capture.open(selector, config, core.get())) {
        std::cerr << "open failed: " << capture.lastError().where << ": " << capture.lastError().message << std::endl;
        return 1;
    }

    std::cout << "opened camera index=" << deviceIndex;
    if (jsonPath) std::cout << " json=" << jsonPath;
    if (offsetX >= 0 || offsetY >= 0) std::cout << " offset=(" << offsetX << "," << offsetY << ")";
    std::cout << std::endl;
    for (int i = 0; maxFrames <= 0 || i < maxFrames; ++i) {
        auto result = capture.read(IC4Ext::ReadMode::LatestFrame);
        if (!result) {
            if (result.error.code != static_cast<int>(IC4Ext::ErrorCode::Timeout)) {
                std::cerr << "read failed: " << result.error.where << ": " << result.error.message << std::endl;
            }
            continue;
        }

        result.frame.ready.wait(1000);
        std::cout << "frame=" << result.frame.timing.frameNumber
                  << " timestampNs=" << result.frame.timing.deviceTimestampNs
                  << " " << result.frame.format.width << "x" << result.frame.format.height
                  << " input=" << IC4Ext::ToString(result.frame.format.actualInputFormat)
                  << " output=" << IC4Ext::ToString(result.frame.format.outputFormat)
                  << " ready=" << result.frame.ready.isReady()
                  << std::endl;
    }

    capture.close();
    return 0;
}

#include <IC4Ext/IC4Ext.hpp>

#include <D3D12Helper/D3D12Core/D3D12Core.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

namespace {

const char* ArgValue(int argc, char** argv, const char* name)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == name) return argv[i + 1];
    }
    return nullptr;
}

bool HasArg(int argc, char** argv, const char* name)
{
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == name) return true;
    }
    return false;
}

IC4Ext::CameraPixelFormat ParseCameraFormat(const std::string& s)
{
    IC4Ext::CameraPixelFormat fmt{};
    if (IC4Ext::ParseCameraPixelFormat(s, fmt)) return fmt;
    return IC4Ext::CameraPixelFormat::BGR8;
}

IC4Ext::GpuFrameFormat ParseOutputFormat(const std::string& s)
{
    if (s == "R8") return IC4Ext::GpuFrameFormat::R8;
    return IC4Ext::GpuFrameFormat::RGBA8;
}

bool CreateD3D12Backend(IC4Ext::D3D12BackendContext& outBackend)
{
    try {
        auto core = D3D12CoreLib::D3D12Core::CreateShared();
        outBackend = IC4Ext::D3D12BackendContext::FromCore(core, IC4Ext::D3D12BackendQueueKind::Direct);
        return outBackend.resolve();
    } catch (const std::exception& e) {
        std::cerr << "D3D12Helper core creation failed: " << e.what() << std::endl;
        return false;
    }
}

} // namespace

int main(int argc, char** argv)
{
    IC4Ext::D3D12BackendContext backend;
    if (!CreateD3D12Backend(backend)) {
        std::cerr << "Failed to create D3D12Helper backend context\n";
        return 1;
    }

    const int deviceIndex = ArgValue(argc, argv, "--device-index") ? std::atoi(ArgValue(argc, argv, "--device-index")) : 0;
    const int width = ArgValue(argc, argv, "--width") ? std::atoi(ArgValue(argc, argv, "--width")) : 0;
    const int height = ArgValue(argc, argv, "--height") ? std::atoi(ArgValue(argc, argv, "--height")) : 0;
    const int offsetX = ArgValue(argc, argv, "--offset-x") ? std::atoi(ArgValue(argc, argv, "--offset-x")) : -1;
    const int offsetY = ArgValue(argc, argv, "--offset-y") ? std::atoi(ArgValue(argc, argv, "--offset-y")) : -1;
    const double fps = ArgValue(argc, argv, "--fps") ? std::atof(ArgValue(argc, argv, "--fps")) : 0.0;
    const int maxFrames = ArgValue(argc, argv, "--frames") ? std::atoi(ArgValue(argc, argv, "--frames")) : 300;
    const char* jsonPath = ArgValue(argc, argv, "--ic4-json");
    const bool forceFormat = HasArg(argc, argv, "--force-format");
    const auto cameraFormat = ParseCameraFormat(ArgValue(argc, argv, "--format") ? ArgValue(argc, argv, "--format") : "BGR8");
    const auto outputFormat = ParseOutputFormat(ArgValue(argc, argv, "--output") ? ArgValue(argc, argv, "--output") : "RGBA8");

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
    config.shaderConfig.shaderDirectory = std::filesystem::current_path() / "shaders" / "d3d12";
    config.shaderConfig.inputKind = IC4Ext::ShaderInputKind::Auto;
    config.queuePolicy = IC4Ext::FrameQueuePolicy::LatestOnly;
    config.maxPendingBuffers = 1;

    IC4Ext::D3D12CameraCapture capture;
    if (!capture.open(selector, config, backend)) {
        std::cerr << "open failed: " << capture.lastError().where << ": " << capture.lastError().message << std::endl;
        return 1;
    }

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
                  << " dxgi=" << static_cast<int>(result.frame.dxgiFormat)
                  << " ready=" << result.frame.ready.isReady()
                  << std::endl;
    }
    return 0;
}

#include <IC4Ext/IC4Ext.hpp>

#include <D3D12Helper/D3D12Core/D3D12Core.hpp>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
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

IC4Ext::CpuFrameFormat ParseCpuFormat(const std::string& s)
{
    if (s == "Gray8" || s == "Gray" || s == "Mono8") return IC4Ext::CpuFrameFormat::Gray8;
    if (s == "RGBA8" || s == "RGBA") return IC4Ext::CpuFrameFormat::RGBA8;
    if (s == "RGB8" || s == "RGB") return IC4Ext::CpuFrameFormat::RGB8;
    if (s == "BGR8" || s == "BGR") return IC4Ext::CpuFrameFormat::BGR8;
    return IC4Ext::CpuFrameFormat::BGR8;
}

bool SaveNetpbm(const IC4Ext::CpuFrame& frame, const std::filesystem::path& path)
{
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;

    if (frame.format == IC4Ext::CpuFrameFormat::Gray8) {
        ofs << "P5\n" << frame.width << " " << frame.height << "\n255\n";
        for (std::uint32_t y = 0; y < frame.height; ++y) {
            const auto* row = frame.data.data() + static_cast<std::size_t>(y) * frame.rowPitch;
            ofs.write(reinterpret_cast<const char*>(row), frame.width);
        }
        return static_cast<bool>(ofs);
    }

    ofs << "P6\n" << frame.width << " " << frame.height << "\n255\n";
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        const auto* row = frame.data.data() + static_cast<std::size_t>(y) * frame.rowPitch;
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            char rgb[3] = {};
            if (frame.format == IC4Ext::CpuFrameFormat::RGB8) {
                rgb[0] = static_cast<char>(row[x * 3u + 0u]);
                rgb[1] = static_cast<char>(row[x * 3u + 1u]);
                rgb[2] = static_cast<char>(row[x * 3u + 2u]);
            } else if (frame.format == IC4Ext::CpuFrameFormat::BGR8) {
                rgb[0] = static_cast<char>(row[x * 3u + 2u]);
                rgb[1] = static_cast<char>(row[x * 3u + 1u]);
                rgb[2] = static_cast<char>(row[x * 3u + 0u]);
            } else if (frame.format == IC4Ext::CpuFrameFormat::RGBA8) {
                rgb[0] = static_cast<char>(row[x * 4u + 0u]);
                rgb[1] = static_cast<char>(row[x * 4u + 1u]);
                rgb[2] = static_cast<char>(row[x * 4u + 2u]);
            } else {
                return false;
            }
            ofs.write(rgb, 3);
        }
    }
    return static_cast<bool>(ofs);
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
    const char* jsonPath = ArgValue(argc, argv, "--ic4-json");
    const bool forceFormat = HasArg(argc, argv, "--force-format");
    const auto cameraFormat = ParseCameraFormat(ArgValue(argc, argv, "--format") ? ArgValue(argc, argv, "--format") : "BGR8");
    const auto outputFormat = ParseOutputFormat(ArgValue(argc, argv, "--output") ? ArgValue(argc, argv, "--output") : "RGBA8");
    const auto cpuFormat = ParseCpuFormat(ArgValue(argc, argv, "--cpu-format") ? ArgValue(argc, argv, "--cpu-format") : "BGR8");
    const std::filesystem::path outPath = ArgValue(argc, argv, "--out") ? ArgValue(argc, argv, "--out") : (cpuFormat == IC4Ext::CpuFrameFormat::Gray8 ? "frame_d3d12.pgm" : "frame_d3d12.ppm");

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
    config.queuePolicy = IC4Ext::FrameQueuePolicy::LatestOnly;
    config.maxPendingBuffers = 1;

    IC4Ext::D3D12CameraCapture capture;
    if (!capture.open(selector, config, backend)) {
        std::cerr << "open failed: " << capture.lastError().where << ": " << capture.lastError().message << std::endl;
        return 1;
    }

    auto result = capture.read(IC4Ext::ReadMode::LatestFrame);
    if (!result) {
        std::cerr << "read failed: " << result.error.where << ": " << result.error.message << std::endl;
        return 1;
    }

    IC4Ext::D3D12FrameReadback readback;
    if (!readback.initialize(backend)) {
        std::cerr << "readback initialize failed: " << readback.lastError().where << ": " << readback.lastError().message << std::endl;
        return 1;
    }

    IC4Ext::CpuFrame cpu;
    if (!readback.readback(result.frame, cpuFormat, cpu, 5000)) {
        std::cerr << "readback failed: " << readback.lastError().where << ": " << readback.lastError().message << std::endl;
        return 1;
    }

    if (!SaveNetpbm(cpu, outPath)) {
        std::cerr << "failed to save " << outPath << std::endl;
        return 1;
    }

    std::cout << "saved " << outPath << " frame=" << cpu.timing.frameNumber
              << " timestampNs=" << cpu.timing.deviceTimestampNs
              << " " << cpu.width << "x" << cpu.height
              << " cpuFormat=" << IC4Ext::ToString(cpu.format) << std::endl;
    return 0;
}

#include <IC4Ext/D3D12/ReadOnlyPipeline.hpp>
#include <IC4Ext/D3D12/D3D12FrameReadback.hpp>

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

int ArgInt(int argc, char** argv, const char* name, int fallback)
{
    const char* value = ArgValue(argc, argv, name);
    return value ? std::atoi(value) : fallback;
}

double ArgDouble(int argc, char** argv, const char* name, double fallback)
{
    const char* value = ArgValue(argc, argv, name);
    return value ? std::atof(value) : fallback;
}

IC4Ext::CameraPixelFormat ParseCameraFormat(const std::string& text)
{
    IC4Ext::CameraPixelFormat format{};
    if (IC4Ext::ParseCameraPixelFormat(text, format)) return format;
    return IC4Ext::CameraPixelFormat::BGR8;
}

IC4Ext::GpuFrameFormat ParseOutputFormat(const std::string& text)
{
    if (text == "R8") return IC4Ext::GpuFrameFormat::R8;
    return IC4Ext::GpuFrameFormat::RGBA8;
}

IC4Ext::CpuFrameFormat ParseCpuFormat(const std::string& text)
{
    if (text == "Gray8" || text == "Gray" || text == "Mono8") return IC4Ext::CpuFrameFormat::Gray8;
    if (text == "RGBA8" || text == "RGBA") return IC4Ext::CpuFrameFormat::RGBA8;
    if (text == "RGB8" || text == "RGB") return IC4Ext::CpuFrameFormat::RGB8;
    if (text == "BGR8" || text == "BGR") return IC4Ext::CpuFrameFormat::BGR8;
    return IC4Ext::CpuFrameFormat::BGR8;
}

bool SaveNetpbm(const IC4Ext::CpuFrame& frame, const std::filesystem::path& path)
{
    std::ofstream stream(path, std::ios::binary);
    if (!stream) return false;

    if (frame.format == IC4Ext::CpuFrameFormat::Gray8) {
        stream << "P5\n" << frame.width << " " << frame.height << "\n255\n";
        for (std::uint32_t y = 0; y < frame.height; ++y) {
            const auto* row = frame.data.data() + static_cast<std::size_t>(y) * frame.rowPitch;
            stream.write(reinterpret_cast<const char*>(row), frame.width);
        }
        return static_cast<bool>(stream);
    }

    stream << "P6\n" << frame.width << " " << frame.height << "\n255\n";
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
            stream.write(rgb, 3);
        }
    }
    return static_cast<bool>(stream);
}

bool CreateD3D12Backend(IC4Ext::D3D12BackendContext& outBackend)
{
    try {
        auto core = D3D12CoreLib::D3D12Core::CreateShared();
        outBackend = IC4Ext::D3D12BackendContext::FromCore(
            core,
            IC4Ext::D3D12BackendQueueKind::Direct);
        return outBackend.resolve();
    } catch (const std::exception& exception) {
        std::cerr << "D3D12Helper core creation failed: " << exception.what() << std::endl;
        return false;
    }
}

IC4Ext::D3D12CameraFrame MakeLegacyFrameForReadback(
    const IC4Ext::D3D12::ReadOnlyFrame& frame)
{
    IC4Ext::D3D12CameraFrame legacy;
    legacy.texture = frame.resource();
    legacy.dxgiFormat = frame.dxgiFormat();
    legacy.resourceState = frame.publishedState();
    legacy.ready = frame.readyToken();
    legacy.timing = frame.timing();
    legacy.format = frame.format();
    legacy.chunkMetadata = frame.chunkMetadata();
    return legacy;
}

} // namespace

int main(int argc, char** argv)
{
    namespace Pipe = IC4Ext::D3D12;

    IC4Ext::D3D12BackendContext backend;
    if (!CreateD3D12Backend(backend)) {
        std::cerr << "Failed to create D3D12 backend context" << std::endl;
        return 1;
    }

    const int deviceIndex = ArgInt(argc, argv, "--device-index", 0);
    const int width = ArgInt(argc, argv, "--width", 0);
    const int height = ArgInt(argc, argv, "--height", 0);
    const int offsetX = ArgInt(argc, argv, "--offset-x", -1);
    const int offsetY = ArgInt(argc, argv, "--offset-y", -1);
    const int framesToRead = ArgInt(argc, argv, "--frames", 1);
    const int timeoutMs = ArgInt(argc, argv, "--timeout-ms", 5000);
    const int poolInitial = ArgInt(argc, argv, "--pool-initial", 8);
    const int poolMax = ArgInt(argc, argv, "--pool-max", 32);
    const double fps = ArgDouble(argc, argv, "--fps", 0.0);
    const char* jsonPath = ArgValue(argc, argv, "--ic4-json");
    const int jsonDeviceIndex = ArgInt(argc, argv, "--json-device-index", 0);
    const bool forceFormat = HasArg(argc, argv, "--force-format");

    const auto cameraFormat = ParseCameraFormat(
        ArgValue(argc, argv, "--format") ? ArgValue(argc, argv, "--format") : "BGR8");
    const auto outputFormat = ParseOutputFormat(
        ArgValue(argc, argv, "--output") ? ArgValue(argc, argv, "--output") : "RGBA8");
    const auto cpuFormat = ParseCpuFormat(
        ArgValue(argc, argv, "--cpu-format") ? ArgValue(argc, argv, "--cpu-format") : "BGR8");

    const std::filesystem::path outputPath = ArgValue(argc, argv, "--out")
        ? ArgValue(argc, argv, "--out")
        : (cpuFormat == IC4Ext::CpuFrameFormat::Gray8
               ? std::filesystem::path("readonly_frame_d3d12.pgm")
               : std::filesystem::path("readonly_frame_d3d12.ppm"));

    IC4Ext::IC4DeviceSelector selector;
    selector.deviceIndex = deviceIndex;

    IC4Ext::CameraCaptureConfig config;
    if (jsonPath) {
        config.ic4StateJson.path = jsonPath;
        config.ic4StateJson.deviceIndex = static_cast<std::size_t>(std::max(0, jsonDeviceIndex));
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

    Pipe::CameraCaptureOptions captureOptions;
    captureOptions.initialFramePoolCapacity = static_cast<std::size_t>(std::max(1, poolInitial));
    captureOptions.maxFramePoolCapacity = static_cast<std::size_t>(std::max(poolInitial, poolMax));

    Pipe::CameraCapture capture;
    if (!capture.open(selector, config, backend, captureOptions)) {
        const auto error = capture.lastError();
        std::cerr << "open failed: " << error.where << ": " << error.message << std::endl;
        return 1;
    }

    Pipe::ReadResult readResult;
    for (int i = 0; i < std::max(1, framesToRead); ++i) {
        readResult = capture.read(IC4Ext::CameraReadOptions{
            i == 0 ? IC4Ext::ReadMode::LatestFrame : IC4Ext::ReadMode::NextFrame,
            static_cast<std::uint32_t>(timeoutMs)});
        if (!readResult) {
            std::cerr << "read failed at frame " << i << ": "
                      << readResult.error.where << ": " << readResult.error.message << std::endl;
            return 1;
        }
    }

    IC4Ext::D3D12FrameReadback readback;
    if (!readback.initialize(backend)) {
        std::cerr << "readback initialize failed: "
                  << readback.lastError().where << ": " << readback.lastError().message << std::endl;
        return 1;
    }

    IC4Ext::CpuFrame cpu;
    auto legacyFrame = MakeLegacyFrameForReadback(readResult.frame);
    if (!readback.readback(legacyFrame, cpuFormat, cpu, static_cast<std::uint32_t>(timeoutMs))) {
        std::cerr << "readback failed: "
                  << readback.lastError().where << ": " << readback.lastError().message << std::endl;
        return 1;
    }

    if (!SaveNetpbm(cpu, outputPath)) {
        std::cerr << "failed to save " << outputPath << std::endl;
        return 1;
    }

    const auto poolStats = capture.framePoolStats();
    std::cout << "saved " << outputPath
              << " frame=" << cpu.timing.frameNumber
              << " timestampNs=" << cpu.timing.deviceTimestampNs
              << " size=" << cpu.width << "x" << cpu.height
              << " cpuFormat=" << IC4Ext::ToString(cpu.format)
              << " poolCapacity=" << poolStats.capacity
              << " poolAvailable=" << poolStats.available
              << " poolPublished=" << poolStats.published
              << std::endl;
    return 0;
}

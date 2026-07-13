#include <IC4Ext/D3D12/MultiCameraStartup.hpp>

#include <chrono>
#include <iostream>
#include <type_traits>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

bool IsInvalidArgument(const IC4Ext::ErrorInfo& error)
{
    return error.code == static_cast<int>(IC4Ext::ErrorCode::InvalidArgument);
}

} // namespace

int main()
{
    namespace Pipe = IC4Ext::D3D12;

    static_assert(std::is_move_constructible_v<Pipe::StartedCameraCapture>);
    static_assert(!std::is_copy_constructible_v<Pipe::StartedCameraCapture>);
    static_assert(std::is_move_constructible_v<Pipe::MultiCameraStartupResult>);
    static_assert(!std::is_copy_constructible_v<Pipe::MultiCameraStartupResult>);

    Pipe::D3D12BackendContext backend;

    {
        const auto result = Pipe::OpenAndStartMultiCameraGroup(
            backend,
            std::vector<Pipe::CameraCaptureStartupConfig>{},
            std::vector<Pipe::CameraCaptureThreadStartupConfig>{});

        Check(!result, "empty startup request must fail");
        Check(IsInvalidArgument(result.error),
              "empty startup request must report InvalidArgument");
        Check(result.captures.empty(), "failed result must contain no captures");
        Check(result.captureThreads.empty(), "failed result must contain no threads");
    }

    {
        Pipe::CameraCaptureStartupConfig first;
        first.cameraId = 7;
        Pipe::CameraCaptureStartupConfig second;
        second.cameraId = 7;

        const auto result = Pipe::OpenAndStartMultiCameraGroup(
            backend,
            std::vector<Pipe::CameraCaptureStartupConfig>{first, second},
            std::vector<Pipe::CameraCaptureThreadStartupConfig>{});

        Check(!result, "duplicate camera IDs must fail before opening a camera");
        Check(IsInvalidArgument(result.error),
              "duplicate camera IDs must report InvalidArgument");
    }

    {
        Pipe::CameraCaptureThreadStartupConfig threaded;
        threaded.capture.cameraId = 3;

        const auto result = Pipe::OpenAndStartMultiCameraGroup(
            backend,
            std::vector<Pipe::CameraCaptureStartupConfig>{},
            std::vector<Pipe::CameraCaptureThreadStartupConfig>{threaded});

        Check(!result, "threaded startup with a null output queue must fail");
        Check(IsInvalidArgument(result.error),
              "null output queue must report InvalidArgument");
    }

    {
        Pipe::CameraCaptureStartupConfig direct;
        direct.cameraId = 1;

        Pipe::MultiCameraStartupOptions options;
        options.interCameraOpenDelay = std::chrono::milliseconds(-1);

        const auto result = Pipe::OpenAndStartMultiCameraGroup(
            backend,
            std::vector<Pipe::CameraCaptureStartupConfig>{direct},
            std::vector<Pipe::CameraCaptureThreadStartupConfig>{},
            options);

        Check(!result, "negative inter-camera delay must fail");
        Check(IsInvalidArgument(result.error),
              "negative inter-camera delay must report InvalidArgument");
    }

    if (failures != 0) {
        std::cerr << failures << " multi-camera startup test(s) failed\n";
        return 1;
    }

    std::cout << "D3D12 multi-camera startup validation passed\n";
    return 0;
}

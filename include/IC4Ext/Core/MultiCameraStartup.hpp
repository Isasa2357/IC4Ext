#pragma once

#include "IC4Ext/Config.hpp"
#include "IC4Ext/Core/Error.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace IC4Ext {

struct MultiCameraStartupOptions
{
    // Optional delay after one camera has been opened and paused, before the
    // next camera is opened. This is useful for camera/transport combinations
    // that need a short settling interval between device opens.
    std::chrono::milliseconds interCameraOpenDelay{0};

    bool isValid() const noexcept
    {
        return interCameraOpenDelay.count() >= 0;
    }
};

namespace Detail {

enum class MultiCameraStartupKind : std::uint32_t
{
    DirectCapture = 0,
    CaptureThread = 1,
};

inline ErrorInfo MakeMultiCameraStartupValidationError(std::string message)
{
    return MakeError(
        ErrorCode::InvalidArgument,
        "OpenAndStartMultiCameraGroup",
        std::move(message));
}

template <class CameraIdType>
ErrorInfo AddMultiCameraStartupContext(
    ErrorInfo error,
    const char* stage,
    CameraIdType cameraId)
{
    const std::string context =
        "cameraId=" +
        std::to_string(static_cast<std::uint64_t>(cameraId)) +
        " stage=" + stage;

    if (!error) {
        return MakeError(
            ErrorCode::InternalError,
            "OpenAndStartMultiCameraGroup",
            context + ": operation failed without an error");
    }

    error.where = std::string("OpenAndStartMultiCameraGroup/") +
                  stage + "/" + error.where;
    error.message = context + ": " + error.message;
    return error;
}

template <
    class BackendContext,
    class Capture,
    class CaptureThread,
    class CaptureStartupConfig,
    class CaptureThreadStartupConfig,
    class StartedCapture,
    class Result>
Result OpenAndStartMultiCameraGroupImpl(
    const BackendContext& backend,
    const std::vector<CaptureStartupConfig>& captureConfigs,
    const std::vector<CaptureThreadStartupConfig>& captureThreadConfigs,
    MultiCameraStartupOptions options)
{
    Result result;

    if (!options.isValid()) {
        result.error = MakeMultiCameraStartupValidationError(
            "interCameraOpenDelay must not be negative");
        return result;
    }

    const std::size_t requestCount =
        captureConfigs.size() + captureThreadConfigs.size();
    if (requestCount == 0) {
        result.error = MakeMultiCameraStartupValidationError(
            "At least one CameraCapture or CameraCaptureThread configuration is required");
        return result;
    }

    std::unordered_set<std::uint64_t> cameraIds;
    cameraIds.reserve(requestCount);

    for (std::size_t index = 0; index < captureConfigs.size(); ++index) {
        const auto& config = captureConfigs[index];
        if (!config.captureOptions.isValid()) {
            result.error = MakeMultiCameraStartupValidationError(
                "Invalid CameraCaptureOptions at captureConfigs[" +
                std::to_string(index) + "]");
            return result;
        }

        const auto id = static_cast<std::uint64_t>(config.cameraId);
        if (!cameraIds.insert(id).second) {
            result.error = MakeMultiCameraStartupValidationError(
                "Duplicate cameraId=" + std::to_string(id));
            return result;
        }
    }

    for (std::size_t index = 0; index < captureThreadConfigs.size(); ++index) {
        const auto& config = captureThreadConfigs[index];
        if (!config.capture.captureOptions.isValid()) {
            result.error = MakeMultiCameraStartupValidationError(
                "Invalid CameraCaptureOptions at captureThreadConfigs[" +
                std::to_string(index) + "]");
            return result;
        }
        if (!config.threadOptions.isValid()) {
            result.error = MakeMultiCameraStartupValidationError(
                "Invalid CameraCaptureThreadOptions at captureThreadConfigs[" +
                std::to_string(index) + "]");
            return result;
        }
        if (!config.outputQueue) {
            result.error = MakeMultiCameraStartupValidationError(
                "outputQueue is null at captureThreadConfigs[" +
                std::to_string(index) + "]");
            return result;
        }

        const auto id = static_cast<std::uint64_t>(config.capture.cameraId);
        if (!cameraIds.insert(id).second) {
            result.error = MakeMultiCameraStartupValidationError(
                "Duplicate cameraId=" + std::to_string(id));
            return result;
        }
    }

    struct StartupRequest
    {
        MultiCameraStartupKind kind = MultiCameraStartupKind::DirectCapture;
        std::size_t configIndex = 0;
        std::uint64_t openOrder = 0;
        std::size_t insertionOrder = 0;
    };

    std::vector<StartupRequest> requests;
    requests.reserve(requestCount);

    std::size_t insertionOrder = 0;
    for (std::size_t index = 0; index < captureConfigs.size(); ++index) {
        requests.push_back(StartupRequest{
            MultiCameraStartupKind::DirectCapture,
            index,
            captureConfigs[index].openOrder,
            insertionOrder++});
    }
    for (std::size_t index = 0; index < captureThreadConfigs.size(); ++index) {
        requests.push_back(StartupRequest{
            MultiCameraStartupKind::CaptureThread,
            index,
            captureThreadConfigs[index].capture.openOrder,
            insertionOrder++});
    }

    std::stable_sort(
        requests.begin(),
        requests.end(),
        [](const StartupRequest& lhs, const StartupRequest& rhs) {
            if (lhs.openOrder != rhs.openOrder) {
                return lhs.openOrder < rhs.openOrder;
            }
            return lhs.insertionOrder < rhs.insertionOrder;
        });

    using CameraIdType = decltype(captureConfigs.front().cameraId);

    struct PreparedCamera
    {
        MultiCameraStartupKind kind = MultiCameraStartupKind::DirectCapture;
        std::size_t resultIndex = 0;
        CameraIdType cameraId{};
        bool acquisitionStartAttempted = false;
    };

    std::vector<PreparedCamera> prepared;
    prepared.reserve(requestCount);
    result.captures.reserve(captureConfigs.size());
    result.captureThreads.reserve(captureThreadConfigs.size());

    const auto rollback = [&]() noexcept {
        for (auto iterator = prepared.rbegin(); iterator != prepared.rend(); ++iterator) {
            if (!iterator->acquisitionStartAttempted) continue;
            try {
                if (iterator->kind == MultiCameraStartupKind::DirectCapture) {
                    result.captures[iterator->resultIndex].capture.stopAcquisition();
                } else {
                    auto& thread = result.captureThreads[iterator->resultIndex];
                    if (thread) thread->stopAcquisition();
                }
            } catch (...) {
            }
        }

        for (auto iterator = result.captureThreads.rbegin();
             iterator != result.captureThreads.rend();
             ++iterator) {
            try {
                if (*iterator) (*iterator)->stopAndJoin();
            } catch (...) {
            }
        }

        for (auto iterator = result.captures.rbegin();
             iterator != result.captures.rend();
             ++iterator) {
            try {
                iterator->capture.close();
            } catch (...) {
            }
        }

        result.captureThreads.clear();
        result.captures.clear();
        result.ok = false;
    };

    try {
        for (std::size_t requestIndex = 0;
             requestIndex < requests.size();
             ++requestIndex) {
            const auto& request = requests[requestIndex];

            if (request.kind == MultiCameraStartupKind::DirectCapture) {
                const auto& config = captureConfigs[request.configIndex];
                auto effectiveCaptureConfig = config.captureConfig;
                effectiveCaptureConfig.acquisitionStartMode =
                    AcquisitionStartMode::Immediate;

                Capture capture;
                if (!capture.open(
                        config.selector,
                        effectiveCaptureConfig,
                        backend,
                        config.captureOptions)) {
                    auto error = AddMultiCameraStartupContext(
                        capture.lastError(),
                        "open",
                        config.cameraId);
                    rollback();
                    result.error = std::move(error);
                    return result;
                }

                if (!capture.stopAcquisition()) {
                    auto error = AddMultiCameraStartupContext(
                        capture.lastError(),
                        "prepare-stop",
                        config.cameraId);
                    capture.close();
                    rollback();
                    result.error = std::move(error);
                    return result;
                }

                const std::size_t resultIndex = result.captures.size();
                result.captures.emplace_back(
                    config.cameraId,
                    std::move(capture));
                prepared.push_back(PreparedCamera{
                    MultiCameraStartupKind::DirectCapture,
                    resultIndex,
                    config.cameraId,
                    false});
            } else {
                const auto& config = captureThreadConfigs[request.configIndex];
                auto effectiveCaptureConfig = config.capture.captureConfig;
                effectiveCaptureConfig.acquisitionStartMode =
                    AcquisitionStartMode::Immediate;

                Capture capture;
                if (!capture.open(
                        config.capture.selector,
                        effectiveCaptureConfig,
                        backend,
                        config.capture.captureOptions)) {
                    auto error = AddMultiCameraStartupContext(
                        capture.lastError(),
                        "open",
                        config.capture.cameraId);
                    rollback();
                    result.error = std::move(error);
                    return result;
                }

                if (!capture.stopAcquisition()) {
                    auto error = AddMultiCameraStartupContext(
                        capture.lastError(),
                        "prepare-stop",
                        config.capture.cameraId);
                    capture.close();
                    rollback();
                    result.error = std::move(error);
                    return result;
                }

                auto thread = std::make_unique<CaptureThread>(
                    config.capture.cameraId,
                    std::move(capture),
                    config.threadOptions);
                thread->setOutputQueue(config.outputQueue);

                const std::size_t resultIndex = result.captureThreads.size();
                result.captureThreads.push_back(std::move(thread));
                prepared.push_back(PreparedCamera{
                    MultiCameraStartupKind::CaptureThread,
                    resultIndex,
                    config.capture.cameraId,
                    false});
            }

            if (requestIndex + 1 < requests.size() &&
                options.interCameraOpenDelay.count() > 0) {
                std::this_thread::sleep_for(options.interCameraOpenDelay);
            }
        }

        for (auto& thread : result.captureThreads) {
            if (!thread->start()) {
                auto error = AddMultiCameraStartupContext(
                    thread->lastError(),
                    "worker-start",
                    thread->cameraId());
                rollback();
                result.error = std::move(error);
                return result;
            }
        }

        for (auto& entry : prepared) {
            entry.acquisitionStartAttempted = true;

            bool started = false;
            ErrorInfo error;
            if (entry.kind == MultiCameraStartupKind::DirectCapture) {
                auto& capture = result.captures[entry.resultIndex].capture;
                started = capture.startAcquisition();
                if (!started) error = capture.lastError();
            } else {
                auto& thread = result.captureThreads[entry.resultIndex];
                started = thread->startAcquisition();
                if (!started) error = thread->lastError();
            }

            if (!started) {
                error = AddMultiCameraStartupContext(
                    std::move(error),
                    "acquisition-start",
                    entry.cameraId);
                rollback();
                result.error = std::move(error);
                return result;
            }
        }
    } catch (const std::exception& exception) {
        rollback();
        result.error = MakeError(
            ErrorCode::InternalError,
            "OpenAndStartMultiCameraGroup",
            exception.what());
        return result;
    } catch (...) {
        rollback();
        result.error = MakeError(
            ErrorCode::InternalError,
            "OpenAndStartMultiCameraGroup",
            "Unknown exception during multi-camera startup");
        return result;
    }

    result.ok = true;
    result.error = NoError();
    return result;
}

} // namespace Detail
} // namespace IC4Ext

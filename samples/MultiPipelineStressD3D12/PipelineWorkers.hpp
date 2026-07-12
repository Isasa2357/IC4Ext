#pragma once

#include "SobelProcessor.hpp"
#include "StressSupport.hpp"

#include <IC4Ext/D3D12/ReadOnlyPipeline.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace IC4ExtStress {

struct WorkerOptions
{
    std::uint32_t readbackTimeoutMs = 5000;
    double recordFps = 160.0;
    int videoFourcc = 0;
    int displayMaximumWidth = 1280;
    int displayMaximumHeight = 720;
    std::filesystem::path outputDirectory;
};

using FrameSetQueue = IC4Ext::D3D12::ReadOnlyFrameSetQueue;
using FrameSetQueuePtr = std::shared_ptr<FrameSetQueue>;

std::thread StartPairDisplayWorker(
    FrameSetQueuePtr queue,
    IC4Ext::D3D12BackendContext backend,
    DisplaySlot& display,
    PipelineMetrics& metrics,
    FatalState& fatal,
    std::atomic<bool>& stopRequested,
    std::atomic<bool>& measuring,
    WorkerOptions options);

std::thread StartSingleDisplayWorker(
    std::string pipelineName,
    IC4Ext::D3D12::CameraId cameraId,
    FrameSetQueuePtr queue,
    IC4Ext::D3D12BackendContext backend,
    DisplaySlot& display,
    PipelineMetrics& metrics,
    FatalState& fatal,
    std::atomic<bool>& stopRequested,
    std::atomic<bool>& measuring,
    WorkerOptions options);

std::thread StartPairVideoWorker(
    FrameSetQueuePtr queue,
    IC4Ext::D3D12BackendContext backend,
    PipelineMetrics& metrics,
    FatalState& fatal,
    std::atomic<bool>& measuring,
    WorkerOptions options);

std::thread StartSingleVideoWorker(
    std::string pipelineName,
    IC4Ext::D3D12::CameraId cameraId,
    std::filesystem::path fileName,
    FrameSetQueuePtr queue,
    IC4Ext::D3D12BackendContext backend,
    PipelineMetrics& metrics,
    FatalState& fatal,
    std::atomic<bool>& measuring,
    WorkerOptions options);

std::thread StartSobelWorker(
    FrameSetQueuePtr queue,
    IC4Ext::D3D12BackendContext backend,
    PipelineMetrics& metrics,
    FatalState& fatal,
    std::atomic<bool>& measuring);

std::thread StartOpenCvCannyWorker(
    FrameSetQueuePtr queue,
    IC4Ext::D3D12BackendContext backend,
    PipelineMetrics& metrics,
    FatalState& fatal,
    std::atomic<bool>& measuring,
    WorkerOptions options);

std::thread StartOpenCvSobelWorker(
    FrameSetQueuePtr queue,
    IC4Ext::D3D12BackendContext backend,
    PipelineMetrics& metrics,
    FatalState& fatal,
    std::atomic<bool>& measuring,
    WorkerOptions options);

std::thread StartOpenCvPairDisplayWorker(
    FrameSetQueuePtr queue,
    IC4Ext::D3D12BackendContext backend,
    DisplaySlot& display,
    PipelineMetrics& metrics,
    FatalState& fatal,
    std::atomic<bool>& stopRequested,
    std::atomic<bool>& measuring,
    WorkerOptions options);

} // namespace IC4ExtStress

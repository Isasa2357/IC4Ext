#include "D3D12FrameResizer.hpp"

#include <D3D12Helper/D3D12Framework/D3D12Helpers.hpp>
#include <D3D12Helper/D3D12Gpu/D3D12ResourceView.hpp>

#include <exception>

namespace IC4Ext {

D3D12FrameResizer::~D3D12FrameResizer()
{
    for (auto& slot : slots_) {
        if (slot.inFlight.isValid()) {
            slot.inFlight.wait(INFINITE);
        }
    }
}

void D3D12FrameResizer::setError(ErrorCode code, const char* where, const std::string& message)
{
    lastError_ = MakeError(code, where, message);
}

bool D3D12FrameResizer::initialize(const D3D12BackendContext& backend,
                                   D3D12FenceManager* fenceManager,
                                   const std::filesystem::path& shaderDirectory)
{
    lastError_ = NoError();
    backend_ = backend;
    if (!backend_.resolve() || !backend_.corePtr || !backend_.queue || !fenceManager) {
        setError(ErrorCode::InvalidArgument,
                 "D3D12FrameResizer::initialize",
                 "D3D12Helper core/queue/fence manager is required");
        return false;
    }

    core_ = backend_.corePtr;
    queue_ = backend_.queue;
    device_ = backend_.device;
    fenceManager_ = fenceManager;

    try {
        for (auto& slot : slots_) {
            slot.cbvSrvUav.Initialize(
                device_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, true);
            slot.sampler.Initialize(
                device_, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2, true);
            slot.processingContext.Initialize(
                *core_, &slot.cbvSrvUav, &slot.sampler, shaderDirectory);
            slot.resizer.Initialize(slot.processingContext);
            slot.commandContext.Initialize(device_, queue_->GetType());
        }
        nextSlot_ = 0;
        initialized_ = true;
        return true;
    } catch (const std::exception& e) {
        setError(ErrorCode::D3D12Error, "D3D12FrameResizer::initialize", e.what());
        return false;
    }
}

D3D12FrameResizer::FrameSlot* D3D12FrameResizer::acquireSlot()
{
    auto& slot = slots_[nextSlot_ % slots_.size()];
    nextSlot_ = (nextSlot_ + 1) % slots_.size();
    if (slot.inFlight.isValid()) {
        if (!slot.inFlight.wait(INFINITE)) {
            setError(ErrorCode::D3D12Error,
                     "D3D12FrameResizer::acquireSlot",
                     "in-flight resize fence wait failed");
            return nullptr;
        }
        slot.inFlight = {};
    }
    slot.cbvSrvUav.Reset();
    slot.sampler.Reset();
    slot.commandContext.Reset();
    return &slot;
}

bool D3D12FrameResizer::createSrv(D3D12CameraFrame& frame)
{
    try {
        frame.srvHeapHelper.Initialize(
            device_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true);
        frame.srvHeap = frame.srvHeapHelper.Get();
        const auto handle = frame.srvHeapHelper.GetHandle(0);
        D3D12CoreLib::CreateTexture2DSrv(
            *core_, frame.textureResource, handle.cpu, frame.dxgiFormat);
        frame.srvCpuHandle = handle.cpu;
        frame.srvGpuHandle = handle.gpu;
        return true;
    } catch (const std::exception& e) {
        setError(ErrorCode::D3D12Error, "D3D12FrameResizer::createSrv", e.what());
        return false;
    }
}

bool D3D12FrameResizer::resizeFrame(const D3D12CameraFrame& src,
                                    const CameraOutputResizeOptions& options,
                                    D3D12CameraFrame& dst)
{
    lastError_ = NoError();
    dst = {};
    if (!initialized_ || !core_ || !queue_ || !device_ || !fenceManager_) {
        setError(ErrorCode::InvalidArgument,
                 "D3D12FrameResizer::resizeFrame",
                 "resizer is not initialized");
        return false;
    }
    if (!options.enabled()) {
        setError(ErrorCode::InvalidArgument,
                 "D3D12FrameResizer::resizeFrame",
                 "resize dimensions are disabled");
        return false;
    }
    if (!src.texture) {
        setError(ErrorCode::InvalidArgument,
                 "D3D12FrameResizer::resizeFrame",
                 "source texture is null");
        return false;
    }
    if (src.format.outputFormat != GpuFrameFormat::RGBA8 ||
        src.dxgiFormat != DXGI_FORMAT_R8G8B8A8_UNORM) {
        setError(ErrorCode::UnsupportedFormat,
                 "D3D12FrameResizer::resizeFrame",
                 "output-queue resize currently supports RGBA8 frames only");
        return false;
    }

    try {
        if (src.ready.isValid()) {
            queue_->GpuWait(src.ready.fence.Get(), src.ready.value);
        }

        FrameSlot* slot = acquireSlot();
        if (!slot) return false;

        dst.textureResource = slot->resizer.CreateOutputTexture(
            *core_,
            options.width,
            options.height,
            src.dxgiFormat,
            D3D12_RESOURCE_STATE_COMMON);
        dst.texture = dst.textureResource.Get();
        dst.dxgiFormat = src.dxgiFormat;

        // The queue wait guarantees execution order, while these references guarantee
        // that all resources used by the source submission remain alive until the
        // resized frame has completed.
        dst.processingSourceKeepAlive = src.texture;
        dst.uploadKeepAlive = src.uploadKeepAlive;
        dst.inputBufferKeepAlive = src.inputBufferKeepAlive;
        dst.commandAllocatorKeepAlive = src.commandAllocatorKeepAlive;
        dst.commandListKeepAlive = src.commandListKeepAlive;

        D3D12CoreLib::Processing::ResizeDesc desc{};
        desc.filter = options.filter == CameraOutputResizeFilter::Point
            ? D3D12CoreLib::Processing::ProcessingFilter::Point
            : D3D12CoreLib::Processing::ProcessingFilter::Linear;

        D3D12CoreLib::Processing::D3D12ProcessingStateDesc state{};
        state.useExplicitStates = true;
        state.srcBefore = src.resourceState;
        state.srcAfter = src.resourceState;
        state.dstBefore = D3D12_RESOURCE_STATE_COMMON;
        state.dstAfter = src.resourceState;

        slot->resizer.RecordResizeView(
            slot->commandContext,
            D3D12CoreLib::D3D12ResourceView(src.texture.Get()),
            D3D12CoreLib::D3D12ResourceView(dst.texture.Get()),
            desc,
            state);

        slot->commandContext.Close();
        ID3D12CommandList* commandLists[] = {
            slot->commandContext.GetCommandList()
        };
        queue_->ExecuteCommandLists(1, commandLists);
        const std::uint64_t fenceValue = queue_->Signal();
        const auto token = fenceManager_->makeToken(fenceValue);
        if (!token.isValid()) {
            setError(ErrorCode::D3D12Error,
                     "D3D12FrameResizer::resizeFrame / signal",
                     "failed to create resize ready token");
            return false;
        }
        slot->inFlight = token;
        dst.ready = token;

        dst.textureResource.SetState(src.resourceState);
        dst.resourceState = src.resourceState;
        dst.timing = src.timing;
        dst.format = src.format;
        dst.format.width = static_cast<int>(options.width);
        dst.format.height = static_cast<int>(options.height);
        dst.format.inputRowPitchBytes = 0;
        dst.chunkMetadata = src.chunkMetadata;

        return createSrv(dst);
    } catch (const std::exception& e) {
        setError(ErrorCode::D3D12Error, "D3D12FrameResizer::resizeFrame", e.what());
        return false;
    }
}

} // namespace IC4Ext

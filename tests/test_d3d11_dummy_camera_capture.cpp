#include <IC4Ext/IC4Ext.hpp>
#include <ThreadKit/Queues/QueueCommon.hpp>

#include <cassert>
#include <iostream>
#include <memory>
#include <optional>

namespace {

class MockControlSink final : public IC4Ext::ID3D11CameraControlSink
{
public:
    bool result = true;
    std::optional<IC4Ext::CameraControlCommand> lastCommand;
    IC4Ext::ErrorInfo error;

    bool submitControlCommand(const IC4Ext::CameraControlCommand& command) override
    {
        lastCommand = command;
        error = result ? IC4Ext::NoError()
                       : IC4Ext::MakeError(IC4Ext::ErrorCode::D3D11Error, "MockControlSink", "forced failure");
        return result;
    }

    const IC4Ext::ErrorInfo& lastError() const noexcept override { return error; }
};

} // namespace

int main()
{
    ThreadKit::Queues::QueueOptions queueOptions;
    queueOptions.maxSize = 4;
    auto queue = std::make_shared<IC4Ext::D3D11FrameQueue>(queueOptions);

    {
        IC4Ext::D3D11DummyCameraCapture noSink(3, queue, std::weak_ptr<IC4Ext::ID3D11CameraControlSink>{});
        assert(noSink.isOpened());
        assert(noSink.cameraIndex() == 3);
        assert(noSink.frameQueue() == queue);
        assert(!noSink.setExposureTime(1000.0));
        assert(noSink.lastError().code == static_cast<int>(IC4Ext::ErrorCode::NotOpened));
    }

    auto sink = std::make_shared<MockControlSink>();
    IC4Ext::D3D11DummyCameraCapture dummy(7, queue, sink);

    assert(dummy.setExposureTime(2000.0));
    assert(sink->lastCommand.has_value());
    assert(sink->lastCommand->type == IC4Ext::CameraControlCommandType::SetExposureTime);
    assert(sink->lastCommand->doubleValue == 2000.0);

    assert(dummy.setOffset(11, 22));
    assert(sink->lastCommand->type == IC4Ext::CameraControlCommandType::SetOffset);
    assert(sink->lastCommand->offsetX == 11);
    assert(sink->lastCommand->offsetY == 22);

    assert(dummy.setIC4Property("ExposureAuto", "Off"));
    assert(sink->lastCommand->type == IC4Ext::CameraControlCommandType::SetPropertyString);
    assert(sink->lastCommand->propertyName == "ExposureAuto");
    assert(sink->lastCommand->stringValue == "Off");

    sink->result = false;
    assert(!dummy.setGain(12.0));
    assert(dummy.lastError().code == static_cast<int>(IC4Ext::ErrorCode::D3D11Error));

    IC4Ext::D3D11CameraFrame frame;
    frame.format.width = 2;
    frame.format.height = 2;
    frame.format.actualInputFormat = IC4Ext::CameraPixelFormat::Mono8;
    frame.format.outputFormat = IC4Ext::GpuFrameFormat::R8;
    queue->push(std::move(frame));

    IC4Ext::CameraReadOptions readOptions;
    readOptions.mode = IC4Ext::ReadMode::NextFrame;
    readOptions.timeoutMs = 0;
    IC4Ext::ReadResult read = dummy.read(readOptions);
    assert(read.ok);
    assert(read.frame.format.width == 2);
    assert(read.frame.format.height == 2);

    dummy.close();
    assert(!dummy.isOpened());
    assert(!dummy.read(readOptions));
    assert(dummy.lastError().code == static_cast<int>(IC4Ext::ErrorCode::NotOpened));

    std::cout << "test_d3d11_dummy_camera_capture passed\n";
    return 0;
}

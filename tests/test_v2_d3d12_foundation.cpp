#include "IC4Ext/V2/Core/FrameSyncOutputConfig.hpp"
#include "IC4Ext/V2/Core/FrameSyncTypes.hpp"
#include "IC4Ext/V2/D3D12/D3D12ReadOnlyFrameSet.hpp"

#include <cassert>
#include <chrono>

int main()
{
    using namespace IC4Ext::V2;

    assert(FrameRateLimit::Maximum().isValid());
    assert(FrameRateLimit::Fixed(60.0).isValid());
    assert(!FrameRateLimit::Fixed(0.0).isValid());

    FrameSyncConfig config;
    config.cameraIds = {0, 1};
    config.policy = FrameSyncPolicy::FrameNumberExact;
    assert(config.isValid());

    config.cameraIds = {0, 0};
    assert(!config.isValid());

    D3D12ReadOnlyFrameSet::FrameList frames;
    frames.push_back(D3D12IndexedReadOnlyFrame{1, {}});
    frames.push_back(D3D12IndexedReadOnlyFrame{0, {}});

    const auto completed = std::chrono::steady_clock::now();
    auto set = D3D12ReadOnlyFrameSet::Create(42, 123456, completed, std::move(frames));
    assert(set.valid());
    assert(set.syncGroupId() == 42);
    assert(set.referenceTimestampNs() == 123456);
    assert(set.completedTime() == completed);
    assert(set.size() == 2);
    assert(set[0].cameraId == 1);
    assert(set[1].cameraId == 0);
    assert(set.contains(1));
    assert(set.contains(0));
    assert(!set.contains(2));

    FrameSyncOutputConfig output;
    output.requiredCameras = {1, 0};
    output.frameRate = FrameRateLimit::Fixed(30.0);
    output.priority = 100;
    assert(output.frameRate.isValid());

    return 0;
}

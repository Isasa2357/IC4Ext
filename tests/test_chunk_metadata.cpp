#include <IC4Ext/IC4Ext.hpp>

#include <cassert>
#include <iostream>
#include <utility>

namespace {

IC4Ext::FrameChunkMetadata MakeChunkMetadata()
{
    IC4Ext::FrameChunkMetadata metadata;
    metadata.hasBlockId = true;
    metadata.blockId = 12345;
    metadata.hasExposureTime = true;
    metadata.exposureTimeUs = 2000.5;
    metadata.hasGain = true;
    metadata.gain = 12.25;
    metadata.hasIMX174FrameId = true;
    metadata.imx174FrameId = 7;
    metadata.hasIMX174FrameSet = true;
    metadata.imx174FrameSet = 8;
    metadata.hasMultiFrameSetId = true;
    metadata.multiFrameSetId = 9;
    metadata.hasMultiFrameSetFrameId = true;
    metadata.multiFrameSetFrameId = 10;
    return metadata;
}

void AssertChunkMetadataEquals(const IC4Ext::FrameChunkMetadata& metadata)
{
    assert(metadata.hasAny());
    assert(metadata.hasBlockId && metadata.blockId == 12345);
    assert(metadata.hasExposureTime && metadata.exposureTimeUs == 2000.5);
    assert(metadata.hasGain && metadata.gain == 12.25);
    assert(metadata.hasIMX174FrameId && metadata.imx174FrameId == 7);
    assert(metadata.hasIMX174FrameSet && metadata.imx174FrameSet == 8);
    assert(metadata.hasMultiFrameSetId && metadata.multiFrameSetId == 9);
    assert(metadata.hasMultiFrameSetFrameId && metadata.multiFrameSetFrameId == 10);
}

} // namespace

int main()
{
    IC4Ext::FrameChunkMetadata empty;
    assert(!empty.hasAny());

    const auto metadata = MakeChunkMetadata();
    AssertChunkMetadataEquals(metadata);

    IC4Ext::CpuFrame cpu;
    cpu.chunkMetadata = metadata;
    AssertChunkMetadataEquals(cpu.chunkMetadata);

#if IC4EXT_ENABLE_D3D11
    IC4Ext::D3D11CameraFrame d3d11Frame;
    d3d11Frame.chunkMetadata = metadata;
    IC4Ext::D3D11IndexedCameraFrame indexed11;
    indexed11.cameraIndex = 2;
    indexed11.frame = std::move(d3d11Frame);
    assert(indexed11.cameraIndex == 2);
    AssertChunkMetadataEquals(indexed11.frame.chunkMetadata);
#endif

#if IC4EXT_ENABLE_D3D12
    IC4Ext::D3D12CameraFrame d3d12Frame;
    d3d12Frame.chunkMetadata = metadata;
    IC4Ext::D3D12IndexedCameraFrame indexed12;
    indexed12.cameraIndex = 3;
    indexed12.frame = std::move(d3d12Frame);
    assert(indexed12.cameraIndex == 3);
    AssertChunkMetadataEquals(indexed12.frame.chunkMetadata);
#endif

    std::cout << "test_chunk_metadata passed\n";
    return 0;
}

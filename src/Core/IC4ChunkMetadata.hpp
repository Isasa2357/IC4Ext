#pragma once

#include "IC4Ext/Core/CoreTypes.hpp"

#include <ic4/ic4.h>

#include <memory>

namespace IC4Ext {
namespace Internal {

inline void TryReadChunkDouble(ic4::PropertyMap& props,
                               const char* name,
                               bool& hasValue,
                               double& value) noexcept
{
    try {
        ic4::Error err;
        const double v = props.getValueDouble(name, err);
        if (!err.isError()) {
            hasValue = true;
            value = v;
        }
    } catch (...) {
    }
}

inline void TryReadChunkInt64(ic4::PropertyMap& props,
                              const char* name,
                              bool& hasValue,
                              std::int64_t& value) noexcept
{
    try {
        ic4::Error err;
        const std::int64_t v = props.getValueInt64(name, err);
        if (!err.isError()) {
            hasValue = true;
            value = v;
        }
    } catch (...) {
    }
}

inline void TryReadChunkUInt64(ic4::PropertyMap& props,
                               const char* name,
                               bool& hasValue,
                               std::uint64_t& value) noexcept
{
    std::int64_t signedValue = 0;
    bool hasSignedValue = false;
    TryReadChunkInt64(props, name, hasSignedValue, signedValue);
    if (hasSignedValue && signedValue >= 0) {
        hasValue = true;
        value = static_cast<std::uint64_t>(signedValue);
    }
}

inline FrameChunkMetadata ReadChunkMetadata(ic4::Grabber* grabber,
                                            const std::shared_ptr<ic4::ImageBuffer>& imageBuffer) noexcept
{
    FrameChunkMetadata metadata;
    if (!grabber || !(*grabber) || !imageBuffer) {
        return metadata;
    }

    try {
        ic4::Error propErr;
        ic4::PropertyMap props = grabber->devicePropertyMap(propErr);
        if (propErr.isError() || !props) {
            return metadata;
        }

        ic4::Error connectErr;
        if (!props.connectChunkData(imageBuffer, connectErr) || connectErr.isError()) {
            return metadata;
        }

        TryReadChunkUInt64(props, "ChunkBlockId", metadata.hasBlockId, metadata.blockId);
        TryReadChunkDouble(props, "ChunkExposureTime", metadata.hasExposureTime, metadata.exposureTimeUs);
        TryReadChunkDouble(props, "ChunkGain", metadata.hasGain, metadata.gain);
        TryReadChunkInt64(props, "ChunkIMX174FrameId", metadata.hasIMX174FrameId, metadata.imx174FrameId);
        TryReadChunkInt64(props, "ChunkIMX174FrameSet", metadata.hasIMX174FrameSet, metadata.imx174FrameSet);
        TryReadChunkInt64(props, "ChunkMultiFrameSetId", metadata.hasMultiFrameSetId, metadata.multiFrameSetId);
        TryReadChunkInt64(props, "ChunkMultiFrameSetFrameId", metadata.hasMultiFrameSetFrameId, metadata.multiFrameSetFrameId);

        ic4::Error disconnectErr;
        props.connectChunkData(std::shared_ptr<ic4::ImageBuffer>{}, disconnectErr);
    } catch (...) {
        return FrameChunkMetadata{};
    }

    return metadata;
}

} // namespace Internal
} // namespace IC4Ext

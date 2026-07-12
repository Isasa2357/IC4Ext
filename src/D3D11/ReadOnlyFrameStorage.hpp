#pragma once

#include "IC4Ext/D3D11/ReadOnlyFrame.hpp"

#include <functional>

namespace IC4Ext::D3D11 {

struct D3D11ReadOnlyFrame::Storage
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    DXGI_FORMAT formatValue = DXGI_FORMAT_UNKNOWN;
    ::IC4Ext::D3D11ReadyToken ready;
    FrameTiming timingValue;
    FrameFormatMetadata frameFormat;
    FrameChunkMetadata chunk;
    std::function<void()> releaseCallback;

    ~Storage()
    {
        if (releaseCallback) releaseCallback();
    }
};

inline const ::IC4Ext::D3D11ReadyToken& EmptyReadyToken() noexcept
{
    static const ::IC4Ext::D3D11ReadyToken value{};
    return value;
}

inline const FrameTiming& EmptyFrameTiming() noexcept
{
    static const FrameTiming value{};
    return value;
}

inline const FrameFormatMetadata& EmptyFrameFormat() noexcept
{
    static const FrameFormatMetadata value{};
    return value;
}

inline const FrameChunkMetadata& EmptyFrameChunk() noexcept
{
    static const FrameChunkMetadata value{};
    return value;
}

} // namespace IC4Ext::D3D11

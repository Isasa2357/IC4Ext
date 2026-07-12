#pragma once

#include "IC4Ext/Core/CoreTypes.hpp"
#include "IC4Ext/D3D11/D3D11ReadyToken.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

namespace IC4Ext {
class D3D11FrameConverter;
}

namespace IC4Ext::D3D11 {

class D3D11FramePool;
class D3D11FrameWriter;
class D3D11PooledFrameConverter;

// Immutable shared view of one completed D3D11 camera/source texture. The
// producer-owned FramePool entry is returned only after the final shared handle
// is released.
class D3D11ReadOnlyFrame final
{
public:
    D3D11ReadOnlyFrame() noexcept = default;

    bool valid() const noexcept;
    explicit operator bool() const noexcept { return valid(); }

    bool hasResource() const noexcept { return texture() != nullptr; }
    bool hasSrv() const noexcept { return srv() != nullptr; }
    bool isReady() const noexcept;
    bool waitReady(std::uint32_t timeoutMs = INFINITE) const noexcept;
    long useCount() const noexcept;
    bool unique() const noexcept { return useCount() == 1; }

    ID3D11Resource* resource() const noexcept;
    ID3D11Texture2D* texture() const noexcept;
    ID3D11ShaderResourceView* srv() const noexcept;
    DXGI_FORMAT dxgiFormat() const noexcept;

    const ::IC4Ext::D3D11ReadyToken& readyToken() const noexcept;
    const FrameTiming& timing() const noexcept;
    const FrameFormatMetadata& format() const noexcept;
    const FrameChunkMetadata& chunkMetadata() const noexcept;

private:
    struct Storage;

    explicit D3D11ReadOnlyFrame(std::shared_ptr<const Storage> storage) noexcept;
    ID3D11UnorderedAccessView* producerUav() const noexcept;

    std::shared_ptr<const Storage> storage_;

    friend class D3D11FramePool;
    friend class D3D11FrameWriter;
    friend class D3D11PooledFrameConverter;
    friend class ::IC4Ext::D3D11FrameConverter;
};

using ReadOnlyFrame = D3D11ReadOnlyFrame;

} // namespace IC4Ext::D3D11

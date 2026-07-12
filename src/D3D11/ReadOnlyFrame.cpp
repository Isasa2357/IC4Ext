#include "IC4Ext/D3D11/ReadOnlyFrame.hpp"

#include "ReadOnlyFrameStorage.hpp"

#include <utility>

namespace IC4Ext::D3D11 {

D3D11ReadOnlyFrame::D3D11ReadOnlyFrame(
    std::shared_ptr<const Storage> storage) noexcept
    : storage_(std::move(storage))
{
}

bool D3D11ReadOnlyFrame::valid() const noexcept
{
    return storage_ && storage_->texture;
}

bool D3D11ReadOnlyFrame::isReady() const noexcept
{
    return !storage_ || !storage_->ready.isValid() || storage_->ready.isReady();
}

bool D3D11ReadOnlyFrame::waitReady(std::uint32_t timeoutMs) const noexcept
{
    return !storage_ || !storage_->ready.isValid() || storage_->ready.wait(timeoutMs);
}

long D3D11ReadOnlyFrame::useCount() const noexcept
{
    return static_cast<long>(storage_.use_count());
}

ID3D11Resource* D3D11ReadOnlyFrame::resource() const noexcept
{
    return texture();
}

ID3D11Texture2D* D3D11ReadOnlyFrame::texture() const noexcept
{
    return storage_ ? storage_->texture.Get() : nullptr;
}

ID3D11ShaderResourceView* D3D11ReadOnlyFrame::srv() const noexcept
{
    return storage_ ? storage_->srv.Get() : nullptr;
}

ID3D11UnorderedAccessView* D3D11ReadOnlyFrame::producerUav() const noexcept
{
    return storage_ ? storage_->uav.Get() : nullptr;
}

DXGI_FORMAT D3D11ReadOnlyFrame::dxgiFormat() const noexcept
{
    return storage_ ? storage_->formatValue : DXGI_FORMAT_UNKNOWN;
}

const ::IC4Ext::D3D11ReadyToken& D3D11ReadOnlyFrame::readyToken() const noexcept
{
    return storage_ ? storage_->ready : EmptyReadyToken();
}

const FrameTiming& D3D11ReadOnlyFrame::timing() const noexcept
{
    return storage_ ? storage_->timingValue : EmptyFrameTiming();
}

const FrameFormatMetadata& D3D11ReadOnlyFrame::format() const noexcept
{
    return storage_ ? storage_->frameFormat : EmptyFrameFormat();
}

const FrameChunkMetadata& D3D11ReadOnlyFrame::chunkMetadata() const noexcept
{
    return storage_ ? storage_->chunk : EmptyFrameChunk();
}

} // namespace IC4Ext::D3D11

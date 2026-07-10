#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <cstdint>

namespace IC4Ext {

struct D3D11ReadyToken
{
    Microsoft::WRL::ComPtr<ID3D11Fence> fence;
    std::uint64_t value = 0;

    bool isValid() const noexcept { return fence != nullptr && value != 0; }
    bool isReady() const noexcept;
    bool wait(std::uint32_t timeoutMs = INFINITE) const noexcept;
};

} // namespace IC4Ext

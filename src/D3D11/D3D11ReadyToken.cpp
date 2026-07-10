#include "IC4Ext/D3D11/D3D11ReadyToken.hpp"

namespace IC4Ext {

bool D3D11ReadyToken::isReady() const noexcept
{
    return isValid() && fence->GetCompletedValue() >= value;
}

bool D3D11ReadyToken::wait(std::uint32_t timeoutMs) const noexcept
{
    if (!isValid()) {
        return false;
    }
    if (fence->GetCompletedValue() >= value) {
        return true;
    }

    HANDLE eventHandle = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!eventHandle) {
        return false;
    }

    HRESULT hr = fence->SetEventOnCompletion(value, eventHandle);
    if (FAILED(hr)) {
        ::CloseHandle(eventHandle);
        return false;
    }

    const DWORD waitResult = ::WaitForSingleObject(eventHandle, timeoutMs);
    ::CloseHandle(eventHandle);
    return waitResult == WAIT_OBJECT_0;
}

} // namespace IC4Ext

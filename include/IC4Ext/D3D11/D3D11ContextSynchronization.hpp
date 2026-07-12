#pragma once

#include <d3d11.h>

#include <memory>
#include <mutex>
#include <unordered_map>

namespace IC4Ext::D3D11::Detail {

// ID3D11Multithread makes individual immediate-context calls thread-safe, but
// bind/update/dispatch/signal is a multi-call transaction. All IC4Ext D3D11
// producers and readback consumers use the same recursive mutex per immediate
// context so those transactions cannot interleave across worker threads.
inline std::shared_ptr<std::recursive_mutex> AcquireImmediateContextMutex(
    ID3D11DeviceContext* context)
{
    if (!context) return {};

    static std::mutex registryMutex;
    static std::unordered_map<
        ID3D11DeviceContext*,
        std::weak_ptr<std::recursive_mutex>> registry;

    std::lock_guard<std::mutex> lock(registryMutex);
    auto& weak = registry[context];
    auto shared = weak.lock();
    if (!shared) {
        shared = std::make_shared<std::recursive_mutex>();
        weak = shared;
    }

    if (registry.size() > 32) {
        for (auto iterator = registry.begin(); iterator != registry.end();) {
            if (iterator->second.expired()) {
                iterator = registry.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }
    return shared;
}

class ImmediateContextSequenceLock final
{
public:
    explicit ImmediateContextSequenceLock(
        const std::shared_ptr<std::recursive_mutex>& mutex)
        : lock_(mutex ? std::unique_lock<std::recursive_mutex>(*mutex)
                      : std::unique_lock<std::recursive_mutex>())
    {
    }

private:
    std::unique_lock<std::recursive_mutex> lock_;
};

} // namespace IC4Ext::D3D11::Detail

#pragma once

#include <ic4/ic4.h>

#include <memory>

// IC Imaging Control 4 SDK releases with explicit validity APIs no longer
// permit the older implicit truth-value expressions used by this IC4Ext
// revision. Keep the compatibility localized here so the pinned IC4Ext source
// can be built with both the installed SDK and older toolchains.
namespace ic4 {

inline bool operator!(const Grabber& value) noexcept
{
    return !value.is_valid();
}

inline bool operator&&(
    const std::unique_ptr<Grabber>& owner,
    const Grabber& value) noexcept
{
    return owner != nullptr && value.is_valid();
}

inline bool operator!(const PropertyMap& value) noexcept
{
    return !value.is_valid();
}

inline bool operator&&(bool lhs, const PropEnumeration& value) noexcept
{
    return lhs && value.is_valid();
}

} // namespace ic4

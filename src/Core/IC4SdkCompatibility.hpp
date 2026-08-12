#pragma once

#include <ic4/ic4.h>

#include <memory>

// IC Imaging Control 4 SDK releases differ in whether several wrapper types
// expose an implicit/contextual bool conversion.  IC4Ext historically used
// those wrappers in boolean expressions, while current SDKs consistently
// expose is_valid().
//
// Keep the old source expressions source-compatible while normalizing the
// actual validity check to is_valid().  This file is internal to IC4Ext and is
// included before the legacy V2 implementation body.

inline bool operator!(const ic4::Grabber& value) noexcept
{
    return !value.is_valid();
}

inline bool operator!(const ic4::PropertyMap& value) noexcept
{
    return !value.is_valid();
}

inline bool operator!(const ic4::Property& value) noexcept
{
    return !value.is_valid();
}

inline bool operator&&(bool lhs, const ic4::Property& rhs) noexcept
{
    return lhs && rhs.is_valid();
}

template <class Deleter>
inline bool operator&&(
    const std::unique_ptr<ic4::Grabber, Deleter>& lhs,
    const ic4::Grabber& rhs) noexcept
{
    return static_cast<bool>(lhs) && rhs.is_valid();
}

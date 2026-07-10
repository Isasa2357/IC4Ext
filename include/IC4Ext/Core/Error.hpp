#pragma once

#include <string>
#include <utility>

namespace IC4Ext {

enum class ErrorCode : int
{
    Ok = 0,
    InvalidArgument = 1,
    NotOpened = 2,
    Timeout = 3,
    UnsupportedFormat = 4,
    IC4Error = 5,
    D3D11Error = 6,
    D3D12Error = 7,
    ShaderError = 8,
    ThreadError = 9,
    InternalError = 10,
};

struct ErrorInfo
{
    int code = 0;
    std::string message;
    std::string where;

    bool ok() const noexcept { return code == 0; }
    explicit operator bool() const noexcept { return code != 0; }
};

inline ErrorInfo MakeError(ErrorCode code, std::string where, std::string message)
{
    return ErrorInfo{static_cast<int>(code), std::move(message), std::move(where)};
}

inline ErrorInfo NoError()
{
    return {};
}

} // namespace IC4Ext

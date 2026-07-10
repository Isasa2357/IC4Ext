#pragma once

#include <string>

namespace IC4Ext {

struct IC4DeviceSelector
{
    std::string serial;
    std::string uniqueName;
    int deviceIndex = -1;
};

} // namespace IC4Ext

// SPDX-License-Identifier: LGPL-2.1-only
#include "bridge.h"
#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size > 65536)
        return 0;
    std::string raw(reinterpret_cast<const char*>(data), size);
    try
    {
        (void)living::update(raw);
    }
    catch (const std::exception&)
    {
    }
    try
    {
        (void)living::camera(raw);
    }
    catch (const std::exception&)
    {
    }
    return 0;
}

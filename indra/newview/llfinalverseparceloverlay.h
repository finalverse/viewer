// Copyright (c) 2026 Finalverse contributors. SPDX-License-Identifier: LGPL-2.1-only
#ifndef LL_FINALVERSE_PARCEL_OVERLAY_H
#define LL_FINALVERSE_PARCEL_OVERLAY_H
#include <cstddef>
#include <cstring>

// Packet sequence numbers are untrusted. Larger OpenSim regions can send more
// chunks than a legacy overlay holds; never infer additional buffer capacity.
inline bool llfinalverseCopyParcelOverlay(unsigned char* destination, std::size_t capacity,
    int chunk, const unsigned char* source, std::size_t bytes, std::size_t chunks)
{
    if (!destination || !source || !capacity || !chunks || capacity % chunks ||
        chunk < 0 || static_cast<std::size_t>(chunk) >= chunks || bytes != capacity / chunks)
        return false;
    std::memcpy(destination + static_cast<std::size_t>(chunk) * bytes, source, bytes);
    return true;
}
#endif

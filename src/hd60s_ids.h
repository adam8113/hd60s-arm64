// The HD60 S ships under three product IDs for the same board, so matching on
// any single one of them misses two thirds of the cards in the wild.
#pragma once
#include <CoreFoundation/CoreFoundation.h>

#define VID 0x0FD9

static const UInt16 kPIDs[] = { 0x004f, 0x005e, 0x0074, 0x0076 };

static inline bool isHD60S(UInt16 v, UInt16 p) {
    if (v != VID) return false;
    for (unsigned i = 0; i < sizeof kPIDs / sizeof *kPIDs; i++)
        if (p == kPIDs[i]) return true;
    return false;
}

#pragma once

#include <iterator>

// The ruler offers three columns; any saved value snaps to the nearest one.
inline constexpr int RulerColumns[] = {80, 100, 120};

inline int normalizeRulerColumn(int saved)
{
    if (saved < 90)
        return 80;
    if (saved < 110)
        return 100;
    return 120;
}

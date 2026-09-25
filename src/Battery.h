#pragma once
// src/Battery.h — battery voltage → charge estimate, shared by the
// telemetry push and the LXMF /battery reply. Pure, no Arduino deps.

#include <stdint.h>
#include <math.h>

namespace rlr { namespace battery {

// Approximate single-cell LiPo charge percentage from terminal voltage.
// A linear 3.30 V (0%) .. 4.20 V (100%) map — coarse but adequate for a
// telemetry indicator; documented as an estimate. Boards with different
// chemistry/cell counts can be refined later.
inline float percent(uint16_t mv) {
    if (mv == 0) return 0.0f;
    float pct = ((float)mv - 3300.0f) / (4200.0f - 3300.0f) * 100.0f;
    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return roundf(pct * 10.0f) / 10.0f;   // 0.1% resolution, like Sideband
}

}} // namespace rlr::battery

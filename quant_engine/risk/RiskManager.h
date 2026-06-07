#pragma once

#include "core/Types.h"

// Pre-trade checks: leverage, position limits, DD halt.

class RiskManager {
public:
    bool approveOrder(const Order& order);
};

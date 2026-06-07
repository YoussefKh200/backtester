#pragma once

#include <vector>
#include "core/Types.h"

// GBM + regime-switching data generator (for testing).

class SyntheticData {
public:
    std::vector<Bar> generateGBM(int assetId, int length);
    std::vector<Bar> generateRegimeSeries(int assetId, int length);
};

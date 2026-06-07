#pragma once

#include <vector>

// Sharpe, Sortino, Calmar, CAGR, win rate, MC resample.

class PerformanceMetrics {
public:
    double sharpe(const std::vector<double>& returns) const;
    double sortino(const std::vector<double>& returns) const;
};

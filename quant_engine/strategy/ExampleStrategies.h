#pragma once

#include "StrategyBase.h"

// MA Crossover, Mean Reversion, Donchian Breakout.

class MovingAverageCrossover : public StrategyBase {
public:
    void onBar(const Bar& bar) override {}
    void onFill(const Fill& fill) override {}
    void onEnd() override {}
};

class MeanReversion : public StrategyBase {
public:
    void onBar(const Bar& bar) override {}
    void onFill(const Fill& fill) override {}
    void onEnd() override {}
};

class DonchianBreakout : public StrategyBase {
public:
    void onBar(const Bar& bar) override {}
    void onFill(const Fill& fill) override {}
    void onEnd() override {}
};

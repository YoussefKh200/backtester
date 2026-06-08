# QuantEngine — Institutional C++ Backtesting System

A production-grade, event-driven backtesting engine for equities and indices built in C++17.

## Architecture

```
quant_engine/
├── core/
│   ├── Types.h              # Canonical types: Bar, Order, Fill, Position, Events
│   ├── EventEngine.h        # Priority queue + pub/sub event dispatcher
│   ├── Backtester.h         # Main orchestrator — wires all layers together
│   └── Optimizer.h          # Grid search + walk-forward framework
├── data/
│   ├── MarketDataLoader.h   # CSV OHLCV loader + multi-asset timestamp merger
│   └── SyntheticData.h      # GBM + regime-switching data generator (for testing)
├── execution/
│   └── ExecutionEngine.h    # Order matching: Market/Limit/Stop/StopLimit,
│                            # slippage, spread, commission models
├── strategy/
│   ├── StrategyBase.h       # Abstract interface (on_bar, on_fill, on_end)
│   └── ExampleStrategies.h  # MA Crossover, Mean Reversion, Donchian Breakout
├── portfolio/
│   └── Portfolio.h          # Position tracking, FIFO PnL, equity curve, trades
├── risk/
│   └── RiskManager.h        # Pre-trade checks: leverage, position limits, DD halt
├── metrics/
│   └── PerformanceMetrics.h # Sharpe, Sortino, Calmar, CAGR, win rate, MC resample
├── main.cpp                 # 4 runnable demos
└── CMakeLists.txt
```

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Or direct compile:
```bash
g++ -std=c++17 -O3 -march=native -I. main.cpp -o quant_engine
```

## Run

```bash
./quant_engine          # All demos
./quant_engine 1        # MA Crossover (SPY+QQQ, 5yr synthetic, Monte Carlo)
./quant_engine 2        # Multi-strategy portfolio (3 assets × 3 strategies)
./quant_engine 3        # Grid search optimization (MA fast/slow params)
./quant_engine csv data/SPY.csv SPY   # Backtest from real CSV file
```

## CSV Format

Default expected columns (configurable via `CsvSchema`):
```
date,open,high,low,close,volume
2023-01-03,380.00,385.20,378.50,382.10,75234100
```

Custom schema example:
```cpp
CsvSchema schema;
schema.col_ts     = 0;
schema.col_open   = 1;
schema.col_high   = 2;
schema.col_low    = 3;
schema.col_close  = 4;
schema.col_volume = 5;
schema.delimiter  = ',';
bt.load_csv("data/SPY.csv", "SPY", schema);
```

## Writing a Custom Strategy

```cpp
#include "strategy/StrategyBase.h"

struct MyParams {
    int    lookback = 20;
    double notional = 100000.0;
};

class MyStrategy : public bt::StrategyBase {
public:
    explicit MyStrategy(MyParams p = MyParams()) : p_(p) {}

    void on_bar(const bt::Bar& bar) override {
        // Access bar: bar.symbol, bar.open, bar.high, bar.low, bar.close, bar.ts
        // Submit orders:
        buy_market(bar.symbol, 100.0);
        sell_market(bar.symbol, 100.0);
        buy_limit(bar.symbol, 100.0, bar.close * 0.99);
        sell_stop(bar.symbol, 100.0, bar.close * 0.95);
    }

    void on_fill(const bt::Fill& fill) override {
        // React to fills — update internal state
    }

    void on_end() override {
        // Close all open positions before teardown
    }

private:
    MyParams p_;
};

// Register with backtester:
bt.add_strategy(std::make_shared<MyStrategy>(p), "my_strategy");
```

## Execution Models

### Slippage
```cpp
cfg.slippage.model    = SlippageModel::Fixed;       // fixed bps
cfg.slippage.model    = SlippageModel::VolumeImpact; // sqrt market impact
cfg.slippage.model    = SlippageModel::VolatilityAdjusted; // fraction of bar range
cfg.slippage.fixed_bps = 2.0;                        // 2 basis points
```

### Commission
```cpp
cfg.commission.per_share   = 0.005;   // IB tiered ~$0.005/share
cfg.commission.min_per_trade = 1.0;
cfg.commission.max_pct_value = 0.005; // 0.5% cap
```

### Spread
```cpp
cfg.spread.half_spread_bps = 1.0;  // 2bps round-trip
```

## Risk Controls

```cpp
cfg.risk.max_drawdown_pct     = 0.20;  // halt trading after 20% DD
cfg.risk.max_position_pct     = 0.25;  // max 25% of equity per position
cfg.risk.max_gross_leverage   = 4.0;   // 4x gross leverage cap
cfg.risk.daily_loss_limit_pct = 0.03;  // 3% daily loss halt
```

## Parameter Optimization

```cpp
GridOptimizer opt(strategy_factory, data_factory, bt_cfg);
opt.add_param({"fast", 5, 25, 5});   // test fast=5,10,15,20,25
opt.add_param({"slow", 30, 80, 10}); // test slow=30,40,...,80
auto results = opt.run(Objective::Sharpe, 5); // top 5 by Sharpe
```

## Walk-Forward Testing

```cpp
WalkForwardTester wft(strategy_factory, data, params);
// Default: 252-bar IS, 63-bar OOS, 63-bar step
auto results = wft.run();
// results[i].is_report / results[i].oos_report — compare IS vs OOS performance
```

## Performance Metrics

All computed automatically at end of backtest:
- **Returns**: Total return, CAGR, annualized volatility
- **Risk-adjusted**: Sharpe ratio, Sortino ratio, Calmar ratio
- **Drawdown**: Max drawdown, average drawdown, max DD duration (bars)
- **Trades**: Win rate, profit factor, average win/loss, expectancy
- **Monte Carlo**: Resampled equity distribution (5th/50th/95th percentile)

## Design Principles

1. **Event-driven, not loop-driven** — bars are events; strategies never see raw arrays
2. **Priority queue** — within same timestamp: MarketData → Orders → Fills (causal order)
3. **No lookahead bias** — orders placed on bar N can only fill on bar N+1 (next open)
4. **Realistic execution** — slippage applied on the correct side, spread added to fill
5. **Modular** — strategy, portfolio, risk, metrics are entirely decoupled modules
6. **Cache-friendly** — bars stored in contiguous vectors; no per-bar heap allocation
7. **Extensible** — add new order types, slippage models, or risk rules without touching core

## Extending to Tick Data

The `MarketDataFeed::next()` → `Bar` pipeline can be replaced with a `Tick` pipeline
by changing the `Event` variant and updating `ExecutionEngine::on_tick()`. The strategy
interface (`on_bar`) would gain `on_tick()` for HFT strategies. The event loop in
`Backtester::run()` is already generic enough to handle this without structural changes.

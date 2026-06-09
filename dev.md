## QuantEngine — Developer Guide

This document is for developers who want to build, extend, debug, or integrate the QuantEngine backtesting system.

**Repository layout**
- [core/](core/): core runtime types and orchestrator (Backtester, EventEngine, Optimizer)
- [data/](data/): market data loaders and synthetic data generators
- [execution/](execution/): execution and order matching models
- [strategy/](strategy/): strategy base class and example strategies
- [portfolio/](portfolio/): portfolio bookkeeping and trade accounting
- [risk/](risk/): pre-trade and portfolio-level risk checks
- [metrics/](metrics/): performance metric calculators
- [tests/](tests/): unit / integration tests and small fixtures
- `main.cpp`: demo runner and CLI
- `CMakeLists.txt`: build configuration

## Build (Linux / WSL / macOS)

Recommended toolchain: `g++`/`clang++` with CMake (C++17). From the repo root:

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Common alternate (single-file quick compile):

```bash
g++ -std=c++17 -O3 -march=native -I. main.cpp -o quant_engine
```

Windows (MSVC): create a Visual Studio generator with CMake, or use MinGW/MSYS2 toolchain. Example (MSVC x64):

```powershell
mkdir build; pushd build
cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
popd
```

## Run

From `build/` (or the binary folder):

```bash
./quant_engine          # run all demos
./quant_engine 1        # demo 1 (MA crossover example)
./quant_engine csv data/SPY.csv SPY   # run backtest from CSV
```

CLI summary (see `main.cpp` for flags):
- `1..4` demo indices
- `csv <path> <symbol>`: run a backtest from a CSV file

## CSV Input Format

Default columns (configurable via `CsvSchema`):

```
date,open,high,low,close,volume
2023-01-03,380.00,385.20,378.50,382.10,75234100
```

Programmatic loader example (in code):

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

## Writing Strategies

Derive from `StrategyBase` and implement `on_bar`, `on_fill`, and `on_end`.

Example (see [strategy/ExampleStrategies.h](strategy/ExampleStrategies.h)):

```cpp
#include "strategy/StrategyBase.h"

struct MyParams { int lookback = 20; double notional = 100000.0; };

class MyStrategy : public bt::StrategyBase {
public:
  explicit MyStrategy(MyParams p = MyParams()) : p_(p) {}

  void on_bar(const bt::Bar& bar) override {
    // submit orders using helper methods
    buy_market(bar.symbol, 100.0);
    sell_limit(bar.symbol, 100.0, bar.close * 1.01);
  }

  void on_fill(const bt::Fill& fill) override { /* update state */ }
  void on_end() override { /* teardown */ }

private:
  MyParams p_;
};

// register with backtester
bt.add_strategy(std::make_shared<MyStrategy>(p), "my_strategy");
```

Key `StrategyBase` methods (browse [strategy/StrategyBase.h](strategy/StrategyBase.h)):
- `buy_market(symbol, qty)` / `sell_market(symbol, qty)`
- `buy_limit(symbol, qty, price)` / `sell_limit(...)`
- `buy_stop(...)` / `sell_stop(...)`

Orders placed during `on_bar` can only fill at the next bar (no lookahead).

## Execution Models & Config

Slippage, commission, and spread are configured via the backtester config object. Example options (see `core/Backtester.h` and config structs):

- Slippage: `SlippageModel::Fixed`, `VolumeImpact`, `VolatilityAdjusted`
- Commission: `per_share`, `min_per_trade`, `max_pct_value`
- Spread: `half_spread_bps`

Example:

```cpp
cfg.slippage.model = SlippageModel::Fixed;
cfg.slippage.fixed_bps = 2.0; // 2 bps
cfg.commission.per_share = 0.005;
cfg.commission.min_per_trade = 1.0;
cfg.spread.half_spread_bps = 1.0;
```

## Risk Controls

Configure via `cfg.risk`:

```cpp
cfg.risk.max_drawdown_pct     = 0.20;  // halt trading after 20% DD
cfg.risk.max_position_pct     = 0.25;  // max 25% of equity per position
cfg.risk.max_gross_leverage   = 4.0;   // 4x gross leverage cap
cfg.risk.daily_loss_limit_pct = 0.03;  // 3% daily loss halt
```

Risk checks run pre-trade in the `RiskManager` (see [risk/RiskManager.h](risk/RiskManager.h)).

## Optimization & Walk-Forward

Grid optimizer usage (see [core/Optimizer.h](core/Optimizer.h)):

```cpp
GridOptimizer opt(strategy_factory, data_factory, bt_cfg);
opt.add_param({"fast", 5, 25, 5});
opt.add_param({"slow", 30, 80, 10});
auto results = opt.run(Objective::Sharpe, 5); // top 5 by Sharpe
```

Walk-forward example (see `WalkForwardTester` in `core/Optimizer.h` or related files):

```cpp
WalkForwardTester wft(strategy_factory, data, params);
// defaults: 252 IS, 63 OOS, 63 step
auto results = wft.run();
```

## Metrics

Performance metrics are calculated automatically at the end of a backtest via `PerformanceMetrics` in [metrics/PerformanceMetrics.h](metrics/PerformanceMetrics.h). Available outputs:
- CAGR, annualized volatility
- Sharpe, Sortino, Calmar
- Max drawdown and DD duration
- Win rate, profit factor, expectancy
- Monte Carlo resampling (percentiles)

## Tests & CI

Unit and integration tests live in [tests/](tests/). Use CTest via the build directory:

```bash
cd build
ctest -j4 --output-on-failure
```

Add tests by creating small fixtures and adding them to `tests/CMakeLists.txt`.

## Developer Notes

- Coding style: follow existing project conventions (no per-bar heap allocations, contiguous containers).
- Use `-O3 -march=native` for release builds; use `-g -O0` for debugging builds.
- Prefer `std::vector`for time series storage; avoid per-bar `new` allocations.
- When adding public headers, update `CMakeLists.txt` accordingly.

### Debugging tips

- Print debug output from `Backtester::run()` or enable logging in the `EventEngine`
- Use address sanitizer and undefined behavior sanitizer (with Clang/GCC):

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
make -j4
```

### Formatting

If you use clang-format, a project style is recommended. Run on changed files before commit.

## Extending to Tick Data

The engine separates the event pipeline from data grain: to add tick-level processing, replace `Bar` events with a `Tick` variant and implement `ExecutionEngine::on_tick()`; strategies can implement `on_tick()` alongside `on_bar()`.

## Where to look in the code

- `core/Backtester.h` — orchestration and configuration
- `core/EventEngine.h` — priority queue and event dispatch
- `execution/ExecutionEngine.h` — order matching and fills
- `strategy/StrategyBase.h` — strategy API
- `metrics/PerformanceMetrics.h` — metrics computation

---

If you'd like, I can:
- add a Doxygen config or generate an API reference, or
- create small examples/tests demonstrating adding a new strategy and running it.

File: [README.dev.md](README.dev.md)

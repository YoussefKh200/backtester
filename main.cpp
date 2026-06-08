// ═══════════════════════════════════════════════════════════════════════════
// QuantEngine — Institutional Backtesting System
// main.cpp — Full demonstration (7 demos)
// ═══════════════════════════════════════════════════════════════════════════
#include "core/Types.h"
#include "core/Backtester.h"
#include "core/Optimizer.h"
#include "data/SyntheticData.h"
#include "data/DataPipeline.h"
#include "strategy/ExampleStrategies.h"
#include "strategy/PortfolioStrategy.h"
#include "strategy/RegimeDetection.h"
#include "metrics/BenchmarkComparison.h"
#include "portfolio/PortfolioConstruction.h"

#include <iostream>
#include <string>
#include <chrono>

using namespace bt;

static void print_params(const ParamSet& ps) {
    for (auto& [k, v] : ps.values) std::cout << "  " << k << " = " << v << "\n";
    std::cout << "  score = " << ps.score << "\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Demo 1: MA crossover multi-asset (SPY + QQQ synthetic, 5yr, Monte Carlo)
// ═══════════════════════════════════════════════════════════════════════════
void demo_ma_crossover() {
    std::cout << "\n╔══════════════════════════════════════════════╗\n";
    std::cout << "║  DEMO 1: MA Crossover — SPY + QQQ 5yr       ║\n";
    std::cout << "╚══════════════════════════════════════════════╝\n";

    SyntheticConfig gc;
    gc.symbol = "SPY"; gc.start_price = 400.0; gc.n_bars = 1260;
    gc.annual_ret = 0.10; gc.annual_vol = 0.17; gc.seed = 1234;
    auto spy = SyntheticDataGen::generate(gc);

    gc.symbol = "QQQ"; gc.start_price = 330.0;
    gc.annual_ret = 0.12; gc.annual_vol = 0.22; gc.seed = 5678;
    auto qqq = SyntheticDataGen::generate(gc);

    BacktestConfig cfg;
    cfg.initial_cash  = 1'000'000.0;
    cfg.annual_rf     = 0.05;
    cfg.bars_per_year = 252;
    cfg.verbose       = true;
    cfg.run_monte_carlo = true;
    cfg.mc_simulations  = 500;
    cfg.slippage.model       = SlippageModel::Fixed;
    cfg.slippage.fixed_bps   = 2.0;
    cfg.spread.half_spread_bps = 1.0;
    cfg.commission.per_share = 0.005;
    cfg.risk.max_drawdown_pct = 0.25;

    Backtester bt(cfg);
    bt.add_data(spy);
    bt.add_data(qqq);

    MACrossParams ma_p;
    ma_p.fast_period = 10; ma_p.slow_period = 50; ma_p.notional = 400'000.0;
    bt.add_strategy(std::make_shared<MACrossStrategy>(ma_p), "MA_10_50");

    auto t0 = std::chrono::high_resolution_clock::now();
    bt.run();
    auto t1 = std::chrono::high_resolution_clock::now();
    std::printf("[Timing] %.1f ms for %zu bars\n",
        std::chrono::duration<double,std::milli>(t1-t0).count(),
        spy.size()+qqq.size());

    bt.export_equity_csv("/tmp/equity_demo1.csv");
    bt.export_trades_csv("/tmp/trades_demo1.csv");
    std::cout << "[Output] /tmp/equity_demo1.csv | /tmp/trades_demo1.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Demo 2: Multi-strategy portfolio (3 assets × 3 strategies)
// ═══════════════════════════════════════════════════════════════════════════
void demo_multi_strategy() {
    std::cout << "\n╔══════════════════════════════════════════════╗\n";
    std::cout << "║  DEMO 2: Multi-Strategy Portfolio            ║\n";
    std::cout << "╚══════════════════════════════════════════════╝\n";

    std::vector<std::tuple<std::string,double,double,unsigned>> assets = {
        {"AAPL_SIM", 175.0, 0.28, 137},
        {"MSFT_SIM", 380.0, 0.24, 274},
        {"SPY_SIM",  450.0, 0.16, 411},
    };

    BacktestConfig cfg;
    cfg.initial_cash = 3'000'000.0;
    cfg.verbose = true;

    Backtester bt(cfg);
    for (auto& [sym, px, vol, seed] : assets) {
        SyntheticConfig gc;
        gc.symbol = sym; gc.start_price = px; gc.n_bars = 756;
        gc.annual_vol = vol; gc.seed = seed;
        bt.add_data(SyntheticDataGen::generate(gc));
    }

    MACrossParams ma; ma.fast_period = 5; ma.slow_period = 20; ma.notional = 200'000.0;
    bt.add_strategy(std::make_shared<MACrossStrategy>(ma), "MA_5_20");

    MeanRevParams mr; mr.lookback = 30; mr.entry_z = -1.8; mr.notional = 150'000.0;
    bt.add_strategy(std::make_shared<MeanReversionStrategy>(mr), "MeanRev");

    BreakoutParams bo; bo.channel_period = 20; bo.notional = 100'000.0;
    bt.add_strategy(std::make_shared<BreakoutStrategy>(bo), "Breakout");

    bt.run();
    bt.export_equity_csv("/tmp/equity_demo2.csv");
    bt.export_trades_csv("/tmp/trades_demo2.csv");
}

// ═══════════════════════════════════════════════════════════════════════════
// Demo 3: Grid search parameter optimization
// ═══════════════════════════════════════════════════════════════════════════
void demo_optimization() {
    std::cout << "\n╔══════════════════════════════════════════════╗\n";
    std::cout << "║  DEMO 3: MA Parameter Optimization           ║\n";
    std::cout << "╚══════════════════════════════════════════════╝\n";

    SyntheticConfig gc; gc.symbol = "OPT"; gc.n_bars = 500; gc.seed = 999;
    auto data = SyntheticDataGen::generate(gc);

    BacktestConfig bt_cfg; bt_cfg.initial_cash = 500'000.0; bt_cfg.verbose = false;

    StrategyFactory sf = [](const ParamSet& ps) -> std::shared_ptr<StrategyBase> {
        MACrossParams p;
        p.fast_period = (int)ps.values.at("fast");
        p.slow_period = (int)ps.values.at("slow");
        p.notional    = 200'000.0;
        return std::make_shared<MACrossStrategy>(p);
    };
    DataFactory df = [&data](Backtester& bt) { bt.add_data(data); };

    GridOptimizer opt(sf, df, bt_cfg);
    opt.add_param({"fast", 5, 25, 5});
    opt.add_param({"slow", 30, 80, 10});
    auto results = opt.run(Objective::Sharpe, 5);

    std::cout << "\n── Top 5 by Sharpe ──\n";
    for (int i = 0; i < (int)results.size(); ++i) {
        std::cout << "#" << (i+1) << ":\n";
        print_params(results[i]);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Demo 4: CSV file backtest (skipped if file not found)
// ═══════════════════════════════════════════════════════════════════════════
void demo_csv_load(const std::string& path, const std::string& symbol) {
    std::cout << "\n╔══════════════════════════════════════════════╗\n";
    std::cout << "║  DEMO 4: CSV File Backtest                   ║\n";
    std::cout << "╚══════════════════════════════════════════════╝\n";

    BacktestConfig cfg; cfg.initial_cash = 1'000'000.0; cfg.verbose = true;
    Backtester bt(cfg);
    try { bt.load_csv(path, symbol); }
    catch (const std::exception& e) { std::cout << "[SKIP] " << e.what() << "\n"; return; }

    MACrossParams p; p.fast_period = 10; p.slow_period = 50; p.notional = 300'000.0;
    bt.add_strategy(std::make_shared<MACrossStrategy>(p), "MA_CSV");
    bt.run();
}

// ═══════════════════════════════════════════════════════════════════════════
// Demo 5: Risk-parity portfolio strategy (5 assets, dynamic rebalancing)
// ═══════════════════════════════════════════════════════════════════════════
void demo_portfolio_construction() {
    std::cout << "\n╔══════════════════════════════════════════════╗\n";
    std::cout << "║  DEMO 5: Risk-Parity Portfolio Strategy      ║\n";
    std::cout << "╚══════════════════════════════════════════════╝\n";

    struct A { std::string s; double px, v, r; unsigned seed; };
    std::vector<A> assets = {
        {"SPY_RP", 450.0, 0.15, 0.10, 101},
        {"QQQ_RP", 380.0, 0.22, 0.12, 202},
        {"IWM_RP", 185.0, 0.20, 0.08, 303},
        {"GLD_RP", 190.0, 0.14, 0.05, 404},
        {"TLT_RP", 100.0, 0.12, 0.03, 505},
    };

    BacktestConfig cfg; cfg.initial_cash = 2'000'000.0; cfg.verbose = true;
    Backtester bt(cfg);

    for (auto& a : assets) {
        SyntheticConfig gc; gc.symbol = a.s; gc.start_price = a.px;
        gc.n_bars = 756; gc.annual_vol = a.v; gc.annual_ret = a.r; gc.seed = a.seed;
        bt.add_data(SyntheticDataGen::generate(gc));
    }

    PortfolioStrategyParams pp;
    pp.fast_period = 10; pp.slow_period = 50;
    pp.rebal_freq = 21; pp.total_notional = 1'800'000.0;
    pp.method = AllocationMethod::RiskParity;
    bt.add_strategy(std::make_shared<PortfolioAwareStrategy>(pp), "RiskParity");
    bt.run();

    bt.export_equity_csv("/tmp/equity_demo5.csv");
    std::cout << "[Output] /tmp/equity_demo5.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Demo 6: Regime-filtered strategy vs buy-and-hold tearsheet
// ═══════════════════════════════════════════════════════════════════════════
void demo_regime_filtered() {
    std::cout << "\n╔══════════════════════════════════════════════╗\n";
    std::cout << "║  DEMO 6: Regime-Filtered vs Buy & Hold       ║\n";
    std::cout << "╚══════════════════════════════════════════════╝\n";

    SyntheticConfig gc;
    gc.symbol = "SPY_RG"; gc.start_price = 400.0; gc.n_bars = 1008;
    gc.annual_vol = 0.18; gc.annual_ret = 0.09; gc.seed = 7777;
    gc.regime_prob_switch = 0.03; gc.bear_vol_mult = 2.5; gc.bear_ret_adj = -0.40;
    auto data = SyntheticDataGen::generate(gc);

    BacktestConfig cfg;
    cfg.initial_cash = 1'000'000.0; cfg.bars_per_year = 252; cfg.verbose = false;

    RFMAParams rp;
    rp.fast_period = 10; rp.slow_period = 50;
    rp.notional = 900'000.0; rp.min_bull_prob = 0.55;

    BenchmarkRunner runner(cfg);
    runner.run(data,
               std::make_shared<RegimeFilteredMAStrategy>(rp),
               "RegimeMA", "SPY_RG", "Buy&Hold");
}

// ═══════════════════════════════════════════════════════════════════════════
// Demo 7: Parallel data pipeline — 8 assets preprocessed concurrently
// ═══════════════════════════════════════════════════════════════════════════
void demo_parallel_pipeline() {
    std::cout << "\n╔══════════════════════════════════════════════╗\n";
    std::cout << "║  DEMO 7: Parallel Data Pipeline (8 assets)   ║\n";
    std::cout << "╚══════════════════════════════════════════════╝\n";

    ParallelDataPipeline pipeline;

    for (int i = 0; i < 8; ++i) {
        SyntheticConfig gc;
        gc.symbol = "ASSET_" + std::to_string(i);
        gc.n_bars = 1000; gc.annual_vol = 0.15 + i*0.02;
        gc.annual_ret = 0.08 + i*0.01; gc.seed = 1000 + i*113;
        pipeline.add_bars(gc.symbol, SyntheticDataGen::generate(gc));
    }

    std::printf("[Pipeline] %zu hardware threads available\n", pipeline.thread_count());

    auto t0 = std::chrono::high_resolution_clock::now();
    auto results = pipeline.run();
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double,std::milli>(t1-t0).count();

    std::size_t total = 0;
    for (auto& r : results) {
        if (!r.error.empty()) { std::cerr << "[ERR] " << r.error << "\n"; continue; }
        total += r.enriched_bars.size();
        const auto& last = r.enriched_bars.back();
        std::printf("  %-12s  bars=%-5zu  VWAP=%7.2f  ATR=%5.2f  RSI=%5.1f  BB%%=%.2f\n",
            r.symbol.c_str(), r.enriched_bars.size(),
            last.vwap, last.atr14, last.rsi14, last.bb_pct);
    }
    std::printf("[Pipeline] %zu enriched bars in %.1f ms\n", total, ms);

    // Run backtest on preprocessed data
    BacktestConfig cfg; cfg.initial_cash = 1'000'000.0; cfg.verbose = true;
    Backtester bt(cfg);
    for (auto& r : results) {
        std::vector<Bar> raw;
        raw.reserve(r.enriched_bars.size());
        for (auto& eb : r.enriched_bars) raw.push_back(eb.bar);
        bt.add_data(std::move(raw));
    }

    MACrossParams ma; ma.fast_period = 10; ma.slow_period = 50; ma.notional = 100'000.0;
    bt.add_strategy(std::make_shared<MACrossStrategy>(ma), "MA_Pipeline");
    bt.run();
}

// ═══════════════════════════════════════════════════════════════════════════
int main(int argc, char** argv) {
    std::cout << "\n";
    std::cout << "  ██████╗ ██╗   ██╗ █████╗ ███╗   ██╗████████╗\n";
    std::cout << " ██╔═══██╗██║   ██║██╔══██╗████╗  ██║╚══██╔══╝\n";
    std::cout << " ██║   ██║██║   ██║███████║██╔██╗ ██║   ██║   \n";
    std::cout << " ██║▄▄ ██║██║   ██║██╔══██║██║╚██╗██║   ██║   \n";
    std::cout << " ╚██████╔╝╚██████╔╝██║  ██║██║ ╚████║   ██║   \n";
    std::cout << "  ╚══▀▀═╝  ╚═════╝ ╚═╝  ╚═╝╚═╝  ╚═══╝   ╚═╝  \n";
    std::cout << " QuantEngine — Institutional Backtesting System v2.0\n\n";

    bool run_all = (argc < 2);
    std::string sel = (argc >= 2) ? argv[1] : "";

    if (run_all || sel == "1") demo_ma_crossover();
    if (run_all || sel == "2") demo_multi_strategy();
    if (run_all || sel == "3") demo_optimization();
    if (run_all || sel == "5") demo_portfolio_construction();
    if (run_all || sel == "6") demo_regime_filtered();
    if (run_all || sel == "7") demo_parallel_pipeline();
    if (argc >= 4 && sel == "csv") demo_csv_load(argv[2], argv[3]);

    return 0;
}

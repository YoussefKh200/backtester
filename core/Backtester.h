#pragma once
#include "Types.h"
#include "EventEngine.h"
#include "../data/MarketDataLoader.h"
#include "../execution/ExecutionEngine.h"
#include "../portfolio/Portfolio.h"
#include "../risk/RiskManager.h"
#include "../metrics/PerformanceMetrics.h"
#include "../strategy/StrategyBase.h"
#include <memory>
#include <vector>
#include <unordered_map>
#include <iostream>
#include <functional>

namespace bt {

struct BacktestConfig {
    double initial_cash     = 1'000'000.0;
    double annual_rf        = 0.05;
    int    bars_per_year    = 252;
    bool   verbose          = true;
    bool   run_monte_carlo  = false;
    int    mc_simulations   = 1000;

    SlippageConfig  slippage;
    SpreadConfig    spread;
    CommissionModel commission;
    RiskConfig      risk;
};

class Backtester {
public:
    explicit Backtester(BacktestConfig cfg = {})
        : cfg_(cfg)
        , portfolio_(cfg.initial_cash, cfg.risk.max_gross_leverage)
        , risk_(cfg.risk)
        , ev_eng_()
        , exec_(ev_eng_, cfg.slippage, cfg.spread, cfg.commission)
    {
        // Wire fill events → portfolio
        ev_eng_.subscribe<FillEvent>([this](const Event& ev) {
            const auto& fe = std::get<FillEvent>(ev);
            portfolio_.on_fill(fe.fill);
            // Route fill to owning strategy
            auto it = strategy_map_.find(fe.fill.strategy_id);
            if (it != strategy_map_.end())
                it->second->on_fill(fe.fill);
        });
    }

    // ── Add data ───────────────────────────────────────────────────────────
    void add_data(std::vector<Bar> bars) {
        feed_.add_asset(std::move(bars));
    }

    void load_csv(const std::string& path, const std::string& symbol,
                  CsvSchema schema = {}) {
        CsvLoader loader(schema);
        add_data(loader.load(path, symbol));
    }

    // ── Add strategy ───────────────────────────────────────────────────────
    void add_strategy(std::shared_ptr<StrategyBase> strat, std::string id = "") {
        if (id.empty()) id = "strat_" + std::to_string(strategies_.size());
        strat->set_id(id);
        strategy_map_[id] = strat;
        strategies_.push_back(strat);
    }

    // ── Run backtest ───────────────────────────────────────────────────────
    PerformanceReport run() {
        if (feed_.exhausted()) throw std::runtime_error("No data loaded");
        feed_.reset();

        // Start strategies
        for (auto& s : strategies_) s->on_start(exec_);

        std::size_t bar_count = 0;
        std::size_t total     = feed_.total_bars();
        Timestamp   prev_day  = 0;
        double      start_eq  = cfg_.initial_cash;

        // ── Main event loop ───────────────────────────────────────────────
        while (true) {
            auto maybe_bar = feed_.next();
            if (!maybe_bar) break;
            const Bar& bar = *maybe_bar;

            // Daily reset for risk tracking
            Timestamp day = bar.ts / 86400000LL;
            if (day != prev_day) {
                risk_.reset_daily();
                prev_day = day;
                start_eq = portfolio_.current_equity();
            }

            // 1. Execution engine processes bar (fills pending orders)
            exec_.on_bar(bar);

            // 2. MTM portfolio at close
            portfolio_.mark_to_market(bar.symbol, bar.close);

            // 3. Snapshot equity
            portfolio_.snapshot(bar.ts);

            // 4. Risk check — halt if daily loss exceeded
            if (risk_.daily_loss_halt(portfolio_.current_equity())) {
                if (cfg_.verbose)
                    std::cerr << "[RISK] Daily loss limit hit at ts="
                              << bar.ts << ", halting trading today\n";
                exec_.cancel_all(); // cancel all pending on halt
            }

            // 5. Dispatch market data event to strategies
            for (auto& strat : strategies_) {
                // Simple filter: all strategies see all bars for now
                strat->on_bar(bar);
            }

            ++bar_count;
            if (cfg_.verbose && bar_count % 10000 == 0) {
                std::cout << "\r[Backtester] " << bar_count << " / "
                          << total << " bars processed..." << std::flush;
            }
        }

        if (cfg_.verbose) std::cout << "\n[Backtester] Done. " << bar_count << " bars.\n";

        // Notify strategies — allows clean exit positions
        for (auto& s : strategies_) s->on_end();

        // Compute metrics
        auto report = PerformanceMetrics::compute(
            portfolio_, cfg_.annual_rf, cfg_.bars_per_year);

        if (cfg_.verbose) {
            std::cout << PerformanceMetrics::format(report);

            if (cfg_.run_monte_carlo) {
                auto mc = PerformanceMetrics::monte_carlo(
                    portfolio_, cfg_.mc_simulations);
                std::cout << "\n── Monte Carlo (" << cfg_.mc_simulations << " sims) ──\n";
                std::printf("  Median Final Equity: $%.2f\n", mc.median_final_equity);
                std::printf("  5th Pct Equity:      $%.2f\n", mc.pct5_final_equity);
                std::printf("  95th Pct Equity:     $%.2f\n", mc.pct95_final_equity);
                std::printf("  95th Pct Max DD:     %.2f%%\n", mc.pct5_max_drawdown * 100);
            }
        }

        return report;
    }

    // ── Walk-forward testing ───────────────────────────────────────────────
    // Splits data into in-sample / out-of-sample windows
    struct WalkForwardResult {
        std::vector<PerformanceReport> is_reports;   // in-sample
        std::vector<PerformanceReport> oos_reports;  // out-of-sample
        PerformanceReport              combined;
    };

    // ── Export equity curve ────────────────────────────────────────────────
    void export_equity_csv(const std::string& path) const {
        std::ofstream f(path);
        f << "timestamp,equity,cash,gross_exposure,net_exposure\n";
        for (auto& p : portfolio_.equity_curve())
            f << p.ts << "," << p.equity << "," << p.cash << ","
              << p.gross_exposure << "," << p.net_exposure << "\n";
    }

    void export_trades_csv(const std::string& path) const {
        std::ofstream f(path);
        f << "symbol,strategy,side,qty,entry_px,exit_px,entry_ts,exit_ts,pnl,commission\n";
        for (auto& t : portfolio_.trades())
            f << t.symbol << "," << t.strategy_id << ","
              << (t.entry_side == OrderSide::Buy ? "LONG" : "SHORT") << ","
              << t.qty << "," << t.entry_px << "," << t.exit_px << ","
              << t.entry_ts << "," << t.exit_ts << ","
              << t.pnl << "," << t.commission << "\n";
    }

    const Portfolio&  portfolio()   const { return portfolio_; }
    const RiskManager& risk_mgr()   const { return risk_; }

private:
    BacktestConfig cfg_;
    Portfolio      portfolio_;
    RiskManager    risk_;
    EventEngine    ev_eng_;
    ExecutionEngine exec_;
    MarketDataFeed  feed_;

    std::vector<std::shared_ptr<StrategyBase>>          strategies_;
    std::unordered_map<std::string, std::shared_ptr<StrategyBase>> strategy_map_;
};

} // namespace bt

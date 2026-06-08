#pragma once
#include "../core/Backtester.h"
#include <vector>
#include <functional>
#include <algorithm>
#include <limits>
#include <iostream>

namespace bt {

struct ParamRange {
    std::string name;
    double      start, stop, step;
    std::vector<double> values() const {
        std::vector<double> v;
        for (double x = start; x <= stop + 1e-12; x += step) v.push_back(x);
        return v;
    }
};

struct ParamSet {
    std::unordered_map<std::string, double> values;
    double score = std::numeric_limits<double>::lowest();
};

using StrategyFactory = std::function<std::shared_ptr<StrategyBase>(const ParamSet&)>;
using DataFactory     = std::function<void(Backtester&)>;

enum class Objective { Sharpe, Sortino, CAGR, ProfitFactor, Calmar };

inline double extract_score(const PerformanceReport& r, Objective obj) {
    switch (obj) {
    case Objective::Sharpe:       return r.sharpe_ratio;
    case Objective::Sortino:      return r.sortino_ratio;
    case Objective::CAGR:         return r.cagr;
    case Objective::ProfitFactor: return r.profit_factor;
    case Objective::Calmar:       return r.calmar_ratio;
    }
    return r.sharpe_ratio;
}

// ── Grid search optimizer ──────────────────────────────────────────────────
class GridOptimizer {
public:
    GridOptimizer(StrategyFactory sf, DataFactory df, BacktestConfig base_cfg = BacktestConfig())
        : strat_factory_(sf), data_factory_(df), base_cfg_(base_cfg) {
        base_cfg_.verbose = false;
    }
    void add_param(ParamRange r) { params_.push_back(std::move(r)); }

    std::vector<ParamSet> run(Objective obj = Objective::Sharpe, int top_n = 5) {
        std::vector<ParamSet> grid = {ParamSet{}};
        for (auto& pr : params_) {
            std::vector<ParamSet> expanded;
            for (auto& ps : grid)
                for (double v : pr.values()) {
                    ParamSet np = ps; np.values[pr.name] = v;
                    expanded.push_back(np);
                }
            grid = std::move(expanded);
        }
        std::cout << "[Optimizer] Testing " << grid.size() << " combinations\n";
        int done = 0;
        for (auto& ps : grid) {
            Backtester bt(base_cfg_);
            data_factory_(bt);
            bt.add_strategy(strat_factory_(ps));
            auto report = bt.run();
            ps.score = extract_score(report, obj);
            if (++done % 10 == 0)
                std::cout << "\r[Optimizer] " << done << "/" << grid.size() << std::flush;
        }
        std::cout << "\n";
        std::sort(grid.begin(), grid.end(),
                  [](const ParamSet& a, const ParamSet& b){ return a.score > b.score; });
        if (top_n > (int)grid.size()) top_n = (int)grid.size();
        grid.resize(top_n);
        return grid;
    }

private:
    StrategyFactory strat_factory_;
    DataFactory     data_factory_;
    BacktestConfig  base_cfg_;
    std::vector<ParamRange> params_;
};

// ── Walk-forward config (outside class) ────────────────────────────────────
struct WFConfig {
    int       is_bars  = 252;
    int       oos_bars = 63;
    int       step     = 63;
    Objective objective = Objective::Sharpe;
};

// ── Walk-forward tester ────────────────────────────────────────────────────
class WalkForwardTester {
public:
    WalkForwardTester(StrategyFactory sf, std::vector<Bar> data,
                      std::vector<ParamRange> params,
                      WFConfig cfg = WFConfig(),
                      BacktestConfig bt_cfg = BacktestConfig())
        : strat_factory_(sf), data_(std::move(data))
        , params_(std::move(params)), wf_cfg_(cfg), bt_cfg_(bt_cfg) {
        bt_cfg_.verbose = false;
    }

    struct WFResult {
        int               window_idx;
        ParamSet          best_is_params;
        PerformanceReport is_report;
        PerformanceReport oos_report;
    };

    std::vector<WFResult> run() {
        std::vector<WFResult> results;
        int total = (int)data_.size(), start = 0;

        while (start + wf_cfg_.is_bars + wf_cfg_.oos_bars <= total) {
            int is_end  = start + wf_cfg_.is_bars;
            int oos_end = std::min(is_end + wf_cfg_.oos_bars, total);

            std::vector<Bar> is_data (data_.begin() + start,  data_.begin() + is_end);
            std::vector<Bar> oos_data(data_.begin() + is_end, data_.begin() + oos_end);

            GridOptimizer opt(strat_factory_,
                [&is_data](Backtester& bt){ bt.add_data(is_data); }, bt_cfg_);
            for (auto& pr : params_) opt.add_param(pr);
            auto best = opt.run(wf_cfg_.objective, 1);
            if (best.empty()) { start += wf_cfg_.step; continue; }
            ParamSet& bp = best[0];

            Backtester is_bt(bt_cfg_); is_bt.add_data(is_data);
            is_bt.add_strategy(strat_factory_(bp));
            auto is_rpt = is_bt.run();

            Backtester oos_bt(bt_cfg_); oos_bt.add_data(oos_data);
            oos_bt.add_strategy(strat_factory_(bp));
            auto oos_rpt = oos_bt.run();

            std::cout << "[WalkForward] Window " << results.size()
                      << "  IS Sharpe="  << is_rpt.sharpe_ratio
                      << "  OOS Sharpe=" << oos_rpt.sharpe_ratio << "\n";

            results.push_back({(int)results.size(), bp, is_rpt, oos_rpt});
            start += wf_cfg_.step;
        }
        return results;
    }

private:
    StrategyFactory       strat_factory_;
    std::vector<Bar>      data_;
    std::vector<ParamRange> params_;
    WFConfig              wf_cfg_;
    BacktestConfig        bt_cfg_;
};

} // namespace bt

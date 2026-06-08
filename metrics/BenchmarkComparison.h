#pragma once
// ── Benchmark Comparison Framework ────────────────────────────────────────
// Runs a benchmark strategy (e.g. buy-and-hold on SPY) alongside the
// primary strategy and produces a side-by-side performance tearsheet.
// Computes information ratio, tracking error, beta, alpha.
// ──────────────────────────────────────────────────────────────────────────
#include "../core/Types.h"
#include "../core/Backtester.h"
#include "../metrics/PerformanceMetrics.h"
#include "../strategy/StrategyBase.h"
#include <vector>
#include <string>
#include <cmath>
#include <numeric>
#include <sstream>
#include <iomanip>

namespace bt {

// ── Buy-and-hold benchmark strategy ───────────────────────────────────────
struct BHStrategyParams {
    std::string symbol;
    double      notional = 950000.0;
};

class BuyAndHoldStrategy : public StrategyBase {
public:
    explicit BuyAndHoldStrategy(BHStrategyParams p = BHStrategyParams()) : p_(p) {}

    void on_bar(const Bar& bar) override {
        if (bar.symbol != p_.symbol || bought_) return;
        double qty = std::floor(p_.notional / bar.close);
        if (qty > 0) { buy_market(bar.symbol, qty); bought_ = true; }
    }

    void on_end() override {
        // Hold to the end — no exit
    }

private:
    BHStrategyParams p_;
    bool   bought_ = false;
};

// ── 60/40 benchmark ────────────────────────────────────────────────────────
struct SFParams {
    std::string equity_symbol = "SPY";
    std::string bond_symbol   = "TLT";
    double      equity_pct    = 0.60;
    double      bond_pct      = 0.40;
    double      total_notional = 950000.0;
    int         rebal_freq    = 63;
};

class SixtyFortyStrategy : public StrategyBase {
public:
explicit SixtyFortyStrategy(SFParams p = SFParams()) : p_(p) {}

    void on_bar(const Bar& bar) override {
        ++bar_count_;
        last_prices_[bar.symbol] = bar.close;

        // Initial buy
        if (!init_done_) {
            if (last_prices_.count(p_.equity_symbol) &&
                last_prices_.count(p_.bond_symbol)) {
                execute_rebal();
                init_done_ = true;
            }
            return;
        }

        // Periodic rebalance
        if (bar_count_ % p_.rebal_freq == 0) execute_rebal();
    }

private:
    void execute_rebal() {
        if (!last_prices_.count(p_.equity_symbol) ||
            !last_prices_.count(p_.bond_symbol)) return;
        // Simplified: just buy target qty (production would track positions)
        double eq_notional   = p_.total_notional * p_.equity_pct;
        double bond_notional = p_.total_notional * p_.bond_pct;
        double eq_qty   = std::floor(eq_notional / last_prices_[p_.equity_symbol]);
        double bond_qty = std::floor(bond_notional / last_prices_[p_.bond_symbol]);
        if (eq_qty   > 0) buy_market(p_.equity_symbol, eq_qty);
        if (bond_qty > 0) buy_market(p_.bond_symbol,   bond_qty);
    }

    SFParams p_;
    std::unordered_map<std::string, double> last_prices_;
    int  bar_count_ = 0;
    bool init_done_ = false;
};

// ── Benchmark statistics ───────────────────────────────────────────────────
struct BenchmarkStats {
    // Primary vs benchmark
    double alpha            = 0;   // Jensen's alpha (annualized)
    double beta             = 0;   // market beta
    double correlation      = 0;   // return correlation
    double tracking_error   = 0;   // annualized TE
    double information_ratio= 0;   // (return - benchmark_return) / TE
    double up_capture       = 0;   // % of benchmark up days captured
    double down_capture     = 0;   // % of benchmark down days captured
};

inline BenchmarkStats compute_benchmark_stats(
    const std::vector<EquityPoint>& strategy_curve,
    const std::vector<EquityPoint>& benchmark_curve,
    int bars_per_year = 252,
    double rf_rate    = 0.05)
{
    BenchmarkStats bs;
    if (strategy_curve.size() < 10 || benchmark_curve.size() < 10) return bs;

    // Align curves by taking common length (both should be same timeline)
    std::size_t n = std::min(strategy_curve.size(), benchmark_curve.size()) - 1;

    std::vector<double> strat_rets, bench_rets;
    strat_rets.reserve(n);
    bench_rets.reserve(n);

    for (std::size_t i = 1; i <= n; ++i) {
        double sp = strategy_curve[i-1].equity;
        double bp = benchmark_curve[i-1].equity;
        if (sp > 0 && bp > 0) {
            strat_rets.push_back((strategy_curve[i].equity - sp) / sp);
            bench_rets.push_back((benchmark_curve[i].equity - bp) / bp);
        }
    }

    if (strat_rets.size() < 5) return bs;

    double rf_d = rf_rate / bars_per_year;
    auto mean = [](const std::vector<double>& v) {
        return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    };
    auto std_dev = [&mean](const std::vector<double>& v) {
        double m = mean(v), s = 0;
        for (double x : v) s += (x-m)*(x-m);
        return std::sqrt(s / (v.size()-1));
    };

    double ms = mean(strat_rets), mb = mean(bench_rets);
    double ss = std_dev(strat_rets), sb = std_dev(bench_rets);

    // Covariance & correlation
    double cov = 0;
    for (std::size_t i = 0; i < strat_rets.size(); ++i)
        cov += (strat_rets[i]-ms) * (bench_rets[i]-mb);
    cov /= (strat_rets.size()-1);

    bs.correlation = (ss*sb > 0) ? cov / (ss*sb) : 0;
    bs.beta        = (sb*sb > 0) ? cov / (sb*sb) : 1;

    // Jensen's alpha: α = Rs - [Rf + β(Rb - Rf)]  (annualized)
    double annual_rs = ms * bars_per_year;
    double annual_rb = mb * bars_per_year;
    bs.alpha = annual_rs - (rf_rate + bs.beta * (annual_rb - rf_rate));

    // Tracking error
    std::vector<double> active_rets;
    for (std::size_t i = 0; i < strat_rets.size(); ++i)
        active_rets.push_back(strat_rets[i] - bench_rets[i]);
    bs.tracking_error = std_dev(active_rets) * std::sqrt(bars_per_year);

    // Information ratio
    double mean_active = mean(active_rets);
    bs.information_ratio = (bs.tracking_error > 0)
        ? (mean_active * bars_per_year) / bs.tracking_error : 0;

    // Up/down capture
    double up_s = 0, up_b = 0, dn_s = 0, dn_b = 0;
    int up_n = 0, dn_n = 0;
    for (std::size_t i = 0; i < strat_rets.size(); ++i) {
        if (bench_rets[i] > 0) {
            up_s += strat_rets[i]; up_b += bench_rets[i]; ++up_n;
        } else if (bench_rets[i] < 0) {
            dn_s += strat_rets[i]; dn_b += bench_rets[i]; ++dn_n;
        }
    }
    bs.up_capture   = (up_n > 0 && up_b != 0) ? (up_s/up_n)/(up_b/up_n)*100 : 0;
    bs.down_capture = (dn_n > 0 && dn_b != 0) ? (dn_s/dn_n)/(dn_b/dn_n)*100 : 0;

    return bs;
}

// ── Tearsheet ──────────────────────────────────────────────────────────────
struct Tearsheet {
    PerformanceReport strategy_report;
    PerformanceReport benchmark_report;
    BenchmarkStats    relative_stats;
    std::string       strategy_name;
    std::string       benchmark_name;
};

inline std::string format_tearsheet(const Tearsheet& ts) {
    std::ostringstream ss;
    auto f2 = [](double v){ char b[32]; snprintf(b,32,"%.2f",v); return std::string(b); };
    auto f3 = [](double v){ char b[32]; snprintf(b,32,"%.3f",v); return std::string(b); };
    auto pct= [](double v){ char b[32]; snprintf(b,32,"%+.2f%%",v); return std::string(b); };

    ss << "\n╔══════════════════════════════════════════════════════════════╗\n";
    ss << "║                   PERFORMANCE TEARSHEET                     ║\n";
    ss << "╠══════════════════════════════════════════════════════════════╣\n";

    auto col = [&](const char* label, std::string sv, std::string bv){
        ss << "║  " << std::left << std::setw(22) << label
           << std::setw(18) << sv
           << std::setw(18) << bv << " ║\n";
    };

    ss << "║  " << std::left << std::setw(22) << "Metric"
       << std::setw(18) << ts.strategy_name
       << std::setw(18) << ts.benchmark_name << " ║\n";
    ss << "╠══════════════════════════════════════════════════════════════╣\n";

    const auto& s = ts.strategy_report;
    const auto& b = ts.benchmark_report;

    col("Total Return",    pct(s.total_return_pct),  pct(b.total_return_pct));
    col("CAGR",            pct(s.cagr),              pct(b.cagr));
    col("Ann. Volatility", f2(s.annualized_vol)+"%", f2(b.annualized_vol)+"%");
    col("Sharpe Ratio",    f3(s.sharpe_ratio),       f3(b.sharpe_ratio));
    col("Sortino Ratio",   f3(s.sortino_ratio),      f3(b.sortino_ratio));
    col("Max Drawdown",    "-"+f2(s.max_drawdown)+"%","-"+f2(b.max_drawdown)+"%");
    col("Calmar Ratio",    f3(s.calmar_ratio),       f3(b.calmar_ratio));
    col("Win Rate",        f2(s.win_rate*100)+"%",   f2(b.win_rate*100)+"%");
    col("Profit Factor",   f3(s.profit_factor),      f3(b.profit_factor));

    ss << "╠══════════════════════════════════════════════════════════════╣\n";
    ss << "║                  RELATIVE TO BENCHMARK                      ║\n";
    ss << "╠══════════════════════════════════════════════════════════════╣\n";

    const auto& r = ts.relative_stats;
    auto row = [&](const char* label, std::string v){
        ss << "║  " << std::left << std::setw(30) << label
           << std::setw(28) << v << " ║\n";
    };

    row("Alpha (annualized)",     pct(r.alpha * 100));
    row("Beta",                   f3(r.beta));
    row("Correlation",            f3(r.correlation));
    row("Tracking Error",         f2(r.tracking_error*100)+"%");
    row("Information Ratio",      f3(r.information_ratio));
    row("Up Capture Ratio",       f2(r.up_capture)+"%");
    row("Down Capture Ratio",     f2(r.down_capture)+"%");

    ss << "╚══════════════════════════════════════════════════════════════╝\n";
    return ss.str();
}

// ── Benchmark runner convenience wrapper ───────────────────────────────────
class BenchmarkRunner {
public:
    BenchmarkRunner(BacktestConfig cfg = BacktestConfig()) : cfg_(cfg) {}

    Tearsheet run(
        std::vector<Bar>               data,
        std::shared_ptr<StrategyBase>  strategy,
        std::string                    strategy_name,
        std::string                    benchmark_symbol,
        std::string                    benchmark_name = "Buy & Hold")
    {
        // Run strategy
        Backtester strat_bt(cfg_);
        strat_bt.add_data(data);
        strat_bt.add_strategy(strategy, strategy_name);
        auto strat_report = strat_bt.run();

        // Run buy-and-hold benchmark on same data
        BacktestConfig bench_cfg = cfg_;
        bench_cfg.verbose = false;
        Backtester bench_bt(bench_cfg);
        bench_bt.add_data(data);

        BHStrategyParams bh_p;
        bh_p.symbol   = benchmark_symbol;
        bh_p.notional = cfg_.initial_cash * 0.95;
        bench_bt.add_strategy(
            std::make_shared<BuyAndHoldStrategy>(bh_p), "benchmark");
        auto bench_report = bench_bt.run();

        // Compute relative stats
        auto rel = compute_benchmark_stats(
            strat_bt.portfolio().equity_curve(),
            bench_bt.portfolio().equity_curve(),
            cfg_.bars_per_year, cfg_.annual_rf);

        Tearsheet ts;
        ts.strategy_report  = strat_report;
        ts.benchmark_report = bench_report;
        ts.relative_stats   = rel;
        ts.strategy_name    = strategy_name;
        ts.benchmark_name   = benchmark_name;

        std::cout << format_tearsheet(ts);
        return ts;
    }

private:
    BacktestConfig cfg_;
};

} // namespace bt

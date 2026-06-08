#pragma once
#include "../core/Types.h"
#include "../portfolio/Portfolio.h"
#include <vector>
#include <cmath>
#include <random>
#include <numeric>
#include <algorithm>
#include <string>
#include <sstream>
#include <iomanip>

namespace bt {

struct PerformanceReport {
    // Return metrics
    double total_return_pct     = 0;
    double cagr                 = 0;
    double annualized_vol       = 0;

    // Risk-adjusted
    double sharpe_ratio         = 0;
    double sortino_ratio        = 0;
    double calmar_ratio         = 0;

    // Drawdown
    double max_drawdown         = 0;
    double avg_drawdown         = 0;
    double max_drawdown_duration= 0; // bars

    // Trade stats
    int    total_trades         = 0;
    int    winning_trades       = 0;
    int    losing_trades        = 0;
    double win_rate             = 0;
    double avg_win              = 0;
    double avg_loss             = 0;
    double profit_factor        = 0;
    double expectancy           = 0;

    // Cost
    double total_commission     = 0;
    double total_slippage_cost  = 0;

    // Portfolio
    double initial_equity       = 0;
    double final_equity         = 0;
    int    total_bars           = 0;
};

class PerformanceMetrics {
public:
    // ── Main compute ───────────────────────────────────────────────────────
    static PerformanceReport compute(const Portfolio& pf,
                                     double annual_rf_rate = 0.05,
                                     int    bars_per_year  = 252) {
        PerformanceReport r;
        const auto& curve  = pf.equity_curve();
        const auto& trades = pf.trades();

        if (curve.size() < 2) return r;

        r.initial_equity    = pf.initial_cash();
        r.final_equity      = curve.back().equity;
        r.total_bars        = (int)curve.size();
        r.total_commission  = pf.total_commission();

        // ── Returns series ─────────────────────────────────────────────────
        std::vector<double> rets;
        rets.reserve(curve.size() - 1);
        for (std::size_t i = 1; i < curve.size(); ++i) {
            double prev = curve[i-1].equity;
            if (prev > 0)
                rets.push_back((curve[i].equity - prev) / prev);
        }

        // ── Total return & CAGR ────────────────────────────────────────────
        r.total_return_pct  = (r.final_equity / r.initial_equity - 1.0) * 100.0;
        double years        = (double)rets.size() / bars_per_year;
        r.cagr = (years > 0)
                 ? (std::pow(r.final_equity / r.initial_equity, 1.0/years) - 1.0) * 100.0
                 : 0.0;

        // ── Vol ────────────────────────────────────────────────────────────
        double mean_ret = mean(rets);
        double vol_daily= std_dev(rets);
        r.annualized_vol = vol_daily * std::sqrt(bars_per_year) * 100.0;

        // ── Sharpe ────────────────────────────────────────────────────────
        double rf_daily  = annual_rf_rate / bars_per_year;
        double excess    = mean_ret - rf_daily;
        r.sharpe_ratio   = (vol_daily > 0)
                           ? (excess / vol_daily) * std::sqrt(bars_per_year)
                           : 0.0;

        // ── Sortino ───────────────────────────────────────────────────────
        double downside_vol = downside_std(rets, rf_daily);
        r.sortino_ratio = (downside_vol > 0)
                          ? (excess / downside_vol) * std::sqrt(bars_per_year)
                          : 0.0;

        // ── Max drawdown ──────────────────────────────────────────────────
        auto [mdd, mdd_dur, avg_dd] = compute_drawdown(curve);
        r.max_drawdown           = mdd * 100.0;
        r.max_drawdown_duration  = mdd_dur;
        r.avg_drawdown           = avg_dd * 100.0;

        // ── Calmar ────────────────────────────────────────────────────────
        r.calmar_ratio = (mdd > 0)
                         ? (r.cagr / 100.0) / mdd
                         : 0.0;

        // ── Trade stats ───────────────────────────────────────────────────
        r.total_trades = (int)trades.size();
        double gross_profit = 0, gross_loss = 0;
        for (auto& t : trades) {
            if (t.pnl >= 0) {
                ++r.winning_trades;
                r.avg_win  += t.pnl;
                gross_profit += t.pnl;
            } else {
                ++r.losing_trades;
                r.avg_loss += t.pnl;
                gross_loss += std::abs(t.pnl);
            }
        }
        if (r.total_trades > 0)
            r.win_rate = (double)r.winning_trades / r.total_trades;
        if (r.winning_trades > 0) r.avg_win /= r.winning_trades;
        if (r.losing_trades  > 0) r.avg_loss /= r.losing_trades;
        r.profit_factor = (gross_loss > 0)
                          ? gross_profit / gross_loss
                          : (gross_profit > 0 ? 999.0 : 0.0);
        r.expectancy = r.win_rate * r.avg_win +
                       (1.0 - r.win_rate) * r.avg_loss;

        return r;
    }

    // ── Monte Carlo resampling ─────────────────────────────────────────────
    struct MonteCarloResult {
        double median_final_equity;
        double pct5_final_equity;
        double pct95_final_equity;
        double pct5_max_drawdown;
    };

    static MonteCarloResult monte_carlo(const Portfolio& pf,
                                        int simulations = 1000,
                                        unsigned seed = 42) {
        const auto& curve = pf.equity_curve();
        if (curve.size() < 2) return {};

        std::vector<double> rets;
        rets.reserve(curve.size() - 1);
        for (std::size_t i = 1; i < curve.size(); ++i) {
            double prev = curve[i-1].equity;
            if (prev > 0)
                rets.push_back((curve[i].equity - prev) / prev);
        }

        std::mt19937 rng(seed);
        std::uniform_int_distribution<std::size_t> dist(0, rets.size()-1);

        int n = (int)rets.size();
        std::vector<double> finals, mdds;
        finals.reserve(simulations);
        mdds.reserve(simulations);

        for (int sim = 0; sim < simulations; ++sim) {
            double equity = pf.initial_cash();
            double peak   = equity;
            double mdd    = 0;
            for (int j = 0; j < n; ++j) {
                equity *= (1.0 + rets[dist(rng)]);
                peak    = std::max(peak, equity);
                mdd     = std::max(mdd, (peak - equity) / peak);
            }
            finals.push_back(equity);
            mdds.push_back(mdd);
        }

        std::sort(finals.begin(), finals.end());
        std::sort(mdds.begin(), mdds.end());

        int idx5  = (int)(simulations * 0.05);
        int idx50 = (int)(simulations * 0.50);
        int idx95 = (int)(simulations * 0.95);

        return {finals[idx50], finals[idx5], finals[idx95], mdds[idx95]};
    }

    // ── Pretty print ──────────────────────────────────────────────────────
    static std::string format(const PerformanceReport& r) {
        std::ostringstream ss;
        auto line = [&](const char* label, double v, const char* fmt="%+.2f") {
            char buf[64]; std::snprintf(buf, sizeof(buf), fmt, v);
            ss << std::left << std::setw(32) << label << buf << "\n";
        };
        auto lini = [&](const char* label, int v) {
            ss << std::left << std::setw(32) << label << v << "\n";
        };

        ss << "\n══════════════════════════════════════════════\n";
        ss << "  BACKTEST PERFORMANCE REPORT\n";
        ss << "══════════════════════════════════════════════\n";
        ss << "── Returns ───────────────────────────────────\n";
        line("Initial Equity ($)",   r.initial_equity,   "%.2f");
        line("Final Equity ($)",     r.final_equity,     "%.2f");
        line("Total Return (%)",     r.total_return_pct);
        line("CAGR (%)",             r.cagr);
        line("Annualized Vol (%)",   r.annualized_vol,   "%.2f");
        ss << "── Risk-Adjusted ─────────────────────────────\n";
        line("Sharpe Ratio",         r.sharpe_ratio,     "%.3f");
        line("Sortino Ratio",        r.sortino_ratio,    "%.3f");
        line("Calmar Ratio",         r.calmar_ratio,     "%.3f");
        ss << "── Drawdown ──────────────────────────────────\n";
        line("Max Drawdown (%)",     r.max_drawdown,     "%.2f");
        line("Avg Drawdown (%)",     r.avg_drawdown,     "%.2f");
        line("Max DD Duration (bars)",r.max_drawdown_duration,"%.0f");
        ss << "── Trade Statistics ──────────────────────────\n";
        lini("Total Trades",         r.total_trades);
        lini("Winning Trades",       r.winning_trades);
        lini("Losing Trades",        r.losing_trades);
        line("Win Rate (%)",         r.win_rate * 100, "%.1f");
        line("Avg Win ($)",          r.avg_win,    "%.2f");
        line("Avg Loss ($)",         r.avg_loss,   "%.2f");
        line("Profit Factor",        r.profit_factor,"%.3f");
        line("Expectancy ($/trade)", r.expectancy, "%.2f");
        ss << "── Costs ─────────────────────────────────────\n";
        line("Total Commission ($)", r.total_commission, "%.2f");
        ss << "══════════════════════════════════════════════\n";
        return ss.str();
    }

private:
    static double mean(const std::vector<double>& v) {
        if (v.empty()) return 0;
        return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    }

    static double std_dev(const std::vector<double>& v) {
        if (v.size() < 2) return 0;
        double m = mean(v);
        double var = 0;
        for (double x : v) var += (x - m) * (x - m);
        return std::sqrt(var / (v.size() - 1));
    }

    static double downside_std(const std::vector<double>& v, double threshold) {
        if (v.size() < 2) return 0;
        double var = 0;
        int    cnt = 0;
        for (double x : v) {
            if (x < threshold) {
                double d = x - threshold;
                var += d * d;
                ++cnt;
            }
        }
        return (cnt > 1) ? std::sqrt(var / (cnt - 1)) : 0.0;
    }

    static std::tuple<double, double, double>
    compute_drawdown(const std::vector<EquityPoint>& curve) {
        double peak = curve[0].equity;
        double mdd  = 0, cur_dd = 0, sum_dd = 0;
        double mdd_dur = 0, cur_dur = 0, cnt = 0;
        for (auto& p : curve) {
            if (p.equity >= peak) {
                peak = p.equity;
                cur_dd = 0; cur_dur = 0;
            } else {
                cur_dd = (peak - p.equity) / peak;
                cur_dur++;
                mdd     = std::max(mdd, cur_dd);
                mdd_dur = std::max(mdd_dur, cur_dur);
            }
            sum_dd += cur_dd;
            cnt++;
        }
        return {mdd, mdd_dur, cnt > 0 ? sum_dd / cnt : 0.0};
    }
};

} // namespace bt

// Need random for MC

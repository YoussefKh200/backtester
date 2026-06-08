#pragma once
// ── Portfolio-Aware Strategy ───────────────────────────────────────────────
// Extends the base strategy interface with portfolio construction awareness.
// Maintains a rolling covariance matrix and dynamically reweights positions
// using risk parity, inverse vol, or Kelly — updated on each rebalance bar.
//
// This demonstrates institutional-grade systematic allocation combined
// with signal generation (MA crossover as entry signal).
// ──────────────────────────────────────────────────────────────────────────
#include "StrategyBase.h"
#include "ExampleStrategies.h"
#include "../portfolio/PortfolioConstruction.h"
#include <unordered_map>
#include <deque>
#include <string>
#include <vector>
#include <cmath>

namespace bt {

enum class AllocationMethod { EqualWeight, InvVol, RiskParity, Kelly };

struct PortfolioStrategyParams {
    int    fast_period     = 10;
    int    slow_period     = 50;
    int    vol_lookback    = 60;    // bars for vol estimation
    int    rebal_freq      = 21;    // rebalance monthly
    double total_notional  = 900'000.0;
    double max_position_pct= 0.40;  // cap per symbol
    AllocationMethod method= AllocationMethod::InvVol;
    bool   allow_short     = false;
};

class PortfolioAwareStrategy : public StrategyBase {
public:
    explicit PortfolioAwareStrategy(PortfolioStrategyParams p = PortfolioStrategyParams())
        : p_(p) {}

    void on_bar(const Bar& bar) override {
        auto& s = sym_[bar.symbol];

        // Initialize per-symbol state on first bar
        if (!s.init) {
            s.fast = RollingMean(p_.fast_period);
            s.slow = RollingMean(p_.slow_period);
            s.init = true;
        }

        // Feed indicators
        s.fast.push(bar.close);
        s.slow.push(bar.close);
        s.last_close = bar.close;

        // Track returns for allocation
        if (s.prev_close > 0)
            s.returns.push_back((bar.close - s.prev_close) / s.prev_close);
        if ((int)s.returns.size() > p_.vol_lookback)
            s.returns.pop_front();
        s.prev_close = bar.close;

        // Update MA signal
        if (s.fast.ready() && s.slow.ready()) {
            double fn = s.fast.value(), sn = s.slow.value();
            bool cross_up = fn >  sn && s.pf <= s.ps;
            bool cross_dn = fn <= sn && s.pf >  s.ps;
            s.pf = fn; s.ps = sn;

            if (cross_up)  s.signal =  1;
            if (cross_dn)  s.signal = -1;
        }

        ++bar_count_;
        // Rebalance on schedule
        if (bar_count_ % p_.rebal_freq == 0) {
            rebalance();
        }
    }

    void on_end() override {
        // Close all
        for (auto& [sym, s] : sym_) {
            if (s.position_qty > 0) sell_market(sym, s.position_qty);
            else if (s.position_qty < 0) buy_market(sym, -s.position_qty);
            s.position_qty = 0;
        }
    }

private:
    struct SymState {
        RollingMean fast{10}, slow{50};
        double pf = 0, ps = 0;
        double last_close = 0, prev_close = 0;
        double position_qty = 0;
        int    signal = 0;   // +1 long, -1 short/flat, 0 neutral
        bool   init   = false;
        std::deque<double> returns;
    };

    void rebalance() {
        // Build active universe: symbols with a long signal
        std::vector<std::string> active;
        for (auto& [sym, s] : sym_)
            if (s.signal > 0 || (p_.allow_short && s.signal < 0))
                active.push_back(sym);

        if (active.empty()) {
            // No signals — exit all
            for (auto& [sym, s] : sym_) {
                if (s.position_qty > 0) { sell_market(sym, s.position_qty); s.position_qty = 0; }
                else if (s.position_qty < 0) { buy_market(sym, -s.position_qty); s.position_qty = 0; }
            }
            return;
        }

        // Compute target weights
        WeightMap target_w;
        switch (p_.method) {
        case AllocationMethod::EqualWeight:
            target_w = equal_weight(active);
            break;
        case AllocationMethod::InvVol:
            target_w = compute_inv_vol(active);
            break;
        case AllocationMethod::RiskParity:
            target_w = compute_risk_parity(active);
            break;
        case AllocationMethod::Kelly:
            target_w = compute_kelly(active);
            break;
        }

        // Clamp weights
        double sum_w = 0;
        for (auto& [sym, w] : target_w) {
            w = std::min(w, p_.max_position_pct);
            w = std::max(w, 0.0);
            sum_w += w;
        }
        if (sum_w > 0)
            for (auto& [sym, w] : target_w) w /= sum_w;

        // Convert to notional, then to shares
        auto notionals = allocate_notional(target_w, p_.total_notional);

        // Exit symbols not in active universe
        for (auto& [sym, s] : sym_) {
            if (target_w.find(sym) == target_w.end() && s.position_qty != 0) {
                if (s.position_qty > 0) sell_market(sym, s.position_qty);
                else buy_market(sym, -s.position_qty);
                s.position_qty = 0;
            }
        }

        // Enter/adjust active symbols
        for (auto& sym : active) {
            auto& s = sym_[sym];
            double price = s.last_close;
            if (price <= 0) continue;

            double notional = notionals.count(sym) ? notionals[sym] : 0;
            double target_sign = (s.signal > 0 || !p_.allow_short) ? 1.0 : -1.0;
            double target_qty  = target_sign * std::floor(notional / price);
            double delta       = target_qty - s.position_qty;

            if (std::abs(delta) < 1) continue;

            if (delta > 0) buy_market(sym, delta);
            else           sell_market(sym, -delta);
            s.position_qty = target_qty;
        }
    }

    WeightMap compute_inv_vol(const std::vector<std::string>& active) {
        std::vector<ReturnSeries> rs;
        for (auto& sym : active) {
            ReturnSeries r;
            r.symbol  = sym;
            r.returns = std::vector<double>(sym_[sym].returns.begin(),
                                            sym_[sym].returns.end());
            rs.push_back(r);
        }
        return inverse_volatility(rs);
    }

    WeightMap compute_risk_parity(const std::vector<std::string>& active) {
        std::vector<ReturnSeries> rs;
        for (auto& sym : active) {
            ReturnSeries r;
            r.symbol  = sym;
            r.returns = std::vector<double>(sym_[sym].returns.begin(),
                                            sym_[sym].returns.end());
            rs.push_back(r);
        }
        return risk_parity(rs);
    }

    WeightMap compute_kelly(const std::vector<std::string>& active) {
        std::vector<ReturnSeries> rs;
        for (auto& sym : active) {
            ReturnSeries r;
            r.symbol  = sym;
            r.returns = std::vector<double>(sym_[sym].returns.begin(),
                                            sym_[sym].returns.end());
            rs.push_back(r);
        }
        return kelly_weights(rs, 0.05/252, 0.5, p_.max_position_pct);
    }

    PortfolioStrategyParams p_;
    std::unordered_map<std::string, SymState> sym_;
    int bar_count_ = 0;
};

} // namespace bt

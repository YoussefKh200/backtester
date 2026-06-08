#pragma once
#include "StrategyBase.h"
#include <deque>
#include <unordered_map>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace bt {

// ── Rolling statistics ─────────────────────────────────────────────────────
class RollingMean {
public:
    explicit RollingMean(int p) : period_(p), sum_(0) {}
    void push(double v) {
        buf_.push_back(v); sum_ += v;
        if ((int)buf_.size() > period_) { sum_ -= buf_.front(); buf_.pop_front(); }
    }
    bool   ready() const { return (int)buf_.size() == period_; }
    double value() const { return buf_.empty() ? 0 : sum_ / buf_.size(); }
    void   reset(int p)  { period_ = p; buf_.clear(); sum_ = 0; }
private:
    int period_; double sum_; std::deque<double> buf_;
};

class RollingStd {
public:
    explicit RollingStd(int p) : period_(p) {}
    void push(double v) { buf_.push_back(v); if ((int)buf_.size() > period_) buf_.pop_front(); }
    bool ready() const { return (int)buf_.size() == period_; }
    double value() const {
        if (buf_.size() < 2) return 0;
        double m = 0; for (double x : buf_) m += x; m /= buf_.size();
        double var = 0; for (double x : buf_) var += (x-m)*(x-m);
        return std::sqrt(var / (buf_.size() - 1));
    }
private:
    int period_; std::deque<double> buf_;
};

// ══════════════════════════════════════════════════════════════════════════
// Strategy 1: Moving Average Crossover
// ══════════════════════════════════════════════════════════════════════════
struct MACrossParams {
    int    fast_period  = 10;
    int    slow_period  = 50;
    double notional     = 100000.0;
    bool   allow_short  = false;
};

class MACrossStrategy : public StrategyBase {
public:
    explicit MACrossStrategy(MACrossParams p = MACrossParams()) : p_(p) {}

    void on_bar(const Bar& bar) override {
        auto& s = state_[bar.symbol];
        if (!s.initialized) {
            s.fast = RollingMean(p_.fast_period);
            s.slow = RollingMean(p_.slow_period);
            s.initialized = true;
        }
        s.fast.push(bar.close);
        s.slow.push(bar.close);
        if (!s.fast.ready() || !s.slow.ready()) return;

        double fn = s.fast.value(), sn = s.slow.value();
        bool cross_up = fn >  sn && s.pf <= s.ps;
        bool cross_dn = fn <= sn && s.pf >  s.ps;
        s.pf = fn; s.ps = sn;

        double qty = s.qty;
        if (cross_up && qty <= 0) {
            if (qty < 0) buy_market(bar.symbol, -qty);
            double nq = std::floor(p_.notional / bar.close);
            if (nq > 0) { buy_market(bar.symbol, nq); s.qty = nq; }
        } else if (cross_dn && qty > 0) {
            sell_market(bar.symbol, qty); s.qty = 0;
            if (p_.allow_short) {
                double sq = std::floor(p_.notional / bar.close);
                if (sq > 0) { sell_market(bar.symbol, sq); s.qty = -sq; }
            }
        }
    }

    void on_end() override {
        for (auto& [sym, s] : state_) {
            if (s.qty > 0) { sell_market(sym, s.qty); s.qty = 0; }
            else if (s.qty < 0) { buy_market(sym, -s.qty); s.qty = 0; }
        }
    }

private:
    struct SymState {
        RollingMean fast{10}, slow{50};
        double pf = 0, ps = 0, qty = 0;
        bool initialized = false;
    };
    MACrossParams p_;
    std::unordered_map<std::string, SymState> state_;
};

// ══════════════════════════════════════════════════════════════════════════
// Strategy 2: Bollinger Band Mean Reversion
// ══════════════════════════════════════════════════════════════════════════
struct MeanRevParams {
    int    lookback  = 20;
    double entry_z   = -2.0;
    double exit_z    = 0.0;
    double notional  = 50000.0;
};

class MeanReversionStrategy : public StrategyBase {
public:
    explicit MeanReversionStrategy(MeanRevParams p = MeanRevParams()) : p_(p) {}

    void on_bar(const Bar& bar) override {
        auto& s = state_[bar.symbol];
        s.mu.push(bar.close);
        s.sig.push(bar.close);
        if (!s.mu.ready()) return;

        double mu = s.mu.value(), sig = s.sig.value();
        if (sig < 1e-9) return;
        double z = (bar.close - mu) / sig;

        if (s.qty == 0 && z <= p_.entry_z) {
            s.qty = std::floor(p_.notional / bar.close);
            if (s.qty > 0) buy_market(bar.symbol, s.qty);
        } else if (s.qty > 0 && z >= p_.exit_z) {
            sell_market(bar.symbol, s.qty); s.qty = 0;
        }
    }

    void on_end() override {
        for (auto& [sym, s] : state_)
            if (s.qty > 0) { sell_market(sym, s.qty); s.qty = 0; }
    }

private:
    struct SymState {
        RollingMean mu{20};
        RollingStd  sig{20};
        double qty = 0;
    };
    MeanRevParams p_;
    std::unordered_map<std::string, SymState> state_;
};

// ══════════════════════════════════════════════════════════════════════════
// Strategy 3: Donchian Channel Breakout with ATR Trailing Stop
// ══════════════════════════════════════════════════════════════════════════
struct BreakoutParams {
    int    channel_period = 20;
    int    atr_period     = 14;
    double atr_mult       = 2.0;
    double notional       = 80000.0;
};

class BreakoutStrategy : public StrategyBase {
public:
    explicit BreakoutStrategy(BreakoutParams p = BreakoutParams()) : p_(p) {}

    void on_bar(const Bar& bar) override {
        auto& s = state_[bar.symbol];
        s.highs.push_back(bar.high);
        s.lows.push_back(bar.low);
        s.atr_vals.push_back(bar.high - bar.low);

        if ((int)s.highs.size()   > p_.channel_period) s.highs.pop_front();
        if ((int)s.lows.size()    > p_.channel_period) s.lows.pop_front();
        if ((int)s.atr_vals.size()> p_.atr_period)     s.atr_vals.pop_front();

        if ((int)s.highs.size() < p_.channel_period) return;

        double ch_high = *std::max_element(s.highs.begin(), s.highs.end());
        double atr = 0;
        for (double v : s.atr_vals) atr += v;
        atr /= s.atr_vals.size();

        if (s.qty == 0 && bar.close > ch_high) {
            s.qty = std::floor(p_.notional / bar.close);
            s.stop_px = bar.close - p_.atr_mult * atr;
            if (s.qty > 0) buy_market(bar.symbol, s.qty);
        } else if (s.qty > 0) {
            s.stop_px = std::max(s.stop_px, bar.close - p_.atr_mult * atr);
            if (bar.low < s.stop_px) {
                sell_market(bar.symbol, s.qty); s.qty = 0;
            }
        }
    }

    void on_end() override {
        for (auto& [sym, s] : state_)
            if (s.qty > 0) { sell_market(sym, s.qty); s.qty = 0; }
    }

private:
    struct SymState {
        std::deque<double> highs, lows, atr_vals;
        double qty = 0, stop_px = 0;
    };
    BreakoutParams p_;
    std::unordered_map<std::string, SymState> state_;
};

} // namespace bt

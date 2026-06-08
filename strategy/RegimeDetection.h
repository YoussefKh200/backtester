#pragma once
// ── Regime Detection ───────────────────────────────────────────────────────
// Two-state volatility regime classifier using a sliding window approach.
// Inspired by Hidden Markov Model (HMM) regime detection used in practice.
//
// State 0 = Low vol / trending (bull)
// State 1 = High vol / mean-reverting (bear/stress)
//
// The classifier uses:
//   - Realized volatility ratio (short / long)
//   - VIX proxy (high-low range ratio)
//   - Trend strength (price vs moving average)
//   - Breadth momentum (for multi-asset)
// ──────────────────────────────────────────────────────────────────────────
#include "../core/Types.h"
#include <deque>
#include <vector>
#include <string>
#include <unordered_map>
#include <cmath>
#include <numeric>
#include <algorithm>

namespace bt {

enum class MarketRegime { Bull, Bear, Transition };

struct RegimeSignals {
    double realized_vol_short = 0;   // 10-bar ann. vol
    double realized_vol_long  = 0;   // 60-bar ann. vol
    double vol_ratio          = 0;   // short/long
    double trend_score        = 0;   // close vs 200MA (normalized)
    double range_ratio        = 0;   // ATR / close
    double momentum_score     = 0;   // 1m vs 12m momentum
    MarketRegime regime       = MarketRegime::Bull;
    double bull_probability   = 0.5; // soft classification
};

struct RegimeDetectorConfig {
    int    short_vol_window   = 10;
    int    long_vol_window    = 60;
    int    trend_ma_window    = 200;
    int    atr_window         = 14;
    int    mom_short_window   = 21;
    int    mom_long_window    = 252;
    double vol_ratio_threshold= 1.4;
    double trend_threshold    = -0.05;
    double smoothing          = 0.85;
    int    bars_per_year      = 252;
};

class RegimeDetector {
public:
    explicit RegimeDetector(RegimeDetectorConfig cfg = RegimeDetectorConfig()) : cfg_(cfg) {}

    // Update detector with a new bar; returns current regime signals
    RegimeSignals update(const Bar& bar) {
        // Push to history
        closes_.push_back(bar.close);
        ranges_.push_back(bar.high - bar.low);

        if ((int)closes_.size() > cfg_.trend_ma_window + 10)
            closes_.pop_front();
        if ((int)ranges_.size() > std::max(cfg_.long_vol_window, cfg_.atr_window) + 5)
            ranges_.pop_front();

        // Log returns
        if (closes_.size() > 1) {
            double prev = closes_[closes_.size()-2];
            if (prev > 0) log_rets_.push_back(std::log(bar.close / prev));
        }
        if ((int)log_rets_.size() > cfg_.long_vol_window + 5) log_rets_.pop_front();

        RegimeSignals sig;

        // Realized vols
        if ((int)log_rets_.size() >= cfg_.short_vol_window) {
            sig.realized_vol_short = rolling_std(log_rets_,
                log_rets_.size()-cfg_.short_vol_window, cfg_.short_vol_window)
                * std::sqrt(cfg_.bars_per_year);
        }
        if ((int)log_rets_.size() >= cfg_.long_vol_window) {
            sig.realized_vol_long = rolling_std(log_rets_,
                log_rets_.size()-cfg_.long_vol_window, cfg_.long_vol_window)
                * std::sqrt(cfg_.bars_per_year);
        }
        if (sig.realized_vol_long > 1e-9)
            sig.vol_ratio = sig.realized_vol_short / sig.realized_vol_long;

        // Trend score: (close / MA200 - 1)
        if ((int)closes_.size() >= cfg_.trend_ma_window) {
            double ma = 0;
            for (int i = (int)closes_.size()-cfg_.trend_ma_window;
                 i < (int)closes_.size(); ++i)
                ma += closes_[i];
            ma /= cfg_.trend_ma_window;
            sig.trend_score = (bar.close / ma) - 1.0;
        }

        // ATR ratio
        if ((int)ranges_.size() >= cfg_.atr_window) {
            double atr = 0;
            for (int i = (int)ranges_.size()-cfg_.atr_window;
                 i < (int)ranges_.size(); ++i)
                atr += ranges_[i];
            atr /= cfg_.atr_window;
            sig.range_ratio = (bar.close > 0) ? atr / bar.close : 0;
        }

        // Momentum score: (close / close_12m) - (close / close_1m)
        if ((int)closes_.size() >= cfg_.mom_long_window) {
            double close_12m = closes_[closes_.size()-cfg_.mom_long_window];
            double close_1m  = closes_[closes_.size()-cfg_.mom_short_window];
            double mom_long  = (close_12m > 0) ? bar.close/close_12m - 1 : 0;
            double mom_short = (close_1m  > 0) ? bar.close/close_1m  - 1 : 0;
            sig.momentum_score = mom_long - mom_short * 2; // penalize short-term reversal
        }

        // Soft classification: bear probability
        // Higher vol_ratio, lower trend score → more bear
        double bear_signal = 0, total_weight = 0;

        if (sig.vol_ratio > 0) {
            double w = 0.40;
            bear_signal += w * std::min(1.0, std::max(0.0,
                (sig.vol_ratio - 1.0) / (cfg_.vol_ratio_threshold - 1.0)));
            total_weight += w;
        }
        if (sig.trend_score != 0) {
            double w = 0.35;
            bear_signal += w * std::min(1.0, std::max(0.0,
                -sig.trend_score / 0.20)); // max signal at -20%
            total_weight += w;
        }
        if (sig.range_ratio > 0) {
            double w = 0.15;
            // High ATR/price → stress
            bear_signal += w * std::min(1.0, sig.range_ratio / 0.04);
            total_weight += w;
        }
        if (sig.momentum_score != 0) {
            double w = 0.10;
            bear_signal += w * std::min(1.0, std::max(0.0, -sig.momentum_score / 0.20));
            total_weight += w;
        }

        double raw_bear_prob = (total_weight > 0) ? bear_signal / total_weight : 0.5;

        // Exponential smoothing
        smoothed_bear_prob_ = cfg_.smoothing * smoothed_bear_prob_
                             + (1-cfg_.smoothing) * raw_bear_prob;

        sig.bull_probability = 1.0 - smoothed_bear_prob_;

        // Hard state
        if (smoothed_bear_prob_ > 0.65)
            sig.regime = MarketRegime::Bear;
        else if (smoothed_bear_prob_ < 0.35)
            sig.regime = MarketRegime::Bull;
        else
            sig.regime = MarketRegime::Transition;

        last_regime_ = sig;
        return sig;
    }

    const RegimeSignals& last() const { return last_regime_; }
    MarketRegime         current_regime() const { return last_regime_.regime; }
    double               bull_prob() const { return last_regime_.bull_probability; }

    static std::string regime_name(MarketRegime r) {
        switch(r) {
        case MarketRegime::Bull:       return "BULL";
        case MarketRegime::Bear:       return "BEAR";
        case MarketRegime::Transition: return "TRANS";
        }
        return "?";
    }

private:
    double rolling_std(const std::deque<double>& data,
                        std::size_t start, int len) const {
        if ((int)(data.size()-start) < len) return 0;
        double m = 0;
        for (int i = 0; i < len; ++i) m += data[start+i];
        m /= len;
        double v = 0;
        for (int i = 0; i < len; ++i) v += (data[start+i]-m)*(data[start+i]-m);
        return std::sqrt(v / (len-1));
    }

    RegimeDetectorConfig cfg_;
    std::deque<double> closes_, log_rets_, ranges_;
    double smoothed_bear_prob_ = 0.3;  // start mildly bullish
    RegimeSignals last_regime_;
};

// ── Regime-filtered MA crossover ───────────────────────────────────────────
// Only takes long signals in bull regime; goes flat in bear/transition.
// In bear regime, optionally takes short signals.
struct RFMAParams {
    int    fast_period    = 10;
    int    slow_period    = 50;
    double notional       = 100000.0;
    bool   short_in_bear  = false;
    double min_bull_prob  = 0.60;
    double max_bear_prob  = 0.60;
};

class RegimeFilteredMAStrategy : public StrategyBase {
public:
    explicit RegimeFilteredMAStrategy(RFMAParams p = RFMAParams()) : p_(p) {}

    void on_bar(const Bar& bar) override {
        auto& s = sym_[bar.symbol];
        if (!s.init) {
            s.fast = RollingMean(p_.fast_period);
            s.slow = RollingMean(p_.slow_period);
            s.init = true;
        }

        s.fast.push(bar.close);
        s.slow.push(bar.close);
        auto regime = s.detector.update(bar);

        if (!s.fast.ready() || !s.slow.ready()) return;

        double fn = s.fast.value(), sn = s.slow.value();
        bool cross_up = fn >  sn && s.pf <= s.ps;
        bool cross_dn = fn <= sn && s.pf >  s.ps;
        s.pf = fn; s.ps = sn;

        double qty = s.qty;
        double bull_p = regime.bull_probability;

        if (cross_up && bull_p >= p_.min_bull_prob && qty <= 0) {
            // Enter long — regime allows it
            if (qty < 0) buy_market(bar.symbol, -qty);
            double nq = std::floor(p_.notional / bar.close);
            if (nq > 0) { buy_market(bar.symbol, nq); s.qty = nq; }

        } else if (cross_dn && qty > 0) {
            // Exit long
            sell_market(bar.symbol, qty); s.qty = 0;

            if (p_.short_in_bear && (1.0-bull_p) >= p_.max_bear_prob) {
                double sq = std::floor(p_.notional / bar.close);
                if (sq > 0) { sell_market(bar.symbol, sq); s.qty = -sq; }
            }

        } else if (qty > 0 && bull_p < (1.0 - p_.max_bear_prob)) {
            // Forced exit: regime flipped to bear even without cross
            sell_market(bar.symbol, qty); s.qty = 0;
        }
    }

    void on_end() override {
        for (auto& [sym, s] : sym_) {
            if (s.qty > 0) { sell_market(sym, s.qty); s.qty = 0; }
            else if (s.qty < 0) { buy_market(sym, -s.qty); s.qty = 0; }
        }
    }

private:
    struct SymState {
        RollingMean    fast{10}, slow{50};
        RegimeDetector detector;
        double pf = 0, ps = 0, qty = 0;
        bool init = false;
    };

    RFMAParams p_;
    std::unordered_map<std::string, SymState> sym_;
};

} // namespace bt

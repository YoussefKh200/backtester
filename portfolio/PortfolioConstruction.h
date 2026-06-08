#pragma once
// ── Portfolio Construction Utilities ──────────────────────────────────────
// Institutional allocation methods:
//   1. Equal Weight
//   2. Inverse Volatility
//   3. Risk Parity (equal risk contribution)
//   4. Maximum Sharpe (mean-variance optimization, analytical for N assets)
//   5. Kelly Criterion (full and fractional)
// ──────────────────────────────────────────────────────────────────────────
#include "../core/Types.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <stdexcept>

namespace bt {

using WeightMap = std::unordered_map<std::string, double>;

// ── Return/covariance helpers ──────────────────────────────────────────────
struct ReturnSeries {
    std::string            symbol;
    std::vector<double>    returns; // daily
};

inline double mean_ret(const std::vector<double>& r) {
    if (r.empty()) return 0;
    return std::accumulate(r.begin(), r.end(), 0.0) / r.size();
}

inline double vol(const std::vector<double>& r) {
    if (r.size() < 2) return 0;
    double m = mean_ret(r), v = 0;
    for (double x : r) v += (x-m)*(x-m);
    return std::sqrt(v / (r.size()-1));
}

inline double cov_pairwise(const std::vector<double>& a,
                            const std::vector<double>& b) {
    std::size_t n = std::min(a.size(), b.size());
    if (n < 2) return 0;
    double ma = mean_ret(a), mb = mean_ret(b), c = 0;
    for (std::size_t i = 0; i < n; ++i) c += (a[i]-ma)*(b[i]-mb);
    return c / (n-1);
}

// ── 1. Equal Weight ────────────────────────────────────────────────────────
inline WeightMap equal_weight(const std::vector<std::string>& symbols) {
    WeightMap w;
    double share = (symbols.empty()) ? 0 : 1.0 / symbols.size();
    for (auto& s : symbols) w[s] = share;
    return w;
}

// ── 2. Inverse Volatility ──────────────────────────────────────────────────
inline WeightMap inverse_volatility(const std::vector<ReturnSeries>& series,
                                     int annualize = 252) {
    WeightMap w;
    double sum_inv_vol = 0;
    std::vector<double> inv_vols;

    for (auto& rs : series) {
        double v = vol(rs.returns) * std::sqrt(annualize);
        double inv = (v > 1e-9) ? 1.0 / v : 0.0;
        inv_vols.push_back(inv);
        sum_inv_vol += inv;
    }

    for (std::size_t i = 0; i < series.size(); ++i)
        w[series[i].symbol] = (sum_inv_vol > 0) ? inv_vols[i] / sum_inv_vol : 0;

    return w;
}

// ── 3. Risk Parity (equal risk contribution) ───────────────────────────────
// Iterative: adjust weights until each asset contributes equal % of port vol
inline WeightMap risk_parity(const std::vector<ReturnSeries>& series,
                              int max_iter = 200, double tol = 1e-8) {
    int N = (int)series.size();
    if (N == 0) return {};

    // Start with equal weight
    std::vector<double> w(N, 1.0/N);

    // Build covariance matrix (upper triangle stored flat)
    std::vector<std::vector<double>> cov(N, std::vector<double>(N, 0));
    for (int i = 0; i < N; ++i)
        for (int j = i; j < N; ++j) {
            double c = cov_pairwise(series[i].returns, series[j].returns);
            cov[i][j] = cov[j][i] = c;
        }

    auto port_var = [&](const std::vector<double>& wt) {
        double pv = 0;
        for (int i = 0; i < N; ++i)
            for (int j = 0; j < N; ++j)
                pv += wt[i]*wt[j]*cov[i][j];
        return pv;
    };

    auto risk_contrib = [&](const std::vector<double>& wt, int i) {
        double mrc = 0;
        for (int j = 0; j < N; ++j) mrc += wt[j]*cov[i][j];
        double pv = port_var(wt);
        return (pv > 0) ? wt[i]*mrc / std::sqrt(pv) : 0;
    };

    // Gradient descent
    for (int it = 0; it < max_iter; ++it) {
        double pv = port_var(w);
        if (pv < 1e-16) break;
        double port_vol = std::sqrt(pv);
        double target_rc = port_vol / N; // equal contribution

        double max_err = 0;
        for (int i = 0; i < N; ++i) {
            double rc = risk_contrib(w, i);
            double err = rc - target_rc;
            max_err = std::max(max_err, std::abs(err));
            w[i] *= (1.0 - 0.2 * err / (port_vol + 1e-12));
            w[i]  = std::max(w[i], 1e-6);
        }
        // Renormalize
        double s = 0; for (double x : w) s += x;
        for (double& x : w) x /= s;
        if (max_err < tol) break;
    }

    WeightMap wm;
    for (int i = 0; i < N; ++i)
        wm[series[i].symbol] = w[i];
    return wm;
}

// ── 4. Kelly Criterion ─────────────────────────────────────────────────────
// Full Kelly: f* = μ / σ²  (single asset, excess return)
// Fractional Kelly: fraction * f*
inline double kelly_fraction(double mean_return, double variance,
                              double fraction = 0.5) {
    if (variance < 1e-12) return 0;
    return fraction * mean_return / variance;
}

// Multi-asset Kelly (diagonal approximation — ignores off-diagonal covariance)
inline WeightMap kelly_weights(const std::vector<ReturnSeries>& series,
                                double rf_daily    = 0.05/252,
                                double fraction    = 0.5,
                                double max_weight  = 0.30) {
    WeightMap w;
    double sum = 0;
    for (auto& rs : series) {
        double mu = mean_ret(rs.returns) - rf_daily;
        double v  = vol(rs.returns);
        double f  = (v > 0) ? fraction * mu / (v*v) : 0;
        f = std::max(0.0, std::min(f, max_weight));
        w[rs.symbol] = f;
        sum += f;
    }
    // Normalize if sum > 1
    if (sum > 1.0)
        for (auto& [sym, wt] : w) wt /= sum;
    return w;
}

// ── Weight utilities ───────────────────────────────────────────────────────
inline void print_weights(const WeightMap& w, const std::string& title = "") {
    if (!title.empty()) std::cout << "\n── " << title << " ──\n";
    double total = 0;
    for (auto& [sym, wt] : w) {
        std::printf("  %-16s  %6.2f%%\n", sym.c_str(), wt*100);
        total += wt;
    }
    std::printf("  %-16s  %6.2f%%\n", "TOTAL", total*100);
}

// Apply weights to compute per-symbol notional allocation
inline std::unordered_map<std::string, double>
allocate_notional(const WeightMap& w, double total_capital) {
    std::unordered_map<std::string, double> alloc;
    for (auto& [sym, wt] : w)
        alloc[sym] = total_capital * wt;
    return alloc;
}

// Extract return series from equity curve (for a single-asset backtest)
inline ReturnSeries equity_to_returns(const std::vector<EquityPoint>& curve,
                                       const std::string& name = "portfolio") {
    ReturnSeries rs;
    rs.symbol = name;
    rs.returns.reserve(curve.size());
    for (std::size_t i = 1; i < curve.size(); ++i) {
        double prev = curve[i-1].equity;
        if (prev > 0)
            rs.returns.push_back((curve[i].equity - prev) / prev);
    }
    return rs;
}

} // namespace bt

#include <iostream>

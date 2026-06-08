#pragma once
#include "../core/Types.h"
#include <vector>
#include <random>
#include <cmath>
#include <string>
#include <fstream>
#include <iomanip>
#include <ctime>
#include <algorithm>

namespace bt {

struct SyntheticConfig {
    std::string symbol      = "SYNTH";
    double      start_price = 100.0;
    int         n_bars      = 1000;
    double      annual_ret  = 0.10;
    double      annual_vol  = 0.20;
    int         bars_per_year = 252;
    Timestamp   start_ts    = 1640000000000LL;
    long        bar_ms      = 86400000L;
    unsigned    seed        = 42;
    bool        use_regimes     = true;
    double      regime_prob_switch = 0.02;
    double      bear_vol_mult   = 2.0;
    double      bear_ret_adj    = -0.30;
};

class SyntheticDataGen {
public:
    static std::vector<Bar> generate(SyntheticConfig cfg = SyntheticConfig()) {
        std::mt19937 rng(cfg.seed);
        std::normal_distribution<double> norm(0.0, 1.0);
        std::uniform_real_distribution<double> uni(0.0, 1.0);

        double dt    = 1.0 / cfg.bars_per_year;
        double price = cfg.start_price;
        bool bear_regime = false;

        std::vector<Bar> bars;
        bars.reserve(cfg.n_bars);

        for (int i = 0; i < cfg.n_bars; ++i) {
            if (cfg.use_regimes && uni(rng) < cfg.regime_prob_switch)
                bear_regime = !bear_regime;

            double mu  = cfg.annual_ret + (bear_regime ? cfg.bear_ret_adj  : 0.0);
            double sig = cfg.annual_vol * (bear_regime ? cfg.bear_vol_mult : 1.0);

            double z     = norm(rng);
            double ret   = (mu - 0.5*sig*sig)*dt + sig*std::sqrt(dt)*z;
            double close = price * std::exp(ret);

            double iv   = sig * std::sqrt(dt) * price * 0.5;
            double high = std::max(price, close) + std::abs(norm(rng)) * iv;
            double low  = std::min(price, close) - std::abs(norm(rng)) * iv;
            double open = price * (1.0 + norm(rng) * sig * std::sqrt(dt) * 0.3);
            open  = std::clamp(open, low, high);
            close = std::max(0.01, close);
            low   = std::min({low, open, close});
            high  = std::max({high, open, close});
            double vol = 1e6 * (1.0 + 0.5 * std::abs(norm(rng)));

            Bar b;
            b.symbol = cfg.symbol;
            b.ts     = cfg.start_ts + (Timestamp)i * cfg.bar_ms;
            b.open   = std::max(0.01, open);
            b.high   = high;
            b.low    = std::max(0.01, low);
            b.close  = close;
            b.volume = vol;
            bars.push_back(b);
            price = close;
        }
        return bars;
    }

    static void to_csv(const std::vector<Bar>& bars, const std::string& path) {
        std::ofstream f(path);
        f << "date,open,high,low,close,volume\n";
        for (auto& b : bars) {
            time_t t = b.ts / 1000;
            char buf[32];
            struct tm* tm_p = gmtime(&t);
            strftime(buf, sizeof(buf), "%Y-%m-%d", tm_p);
            f << buf << ","
              << std::fixed << std::setprecision(4)
              << b.open << "," << b.high << "," << b.low << "," << b.close << ","
              << (long long)b.volume << "\n";
        }
    }
};

} // namespace bt

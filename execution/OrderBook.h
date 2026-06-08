#pragma once
// ── Simulated Limit Order Book (LOB) ──────────────────────────────────────
// Provides a realistic intra-bar price path simulation for:
//   - more accurate fill prices on limit orders within a bar
//   - partial fill simulation based on simulated volume distribution
//   - price impact modeling based on order size vs. book depth
//
// Architecture: The LOB is a simplified model — it doesn't require real
// tick data. It synthesizes a plausible intra-bar price path from OHLCV
// using a micro-structural model, then replays orders against it.
// For real tick data, replace synthesize_path() with a tick loader.
// ──────────────────────────────────────────────────────────────────────────
#include "../core/Types.h"
#include <vector>
#include <random>
#include <cmath>
#include <algorithm>
#include <functional>

namespace bt {

// ── Simulated tick ─────────────────────────────────────────────────────────
struct Tick {
    Timestamp ts;
    double    price;
    double    bid;
    double    ask;
    double    volume;   // volume traded at this tick
    bool      is_buy;   // aggressor side
};

// ── Intra-bar path synthesizer ─────────────────────────────────────────────
// Generates N ticks that are consistent with OHLCV constraints.
// Uses a constrained Brownian bridge: must start at open, pass through
// either high or low (randomly ordered), and end at close.
class IntraBarSynthesizer {
public:
    struct Config {
        int      n_ticks      = 100;
        double   spread_bps   = 1.0;   // bid-ask spread
        bool     random_order = true;  // randomize high/low order
        unsigned seed         = 0;     // 0 = use bar timestamp as seed
    };

    static std::vector<Tick> synthesize(const Bar& bar, Config cfg = Config()) {
        unsigned seed = (cfg.seed == 0) ? (unsigned)(bar.ts & 0xFFFFFFFF) : cfg.seed;
        std::mt19937 rng(seed);
        std::normal_distribution<double> norm(0, 1);
        std::uniform_real_distribution<double> uni(0, 1);

        const int N = cfg.n_ticks;
        std::vector<double> path(N);

        // Brownian bridge from open → close, constrained to [low, high]
        // Step 1: generate unconstrained bridge
        double start = bar.open, end = bar.close;
        path[0] = start;
        for (int i = 1; i < N-1; ++i) {
            double t = (double)i / (N-1);
            // Interpolated mean + noise scaled by bridge variance
            double mu  = start + t * (end - start);
            double var = t * (1-t) * (bar.high - bar.low) * 0.5;
            path[i] = mu + norm(rng) * std::sqrt(std::max(var, 1e-10));
        }
        path[N-1] = end;

        // Step 2: must include high and low somewhere
        // Find the current max/min and rescale linearly to fit [low, high]
        double cur_max = *std::max_element(path.begin(), path.end());
        double cur_min = *std::min_element(path.begin(), path.end());
        double cur_rng = cur_max - cur_min;
        double tgt_rng = bar.high - bar.low;

        if (cur_rng > 1e-9) {
            for (auto& p : path)
                p = bar.low + (p - cur_min) / cur_rng * tgt_rng;
        }

        // Step 3: clamp to [low, high]
        for (auto& p : path)
            p = std::clamp(p, bar.low, bar.high);

        // Step 4: build ticks with bid/ask spread
        double half_spread = bar.close * cfg.spread_bps / 20000.0;
        double bar_volume  = bar.volume;
        double tick_vol    = bar_volume / N;

        std::vector<Tick> ticks;
        ticks.reserve(N);

        long tick_ms = 60000 / N; // spread across 1 minute (for daily bars this is symbolic)

        for (int i = 0; i < N; ++i) {
            Tick t;
            t.ts     = bar.ts + (Timestamp)i * tick_ms;
            t.price  = path[i];
            t.bid    = path[i] - half_spread;
            t.ask    = path[i] + half_spread;
            t.volume = tick_vol * (0.5 + 0.5 * uni(rng)); // random vol per tick
            t.is_buy = (uni(rng) > 0.5);
            ticks.push_back(t);
        }
        return ticks;
    }
};

// ── Simulated depth level ──────────────────────────────────────────────────
struct DepthLevel {
    double price;
    double size;
};

// ── Simulated order book ───────────────────────────────────────────────────
class SimulatedOrderBook {
public:
    struct Config {
        int    depth_levels   = 5;
        double level_spacing_bps = 1.0;   // spacing between levels
        double base_size      = 10000.0;  // shares per level
        double size_decay     = 0.7;      // size decay per level away from top
    };

    explicit SimulatedOrderBook(Config cfg = Config()) : cfg_(cfg) {}

    void update(const Tick& tick) {
        mid_ = (tick.bid + tick.ask) / 2.0;
        spread_ = tick.ask - tick.bid;
        rebuild_book();
    }

    void update(double mid, double spread) {
        mid_ = mid; spread_ = spread;
        rebuild_book();
    }

    // Estimate fill price and actual filled quantity for a market order
    struct FillEstimate {
        double avg_price;
        double filled_qty;
        double price_impact; // additional cost beyond first level
    };

    FillEstimate estimate_market_fill(OrderSide side, double qty) const {
        auto& levels = (side == OrderSide::Buy) ? asks_ : bids_;
        double total_cost = 0, total_filled = 0;
        double remaining = qty;

        for (auto& lvl : levels) {
            if (remaining <= 0) break;
            double fill = std::min(remaining, lvl.size);
            total_cost   += fill * lvl.price;
            total_filled += fill;
            remaining    -= fill;
        }

        if (total_filled < qty && !levels.empty()) {
            // Beyond book — assume last level price + penalty
            double last_px = levels.back().price * (1.0 + 0.001);
            total_cost   += remaining * last_px;
            total_filled += remaining;
        }

        double avg = (total_filled > 0) ? total_cost / total_filled : mid_;
        double first_px = levels.empty() ? mid_ : levels[0].price;
        return {avg, total_filled, std::abs(avg - first_px)};
    }

    double best_bid() const { return bids_.empty() ? mid_ - spread_/2 : bids_[0].price; }
    double best_ask() const { return asks_.empty() ? mid_ + spread_/2 : asks_[0].price; }
    double mid()      const { return mid_; }

    const std::vector<DepthLevel>& bids() const { return bids_; }
    const std::vector<DepthLevel>& asks() const { return asks_; }

private:
    void rebuild_book() {
        bids_.clear(); asks_.clear();
        double spacing = mid_ * cfg_.level_spacing_bps / 10000.0;
        for (int i = 0; i < cfg_.depth_levels; ++i) {
            double sz = cfg_.base_size * std::pow(cfg_.size_decay, i);
            bids_.push_back({mid_ - spread_/2 - i*spacing, sz});
            asks_.push_back({mid_ + spread_/2 + i*spacing, sz});
        }
    }

    Config cfg_;
    double mid_ = 0, spread_ = 0;
    std::vector<DepthLevel> bids_, asks_;
};

// ── Tick-level execution engine ────────────────────────────────────────────
// Drop-in replacement for the bar-level execution engine when you have
// (real or synthesized) tick data.
class TickExecutionEngine {
public:
    struct Config {
        bool   synthesize_ticks  = true;
        int    ticks_per_bar     = 100;
        double spread_bps        = 1.0;
        double commission_per_share = 0.005;
    };

    explicit TickExecutionEngine(Config cfg = Config()) : cfg_(cfg) {}

    // For each bar, synthesize ticks and attempt to fill pending orders
    std::vector<Fill> process_bar(const Bar& bar,
                                   std::vector<Order>& pending_orders) {
        std::vector<Fill> fills;
        if (pending_orders.empty()) return fills;

        IntraBarSynthesizer::Config syn_cfg;
        syn_cfg.n_ticks    = cfg_.ticks_per_bar;
        syn_cfg.spread_bps = cfg_.spread_bps;
        syn_cfg.seed       = (unsigned)(bar.ts & 0xFFFFFFFF);
        auto ticks = IntraBarSynthesizer::synthesize(bar, syn_cfg);

        SimulatedOrderBook book;

        for (auto& tick : ticks) {
            book.update(tick);

            for (auto& order : pending_orders) {
                if (order.status != OrderStatus::Pending) continue;
                if (order.symbol != bar.symbol) continue;

                bool triggered = false;
                double fill_px = 0;

                switch (order.type) {
                case OrderType::Market:
                    // Fill at first tick (open equivalent)
                    if (&tick == &ticks[0]) {
                        auto est = book.estimate_market_fill(order.side, order.quantity);
                        fill_px = est.avg_price;
                        triggered = true;
                    }
                    break;

                case OrderType::Limit:
                    if (order.side == OrderSide::Buy  && tick.ask <= order.limit_px) {
                        fill_px = tick.ask; triggered = true;
                    } else if (order.side == OrderSide::Sell && tick.bid >= order.limit_px) {
                        fill_px = tick.bid; triggered = true;
                    }
                    break;

                case OrderType::Stop:
                    if (order.side == OrderSide::Buy  && tick.price >= order.stop_px) {
                        auto est = book.estimate_market_fill(order.side, order.quantity);
                        fill_px = est.avg_price; triggered = true;
                    } else if (order.side == OrderSide::Sell && tick.price <= order.stop_px) {
                        auto est = book.estimate_market_fill(order.side, order.quantity);
                        fill_px = est.avg_price; triggered = true;
                    }
                    break;

                default: break;
                }

                if (triggered) {
                    Fill f;
                    f.order_id   = order.id;
                    f.symbol     = bar.symbol;
                    f.side       = order.side;
                    f.quantity   = order.quantity;
                    f.price      = fill_px;
                    f.commission = order.quantity * cfg_.commission_per_share;
                    f.ts         = tick.ts;
                    f.strategy_id= order.strategy_id;

                    order.status      = OrderStatus::Filled;
                    order.filled_qty  = order.quantity;
                    order.avg_fill_px = fill_px;
                    order.ts_filled   = tick.ts;

                    fills.push_back(f);
                }
            }
        }
        return fills;
    }

private:
    Config cfg_;
};

} // namespace bt

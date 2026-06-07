#pragma once
#include "../core/Types.h"
#include "../strategy/StrategyBase.h"
#include "../core/EventEngine.h"
#include <unordered_map>
#include <vector>
#include <atomic>
#include <cmath>
#include <functional>

namespace bt {

// ── Commission models ──────────────────────────────────────────────────────
struct CommissionModel {
    double per_share      = 0.005;  // $0.005/share (IB tier)
    double min_per_trade  = 1.0;
    double max_pct_value  = 0.005;  // 0.5% of trade value cap

    double calculate(double qty, double price) const {
        double raw = qty * per_share;
        raw = std::max(raw, min_per_trade);
        raw = std::min(raw, qty * price * max_pct_value);
        return raw;
    }
};

// ── Slippage models ────────────────────────────────────────────────────────
enum class SlippageModel { Fixed, VolumeImpact, VolatilityAdjusted };

struct SlippageConfig {
    SlippageModel model    = SlippageModel::Fixed;
    double fixed_bps       = 2.0;   // 2bps fixed slippage
    double volume_pct      = 0.01;  // assume we're 1% of bar volume
    double volatility_mult = 0.1;   // fraction of bar range
};

inline double compute_slippage(const SlippageConfig& cfg,
                                const Bar& bar, OrderSide side,
                                double qty) {
    double base_px = bar.close;
    double slip = 0.0;
    switch (cfg.model) {
    case SlippageModel::Fixed:
        slip = base_px * cfg.fixed_bps / 10000.0;
        break;
    case SlippageModel::VolumeImpact:
        if (bar.volume > 0) {
            double pct = qty / bar.volume;
            slip = base_px * pct * 0.5; // simplified square-root model
        } else {
            slip = base_px * cfg.fixed_bps / 10000.0;
        }
        break;
    case SlippageModel::VolatilityAdjusted:
        slip = (bar.high - bar.low) * cfg.volatility_mult;
        break;
    }
    return (side == OrderSide::Buy) ? slip : -slip;
}

// ── Spread model ───────────────────────────────────────────────────────────
struct SpreadConfig {
    double half_spread_bps = 1.0;  // 1bp half-spread → 2bp total

    double cost(double mid, OrderSide side) const {
        double h = mid * half_spread_bps / 10000.0;
        return (side == OrderSide::Buy) ? h : -h;
    }
};

// ── Execution engine ───────────────────────────────────────────────────────
class ExecutionEngine : public IOrderRouter {
public:
    ExecutionEngine(EventEngine& ev_eng,
                    SlippageConfig slip  = {},
                    SpreadConfig   spread= {},
                    CommissionModel comm = {})
        : ev_eng_(ev_eng), slip_(slip), spread_(spread), comm_(comm) {}

    // IOrderRouter
    std::uint64_t submit(Order order) override {
        order.id        = ++order_counter_;
        order.status    = OrderStatus::Pending;
        order.ts_placed = current_ts_;
        pending_[order.id] = order;
        // Publish order event so it's visible to listeners
        ev_eng_.publish(OrderEvent{order});
        return order.id;
    }

    bool cancel(std::uint64_t order_id) override {
        auto it = pending_.find(order_id);
        if (it == pending_.end()) return false;
        it->second.status = OrderStatus::Cancelled;
        ev_eng_.publish(OrderEvent{it->second});
        pending_.erase(it);
        return true;
    }

    // Called by Backtester on every bar — try to fill pending orders
    void on_bar(const Bar& bar) {
        current_ts_ = bar.ts;
        last_bar_[bar.symbol] = bar;

        // Collect ids to try (avoid modifying map while iterating)
        std::vector<std::uint64_t> to_try;
        for (auto& [id, o] : pending_)
            if (o.symbol == bar.symbol)
                to_try.push_back(id);

        for (auto id : to_try) {
            auto it = pending_.find(id);
            if (it == pending_.end()) continue;
            auto& o = it->second;
            if (try_fill(o, bar)) {
                pending_.erase(it);
            }
        }
    }

    // Cancel all pending orders for a symbol (e.g. end of day)
    void cancel_all(const std::string& symbol = "") {
        for (auto& [id, o] : pending_) {
            if (symbol.empty() || o.symbol == symbol)
                o.status = OrderStatus::Cancelled;
        }
        if (symbol.empty()) pending_.clear();
        else {
            for (auto it = pending_.begin(); it != pending_.end();)
                if (it->second.symbol == symbol) it = pending_.erase(it);
                else ++it;
        }
    }

    std::size_t pending_count() const { return pending_.size(); }

private:
    bool try_fill(Order& o, const Bar& bar) {
        double fill_px = 0;
        bool   triggered = false;

        switch (o.type) {
        case OrderType::Market:
            // Fill at open of next bar (we have open here)
            fill_px   = bar.open;
            triggered = true;
            break;

        case OrderType::Limit:
            if (o.side == OrderSide::Buy  && bar.low  <= o.limit_px) {
                fill_px = std::min(o.limit_px, bar.open);
                triggered = true;
            } else if (o.side == OrderSide::Sell && bar.high >= o.limit_px) {
                fill_px = std::max(o.limit_px, bar.open);
                triggered = true;
            }
            break;

        case OrderType::Stop:
            if (o.side == OrderSide::Buy  && bar.high >= o.stop_px) {
                fill_px = std::max(o.stop_px, bar.open);
                triggered = true;
            } else if (o.side == OrderSide::Sell && bar.low <= o.stop_px) {
                fill_px = std::min(o.stop_px, bar.open);
                triggered = true;
            }
            break;

        case OrderType::StopLimit:
            if (o.side == OrderSide::Buy && bar.high >= o.stop_px
                && bar.low <= o.limit_px) {
                fill_px   = o.limit_px;
                triggered = true;
            } else if (o.side == OrderSide::Sell && bar.low <= o.stop_px
                && bar.high >= o.limit_px) {
                fill_px   = o.limit_px;
                triggered = true;
            }
            break;
        }

        if (!triggered) return false;

        // Apply spread cost
        fill_px += spread_.cost(fill_px, o.side);
        // Apply slippage
        fill_px += compute_slippage(slip_, bar, o.side, o.quantity);
        // Clamp to be physically reasonable (never below 0)
        fill_px = std::max(fill_px, 0.01);

        double commission = comm_.calculate(o.quantity, fill_px);

        Fill f;
        f.order_id    = o.id;
        f.symbol      = o.symbol;
        f.side        = o.side;
        f.quantity    = o.quantity;
        f.price       = fill_px;
        f.commission  = commission;
        f.ts          = bar.ts;
        f.strategy_id = o.strategy_id;

        o.status      = OrderStatus::Filled;
        o.filled_qty  = o.quantity;
        o.avg_fill_px = fill_px;
        o.ts_filled   = bar.ts;

        ev_eng_.publish(FillEvent{f});
        ev_eng_.publish(OrderEvent{o});
        return true;
    }

    EventEngine&   ev_eng_;
    SlippageConfig slip_;
    SpreadConfig   spread_;
    CommissionModel comm_;

    std::unordered_map<std::uint64_t, Order> pending_;
    std::unordered_map<std::string, Bar>     last_bar_;
    std::uint64_t order_counter_ = 0;
    Timestamp     current_ts_    = 0;
};

} // namespace bt
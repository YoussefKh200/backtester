#pragma once
#include "../core/Types.h"
#include <string>
#include <functional>
#include <unordered_map>

namespace bt {

// Forward-declare so strategy can submit orders without circular deps
class IOrderRouter {
public:
    virtual ~IOrderRouter() = default;
    virtual std::uint64_t submit(Order order) = 0;
    virtual bool          cancel(std::uint64_t order_id) = 0;
};

// ── Strategy interface ─────────────────────────────────────────────────────
class StrategyBase {
public:
    virtual ~StrategyBase() = default;

    // Called once before first bar
    virtual void on_start(IOrderRouter& router) { router_ = &router; }

    // Called on every market bar for any subscribed symbol
    virtual void on_bar(const Bar& bar) = 0;

    // Called on every fill event for orders submitted by this strategy
    virtual void on_fill(const Fill& fill) {}

    // Called once after last bar — good place to close open positions
    virtual void on_end() {}

    const std::string& id() const { return id_; }
    void set_id(std::string s)    { id_ = std::move(s); }

protected:
    // ── Helpers for derived strategies ────────────────────────────────────
    std::uint64_t buy_market(const std::string& sym, double qty) {
        Order o;
        o.symbol   = sym;
        o.type     = OrderType::Market;
        o.side     = OrderSide::Buy;
        o.quantity = qty;
        o.strategy_id = id_;
        return router_->submit(std::move(o));
    }

    std::uint64_t sell_market(const std::string& sym, double qty) {
        Order o;
        o.symbol   = sym;
        o.type     = OrderType::Market;
        o.side     = OrderSide::Sell;
        o.quantity = qty;
        o.strategy_id = id_;
        return router_->submit(std::move(o));
    }

    std::uint64_t buy_limit(const std::string& sym, double qty, double px) {
        Order o;
        o.symbol   = sym;
        o.type     = OrderType::Limit;
        o.side     = OrderSide::Buy;
        o.quantity = qty;
        o.limit_px = px;
        o.strategy_id = id_;
        return router_->submit(std::move(o));
    }

    std::uint64_t sell_limit(const std::string& sym, double qty, double px) {
        Order o;
        o.symbol   = sym;
        o.type     = OrderType::Limit;
        o.side     = OrderSide::Sell;
        o.quantity = qty;
        o.limit_px = px;
        o.strategy_id = id_;
        return router_->submit(std::move(o));
    }

    std::uint64_t buy_stop(const std::string& sym, double qty, double stop) {
        Order o;
        o.symbol   = sym;
        o.type     = OrderType::Stop;
        o.side     = OrderSide::Buy;
        o.quantity = qty;
        o.stop_px  = stop;
        o.strategy_id = id_;
        return router_->submit(std::move(o));
    }

    std::uint64_t sell_stop(const std::string& sym, double qty, double stop) {
        Order o;
        o.symbol   = sym;
        o.type     = OrderType::Stop;
        o.side     = OrderSide::Sell;
        o.quantity = qty;
        o.stop_px  = stop;
        o.strategy_id = id_;
        return router_->submit(std::move(o));
    }

    bool cancel_order(std::uint64_t id) { return router_->cancel(id); }

    IOrderRouter* router_ = nullptr;
    std::string   id_;
};

} // namespace bt
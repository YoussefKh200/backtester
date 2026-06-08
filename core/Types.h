#pragma once
#include <string>
#include <chrono>
#include <cstdint>
#include <variant>

namespace bt {

// ── Timestamp ──────────────────────────────────────────────────────────────
using Timestamp = std::int64_t;   // Unix ms

// ── Basic market structures ────────────────────────────────────────────────
struct Bar {
    Timestamp ts   = 0;
    double    open = 0, high = 0, low = 0, close = 0;
    double    volume = 0;
    std::string symbol;
};

// ── Order types ────────────────────────────────────────────────────────────
enum class OrderType  { Market, Limit, Stop, StopLimit };
enum class OrderSide  { Buy, Sell };
enum class OrderStatus{ Pending, PartialFill, Filled, Cancelled, Rejected };

struct Order {
    std::uint64_t id        = 0;
    std::string   symbol;
    OrderType     type      = OrderType::Market;
    OrderSide     side      = OrderSide::Buy;
    OrderStatus   status    = OrderStatus::Pending;
    double        quantity  = 0;
    double        filled_qty= 0;
    double        limit_px  = 0;
    double        stop_px   = 0;
    Timestamp     ts_placed = 0;
    Timestamp     ts_filled = 0;
    double        avg_fill_px= 0;
    std::string   strategy_id;
};

struct Fill {
    std::uint64_t order_id  = 0;
    std::string   symbol;
    OrderSide     side      = OrderSide::Buy;
    double        quantity  = 0;
    double        price     = 0;
    double        commission= 0;
    Timestamp     ts        = 0;
    std::string   strategy_id;
};

// ── Events ─────────────────────────────────────────────────────────────────
struct MarketDataEvent { Bar bar; };
struct OrderEvent      { Order order; };
struct FillEvent       { Fill fill; };
struct EndOfDataEvent  {};

using Event = std::variant<MarketDataEvent, OrderEvent, FillEvent, EndOfDataEvent>;

// ── Position ───────────────────────────────────────────────────────────────
struct Position {
    std::string symbol;
    double      qty          = 0;   // signed (+long, -short)
    double      avg_cost     = 0;
    double      realized_pnl = 0;
    double      market_value(double mid) const { return qty * mid; }
    double      unrealized_pnl(double mid) const {
        return qty * (mid - avg_cost);
    }
};

} // namespace bt

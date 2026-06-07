#pragma once
#include "../core/Types.h"
#include <unordered_map>
#include <vector>
#include <string>
#include <cmath>
#include <stdexcept>
#include <algorithm>

namespace bt {

struct EquityPoint {
    Timestamp ts;
    double    equity;
    double    cash;
    double    gross_exposure;
    double    net_exposure;
};

struct TradeRecord {
    std::string   symbol;
    std::string   strategy_id;
    OrderSide     entry_side;
    double        qty;
    double        entry_px;
    double        exit_px;
    Timestamp     entry_ts;
    Timestamp     exit_ts;
    double        pnl;         // realized, net of commissions
    double        commission;
};

class Portfolio {
public:
    explicit Portfolio(double initial_cash, double max_leverage = 4.0)
        : cash_(initial_cash)
        , initial_cash_(initial_cash)
        , max_leverage_(max_leverage) {}

    // ── Process a fill ─────────────────────────────────────────────────────
    void on_fill(const Fill& fill) {
        double signed_qty = (fill.side == OrderSide::Buy)
                            ?  fill.quantity
                            : -fill.quantity;

        auto& pos = positions_[fill.symbol];
        pos.symbol = fill.symbol;

        double old_qty = pos.qty;
        double new_qty = old_qty + signed_qty;

        // Realized PnL when reducing or flipping position
        double realized = 0.0;
        if (std::abs(old_qty) > 1e-9) {
            // Fraction being closed
            double close_qty = 0.0;
            if ((old_qty > 0 && signed_qty < 0) ||
                (old_qty < 0 && signed_qty > 0)) {
                close_qty = std::min(std::abs(old_qty), std::abs(signed_qty));
                if (old_qty < 0) close_qty = -close_qty;
                // pnl = closed_qty * (exit - entry) for longs
                realized = close_qty * (fill.price - pos.avg_cost);
            }
        }

        // Update avg cost (FIFO approximation)
        if (std::abs(new_qty) > 1e-9) {
            if ((old_qty >= 0 && signed_qty > 0) ||
                (old_qty <= 0 && signed_qty < 0)) {
                // Adding to position
                pos.avg_cost = (pos.avg_cost * std::abs(old_qty) +
                                fill.price * std::abs(signed_qty)) /
                               std::abs(new_qty);
            } else if (std::abs(new_qty) < std::abs(old_qty)) {
                // Partial close — avg cost stays
            } else {
                // Flip — new avg cost from remaining
                pos.avg_cost = fill.price;
            }
        } else {
            pos.avg_cost = 0;
        }

        pos.qty            = new_qty;
        pos.realized_pnl  += realized;
        realized_pnl_total_+= realized;

        // Cash accounting
        double cash_delta = -signed_qty * fill.price - fill.commission;
        cash_ += cash_delta;
        total_commission_ += fill.commission;

        // Record trade if position closed/reduced
        if (std::abs(realized) > 1e-9) {
            TradeRecord tr;
            tr.symbol      = fill.symbol;
            tr.strategy_id = fill.strategy_id;
            tr.entry_side  = (old_qty > 0) ? OrderSide::Buy : OrderSide::Sell;
            tr.qty         = std::abs(realized / (fill.price - pos.avg_cost + 1e-12));
            tr.entry_px    = pos.avg_cost;
            tr.exit_px     = fill.price;
            tr.entry_ts    = 0; // filled from open order tracking
            tr.exit_ts     = fill.ts;
            tr.pnl         = realized - fill.commission;
            tr.commission  = fill.commission;
            trades_.push_back(tr);
        }
    }

    // ── Mark-to-market ─────────────────────────────────────────────────────
    void mark_to_market(const std::string& symbol, double mid) {
        last_prices_[symbol] = mid;
    }

    void snapshot(Timestamp ts) {
        double mkt_val = 0, gross = 0;
        for (auto& [sym, pos] : positions_) {
            auto it = last_prices_.find(sym);
            if (it == last_prices_.end()) continue;
            double val  = pos.qty * it->second;
            mkt_val    += val;
            gross      += std::abs(val);
        }
        double equity = cash_ + mkt_val;
        max_equity_   = std::max(max_equity_, equity);
        double dd     = (max_equity_ > 0) ? (max_equity_ - equity) / max_equity_ : 0;
        max_drawdown_ = std::max(max_drawdown_, dd);

        equity_curve_.push_back({ts, equity, cash_, gross, mkt_val});
    }

    // ── Leverage check ─────────────────────────────────────────────────────
    bool within_leverage(double order_value) const {
        double equity = current_equity();
        if (equity <= 0) return false;
        double current_gross = gross_exposure();
        return (current_gross + std::abs(order_value)) / equity <= max_leverage_;
    }

    // ── Accessors ──────────────────────────────────────────────────────────
    double cash()          const { return cash_; }
    double initial_cash()  const { return initial_cash_; }
    double total_commission()const{ return total_commission_; }
    double realized_pnl()  const { return realized_pnl_total_; }
    double max_drawdown()  const { return max_drawdown_; }

    double current_equity() const {
        double mkt = 0;
        for (auto& [sym, pos] : positions_) {
            auto it = last_prices_.find(sym);
            if (it != last_prices_.end())
                mkt += pos.qty * it->second;
        }
        return cash_ + mkt;
    }

    double gross_exposure() const {
        double g = 0;
        for (auto& [sym, pos] : positions_) {
            auto it = last_prices_.find(sym);
            if (it != last_prices_.end())
                g += std::abs(pos.qty * it->second);
        }
        return g;
    }

    const Position* get_position(const std::string& sym) const {
        auto it = positions_.find(sym);
        return (it != positions_.end()) ? &it->second : nullptr;
    }

    const std::vector<EquityPoint>& equity_curve() const { return equity_curve_; }
    const std::vector<TradeRecord>& trades()        const { return trades_; }
    const std::unordered_map<std::string, Position>& positions() const { return positions_; }

private:
    double cash_;
    double initial_cash_;
    double max_leverage_;
    double realized_pnl_total_ = 0;
    double total_commission_   = 0;
    double max_equity_         = 0;
    double max_drawdown_       = 0;

    std::unordered_map<std::string, Position> positions_;
    std::unordered_map<std::string, double>   last_prices_;
    std::vector<EquityPoint>                  equity_curve_;
    std::vector<TradeRecord>                  trades_;
};

} // namespace bt
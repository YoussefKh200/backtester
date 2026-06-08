#pragma once
#include "../core/Types.h"
#include "../portfolio/Portfolio.h"
#include <string>
#include <unordered_map>
#include <stdexcept>

namespace bt {

struct RiskConfig {
    double max_drawdown_pct    = 0.20;   // halt if DD > 20%
    double max_position_pct    = 0.25;   // no single position > 25% equity
    double max_gross_leverage  = 4.0;
    double max_net_leverage    = 2.0;
    double daily_loss_limit_pct= 0.03;   // 3% daily loss limit
};

enum class RiskAction { Allow, Block, Reduce };

struct RiskDecision {
    RiskAction  action = RiskAction::Allow;
    std::string reason;
    double      allowed_qty = 0; // if Reduce
};

class RiskManager {
public:
    explicit RiskManager(RiskConfig cfg = {}) : cfg_(cfg) {}

    // ── Pre-trade check ────────────────────────────────────────────────────
    RiskDecision check_order(const Order& o, const Portfolio& pf,
                             double last_px) const {
        double equity = pf.current_equity();
        if (equity <= 0)
            return {RiskAction::Block, "Zero equity"};

        // Max drawdown halt
        if (pf.max_drawdown() >= cfg_.max_drawdown_pct)
            return {RiskAction::Block,
                    "Max drawdown breached: " +
                    std::to_string(pf.max_drawdown() * 100) + "%"};

        // Position size limit
        double order_value = o.quantity * last_px;
        double max_position = equity * cfg_.max_position_pct;
        if (order_value > max_position) {
            double allowed = max_position / last_px;
            return {RiskAction::Reduce,
                    "Position limit: capping to " + std::to_string(allowed) + " shares",
                    allowed};
        }

        // Gross leverage
        double gross     = pf.gross_exposure();
        double new_gross = gross + std::abs(order_value);
        if (new_gross / equity > cfg_.max_gross_leverage)
            return {RiskAction::Block,
                    "Gross leverage limit: " +
                    std::to_string(new_gross / equity)};

        return {RiskAction::Allow, ""};
    }

    // ── Intra-day monitoring ───────────────────────────────────────────────
    void update_daily(double pnl_today) {
        daily_pnl_ = pnl_today;
    }

    bool daily_loss_halt(double equity) const {
        if (equity <= 0) return true;
        return daily_pnl_ / equity < -cfg_.daily_loss_limit_pct;
    }

    void reset_daily() { daily_pnl_ = 0; }

    const RiskConfig& config() const { return cfg_; }

private:
    RiskConfig cfg_;
    double     daily_pnl_ = 0;
};

} // namespace bt

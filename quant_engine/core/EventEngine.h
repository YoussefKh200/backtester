#pragma once
#include "Types.h"
#include <queue>
#include <functional>
#include <vector>
#include <unordered_map>

namespace bt {

// ── Event handler signature ────────────────────────────────────────────────
using EventHandler = std::function<void(const Event&)>;

// ── Priority-based event queue ─────────────────────────────────────────────
// Ensures MarketData processed before Orders before Fills within same ts
class EventQueue {
public:
    // Lower priority value = processed first
    static constexpr int PRI_MARKET = 0;
    static constexpr int PRI_ORDER  = 1;
    static constexpr int PRI_FILL   = 2;
    static constexpr int PRI_EOD    = 10;

    struct Entry {
        int       priority;
        Timestamp ts;
        Event     event;
        bool operator>(const Entry& o) const {
            if (ts != o.ts) return ts > o.ts;
            return priority > o.priority;
        }
    };

    void push(Event ev, Timestamp ts) {
        int pri = std::visit([](auto&& e) -> int {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, MarketDataEvent>) return PRI_MARKET;
            if constexpr (std::is_same_v<T, OrderEvent>)      return PRI_ORDER;
            if constexpr (std::is_same_v<T, FillEvent>)       return PRI_FILL;
            return PRI_EOD;
        }, ev);
        q_.push({pri, ts, std::move(ev)});
    }

    bool pop(Event& out) {
        if (q_.empty()) return false;
        out = std::move(q_.top().event);
        q_.pop();
        return true;
    }

    bool empty() const { return q_.empty(); }
    std::size_t size() const { return q_.size(); }

private:
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> q_;
};

// ── Event dispatcher: publish/subscribe ───────────────────────────────────
class EventEngine {
public:
    template<typename EventT>
    void subscribe(EventHandler handler) {
        auto idx = type_index<EventT>();
        handlers_[idx].push_back(std::move(handler));
    }

    void publish(const Event& ev) {
        std::visit([&](auto&& e) {
            auto idx = type_index<std::decay_t<decltype(e)>>();
            auto it  = handlers_.find(idx);
            if (it != handlers_.end())
                for (auto& h : it->second) h(ev);
        }, ev);
    }

private:
    template<typename T>
    static std::size_t type_index() {
        static const std::size_t id = next_id_++;
        return id;
    }
    static inline std::size_t next_id_ = 0;
    std::unordered_map<std::size_t, std::vector<EventHandler>> handlers_;
};

} // namespace bt
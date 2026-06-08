#pragma once
#include "../core/Types.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <functional>
#include <ctime>
#include <iomanip>

#include <optional>
namespace bt {

// ── Parse helpers ──────────────────────────────────────────────────────────
inline Timestamp parse_iso8601(const std::string& s) {
    // Accepts: "2023-01-03", "2023-01-03 09:30:00", "2023-01-03T09:30:00"
    std::tm tm = {};
    std::istringstream ss(s);
    if (s.size() == 10)
        ss >> std::get_time(&tm, "%Y-%m-%d");
    else if (s.find('T') != std::string::npos)
        ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    else
        ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (ss.fail()) throw std::runtime_error("Bad timestamp: " + s);
#ifdef _WIN32
    return static_cast<Timestamp>(_mkgmtime(&tm)) * 1000LL;
#else
    return static_cast<Timestamp>(timegm(&tm)) * 1000LL;
#endif
}

// ── Column mapping ─────────────────────────────────────────────────────────
struct CsvSchema {
    int col_ts     = 0;
    int col_open   = 1;
    int col_high   = 2;
    int col_low    = 3;
    int col_close  = 4;
    int col_volume = 5;
    char delimiter = ',';
    bool has_header= true;
};

// ── Single-asset loader ────────────────────────────────────────────────────
class CsvLoader {
public:
    explicit CsvLoader(CsvSchema schema = {}) : schema_(schema) {}

    std::vector<Bar> load(const std::string& path, const std::string& symbol) {
        std::ifstream f(path);
        if (!f.is_open())
            throw std::runtime_error("Cannot open: " + path);

        std::vector<Bar> bars;
        bars.reserve(1 << 20); // pre-alloc 1M
        std::string line;

        if (schema_.has_header) std::getline(f, line); // skip header

        while (std::getline(f, line)) {
            if (line.empty()) continue;
            auto cols = split(line, schema_.delimiter);
            if ((int)cols.size() <= std::max({schema_.col_ts, schema_.col_close,
                                               schema_.col_volume}))
                continue;
            Bar b;
            b.symbol = symbol;
            b.ts     = parse_iso8601(trim(cols[schema_.col_ts]));
            b.open   = std::stod(cols[schema_.col_open]);
            b.high   = std::stod(cols[schema_.col_high]);
            b.low    = std::stod(cols[schema_.col_low]);
            b.close  = std::stod(cols[schema_.col_close]);
            b.volume = (schema_.col_volume >= 0 && (int)cols.size() > schema_.col_volume)
                       ? std::stod(cols[schema_.col_volume]) : 0.0;
            bars.push_back(b);
        }
        std::sort(bars.begin(), bars.end(),
                  [](const Bar& a, const Bar& b){ return a.ts < b.ts; });
        return bars;
    }

private:
    CsvSchema schema_;

    static std::vector<std::string> split(const std::string& s, char d) {
        std::vector<std::string> r;
        std::istringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, d)) r.push_back(tok);
        return r;
    }
    static std::string trim(std::string s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(),
                [](unsigned char c){ return !std::isspace(c); }));
        s.erase(std::find_if(s.rbegin(), s.rend(),
                [](unsigned char c){ return !std::isspace(c); }).base(), s.end());
        // strip quotes
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
            s = s.substr(1, s.size()-2);
        return s;
    }
};

// ── Multi-asset feed with timestamp alignment ──────────────────────────────
// Merges N asset timelines; produces events in chronological order.
class MarketDataFeed {
public:
    void add_asset(std::vector<Bar> bars) {
        if (bars.empty()) return;
        feeds_.push_back(std::move(bars));
        cursors_.push_back(0);
    }

    // Returns next bar in global time order, or nullopt if exhausted
    std::optional<Bar> next() {
        // Find feed with smallest next timestamp
        int   best = -1;
        Timestamp bt_ts = std::numeric_limits<Timestamp>::max();
        for (int i = 0; i < (int)feeds_.size(); ++i) {
            if (cursors_[i] < feeds_[i].size()) {
                Timestamp t = feeds_[i][cursors_[i]].ts;
                if (t < bt_ts) { bt_ts = t; best = i; }
            }
        }
        if (best < 0) return std::nullopt;
        return feeds_[best][cursors_[best]++];
    }

    bool exhausted() const {
        for (int i = 0; i < (int)feeds_.size(); ++i)
            if (cursors_[i] < feeds_[i].size()) return false;
        return true;
    }

    std::size_t total_bars() const {
        std::size_t n = 0;
        for (auto& f : feeds_) n += f.size();
        return n;
    }

    void reset() { std::fill(cursors_.begin(), cursors_.end(), 0); }

private:
    std::vector<std::vector<Bar>> feeds_;
    std::vector<std::size_t>      cursors_;
};

} // namespace bt

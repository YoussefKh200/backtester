#pragma once
// ── Multi-threaded data preprocessing ─────────────────────────────────────
// Loads and preprocesses multiple CSV files in parallel using a thread pool.
// Computes derived features (returns, rolling stats, VWAP) concurrently
// before the single-threaded event loop begins.
//
// Design: preprocessing is embarrassingly parallel per-symbol; results are
// merged into a sorted multi-asset feed after all threads complete.
// ──────────────────────────────────────────────────────────────────────────
#include "../core/Types.h"
#include "../data/MarketDataLoader.h"
#include <thread>
#include <future>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <stdexcept>
#include <cmath>

namespace bt {

// ── Derived bar: enriched OHLCV with pre-computed features ────────────────
struct EnrichedBar {
    Bar    bar;
    double log_return   = 0;  // ln(close_t / close_{t-1})
    double vwap         = 0;  // running daily VWAP
    double atr14        = 0;  // ATR(14)
    double rsi14        = 0;  // RSI(14)
    double bb_upper     = 0;  // Bollinger upper (20, 2σ)
    double bb_lower     = 0;  // Bollinger lower
    double bb_pct       = 0;  // (close - lower) / (upper - lower)
    double volume_ratio = 0;  // vol / 20-bar avg vol (relative volume)
};

// ── Per-symbol preprocessing ───────────────────────────────────────────────
inline std::vector<EnrichedBar> preprocess_symbol(std::vector<Bar> bars) {
    if (bars.empty()) return {};
    std::vector<EnrichedBar> out;
    out.reserve(bars.size());

    // Rolling buffers
    std::deque<double> closes, vols, trs;
    double cum_pv = 0, cum_v = 0;          // for VWAP
    double avg_gain = 0, avg_loss = 0;      // for RSI
    bool rsi_seeded = false;
    const int RSI_P = 14, BB_P = 20, ATR_P = 14, VOL_P = 20;

    double prev_close = bars[0].close;

    for (std::size_t i = 0; i < bars.size(); ++i) {
        const Bar& b = bars[i];
        EnrichedBar eb;
        eb.bar = b;

        // Log return
        if (i > 0 && prev_close > 0)
            eb.log_return = std::log(b.close / prev_close);

        // VWAP (resets on new day — detect by ts day boundary)
        Timestamp day = b.ts / 86400000LL;
        Timestamp prev_day = (i > 0) ? bars[i-1].ts / 86400000LL : day;
        if (day != prev_day) { cum_pv = 0; cum_v = 0; }
        double typical = (b.high + b.low + b.close) / 3.0;
        cum_pv += typical * b.volume;
        cum_v  += b.volume;
        eb.vwap = (cum_v > 0) ? cum_pv / cum_v : typical;

        // True Range
        double tr = b.high - b.low;
        if (i > 0) tr = std::max(tr, std::max(
            std::abs(b.high - prev_close), std::abs(b.low - prev_close)));
        trs.push_back(tr);
        if ((int)trs.size() > ATR_P) trs.pop_front();
        if ((int)trs.size() == ATR_P) {
            double atr = 0; for (double v : trs) atr += v;
            eb.atr14 = atr / ATR_P;
        }

        // RSI (Wilder smoothing)
        if (i > 0) {
            double delta = b.close - prev_close;
            double gain  = std::max(delta, 0.0);
            double loss  = std::max(-delta, 0.0);
            if (!rsi_seeded && (int)closes.size() >= RSI_P - 1) {
                // seed with simple average
                double sg = 0, sl = 0;
                // recompute from closes buffer
                for (int j = 1; j < RSI_P && j <= (int)closes.size(); ++j) {
                    double d = closes[closes.size()-j+1 < closes.size()
                                      ? closes.size()-j : 0]
                               - (j > 1 ? closes[closes.size()-j] : prev_close);
                    // simplified: just use current gain/loss for seeding
                }
                avg_gain = gain; avg_loss = loss; // simplified seed
                rsi_seeded = true;
            } else if (rsi_seeded) {
                avg_gain = (avg_gain * (RSI_P-1) + gain) / RSI_P;
                avg_loss = (avg_loss * (RSI_P-1) + loss) / RSI_P;
                double rs = (avg_loss > 0) ? avg_gain / avg_loss : 100.0;
                eb.rsi14 = 100.0 - (100.0 / (1.0 + rs));
            }
        }

        // Bollinger Bands (20, 2σ)
        closes.push_back(b.close);
        if ((int)closes.size() > BB_P) closes.pop_front();
        if ((int)closes.size() == BB_P) {
            double mu = 0; for (double c : closes) mu += c; mu /= BB_P;
            double var = 0; for (double c : closes) var += (c-mu)*(c-mu);
            double sd = std::sqrt(var / BB_P);
            eb.bb_upper = mu + 2*sd;
            eb.bb_lower = mu - 2*sd;
            double rng = eb.bb_upper - eb.bb_lower;
            eb.bb_pct   = (rng > 0) ? (b.close - eb.bb_lower) / rng : 0.5;
        }

        // Relative volume
        vols.push_back(b.volume);
        if ((int)vols.size() > VOL_P) vols.pop_front();
        if ((int)vols.size() == VOL_P) {
            double avg = 0; for (double v : vols) avg += v; avg /= VOL_P;
            eb.volume_ratio = (avg > 0) ? b.volume / avg : 1.0;
        }

        prev_close = b.close;
        out.push_back(eb);
    }
    return out;
}

// ── Thread pool ────────────────────────────────────────────────────────────
class ThreadPool {
public:
    explicit ThreadPool(std::size_t n_threads)
        : stop_(false)
    {
        for (std::size_t i = 0; i < n_threads; ++i) {
            workers_.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lk(mu_);
                        cv_.wait(lk, [this]{ return stop_ || !tasks_.empty(); });
                        if (stop_ && tasks_.empty()) return;
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    task();
                }
            });
        }
    }

    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using R = std::invoke_result_t<F, Args...>;
        auto task = std::make_shared<std::packaged_task<R()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        auto fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (stop_) throw std::runtime_error("ThreadPool stopped");
            tasks_.push([task]{ (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

    ~ThreadPool() {
        { std::lock_guard<std::mutex> lk(mu_); stop_ = true; }
        cv_.notify_all();
        for (auto& w : workers_) if (w.joinable()) w.join();
    }

    std::size_t thread_count() const { return workers_.size(); }

private:
    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex                        mu_;
    std::condition_variable           cv_;
    bool                              stop_;
};

// ── Parallel data pipeline ─────────────────────────────────────────────────
struct DataJob {
    std::string csv_path;
    std::string symbol;
    CsvSchema   schema;
};

struct PipelineResult {
    std::string                 symbol;
    std::vector<Bar>            raw_bars;
    std::vector<EnrichedBar>    enriched_bars;
    std::string                 error;   // non-empty on failure
};

class ParallelDataPipeline {
public:
    explicit ParallelDataPipeline(std::size_t n_threads = 0) {
        if (n_threads == 0)
            n_threads = std::max(1u, std::thread::hardware_concurrency());
        pool_ = std::make_unique<ThreadPool>(n_threads);
    }

    void add_job(DataJob job) { jobs_.push_back(std::move(job)); }

    // Also accept pre-loaded bar vectors
    void add_bars(std::string symbol, std::vector<Bar> bars) {
        preloaded_.push_back({std::move(symbol), std::move(bars)});
    }

    // Run all jobs in parallel, return results
    std::vector<PipelineResult> run() {
        std::vector<std::future<PipelineResult>> futures;
        futures.reserve(jobs_.size() + preloaded_.size());

        // CSV jobs
        for (auto& job : jobs_) {
            futures.push_back(pool_->submit([job]() mutable -> PipelineResult {
                PipelineResult r;
                r.symbol = job.symbol;
                try {
                    CsvLoader loader(job.schema);
                    r.raw_bars = loader.load(job.csv_path, job.symbol);
                    r.enriched_bars = preprocess_symbol(r.raw_bars);
                } catch (const std::exception& e) {
                    r.error = e.what();
                }
                return r;
            }));
        }

        // Pre-loaded bar vectors
        for (auto& [sym, bars] : preloaded_) {
            futures.push_back(pool_->submit([sym, bars]() mutable -> PipelineResult {
                PipelineResult r;
                r.symbol     = sym;
                r.raw_bars   = std::move(bars);
                r.enriched_bars = preprocess_symbol(r.raw_bars);
                return r;
            }));
        }

        std::vector<PipelineResult> results;
        results.reserve(futures.size());
        for (auto& f : futures) results.push_back(f.get());
        return results;
    }

    std::size_t thread_count() const { return pool_->thread_count(); }

private:
    std::unique_ptr<ThreadPool>            pool_;
    std::vector<DataJob>                   jobs_;
    std::vector<std::pair<std::string, std::vector<Bar>>> preloaded_;
};

} // namespace bt

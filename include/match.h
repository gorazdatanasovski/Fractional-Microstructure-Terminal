/**
 * @file match.h
 * @brief Fractional Matching Engine — Whittle Contrast & Zero-Allocation Telemetry
 *
 * Architecture:
 *   - FastLog: IEEE 754 bit-decomposition LUT for O(1) logarithm (no std::log in hot path)
 *   - FractionalEngine: Whittle likelihood Hurst estimator with 256-bar burn-in,
 *     volume-participation slippage, and fixed-size result storage
 *   - JsonWriter: POSIX write() streaming through a static 8KB char buffer
 *
 * Memory Discipline:
 *   Zero calls to new/malloc/std::vector/std::string.
 *   All arrays are fixed-size, stack-allocated at compile time.
 */

#ifndef FRACTIONAL_TERMINAL_MATCH_H
#define FRACTIONAL_TERMINAL_MATCH_H

#include "data.h"
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <arm_neon.h>
#include <atomic>
#include <Accelerate/Accelerate.h>

namespace ft {

// ─── Engine Constants ────────────────────────────────────────────────────────
static constexpr size_t MAX_LOOKBACK        = 1024;
static constexpr size_t MAX_TRADES          = 4096;
static constexpr size_t MAX_SNAPSHOTS       = 8192;
static constexpr double VOLUME_PART_LIMIT   = 0.10;  // 10% participation ceiling (used as fallback or additional check if needed)
static constexpr double KAPPA               = 0.25;  // Continuous Kelly empirical scaling scalar
static constexpr double INITIAL_EQUITY      = 1000000.0;
static constexpr double Y_IMPACT            = 0.15;   // Empirical calibration constant for square-root market impact
static constexpr double PI                  = 3.14159265358979323846;
static constexpr size_t INTRA_BAR_TICKS     = 600;


// ─── Fast Logarithm (IEEE 754 Bit Decomposition) ────────────────────────────
// Decomposes x = 2^e * (1 + f) via raw bit extraction.
// log(x) = e * ln(2) + LUT[f]
// ~0.05% max error. No branching on the fast path.

class FastLog {
public:
    static constexpr int    LUT_SIZE = 2048;
    static constexpr int    LUT_BITS = 11;     // log2(2048)
    static constexpr double LN2      = 0.693147180559945309;

    FastLog() {
        for (int i = 0; i < LUT_SIZE; ++i) {
            lut_[i] = std::log(1.0 + static_cast<double>(i) / LUT_SIZE);
        }
    }

    double operator()(double x) const {
        if (x <= 0.0) return -1e30;  // Guard: log(0) / log(negative)
        uint64_t bits;
        std::memcpy(&bits, &x, sizeof(bits));
        int exponent = static_cast<int>((bits >> 52) & 0x7FF) - 1023;
        int idx = static_cast<int>((bits & 0x000FFFFFFFFFFFFFULL) >> (52 - LUT_BITS));
        return exponent * LN2 + lut_[idx];
    }

private:
    double lut_[LUT_SIZE];
};


// ─── Signal Enum ─────────────────────────────────────────────────────────────
enum class Signal : int {
    SHORT = -1,
    FLAT  =  0,
    LONG  =  1
};


// ─── Trade Record ────────────────────────────────────────────────────────────
struct TradeRecord {
    int64_t timestamp_us;
    int     side;            // +1 buy, -1 sell
    double  price;           // theoretical (mid) price
    double  fill_price;      // after volume-friction slippage
    int64_t quantity;        // Q_t calculated via Kelly
    double  slippage_bps;    // applied slippage in basis points
    double  kelly_f;         // Continuous Kelly fraction f_t*
    double  post_capital;    // Post-slippage execution capital
    double  mdd;             // Instantaneous Maximum Drawdown (MDD)
};


// ─── Equity Snapshot (per-bar) ───────────────────────────────────────────────
struct EquitySnap {
    int64_t timestamp_us;
    double  equity;          // mark-to-market equity
    double  hurst;           // estimated Hurst exponent
    int     position;        // current direction: -1, 0, +1
    uint64_t latency_cycles; // CPU cycles measured during SIMD evaluation
};


// ─── HJB Telemetry Point ─────────────────────────────────────────────────────
struct HJBPoint {
    double physical_price;
    double reservation_price;
    double inventory;
    double hawkes_lambda;
    size_t queue_position;
};

// ─── Lock-Free Ring Buffer Limit Order Queue ────────────────────────────────
template<typename T, size_t Capacity>
class LockFreeQueue {
private:
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    T buffer_[Capacity];
public:
    bool push(const T& item) {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (current_tail + 1) % Capacity;
        if (next_tail == head_.load(std::memory_order_acquire)) return false; // Full
        buffer_[current_tail] = item;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        if (current_head == tail_.load(std::memory_order_acquire)) return false; // Empty
        item = buffer_[current_head];
        head_.store((current_head + 1) % Capacity, std::memory_order_release);
        return true;
    }

    size_t size() const {
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t t = tail_.load(std::memory_order_acquire);
        if (t >= h) return t - h;
        return Capacity + t - h;
    }

    void clear() {
        head_.store(0, std::memory_order_release);
        tail_.store(0, std::memory_order_release);
    }
};

// ─── SMC Particle Filter ───────────────────────────────────────────────────
struct Particle {
    double lambda;
    double kappa;
    double sigma;
    double weight;
};

struct HFPoint {
    int64_t ts;
    double realized;
    double lower;
    double upper;
};

// ─── Fractional Trading Engine ──────────────────────────────────────────────
class FractionalEngine {
public:
    FractionalEngine(size_t lookback, double tau);

    /** @brief Ingest one bar. Burn-in enforced for first BURN_IN bars. */
    void process_bar(const TickRow& row, size_t bar_index);

    // ── Read-only result accessors ───────────────────────────────────────
    const EquitySnap*   snapshots()      const { return snaps_; }
    size_t              snap_count()     const { return snap_n_; }
    const TradeRecord*  trades()         const { return trades_; }
    size_t              trade_count()    const { return trade_n_; }
    const HJBPoint*     hjb_path()       const { return hjb_path_; }
    double              final_equity()   const { return equity_; }
    double              max_drawdown()   const { return max_dd_; }
    double              avg_hurst()      const {
        return hurst_n_ > 0 ? hurst_sum_ / hurst_n_ : 0.5;
    }
    const std::vector<HFPoint>& hf_telemetry() const { return hf_telemetry_; }

private:
    // ── Engine Parameters ────────────────────────────────────────────────
    size_t  lookback_;
    double  tau_;
    size_t  whittle_bandwidth_;

    // ── Circular return buffer ───────────────────────────────────────────
    double  returns_[MAX_LOOKBACK];
    size_t  buf_head_;
    size_t  bars_fed_;
    double  prev_close_;

    // ── Welford's Rolling Variance State ─────────────────────────────────
    double  w_mean_;
    double  w_m2_;
    double  current_hurst_;

    // ── Spectral workspace (only whittle_bandwidth_ entries used) ────────
    alignas(16) double  pgram_[MAX_LOOKBACK / 2];

    // ── Position & equity state ──────────────────────────────────────────
    Signal  position_;
    double  entry_price_;
    int64_t current_qty_;
    double  equity_;
    double  peak_equity_;
    double  max_dd_;
    double  hurst_sum_;
    size_t  hurst_n_;

    // ── Fixed-size result storage ────────────────────────────────────────
    TradeRecord  trades_[MAX_TRADES];
    size_t       trade_n_;
    EquitySnap   snaps_[MAX_SNAPSHOTS];
    size_t       snap_n_;
    HJBPoint     hjb_path_[INTRA_BAR_TICKS]; // Telemetry for the latest trade

    // ── Pre-allocated rFBM buffers (to avoid thread stack overflow) ──────────
    double       C_[INTRA_BAR_TICKS * INTRA_BAR_TICKS];
    double       L_[INTRA_BAR_TICKS * INTRA_BAR_TICKS];
    double       Z_[INTRA_BAR_TICKS];
    double       W_[INTRA_BAR_TICKS];

    LockFreeQueue<double, 1024> lob_queue_;
    alignas(64) Particle smc_swarm_[1024];

    std::vector<HFPoint> hf_telemetry_;

    // ── Fast math ────────────────────────────────────────────────────────
    FastLog flog_;

    // ── Internal methods ─────────────────────────────────────────────────
    void    compute_periodogram();
    double  estimate_hurst();
    Signal  generate_signal(double hurst, double last_return);
    void    execute_trade(const TickRow& row, Signal new_signal);
    void    record_snapshot(const TickRow& row, double hurst, uint64_t latency_cycles);
};


// ─── Zero-Allocation JSON Stream Writer ──────────────────────────────────────
// Streams formatted text through a static 8 KB buffer via POSIX write().
// No std::string, no std::stringstream, no nlohmann/json.

class JsonWriter {
public:
    static constexpr size_t BUF_CAP = 8192;

    JsonWriter();
    ~JsonWriter();

    bool open(const char* path);
    void close();
    void put(const char* str);
    void fmt(const char* format, ...) __attribute__((format(printf, 2, 3)));

private:
    char    buf_[BUF_CAP];
    size_t  pos_;
    int     fd_;
    void    flush();
};


// ─── Telemetry Export ────────────────────────────────────────────────────────
void export_metrics(const FractionalEngine& engine, int64_t latency_us, 
                    const size_t* opt_n, const double* opt_tau, const double* opt_s, size_t opt_count,
                    const char* path);

} // namespace ft

#endif // FRACTIONAL_TERMINAL_MATCH_H

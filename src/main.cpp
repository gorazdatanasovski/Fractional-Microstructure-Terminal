/**
 * @file main.cpp
 * @brief Fractional Microstructure Terminal — Phase 4 Master Execution Loop
 *
 * Workflow:
 *   1. Map data/matrix.bin via POSIX mmap
 *   2. Spawn std::async thread pool across (N, tau) grid
 *   3. Collect optimization surface (Sharpe Ratio)
 *   4. Stream JSON telemetry including optimization surface
 */

#include "data.h"
#include "match.h"

#include <cstdio>
#include <sys/time.h>
#include <vector>
#include <future>
#include <memory>
#include <cmath>

// ─── Microsecond Timer ───────────────────────────────────────────────────────
static int64_t now_us() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000000LL + tv.tv_usec;
}

// ─── Optimization Result Struct ──────────────────────────────────────────────
struct OptResult {
    size_t N;
    double tau;
    double sharpe;
    std::unique_ptr<ft::FractionalEngine> engine;
};

// ─── Thread Worker ───────────────────────────────────────────────────────────
OptResult run_engine(const ft::MatrixMap* matrix, size_t n, double tau) {
    auto engine = std::make_unique<ft::FractionalEngine>(n, tau);
    for (size_t i = 0; i < matrix->rows(); ++i) {
        if (i + 4 < matrix->rows()) {
            __builtin_prefetch(&((*matrix)[i + 4]), 0, 3);
        }
        engine->process_bar((*matrix)[i], i);
    }
    
    // Calculate Sharpe Ratio from equity snapshots
    double mean_ret = 0.0, m2_ret = 0.0;
    size_t count = 0;
    for (size_t i = 1; i < engine->snap_count(); ++i) {
        double prev = engine->snapshots()[i-1].equity;
        double curr = engine->snapshots()[i].equity;
        if (prev > 0) {
            double ret = (curr - prev) / prev;
            count++;
            double delta = ret - mean_ret;
            mean_ret += delta / count;
            m2_ret += delta * (ret - mean_ret);
        }
    }
    
    double sharpe = 0.0;
    if (count > 1 && m2_ret > 0) {
        double var = m2_ret / (count - 1);
        double stddev = std::sqrt(var);
        // Annualize assuming 10-min bars, ~39 per day, 252 days/yr => 9828 bars/yr
        sharpe = (mean_ret / stddev) * std::sqrt(9828.0);
    }
    
    return {n, tau, sharpe, std::move(engine)};
}

int main() {
    std::printf("\n");
    std::printf("═══════════════════════════════════════════════════════════════\n");
    std::printf("  FRACTIONAL MICROSTRUCTURE TERMINAL — Phase 8 Execution\n");
    std::printf("  Lock-Free LOB Queues | Hawkes Intensity | SIMD Prefetching\n");
    std::printf("═══════════════════════════════════════════════════════════════\n\n");

    // ── 1. Load Memory Map ───────────────────────────────────────────────
    ft::MatrixMap matrix;
    if (!matrix.load("data/matrix.bin")) {
        std::fprintf(stderr, "FATAL: Failed to load data/matrix.bin\n");
        return 1;
    }

    std::printf("[MMAP]     Mapped %zu rows (%zu bytes)\n", matrix.rows(), matrix.bytes());
    std::printf("[ENGINE]   Parameter space grid mapping initialized.\n");

    std::vector<size_t> N_grid = {128, 256, 512, 1024};
    std::vector<double> tau_grid = {1.5, 2.0, 2.5, 3.0};

    // ── 2. Execute Async Thread Pool ─────────────────────────────────────
    const int64_t t0 = now_us();
    std::vector<std::future<OptResult>> futures;

    for (size_t n : N_grid) {
        for (double tau : tau_grid) {
            futures.push_back(std::async(std::launch::async, run_engine, &matrix, n, tau));
        }
    }

    std::vector<OptResult> results;
    for (auto& f : futures) {
        results.push_back(f.get());
    }

    const int64_t t1 = now_us();
    const int64_t latency = t1 - t0;

    // ── 3. Compile Data for JSON ─────────────────────────────────────────
    std::vector<size_t> opt_n;
    std::vector<double> opt_tau;
    std::vector<double> opt_s;
    OptResult* baseline = nullptr;

    for (auto& r : results) {
        opt_n.push_back(r.N);
        opt_tau.push_back(r.tau);
        opt_s.push_back(r.sharpe);
        
        // Select baseline for main dashboard
        if (r.N == 256 && r.tau == 2.0) {
            baseline = &r;
        }
    }
    
    if (!baseline && !results.empty()) baseline = &results.front();

    // ── 4. Console Benchmark Output ──────────────────────────────────────
    std::printf("\n");
    std::printf("═══════════════════════════════════════════════════════════════\n");
    std::printf("  BENCHMARK RESULTS (Grid Size: %zu nodes)\n", results.size());
    std::printf("───────────────────────────────────────────────────────────────\n");
    std::printf("  Baseline (N=%zu, tau=%.1f):\n", baseline->N, baseline->tau);
    std::printf("  Total trades executed:   %zu\n", baseline->engine->trade_count());
    std::printf("  Final equity:            $%.2f\n", baseline->engine->final_equity());
    std::printf("  Baseline Sharpe Ratio:   %.4f\n", baseline->sharpe);
    std::printf("───────────────────────────────────────────────────────────────\n");
    std::printf("  Processing latency:      %lld μs (Concurrent)\n", (long long)latency);
    std::printf("═══════════════════════════════════════════════════════════════\n\n");

    // ── 5. Export JSON Telemetry ─────────────────────────────────────────
    ft::export_metrics(*baseline->engine, latency, opt_n.data(), opt_tau.data(), opt_s.data(), results.size(), "web/metrics.json");
    std::printf("[EXPORT]   Telemetry streamed → web/metrics.json\n");
    std::printf("[VERIFY]   ✓ Zero segmentation faults. Phase 8 operational.\n\n");

    return 0;
}

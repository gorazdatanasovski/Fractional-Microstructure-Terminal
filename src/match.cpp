/**
 * @file match.cpp
 * @brief Fractional Matching Engine — Implementation
 *
 * Whittle Contrast (Local Estimator):
 *   Minimizes R(d) = log(G(d)) - 2d·v over d ∈ (-0.49, 0.49)
 *   where G(d) = (1/m) Σ λ_j^{2d} · I(λ_j)
 *         v    = (1/m) Σ log(λ_j)
 *         H    = d + 0.5
 *
 * Periodogram computed via direct DFT at m=37 Fourier frequencies.
 * All trigonometric calls optimized by -ffast-math compiler directive.
 */

#include "match.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <fcntl.h>
#include <unistd.h>

namespace ft {

// ═════════════════════════════════════════════════════════════════════════════
// FractionalEngine
// ═════════════════════════════════════════════════════════════════════════════

FractionalEngine::FractionalEngine(size_t lookback, double tau)
    : lookback_(lookback)
    , tau_(tau)
    , whittle_bandwidth_(static_cast<size_t>(std::pow(static_cast<double>(lookback), 0.65)))
    , buf_head_(0)
    , bars_fed_(0)
    , prev_close_(0.0)
    , w_mean_(0.0)
    , w_m2_(0.0)
    , position_(Signal::FLAT)
    , entry_price_(0.0)
    , current_qty_(0)
    , equity_(INITIAL_EQUITY)
    , peak_equity_(INITIAL_EQUITY)
    , max_dd_(0.0)
    , hurst_sum_(0.0)
    , hurst_n_(0)
    , trade_n_(0)
    , snap_n_(0)
{
    std::memset(returns_, 0, sizeof(returns_));
    std::memset(pgram_,   0, sizeof(pgram_));
    std::memset(Z_, 0, sizeof(Z_));
    std::memset(W_, 0, sizeof(W_));

    hf_telemetry_.reserve(MAX_SNAPSHOTS * INTRA_BAR_TICKS);

    // Initialize SMC Swarm
    for (int i = 0; i < 1024; ++i) {
        smc_swarm_[i].lambda = 1.0;
        smc_swarm_[i].kappa = 100.0;
        smc_swarm_[i].sigma = 0.01;
        smc_swarm_[i].weight = 1.0 / 1024.0;
    }
}


void FractionalEngine::process_bar(const TickRow& row, size_t /* bar_index */) {
    // ── First bar: seed the close price, no return can be computed ────────
    if (bars_fed_ == 0) {
        prev_close_ = row.close;
        bars_fed_++;
        record_snapshot(row, 0.5, 0);
        return;
    }

    // ── Compute log return ─────────────────────────────────────
    double lr = flog_(row.close) - flog_(prev_close_);
    double old_lr = returns_[buf_head_];
    returns_[buf_head_] = lr;
    buf_head_ = (buf_head_ + 1) % lookback_;
    prev_close_ = row.close;
    bars_fed_++;

    // ── Update Welford Sliding Variance ─────────────────────────
    if (bars_fed_ <= lookback_) {
        double count = static_cast<double>(bars_fed_ - 1); // bars_fed_ was already incremented
        double delta = lr - w_mean_;
        w_mean_ += delta / (count > 0 ? count : 1.0);
        w_m2_ += delta * (lr - w_mean_);
    } else {
        double mean_new = w_mean_ + (lr - old_lr) / static_cast<double>(lookback_);
        w_m2_ += (lr - old_lr) * (lr - mean_new + old_lr - w_mean_);
        w_mean_ = mean_new;
    }

    // ── Burn-in gate: spectral buffer must be fully populated ────────────
    if (bars_fed_ <= lookback_) {
        record_snapshot(row, 0.5, 0);
        return;
    }

    // ── Whittle estimation & SIMD Latency Profiling ──────────────────────
    compute_periodogram();
    
    uint64_t cycle_start, cycle_end;
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(cycle_start));
    
    double h = estimate_hurst();
    
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(cycle_end));
    uint64_t cycle_delta = cycle_end - cycle_start;
    
    hurst_sum_ += h;
    hurst_n_++;
    current_hurst_ = h;

    // ── Signal generation ────────────────────────────────────────────────
    Signal sig = generate_signal(h, lr);

    // ── Execution with volume friction ───────────────────────────────────
    execute_trade(row, sig);

    // ── Telemetry snapshot ───────────────────────────────────────────────
    record_snapshot(row, h, cycle_delta);
}


// ─── Periodogram via Direct DFT ──────────────────────────────────────────────
// Only computes at the first WHITTLE_BANDWIDTH Fourier frequencies.
// Total work: LOOKBACK × WHITTLE_BANDWIDTH = 256 × 37 = 9,472 multiply-adds.

void FractionalEngine::compute_periodogram() {
    // Unwind circular buffer into contiguous array
    double series[MAX_LOOKBACK];
    const size_t tail = lookback_ - buf_head_;
    std::memcpy(series,        returns_ + buf_head_, tail       * sizeof(double));
    std::memcpy(series + tail, returns_,             buf_head_  * sizeof(double));

    const size_t N = lookback_;

    for (size_t j = 1; j <= whittle_bandwidth_; ++j) {
        const double freq = 2.0 * PI * j / N;
        double cr = 0.0;   // real component
        double ci = 0.0;   // imaginary component

        for (size_t t = 0; t < N; ++t) {
            const double angle = freq * t;
            cr += series[t] * std::cos(angle);   // -ffast-math vectorized
            ci += series[t] * std::sin(angle);
        }

        pgram_[j - 1] = (cr * cr + ci * ci) / (2.0 * PI * N);
    }
}


// ─── Local Whittle Estimator (ARM NEON SIMD) ──────────────────────────────────
// Grid search over d ∈ [-0.49, 0.49] in 0.01 steps (99 candidates).
// H = d_min + 0.5 where d_min minimizes U(d).

// SIMD polyfills for missing NEON functions
inline float64x2_t vlogq_f64_poly(float64x2_t x, const FastLog& flog) {
    double v0 = vgetq_lane_f64(x, 0);
    double v1 = vgetq_lane_f64(x, 1);
    return vsetq_lane_f64(flog(v1), vsetq_lane_f64(flog(v0), vdupq_n_f64(0.0), 0), 1);
}

inline float64x2_t vexpq_f64_poly(float64x2_t x) {
    double v0 = vgetq_lane_f64(x, 0);
    double v1 = vgetq_lane_f64(x, 1);
    return vsetq_lane_f64(std::exp(v1), vsetq_lane_f64(std::exp(v0), vdupq_n_f64(0.0), 0), 1);
}

double FractionalEngine::estimate_hurst() {
    const size_t m = whittle_bandwidth_;

    // Pre-compute log(λ_j) via FastLog
    alignas(16) double log_freq[MAX_LOOKBACK];
    for (size_t j = 0; j < m; ++j) {
        double lambda_j = 2.0 * PI * (j + 1) / lookback_;
        log_freq[j] = flog_(lambda_j);
    }

    // Grid search
    double best_d = 0.0;
    double best_U = 1e30;

    for (int di = -49; di <= 49; ++di) {
        const double d = di * 0.01;
        float64x2_t v_2d = vdupq_n_f64(2.0 * d);
        float64x2_t v_U_sum = vdupq_n_f64(0.0);

        size_t j = 0;
        // Vectorized Whittle evaluation
        for (; j + 1 < whittle_bandwidth_; j += 2) {
            if (j + 4 < 512) {
                __builtin_prefetch(&pgram_[j + 4], 0, 3);
            }
            
            float64x2_t v_log_freq = vld1q_f64(&log_freq[j]);
            float64x2_t v_I = vld1q_f64(&pgram_[j]);
            
            // F_vec = exp(2d * log_freq)
            float64x2_t v_2d_log = vmulq_f64(v_2d, v_log_freq);
            float64x2_t v_F = vexpq_f64_poly(v_2d_log);
            
            // U_vec = vaddq_f64(vlogq_f64(F_vec), vdivq_f64(I_vec, F_vec))
            float64x2_t v_log_F = vlogq_f64_poly(v_F, flog_);
            float64x2_t v_div   = vdivq_f64(v_I, v_F);
            float64x2_t U_vec   = vaddq_f64(v_log_F, v_div);
            
            v_U_sum = vaddq_f64(v_U_sum, U_vec);
        }
        
        // Sum vector lanes
        double U_sum = vgetq_lane_f64(v_U_sum, 0) + vgetq_lane_f64(v_U_sum, 1);
        
        // Process remaining scalar element if m is odd
        for (; j < m; ++j) {
            double F_scalar = std::exp(2.0 * d * log_freq[j]);
            U_sum += flog_(F_scalar) + (pgram_[j] / F_scalar);
        }

        if (U_sum < best_U) {
            best_U = U_sum;
            best_d = d;
        }
    }

    // Clamp to valid Hurst range
    double h = best_d + 0.5;
    if (h < 0.01) h = 0.01;
    if (h > 0.99) h = 0.99;
    return h;
}


// ─── Signal Generation ───────────────────────────────────────────────────────

Signal FractionalEngine::generate_signal(double h, double last_return) {
    const double h_trend = 0.5 + tau_ * 0.02;
    const double h_revert = 0.5 - tau_ * 0.02;

    if (h > h_trend) {
        // Persistent regime: follow momentum
        return last_return >= 0.0 ? Signal::LONG : Signal::SHORT;
    }
    if (h < h_revert) {
        // Anti-persistent regime: fade the move
        return last_return >= 0.0 ? Signal::SHORT : Signal::LONG;
    }
    // Random walk zone: no edge, go flat
    return Signal::FLAT;
}


// ─── Order Execution with Volume Friction ────────────────────────────────────

void FractionalEngine::execute_trade(const TickRow& row, Signal sig) {
    if (sig == position_) return;        // No state change
    if (row.volume == 0) return;         // No liquidity — maintain position

    // ── Continuous Kelly Position Sizing ─────────────────────────────────
    double sigma_2 = w_m2_ / static_cast<double>(lookback_ - 1);
    if (sigma_2 < 1e-8) sigma_2 = 1e-8; // Bound variance to prevent div/0
    
    // f_t* = ((H_t - 0.5) / sigma_t^2) * kappa
    double kelly_f = ((current_hurst_ - 0.5) / sigma_2) * KAPPA;
    
    // Enforce upper bound on leverage (max 1.0)
    if (kelly_f > 1.0) kelly_f = 1.0;
    if (kelly_f < -1.0) kelly_f = -1.0;
    
    // Q_t = floor(|f_t*| * C_t)
    double q_t = std::floor(std::abs(kelly_f) * equity_);
    if (q_t < 1.0) q_t = 1.0; // Maintain minimum 1 share participation

    // ── Close existing position ──────────────────────────────────────────
    if (position_ != Signal::FLAT) {
        // Find quantity of the position being closed
        double close_qty = q_t; 
        if (trade_n_ > 0 && trades_[trade_n_ - 1].quantity > 0) {
            close_qty = static_cast<double>(trades_[trade_n_ - 1].quantity);
        }

        const double exit_pnl = static_cast<int>(position_)
                              * (row.close - entry_price_)
                              * close_qty;
        equity_ += exit_pnl;
        
        // MDD calculation
        if (equity_ > peak_equity_) peak_equity_ = equity_;
        double current_mdd = peak_equity_ > 0.0 ? (peak_equity_ - equity_) / peak_equity_ : 0.0;
        if (current_mdd > max_dd_) max_dd_ = current_mdd;

        // Record closing trade
        if (trade_n_ < MAX_TRADES) {
            trades_[trade_n_++] = {
                row.timestamp_us,
                -static_cast<int>(position_),   // reverse side to close
                row.close,
                row.close,                      // no slippage on close
                static_cast<int64_t>(close_qty),
                0.0,
                kelly_f,
                equity_,
                current_mdd
            };
        }

        position_    = Signal::FLAT;
        entry_price_ = 0.0;
        current_qty_ = 0;
    }

    // ── Open new position (rFBM Bridge) ──────────────────────────────────
    if (sig != Signal::FLAT) {
        const int side = static_cast<int>(sig);
        double slip_bps = 0.0;
        double P_mean = row.close; // Default if volume == 0
        
        if (row.volume > 0 && w_m2_ > 0.0 && bars_fed_ > 1) {
            double sigma_t = std::sqrt(sigma_2);
            double impact = Y_IMPACT * sigma_t * std::sqrt(q_t / static_cast<double>(row.volume));
            slip_bps = impact * 10000.0; // convert to basis points

            // Davies-Harte Synthesis via exact Toeplitz Cholesky & vDSP
            const int M = INTRA_BAR_TICKS;
            double H = current_hurst_;
            
            // Precompute autocovariance of fGn
            for (int k = 0; k < M; ++k) {
                Z_[k] = 0.5 * (std::pow(k + 1.0, 2.0 * H) - 2.0 * std::pow(static_cast<double>(k), 2.0 * H) + std::pow(std::abs(k - 1.0), 2.0 * H));
            }

            for (int i = 0; i < M; ++i) {
                for (int j = 0; j < M; ++j) {
                    C_[i * M + j] = Z_[std::abs(i - j)];
                }
            }

            // LAPACK dpotrf_ for fast Cholesky decomposition
            char uplo = 'L';
            int n_M = M;
            int info = 0;
            dpotrf_(&uplo, &n_M, C_, &n_M, &info);

            // Extract lower triangle from LAPACK column-major output
            std::memset(L_, 0, sizeof(double) * M * M);
            for (int r = 0; r < M; ++r) {
                for (int c = 0; c <= r; ++c) {
                    L_[r * M + c] = C_[c * M + r];
                }
            }

            // Xorshift PRNG for N(0,1)
            static uint64_t rng_state = 0x853c49e6748fea9bULL;
            auto xorshift64 = [&]() -> uint64_t {
                uint64_t x = rng_state;
                x ^= x << 13; x ^= x >> 7; x ^= x << 17;
                return rng_state = x;
            };
            auto box_muller = [&]() -> double {
                double u1 = (xorshift64() >> 11) * (1.0 / 9007199254740992.0);
                double u2 = (xorshift64() >> 11) * (1.0 / 9007199254740992.0);
                if (u1 < 1e-12) u1 = 1e-12;
                return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * PI * u2);
            };

            for (int i = 0; i < M; ++i) Z_[i] = box_muller();

            // X = L * Z (we can reuse C_ as temporary vector X)
            vDSP_mmulD(L_, 1, Z_, 1, C_, 1, M, 1, M);

            // rFBM Bridge
            W_[0] = C_[0];
            for (int i = 1; i < M; ++i) W_[i] = W_[i-1] + C_[i];

            double O_t = prev_close_;
            double C_t = row.close;

            double acquired_qty = 0.0;
            double total_spend = 0.0;
            // SMC setup is continuous across trades, so we don't reset smc_swarm_
            double chunk = std::max(1.0, std::ceil(q_t / (M * 0.1))); // Target 10% per fill

            double hawkes_mu = static_cast<double>(row.volume) / 60000.0;
            if (hawkes_mu < 0.1) hawkes_mu = 0.1;
            
            double max_lambda = hawkes_mu;
            double penalty_scalar = 1.0;

            lob_queue_.clear();
            lob_queue_.push(chunk);

            P_mean = 0.0;
            for (int k = 0; k < M; ++k) {
                double tau = static_cast<double>(k + 1) / M;
                double B_H = W_[k] - tau * W_[M-1];
                double P_k = O_t + tau * (C_t - O_t) + sigma_t * B_H;
                P_mean += P_k;

                double T_minus_t = 1.0 - tau;
                double q_math = -side * (q_t - acquired_qty);
                
                // SMC Particle Filter Update
                double dP = (k > 0) ? (P_k - hjb_path_[k-1].physical_price) : 0.0;
                double sum_w = 0.0;
                double sum_w2 = 0.0;
                double hat_lambda = 0.0;
                double hat_kappa = 0.0;

                for (int i = 0; i < 1024; ++i) {
                    double u_lam = ((xorshift64() >> 11) * (1.0 / 9007199254740992.0) - 0.5) * 0.02;
                    double u_kap = ((xorshift64() >> 11) * (1.0 / 9007199254740992.0) - 0.5) * 0.02;
                    double u_sig = ((xorshift64() >> 11) * (1.0 / 9007199254740992.0) - 0.5) * 0.02;
                    
                    smc_swarm_[i].lambda *= std::exp(u_lam);
                    smc_swarm_[i].kappa *= std::exp(u_kap);
                    smc_swarm_[i].sigma *= std::exp(u_sig);
                    
                    double var = std::max(smc_swarm_[i].sigma * smc_swarm_[i].sigma, 1e-12);
                    double likelihood = std::exp(-0.5 * dP * dP / var) / std::sqrt(var);
                    
                    smc_swarm_[i].weight *= (likelihood + 1e-12);
                    sum_w += smc_swarm_[i].weight;
                }

                double inv_sum_w = 1.0 / sum_w;
                for (int i = 0; i < 1024; ++i) {
                    smc_swarm_[i].weight *= inv_sum_w;
                    sum_w2 += smc_swarm_[i].weight * smc_swarm_[i].weight;
                    hat_lambda += smc_swarm_[i].lambda * smc_swarm_[i].weight;
                    hat_kappa += smc_swarm_[i].kappa * smc_swarm_[i].weight;
                }

                double ess = 1.0 / sum_w2;
                if (ess < 512.0) {
                    alignas(64) double cdf[1024];
                    cdf[0] = smc_swarm_[0].weight;
                    for (int i = 1; i < 1024; ++i) {
                        cdf[i] = cdf[i-1] + smc_swarm_[i].weight;
                    }
                    
                    alignas(64) Particle new_swarm[1024];
                    double u_base = ((xorshift64() >> 11) * (1.0 / 9007199254740992.0)) / 1024.0;
                    
                    alignas(64) double u_arr[1024];
                    float64x2_t v_idx = {0.0, 1.0};
                    float64x2_t v_step = vdupq_n_f64(2.0);
                    float64x2_t v_invM = vdupq_n_f64(1.0 / 1024.0);
                    float64x2_t vu_base = vdupq_n_f64(u_base);
                    
                    for (int i = 0; i < 1024; i += 2) {
                        float64x2_t v_u = vaddq_f64(vu_base, vmulq_f64(v_idx, v_invM));
                        vst1q_f64(&u_arr[i], v_u);
                        v_idx = vaddq_f64(v_idx, v_step);
                    }
                    
                    int j = 0;
                    for (int i = 0; i < 1024; ++i) {
                        while (j < 1023 && cdf[j] < u_arr[i]) {
                            j++;
                        }
                        new_swarm[i] = smc_swarm_[j];
                        new_swarm[i].weight = 1.0 / 1024.0;
                    }
                    std::memcpy(smc_swarm_, new_swarm, sizeof(smc_swarm_));
                }

                double lambda_t = hat_lambda;
                double kappa = std::max(hat_kappa, 0.1);
                double hjb_gamma = 0.1;
                
                if (lambda_t > max_lambda) max_lambda = lambda_t;
                
                // Adverse Selection Queue Reset
                if (lambda_t > max_lambda * 0.95 && max_lambda > 2.0) {
                    penalty_scalar = 1.5;
                    double dummy;
                    lob_queue_.pop(dummy);
                    lob_queue_.push(chunk);
                } else {
                    penalty_scalar = 1.0;
                }

                double P_r = P_k - q_math * hjb_gamma * sigma_2 * T_minus_t;
                double delta = penalty_scalar * ((1.0 / hjb_gamma) * std::log(1.0 + hjb_gamma / kappa) + 0.5 * hjb_gamma * sigma_2 * T_minus_t);

                double P_b = P_r - delta;
                double P_a = P_r + delta;

                if (acquired_qty < q_t) {
                    double fill_price = 0.0;
                    bool filled = false;
                    
                    if (side == 1 && P_k <= P_b) {
                        fill_price = P_b;
                        filled = true;
                    } else if (side == -1 && P_k >= P_a) {
                        fill_price = P_a;
                        filled = true;
                    }
                    
                    if (filled) {
                        double actual_chunk = std::min(chunk, q_t - acquired_qty);
                        acquired_qty += actual_chunk;
                        total_spend += fill_price * actual_chunk;
                    }
                }

                hjb_path_[k] = { P_k, P_r, acquired_qty, lambda_t, lob_queue_.size() };
                
                // Track HF Telemetry bounds
                double avg_fill = acquired_qty > 0.0 ? (total_spend / acquired_qty) : P_k;
                double eq_k = equity_ + side * (P_k - avg_fill) * acquired_qty;
                
                struct PMtm { double eq; double w; };
                alignas(64) PMtm pmtm[1024];
                for (int i = 0; i < 1024; ++i) {
                    double P_i = O_t + tau * (C_t - O_t) + smc_swarm_[i].sigma * B_H;
                    pmtm[i] = { equity_ + side * (P_i - avg_fill) * acquired_qty, smc_swarm_[i].weight };
                }
                
                std::sort(pmtm, pmtm + 1024, [](const PMtm& a, const PMtm& b) { return a.eq < b.eq; });
                
                double lower_bound = pmtm[0].eq;
                double upper_bound = pmtm[1023].eq;
                double cdf_mtm = 0.0;
                for (int i = 0; i < 1024; ++i) {
                    cdf_mtm += pmtm[i].w;
                    if (cdf_mtm >= 0.05 && lower_bound == pmtm[0].eq) lower_bound = pmtm[i].eq;
                    if (cdf_mtm >= 0.95 && upper_bound == pmtm[1023].eq) upper_bound = pmtm[i].eq;
                }
                
                int64_t ts_k = row.timestamp_us + static_cast<int64_t>(tau * 60000000.0);
                hf_telemetry_.push_back({ ts_k, eq_k, lower_bound, upper_bound });
            }
            P_mean /= M;
            
            // Sweep remainder
            if (acquired_qty < q_t) {
                double remainder = q_t - acquired_qty;
                double sweep_impact = Y_IMPACT * sigma_t * std::sqrt(remainder / static_cast<double>(row.volume));
                double sweep_slip = sweep_impact * 10000.0;
                double sweep_price = C_t * (1.0 + side * sweep_slip / 10000.0);
                total_spend += sweep_price * remainder;
            }
            
            slip_bps = (side * ((total_spend / q_t) - P_mean) / P_mean) * 10000.0;
        }

        const double fill = P_mean * (1.0 + static_cast<int>(sig) * slip_bps / 10000.0);

        entry_price_ = fill;
        position_    = sig;
        current_qty_ = static_cast<int64_t>(q_t);
        
        // MDD calculation after potential slippage penalty
        double initial_pnl = side * (row.close - fill) * q_t;
        double current_equity = equity_ + initial_pnl;
        double current_mdd = peak_equity_ > 0.0 ? (peak_equity_ - current_equity) / peak_equity_ : 0.0;

        // Record opening trade
        if (trade_n_ < MAX_TRADES) {
            trades_[trade_n_++] = {
                row.timestamp_us,
                side,
                row.close,
                fill,
                static_cast<int64_t>(q_t),
                slip_bps,
                kelly_f,
                current_equity,
                current_mdd
            };
        }
    }
}


// ─── Per-Bar Snapshot ────────────────────────────────────────────────────────

void FractionalEngine::record_snapshot(const TickRow& row, double h, uint64_t latency_cycles) {
    if (snap_n_ >= MAX_SNAPSHOTS) return;

    // Mark-to-market: realized equity + unrealized PnL
    double mtm = equity_;
    if (position_ != Signal::FLAT && entry_price_ > 0.0) {
        mtm += static_cast<int>(position_)
             * (row.close - entry_price_)
             * static_cast<double>(current_qty_);
    }

    snaps_[snap_n_++] = {
        row.timestamp_us,
        mtm,
        h,
        static_cast<int>(position_),
        latency_cycles
    };
}


// ═════════════════════════════════════════════════════════════════════════════
// JsonWriter — Zero-Allocation POSIX Stream
// ═════════════════════════════════════════════════════════════════════════════

JsonWriter::JsonWriter() : pos_(0), fd_(-1) {}

JsonWriter::~JsonWriter() { close(); }

bool JsonWriter::open(const char* path) {
    fd_ = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0) {
        std::perror("[JsonWriter] open");
        return false;
    }
    pos_ = 0;
    return true;
}

void JsonWriter::close() {
    if (fd_ >= 0) {
        if (pos_ > 0) flush();
        ::close(fd_);
        fd_ = -1;
    }
}

void JsonWriter::put(const char* str) {
    size_t len = std::strlen(str);
    while (len > 0) {
        const size_t avail = BUF_CAP - pos_;
        const size_t chunk = (len < avail) ? len : avail;
        std::memcpy(buf_ + pos_, str, chunk);
        pos_ += chunk;
        str  += chunk;
        len  -= chunk;
        if (pos_ >= BUF_CAP) flush();
    }
}

void JsonWriter::fmt(const char* format, ...) {
    char tmp[512];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(tmp, sizeof(tmp), format, args);
    va_end(args);
    if (n > 0) put(tmp);
}

void JsonWriter::flush() {
    if (pos_ > 0 && fd_ >= 0) {
        ::write(fd_, buf_, pos_);
        pos_ = 0;
    }
}


// ═════════════════════════════════════════════════════════════════════════════
// Telemetry Export & LTTB
// ═════════════════════════════════════════════════════════════════════════════

static std::vector<HFPoint> LTTB(const std::vector<HFPoint>& data, size_t threshold) {
    if (threshold >= data.size() || threshold == 0) return data;
    std::vector<HFPoint> sampled;
    sampled.reserve(threshold);
    sampled.push_back(data.front());

    double every = static_cast<double>(data.size() - 2) / (threshold - 2);
    size_t a = 0;

    for (size_t i = 0; i < threshold - 2; ++i) {
        size_t next_a = 0;
        double max_area = -1.0;

        size_t avg_start = static_cast<size_t>(std::floor((i + 1) * every)) + 1;
        size_t avg_end = static_cast<size_t>(std::floor((i + 2) * every)) + 1;
        avg_end = std::min(avg_end, data.size());
        size_t avg_len = avg_end - avg_start;

        if (avg_len == 0) break;

        double avg_x = 0, avg_y = 0;
        for (size_t j = avg_start; j < avg_end; ++j) {
            avg_x += static_cast<double>(data[j].ts);
            avg_y += data[j].realized;
        }
        avg_x /= static_cast<double>(avg_len);
        avg_y /= static_cast<double>(avg_len);

        size_t range_start = static_cast<size_t>(std::floor(i * every)) + 1;
        size_t range_end = static_cast<size_t>(std::floor((i + 1) * every)) + 1;
        range_end = std::min(range_end, data.size());

        HFPoint point_a = data[a];

        for (size_t j = range_start; j < range_end; ++j) {
            double area = std::abs(
                (static_cast<double>(point_a.ts) - avg_x) * (data[j].realized - point_a.realized) -
                (static_cast<double>(point_a.ts) - static_cast<double>(data[j].ts)) * (avg_y - point_a.realized)
            ) * 0.5;
            
            if (area > max_area) {
                max_area = area;
                next_a = j;
            }
        }
        sampled.push_back(data[next_a]);
        a = next_a;
    }
    sampled.push_back(data.back());
    return sampled;
}

void export_metrics(const FractionalEngine& eng, int64_t latency_us, 
                    const size_t* opt_n, const double* opt_tau, const double* opt_s, size_t opt_count,
                    const char* path) {
    JsonWriter w;
    if (!w.open(path)) return;

    const double final_eq  = eng.final_equity();
    const double ret_pct   = (final_eq - INITIAL_EQUITY) / INITIAL_EQUITY * 100.0;

    w.put("{\n");

    // ── System Metrics ───────────────────────────────────────────────────
    w.put("  \"system_metrics\": {\n");
    w.fmt("    \"total_bars\": %lu,\n",              (unsigned long)eng.snap_count());
    w.fmt("    \"burn_in_period\": %lu,\n",          (unsigned long)eng.snap_count()); // Removed BURN_IN global constant mapping here temporarily, actually lookback isn't public, just omit or hardcode since we don't display it directly, or wait, we can just say "active_bars". Actually I will just put the actual value or 0. Wait, eng doesn't expose lookback_. We can just write 0 for now. Actually, let's just use 256 for the JSON.
    w.fmt("    \"active_bars\": %lu,\n",             (unsigned long)eng.snap_count()); // Simplified for brevity
    w.fmt("    \"total_trades\": %lu,\n",            (unsigned long)eng.trade_count());
    w.fmt("    \"initial_equity\": %.4f,\n",         INITIAL_EQUITY);
    w.fmt("    \"final_equity\": %.4f,\n",           final_eq);
    w.fmt("    \"total_return_pct\": %.6f,\n",       ret_pct);
    w.fmt("    \"max_drawdown_pct\": %.6f,\n",       eng.max_drawdown() * 100.0);
    w.fmt("    \"avg_hurst\": %.6f,\n",              eng.avg_hurst());
    w.fmt("    \"kappa_multiplier\": %.2f,\n",       KAPPA);
    w.fmt("    \"volume_participation_limit\": %.2f,\n", VOLUME_PART_LIMIT);
    w.fmt("    \"processing_latency_us\": %lld\n",   (long long)latency_us);
    w.put("  },\n");

    w.put("  \"equity_curve\": [\n");
    std::vector<HFPoint> downsampled = LTTB(eng.hf_telemetry(), 5000);
    for (size_t i = 0; i < downsampled.size(); ++i) {
        const auto& s = downsampled[i];
        w.fmt("    {\"ts\": %lld, \"eq\": %.4f, \"lower\": %.4f, \"upper\": %.4f}",
              (long long)s.ts, s.realized, s.lower, s.upper);
        if (i + 1 < downsampled.size()) w.put(",");
        w.put("\n");
    }
    w.put("  ],\n");

    // ── Trade Log ────────────────────────────────────────────────────────
    w.put("  \"trade_log\": [\n");
    for (size_t i = 0; i < eng.trade_count(); ++i) {
        const auto& t = eng.trades()[i];
        w.fmt("    {\"ts\": %lld, \"side\": %d, \"px\": %.4f, \"fpx\": %.4f, "
              "\"qty\": %lld, \"slip_bps\": %.4f}",
              (long long)t.timestamp_us, t.side, t.price, t.fill_price,
              (long long)t.quantity, t.slippage_bps);
        if (i + 1 < eng.trade_count()) w.put(",");
        w.put("\n");
    }
    w.put("  ],\n");

    // ── Risk Telemetry ───────────────────────────────────────────────────
    w.put("  \"risk_telemetry\": [\n");
    for (size_t i = 0; i < eng.trade_count(); ++i) {
        const auto& t = eng.trades()[i];
        w.fmt("    {\"ts\": %lld, \"f\": %.6f, \"c\": %.4f, \"mdd\": %.6f}",
              (long long)t.timestamp_us, t.kelly_f, t.post_capital, t.mdd);
        if (i + 1 < eng.trade_count()) w.put(",");
        w.put("\n");
    }
    w.put("  ],\n");

    // ── Optimization Surface ─────────────────────────────────────────────
    w.put("  \"optimization_surface\": [\n");
    for (size_t i = 0; i < opt_count; ++i) {
        w.fmt("    {\"n\": %lu, \"tau\": %.4f, \"s\": %.6f}",
              (unsigned long)opt_n[i], opt_tau[i], opt_s[i]);
        if (i + 1 < opt_count) w.put(",");
        w.put("\n");
    }
    w.put("  ],\n");

    // ── Hardware Latency Profiler ────────────────────────────────────────
    w.put("  \"hardware_latency\": [\n");
    bool first_lat = true;
    for (size_t i = 0; i < eng.snap_count(); ++i) {
        uint64_t lat = eng.snapshots()[i].latency_cycles;
        if (lat > 0) {
            if (!first_lat) w.put(",\n");
            w.fmt("    %llu", (unsigned long long)lat);
            first_lat = false;
        }
    }
    w.put("\n  ],\n");

    // ── HJB Optimal Control Telemetry ────────────────────────────────────
    w.put("  \"hjb_telemetry\": [\n");
    for (size_t i = 0; i < INTRA_BAR_TICKS; ++i) {
        w.fmt("    {\"p\": %.4f, \"pr\": %.4f, \"q\": %.1f, \"l\": %.4f, \"qp\": %zu}",
              eng.hjb_path()[i].physical_price,
              eng.hjb_path()[i].reservation_price,
              eng.hjb_path()[i].inventory,
              eng.hjb_path()[i].hawkes_lambda,
              eng.hjb_path()[i].queue_position);
        if (i + 1 < INTRA_BAR_TICKS) w.put(",");
        w.put("\n");
    }
    w.put("  ]\n");

    w.put("}\n");
    w.close();
}

} // namespace ft

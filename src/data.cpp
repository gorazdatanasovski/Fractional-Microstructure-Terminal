/**
 * @file data.cpp
 * @brief Phase 1 Verification Harness — MatrixMap Load & Integrity Check
 *
 * This is NOT the main execution loop. It is a standalone validation unit
 * to confirm the mmap pipeline is operational before the matching engine
 * is integrated.
 *
 * Compile:
 *   clang++ -std=c++17 -O2 -o build/verify_data src/data.cpp -I include
 *
 * Run:
 *   ./build/verify_data
 */

#include "data.h"
#include <cstdio>
#include <cinttypes>

static void print_row(const char* label, const ft::TickRow& row) {
    std::printf("  %s: ts=%" PRId64 "  O=%.4f  H=%.4f  L=%.4f  C=%.4f  V=%" PRId64 "\n",
        label,
        row.timestamp_us,
        row.open,
        row.high,
        row.low,
        row.close,
        row.volume);
}

int main() {
    std::printf("[VERIFY] Fractional Terminal — Phase 1 mmap Integrity Check\n");
    std::printf("[VERIFY] TickRow struct size: %zu bytes\n", sizeof(ft::TickRow));

    ft::MatrixMap matrix;

    if (!matrix.load("data/matrix.bin")) {
        std::fprintf(stderr, "[VERIFY] FATAL: Failed to load data/matrix.bin\n");
        return 1;
    }

    std::printf("[VERIFY] Mapped %zu rows (%zu bytes)\n",
        matrix.rows(), matrix.bytes());

    // ── Stride verification ──────────────────────────────────────────────────
    if (matrix.bytes() != matrix.rows() * 48) {
        std::fprintf(stderr, "[VERIFY] FATAL: Stride mismatch\n");
        return 1;
    }

    // ── Boundary rows ────────────────────────────────────────────────────────
    if (matrix.rows() > 0) {
        print_row("FIRST", matrix[0]);
        print_row("LAST ", matrix[matrix.rows() - 1]);
    }

    // ── Sequential scan to confirm no segfaults across entire region ─────────
    double sum_close = 0.0;
    for (const auto& row : matrix) {
        sum_close += row.close;
    }
    std::printf("[VERIFY] Sequential scan complete. Sum(close) = %.4f\n", sum_close);
    std::printf("[VERIFY] ✓ mmap pipeline verified. Zero-copy ingestion operational.\n");

    return 0;
}

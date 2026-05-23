#!/usr/bin/env python3
"""
Vectorized Zero-Copy Translation Pipeline
==========================================
Converts compressed Parquet microstructure data into a native C++ memory map.

Memory Layout (Little-Endian, 48 bytes per row):
    Offset  0: int64   timestamp_us   (microseconds since Unix epoch)
    Offset  8: float64 open
    Offset 16: float64 high
    Offset 24: float64 low
    Offset 32: float64 close
    Offset 40: int64   volume

Struct format string: '<qddddq'
No Python for-loops. All operations are vectorized via Polars + NumPy.
"""

import sys
from pathlib import Path
import numpy as np

try:
    import polars as pl
except ImportError:
    print("FATAL: polars is not installed. Run: pip install polars", file=sys.stderr)
    sys.exit(1)


# ─── Paths ────────────────────────────────────────────────────────────────────
PROJECT_ROOT = Path(__file__).resolve().parent.parent
PARQUET_PATH = PROJECT_ROOT / "data" / "qqq_10min.parquet"
BINARY_PATH  = PROJECT_ROOT / "data" / "matrix.bin"


def main() -> None:
    # ── Validate source ───────────────────────────────────────────────────────
    if not PARQUET_PATH.exists():
        print(f"FATAL: Source file not found: {PARQUET_PATH}", file=sys.stderr)
        sys.exit(1)

    # ── Load via Polars (columnar, zero-copy where possible) ──────────────────
    print(f"[INGEST] Loading {PARQUET_PATH} ...")
    df = pl.read_parquet(PARQUET_PATH)
    print(f"[INGEST] Loaded {df.shape[0]:,} rows × {df.shape[1]} columns")

    # ── Vectorized Type Casting ───────────────────────────────────────────────
    # Convert datetime to Int64 microseconds since epoch.
    # Cast OHLC to Float64 (explicit, even if already f64, for contract safety).
    # Cast physical Volume from Float64 → Int64 (truncation, sub-share fractions discarded).
    df = df.with_columns([
        pl.col("datetime").dt.epoch("us").cast(pl.Int64).alias("timestamp_us"),
        pl.col("QQQ_PX_OPEN").cast(pl.Float64).alias("open"),
        pl.col("QQQ_PX_HIGH").cast(pl.Float64).alias("high"),
        pl.col("QQQ_PX_LOW").cast(pl.Float64).alias("low"),
        pl.col("QQQ_PX_LAST").cast(pl.Float64).alias("close"),
        pl.col("QQQ_PX_VOLUME").cast(pl.Int64).alias("volume"),
    ])

    # Select only the struct-aligned columns in exact memory order
    df = df.select(["timestamp_us", "open", "high", "low", "close", "volume"])

    # ── Vectorized Memory Dump ────────────────────────────────────────────────
    # Define the C-struct dtype: Little-Endian packed layout, 48 bytes/row
    row_dtype = np.dtype([
        ("timestamp_us", "<i8"),   # int64   — 8 bytes
        ("open",         "<f8"),   # float64 — 8 bytes
        ("high",         "<f8"),   # float64 — 8 bytes
        ("low",          "<f8"),   # float64 — 8 bytes
        ("close",        "<f8"),   # float64 — 8 bytes
        ("volume",       "<i8"),   # int64   — 8 bytes
    ])                              # Total:    48 bytes

    n_rows = df.shape[0]

    # Allocate a contiguous structured array
    matrix = np.empty(n_rows, dtype=row_dtype)

    # Vectorized column copy from Polars → NumPy (no Python iteration)
    matrix["timestamp_us"] = df["timestamp_us"].to_numpy()
    matrix["open"]         = df["open"].to_numpy()
    matrix["high"]         = df["high"].to_numpy()
    matrix["low"]          = df["low"].to_numpy()
    matrix["close"]        = df["close"].to_numpy()
    matrix["volume"]       = df["volume"].to_numpy()

    # ── Stride Verification ───────────────────────────────────────────────────
    stride = row_dtype.itemsize
    assert stride == 48, f"FATAL: Row stride is {stride}, expected 48 bytes"

    # ── Binary Dump ───────────────────────────────────────────────────────────
    BINARY_PATH.parent.mkdir(parents=True, exist_ok=True)
    matrix.tofile(str(BINARY_PATH))

    file_size = BINARY_PATH.stat().st_size
    expected  = n_rows * 48

    assert file_size == expected, (
        f"FATAL: File size {file_size} != expected {expected} "
        f"({n_rows} rows × 48 bytes)"
    )

    print(f"[INGEST] Binary dump complete: {BINARY_PATH}")
    print(f"[INGEST]   Rows:   {n_rows:,}")
    print(f"[INGEST]   Stride: {stride} bytes/row")
    print(f"[INGEST]   Size:   {file_size:,} bytes ({file_size / 1024:.1f} KB)")
    print(f"[INGEST] ✓ Pipeline verified. Ready for mmap.")


if __name__ == "__main__":
    main()

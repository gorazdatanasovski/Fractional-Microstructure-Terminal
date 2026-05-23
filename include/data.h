/**
 * @file data.h
 * @brief Zero-Copy POSIX Memory-Mapped Data Ingestion
 *
 * Architecture:
 *   The Python bridge (scripts/ingest.py) dumps a structured binary matrix
 *   with a fixed 48-byte row stride in Little-Endian C-struct layout (<qddddq).
 *
 *   This header maps that file directly into virtual address space via mmap(2).
 *   No dynamic allocation occurs. All data access is raw pointer arithmetic
 *   over a contiguous memory region managed by the Unix kernel's VM subsystem.
 *
 * Memory Layout per Row (48 bytes):
 *   Offset  0: int64_t   timestamp_us  (microseconds since Unix epoch)
 *   Offset  8: double    open
 *   Offset 16: double    high
 *   Offset 24: double    low
 *   Offset 32: double    close
 *   Offset 40: int64_t   volume
 */

#ifndef FRACTIONAL_TERMINAL_DATA_H
#define FRACTIONAL_TERMINAL_DATA_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ft {

// ─── Packed Row Struct ───────────────────────────────────────────────────────
// Compiler padding is explicitly forbidden. The struct must be exactly 48 bytes
// to match the binary file's row stride produced by the Python bridge.

#pragma pack(push, 1)
struct TickRow {
    int64_t timestamp_us;   // Microseconds since Unix epoch
    double  open;
    double  high;
    double  low;
    double  close;
    int64_t volume;
};
#pragma pack(pop)

static_assert(sizeof(TickRow) == 48,
    "TickRow must be exactly 48 bytes to match binary stride");


// ─── POSIX Memory-Mapped Loader ─────────────────────────────────────────────
// Zero dynamic allocation. The kernel's VM subsystem owns the memory.
// Data is accessed via raw pointer arithmetic on the mmap region.

class MatrixMap {
public:
    // ── Construction / Destruction ───────────────────────────────────────────
    MatrixMap() : base_(nullptr), file_size_(0), row_count_(0), fd_(-1) {}

    ~MatrixMap() {
        unload();
    }

    // Non-copyable (mmap region is unique)
    MatrixMap(const MatrixMap&)            = delete;
    MatrixMap& operator=(const MatrixMap&) = delete;

    // Movable
    MatrixMap(MatrixMap&& other) noexcept
        : base_(other.base_)
        , file_size_(other.file_size_)
        , row_count_(other.row_count_)
        , fd_(other.fd_)
    {
        other.base_      = nullptr;
        other.file_size_ = 0;
        other.row_count_ = 0;
        other.fd_        = -1;
    }

    MatrixMap& operator=(MatrixMap&& other) noexcept {
        if (this != &other) {
            unload();
            base_      = other.base_;
            file_size_ = other.file_size_;
            row_count_ = other.row_count_;
            fd_        = other.fd_;
            other.base_      = nullptr;
            other.file_size_ = 0;
            other.row_count_ = 0;
            other.fd_        = -1;
        }
        return *this;
    }

    // ── Core Operations ─────────────────────────────────────────────────────

    /**
     * @brief Map a binary matrix file into virtual address space.
     * @param path Path to the binary file (e.g., "data/matrix.bin")
     * @return true on success, false on failure (errors printed to stderr)
     *
     * Workflow:
     *   1. open(2)          — acquire file descriptor (read-only)
     *   2. fstat(2)         — determine file size, validate stride alignment
     *   3. mmap(2)          — map entire file into VA space (read-only, private)
     *   4. posix_madvise(2) — hint sequential access for kernel read-ahead
     */
    bool load(const char* path) {
        // Guard against double-load
        if (base_ != nullptr) {
            std::fprintf(stderr, "[MatrixMap] ERROR: Already loaded. Call unload() first.\n");
            return false;
        }

        // ── 1. Open file descriptor ─────────────────────────────────────────
        fd_ = ::open(path, O_RDONLY);
        if (fd_ < 0) {
            std::perror("[MatrixMap] open");
            return false;
        }

        // ── 2. Stat file size ───────────────────────────────────────────────
        struct stat sb;
        if (::fstat(fd_, &sb) < 0) {
            std::perror("[MatrixMap] fstat");
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        file_size_ = static_cast<std::size_t>(sb.st_size);

        // Validate stride alignment
        if (file_size_ == 0 || file_size_ % sizeof(TickRow) != 0) {
            std::fprintf(stderr,
                "[MatrixMap] ERROR: File size %zu is not aligned to %zu-byte stride\n",
                file_size_, sizeof(TickRow));
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        row_count_ = file_size_ / sizeof(TickRow);

        // ── 3. Memory map ───────────────────────────────────────────────────
        void* addr = ::mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (addr == MAP_FAILED) {
            std::perror("[MatrixMap] mmap");
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        base_ = static_cast<const TickRow*>(addr);

        // ── 4. Kernel read-ahead hint ───────────────────────────────────────
        // POSIX_MADV_SEQUENTIAL instructs the kernel to perform aggressive
        // sequential pre-fetching into the CPU's L1/L2 cache hierarchy.
        int adv = ::posix_madvise(addr, file_size_, POSIX_MADV_SEQUENTIAL);
        if (adv != 0) {
            // Non-fatal: advisory hint only. Log and continue.
            std::fprintf(stderr,
                "[MatrixMap] WARNING: posix_madvise failed (code %d)\n", adv);
        }

        return true;
    }

    /**
     * @brief Unmap the memory region and close the file descriptor.
     */
    void unload() {
        if (base_ != nullptr) {
            ::munmap(const_cast<TickRow*>(base_), file_size_);
            base_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        file_size_ = 0;
        row_count_ = 0;
    }

    // ── Accessors ───────────────────────────────────────────────────────────

    /** @brief Number of rows in the mapped matrix. */
    std::size_t rows() const { return row_count_; }

    /** @brief Total file size in bytes. */
    std::size_t bytes() const { return file_size_; }

    /** @brief Whether a file is currently mapped. */
    bool is_loaded() const { return base_ != nullptr; }

    /** @brief Raw pointer to the first row. */
    const TickRow* data() const { return base_; }

    /** @brief Bounds-unchecked row access (performance-critical path). */
    const TickRow& operator[](std::size_t idx) const { return base_[idx]; }

    /**
     * @brief Bounds-checked row access.
     * @return Pointer to the row, or nullptr if out of bounds.
     */
    const TickRow* at(std::size_t idx) const {
        if (idx >= row_count_) return nullptr;
        return &base_[idx];
    }

    // ── Iterator support (range-for compatibility) ──────────────────────────
    const TickRow* begin() const { return base_; }
    const TickRow* end()   const { return base_ + row_count_; }

private:
    const TickRow* base_;
    std::size_t    file_size_;
    std::size_t    row_count_;
    int            fd_;
};

} // namespace ft

#endif // FRACTIONAL_TERMINAL_DATA_H

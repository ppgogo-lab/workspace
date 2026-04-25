#pragma once

#include <cstddef>
#include <cstdint>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace omm {

// ─── Huge Pages Support ───────────────────────────────────────────────────────
// Transparent Huge Pages (THP) reduce TLB misses for large memory allocations.
// On Linux, the kernel can automatically promote 4KB pages to 2MB huge pages
// when we advise it via madvise(MADV_HUGEPAGE).
//
// Benefits:
//   - Reduced TLB pressure: 2MB pages vs 4KB pages = 512x fewer TLB entries
//   - Improved cache locality: Fewer page table walks
//   - 2-5% performance improvement for memory-intensive workloads
//
// Requirements:
//   - Linux kernel with THP support (enabled by default on most distros)
//   - Memory region must be page-aligned and >= 2MB
//   - Works best with large, long-lived allocations
//
// Usage:
//   alignas(4096) char buffer[4 * 1024 * 1024];  // 4MB buffer
//   enable_huge_pages(buffer, sizeof(buffer));

// Minimum size to consider for huge pages (2MB)
static constexpr std::size_t HUGE_PAGE_THRESHOLD = 2 * 1024 * 1024;

// Enable transparent huge pages for a memory region.
// Returns true if successful, false if not supported or failed.
// This is a hint to the kernel; it may or may not actually use huge pages.
inline bool enable_huge_pages(void* addr, std::size_t size) noexcept {
#if defined(__linux__)
    // Only advise for regions >= 2MB
    if (size < HUGE_PAGE_THRESHOLD) {
        return false;
    }

    // Check if address is page-aligned (required for madvise)
    const std::uintptr_t addr_int = reinterpret_cast<std::uintptr_t>(addr);
    const std::size_t page_size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    if (addr_int % page_size != 0) {
        return false;  // Not page-aligned
    }

    // Advise kernel to use huge pages for this region
    // MADV_HUGEPAGE: eligible for THP promotion
    // This is a hint; kernel decides whether to actually use huge pages
    const int result = madvise(addr, size, MADV_HUGEPAGE);
    return result == 0;
#else
    // Not supported on non-Linux platforms
    (void)addr;
    (void)size;
    return false;
#endif
}

// Disable transparent huge pages for a memory region.
// Useful if you want to prevent THP for specific allocations.
inline bool disable_huge_pages(void* addr, std::size_t size) noexcept {
#if defined(__linux__)
    const int result = madvise(addr, size, MADV_NOHUGEPAGE);
    return result == 0;
#else
    (void)addr;
    (void)size;
    return false;
#endif
}

// Check if transparent huge pages are available on this system.
// Returns true if THP is supported and enabled.
inline bool huge_pages_available() noexcept {
#if defined(__linux__)
    // Check if /sys/kernel/mm/transparent_hugepage/enabled exists
    // This is a simple heuristic; actual availability depends on kernel config
    FILE* f = fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
#else
    return false;
#endif
}

} // namespace omm

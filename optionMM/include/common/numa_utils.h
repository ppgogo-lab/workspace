#pragma once

#include <cstddef>
#include <cstdint>

#if defined(__linux__)
#include <numaif.h>
#include <numa.h>
#endif

namespace omm {

// ─── NUMA-Aware Memory Allocation ─────────────────────────────────────────────
// On multi-socket systems, memory access latency depends on which NUMA node
// the memory is allocated on relative to the CPU accessing it:
//   - Local access: ~80ns (same NUMA node)
//   - Remote access: ~140ns (different NUMA node)
//   - Cross-socket penalty: 1.75x slower
//
// For optimal performance, allocate memory on the same NUMA node as the thread
// that will access it most frequently.
//
// Requirements:
//   - Linux with NUMA support (libnuma)
//   - Multi-socket system (single-socket systems have only one NUMA node)
//
// Usage:
//   int numa_node = get_numa_node_for_core(core_id);
//   bind_thread_to_numa_node(numa_node);  // Allocate on this node

// Check if NUMA is available and the system has multiple NUMA nodes.
// Returns true if NUMA is supported and there are >= 2 nodes.
inline bool numa_available_multi_node() noexcept {
#if defined(__linux__)
    if (numa_available() < 0) {
        return false;  // NUMA not supported
    }
    return numa_num_configured_nodes() >= 2;
#else
    return false;
#endif
}

// Get the number of NUMA nodes on this system.
// Returns 1 if NUMA is not available (single-node system).
inline int numa_node_count() noexcept {
#if defined(__linux__)
    if (numa_available() < 0) {
        return 1;
    }
    return numa_num_configured_nodes();
#else
    return 1;
#endif
}

// Bind the calling thread's memory allocations to a specific NUMA node.
// Future allocations will prefer this node (but may fall back to others if full).
// This is a "preferred" policy, not a strict binding.
inline bool bind_thread_to_numa_node(int node) noexcept {
#if defined(__linux__)
    if (numa_available() < 0) {
        return false;
    }
    if (node < 0 || node >= numa_num_configured_nodes()) {
        return false;
    }
    // Set preferred NUMA node for this thread
    numa_set_preferred(node);
    return true;
#else
    (void)node;
    return false;
#endif
}

// Bind the calling thread's memory allocations to the local NUMA node
// (the node containing the CPU this thread is currently running on).
inline bool bind_thread_to_local_numa_node() noexcept {
#if defined(__linux__)
    if (numa_available() < 0) {
        return false;
    }
    // Get current CPU
    int cpu = sched_getcpu();
    if (cpu < 0) {
        return false;
    }
    // Get NUMA node for this CPU
    int node = numa_node_of_cpu(cpu);
    if (node < 0) {
        return false;
    }
    // Set preferred NUMA node
    numa_set_preferred(node);
    return true;
#else
    return false;
#endif
}

// Allocate memory on a specific NUMA node.
// Returns nullptr if allocation fails or NUMA is not available.
// The caller is responsible for freeing with numa_free().
inline void* numa_alloc_on_node(std::size_t size, int node) noexcept {
#if defined(__linux__)
    if (numa_available() < 0) {
        return nullptr;
    }
    if (node < 0 || node >= numa_num_configured_nodes()) {
        return nullptr;
    }
    return numa_alloc_onnode(size, node);
#else
    (void)size;
    (void)node;
    return nullptr;
#endif
}

// Allocate memory on the local NUMA node (the node containing the current CPU).
// Returns nullptr if allocation fails or NUMA is not available.
// The caller is responsible for freeing with numa_free().
inline void* numa_alloc_local(std::size_t size) noexcept {
#if defined(__linux__)
    if (numa_available() < 0) {
        return nullptr;
    }
    return numa_alloc_local(size);
#else
    (void)size;
    return nullptr;
#endif
}

// Free memory allocated with numa_alloc_on_node() or numa_alloc_local().
inline void numa_free_memory(void* ptr, std::size_t size) noexcept {
#if defined(__linux__)
    if (numa_available() >= 0 && ptr) {
        numa_free(ptr, size);
    }
#else
    (void)ptr;
    (void)size;
#endif
}

// Migrate existing memory pages to a specific NUMA node.
// This is expensive (requires page faults) but useful for pre-allocated memory.
// Returns true if successful.
inline bool numa_migrate_pages(void* addr, std::size_t size, int target_node) noexcept {
#if defined(__linux__)
    if (numa_available() < 0) {
        return false;
    }
    if (target_node < 0 || target_node >= numa_num_configured_nodes()) {
        return false;
    }

    // Create node mask with only target node set
    struct bitmask* target_mask = numa_allocate_nodemask();
    if (!target_mask) {
        return false;
    }
    numa_bitmask_clearall(target_mask);
    numa_bitmask_setbit(target_mask, target_node);

    // Migrate pages to target node
    const int result = mbind(addr, size, MPOL_BIND, target_mask->maskp,
                            target_mask->size + 1, MPOL_MF_MOVE | MPOL_MF_STRICT);

    numa_free_nodemask(target_mask);
    return result == 0;
#else
    (void)addr;
    (void)size;
    (void)target_node;
    return false;
#endif
}

// Get statistics about NUMA memory usage.
// Returns the amount of memory (in bytes) allocated on each NUMA node.
inline bool numa_get_memory_stats(int node, std::size_t* free_bytes, std::size_t* total_bytes) noexcept {
#if defined(__linux__)
    if (numa_available() < 0) {
        return false;
    }
    if (node < 0 || node >= numa_num_configured_nodes()) {
        return false;
    }

    long long free_mem = 0;
    long long total_mem = 0;

    if (numa_node_size64(node, &total_mem) < 0) {
        return false;
    }

    // Free memory is total - used, but we don't have a direct API for used
    // So we just return total for now
    if (free_bytes) *free_bytes = 0;  // Not available via libnuma
    if (total_bytes) *total_bytes = static_cast<std::size_t>(total_mem);

    return true;
#else
    (void)node;
    (void)free_bytes;
    (void)total_bytes;
    return false;
#endif
}

} // namespace omm

#include "common/thread_utils.h"

#include <pthread.h>
#include <sched.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <thread>
#include <cstdint>

// Read a single integer from a sysfs file. Returns -1 on failure.
static int read_sysfs_int(const char* path) noexcept {
    FILE* f = std::fopen(path, "r");
    if (!f) return -1;
    int val = -1;
    std::fscanf(f, "%d", &val);
    std::fclose(f);
    return val;
}

namespace omm {

void pin_thread_to_core(int core_id) noexcept {
    const int max_cores = static_cast<int>(std::thread::hardware_concurrency());
    if (core_id < 0 || core_id >= max_cores) {
        std::fprintf(stderr,
            "[omm] FATAL: pin_thread_to_core(%d) — invalid core, "
            "hardware has %d logical CPUs\n", core_id, max_cores);
        std::abort();
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::fprintf(stderr,
            "[omm] FATAL: pthread_setaffinity_np(core=%d) failed: %s\n",
            core_id, std::strerror(rc));
        std::abort();
    }
}

void set_thread_name(std::string_view name) noexcept {
    char buf[16]{};   // Linux limit: 15 chars + NUL
    std::size_t len = std::min(name.size(), sizeof(buf) - 1);
    std::memcpy(buf, name.data(), len);
    buf[len] = '\0';
    pthread_setname_np(pthread_self(), buf);
}

void set_realtime_priority(int priority) noexcept {
    if (priority < 1 || priority > 99) {
        std::fprintf(stderr,
            "[omm] FATAL: set_realtime_priority(%d) — priority must be 1–99\n",
            priority);
        std::abort();
    }

    struct sched_param param{};
    param.sched_priority = priority;

    int rc = sched_setscheduler(0, SCHED_FIFO, &param);
    if (rc != 0) {
        std::fprintf(stderr,
            "[omm] FATAL: sched_setscheduler(SCHED_FIFO, priority=%d) failed: %s\n"
            "             Requires CAP_SYS_NICE or root.\n",
            priority, std::strerror(errno));
        std::abort();
    }
}

bool try_set_realtime_priority(int priority) noexcept {
    if (priority < 1 || priority > 99) {
        return false;
    }

    struct sched_param param{};
    param.sched_priority = priority;
    return sched_setscheduler(0, SCHED_FIFO, &param) == 0;
}

int get_numa_node_for_core(int core_id) noexcept {
    // Scan /sys/devices/system/cpu/cpuN/node*/cpumap — the directory name
    // node0, node1, ... tells us which NUMA node this CPU belongs to.
    for (int node = 0; node < 8; ++node) {
        char path[128];
        std::snprintf(path, sizeof(path),
            "/sys/devices/system/cpu/cpu%d/node%d/cpumap", core_id, node);
        FILE* f = std::fopen(path, "r");
        if (f) {
            std::fclose(f);
            return node;
        }
    }
    return -1;
}

int get_physical_core_id(int logical_cpu) noexcept {
    char path[128];
    std::snprintf(path, sizeof(path),
        "/sys/devices/system/cpu/cpu%d/topology/core_id", logical_cpu);
    return read_sysfs_int(path);
}

int get_ht_sibling(int logical_cpu) noexcept {
    // thread_siblings_list contains e.g. "2,18" — parse the sibling
    char path[128];
    std::snprintf(path, sizeof(path),
        "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list",
        logical_cpu);
    FILE* f = std::fopen(path, "r");
    if (!f) return -1;

    int a = -1, b = -1;
    std::fscanf(f, "%d,%d", &a, &b);
    std::fclose(f);

    if (a == logical_cpu) return b;
    if (b == logical_cpu) return a;
    return -1;
}

void assert_not_ht_siblings(int core_a, int core_b,
                              std::string_view name_a,
                              std::string_view name_b) noexcept {
    int phys_a = get_physical_core_id(core_a);
    int phys_b = get_physical_core_id(core_b);

    if (phys_a < 0 || phys_b < 0) {
        // Cannot read topology — warn but don't abort (CI/VM environments)
        std::fprintf(stderr,
            "[omm] WARN: cannot verify HT siblings for cores %d (%.*s) and %d (%.*s)\n",
            core_a, (int)name_a.size(), name_a.data(),
            core_b, (int)name_b.size(), name_b.data());
        return;
    }

    if (phys_a == phys_b) {
        std::fprintf(stderr,
            "[omm] FATAL: cores %d (%.*s) and %d (%.*s) are HyperThreading siblings "
            "(same physical core %d). They share L1/L2 cache and will degrade each "
            "other's latency. Fix config: assign them to separate physical cores.\n",
            core_a, (int)name_a.size(), name_a.data(),
            core_b, (int)name_b.size(), name_b.data(),
            phys_a);
        std::abort();
    }
}

} // namespace omm

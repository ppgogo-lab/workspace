#!/bin/bash
# verify_tuning.sh — OptionMM system tuning verification
# Run as root after tune_system.sh to confirm all settings are active.

PASS=0
FAIL=0
WARN=0

pass() { echo "  [PASS] $1"; ((PASS++)); }
fail() { echo "  [FAIL] $1"; ((FAIL++)); }
warn() { echo "  [WARN] $1"; ((WARN++)); }

require_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "ERROR: This script must be run as root."
        exit 1
    fi
}

require_root

echo "=== OptionMM System Tuning Verification ==="
echo "    $(date)"
echo ""

# ── Kernel Boot Parameters ─────────────────────────────────────────────────────
echo "── Kernel Boot Parameters ──"

ISOLATED=$(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo "")
if [ "$ISOLATED" = "2-15" ]; then
    pass "isolcpus=2-15 active (/sys/devices/system/cpu/isolated)"
else
    fail "isolcpus not active (got: '$ISOLATED', expected: '2-15') → add to GRUB and reboot"
fi

if grep -q "nohz_full" /proc/cmdline; then
    NOHZ=$(grep -o "nohz_full=[^ ]*" /proc/cmdline | head -1)
    pass "nohz_full active ($NOHZ)"
else
    fail "nohz_full not in /proc/cmdline → add to GRUB and reboot"
fi

if grep -q "rcu_nocbs" /proc/cmdline; then
    pass "rcu_nocbs active"
else
    warn "rcu_nocbs not in /proc/cmdline (usually implied by nohz_full but recommended explicit)"
fi

CSTATE=$(cat /sys/module/intel_idle/parameters/max_cstate 2>/dev/null || echo "unknown")
if [ "$CSTATE" = "1" ]; then
    pass "C-state max=1 (intel_idle.max_cstate=1)"
else
    fail "C-state not limited (got: $CSTATE, expected: 1) → add intel_idle.max_cstate=1 to GRUB"
fi

THP_ENABLED=$(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo "unknown")
if echo "$THP_ENABLED" | grep -q "\[never\]"; then
    pass "Transparent Huge Pages disabled"
else
    fail "THP not disabled (got: $THP_ENABLED) → run tune_system.sh or add transparent_hugepage=never to GRUB"
fi

echo ""

# ── Memory ────────────────────────────────────────────────────────────────────
echo "── Memory ──"

HP=$(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo 0)
if [ "$HP" -ge 256 ]; then
    pass "2 MB huge pages: $HP allocated ($(( HP * 2 )) MB)"
else
    fail "2 MB huge pages: only $HP allocated (need ≥ 256) → run tune_system.sh"
fi

HP1G=$(grep "HugePages_Total" /proc/meminfo | awk '{print $2}' 2>/dev/null || echo 0)
HP1G_SZ=$(grep "Hugepagesize" /proc/meminfo | awk '{print $2}' 2>/dev/null || echo 0)
if [ "$HP1G_SZ" = "1048576" ] && [ "$HP1G" -ge 4 ]; then
    pass "1 GB huge pages: $HP1G allocated"
else
    warn "1 GB huge pages may not be available (need hugepagesz=1G hugepages=4 in GRUB)"
fi

SWAP=$(sysctl -n vm.swappiness 2>/dev/null || echo 99)
if [ "$SWAP" = "0" ]; then
    pass "vm.swappiness=0"
else
    fail "vm.swappiness=$SWAP (expected 0) → run tune_system.sh"
fi

NUMA_BAL=$(sysctl -n kernel.numa_balancing 2>/dev/null || echo 1)
if [ "$NUMA_BAL" = "0" ]; then
    pass "NUMA balancing disabled (kernel.numa_balancing=0)"
else
    fail "NUMA balancing enabled (kernel.numa_balancing=$NUMA_BAL) → run tune_system.sh"
fi

echo ""

# ── Scheduling ────────────────────────────────────────────────────────────────
echo "── Scheduling ──"

if ! systemctl is-active irqbalance > /dev/null 2>&1; then
    pass "irqbalance is stopped"
else
    fail "irqbalance is running → systemctl stop irqbalance && systemctl disable irqbalance"
fi

GOV=$(cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor 2>/dev/null || echo "unknown")
if [ "$GOV" = "performance" ]; then
    pass "CPU governor = performance (checked core 2)"
else
    fail "CPU governor = $GOV on core 2 (expected: performance) → run tune_system.sh"
fi

RT=$(sysctl -n kernel.sched_rt_runtime_us 2>/dev/null || echo 950000)
if [ "$RT" = "-1" ]; then
    pass "kernel.sched_rt_runtime_us=-1 (RT threads unrestricted)"
else
    warn "kernel.sched_rt_runtime_us=$RT (expected -1 for unrestricted RT) → run tune_system.sh"
fi

ASLR=$(sysctl -n kernel.randomize_va_space 2>/dev/null || echo 2)
if [ "$ASLR" = "0" ]; then
    pass "ASLR disabled (kernel.randomize_va_space=0)"
else
    warn "ASLR enabled (kernel.randomize_va_space=$ASLR) → may cause address non-determinism"
fi

echo ""

# ── Network ───────────────────────────────────────────────────────────────────
echo "── Network ──"

RMEM=$(sysctl -n net.core.rmem_max 2>/dev/null || echo 0)
if [ "$RMEM" -ge 134217728 ]; then
    pass "net.core.rmem_max=$RMEM (≥ 128 MB)"
else
    fail "net.core.rmem_max=$RMEM (expected ≥ 134217728) → run tune_system.sh"
fi

BPOLL=$(sysctl -n net.core.busy_poll 2>/dev/null || echo 0)
if [ "$BPOLL" = "50" ]; then
    pass "net.core.busy_poll=50"
else
    warn "net.core.busy_poll=$BPOLL (expected 50) → run tune_system.sh"
fi

echo ""

# ── IRQ Affinity ──────────────────────────────────────────────────────────────
echo "── IRQ Affinity ──"

NIC_IRQS=$(grep -i "sfc\|sfn\|solarflare" /proc/interrupts 2>/dev/null \
           | awk -F: '{print $1}' | tr -d ' ' || echo "")
if [ -z "$NIC_IRQS" ]; then
    warn "No Solarflare NIC IRQs found (driver may not be loaded)"
else
    NIC_OK=1
    for irq in $NIC_IRQS; do
        aff=$(cat /proc/irq/$irq/smp_affinity_list 2>/dev/null || echo "?")
        if [ "$aff" != "1" ]; then
            NIC_OK=0
            fail "NIC IRQ $irq → CPU $aff (expected: 1)"
        fi
    done
    [ "$NIC_OK" = "1" ] && pass "All Solarflare NIC IRQs pinned to CPU 1"
fi

echo ""

# ── Turbo Boost ───────────────────────────────────────────────────────────────
echo "── Performance ──"

TURBO_FILE="/sys/devices/system/cpu/intel_pstate/no_turbo"
if [ -f "$TURBO_FILE" ]; then
    TURBO=$(cat "$TURBO_FILE")
    if [ "$TURBO" = "0" ]; then
        pass "Turbo Boost enabled (no_turbo=0)"
    else
        warn "Turbo Boost disabled (no_turbo=1) → may reduce max throughput"
    fi
else
    warn "Turbo Boost control not available (intel_pstate not active)"
fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "═══════════════════════════════════════"
echo "  Results: ${PASS} passed  ${FAIL} failed  ${WARN} warnings"
echo "═══════════════════════════════════════"
if [ "$FAIL" -eq 0 ]; then
    echo "  System is ready for trading."
    exit 0
else
    echo "  Fix all FAIL items before starting the trading engine."
    exit 1
fi

#!/bin/bash
# tune_system.sh — OptionMM runtime system tuning
# Run as root before starting the trading engine. Idempotent.
# Usage: sudo bash scripts/tune_system.sh [NIC_INTERFACE]
#   NIC_INTERFACE defaults to enp65s0f0 if not provided

set -euo pipefail

IFACE="${1:-enp65s0f0}"
ISOLATED_CORES="2-15"
IRQ_CORE=1                  # CPU to receive all NIC IRQs
IRQ_CORE_MASK="2"           # hex bitmask for IRQ_CORE (2^1 = 0x2)
OS_CORES_MASK="3"           # hex bitmask for CPUs 0-1 (default IRQ affinity)

echo "=== OptionMM System Tuning ==="
echo "    NIC interface : $IFACE"
echo "    Isolated cores: $ISOLATED_CORES"
echo "    NIC IRQ core  : $IRQ_CORE"
echo ""

# ── 1. Stop and disable IRQ balance daemon ────────────────────────────────────
echo "[1/11] Stopping irqbalance..."
systemctl stop irqbalance 2>/dev/null || true
systemctl disable irqbalance 2>/dev/null || true

# ── 2. CPU frequency governor → performance ───────────────────────────────────
echo "[2/11] Setting CPU governor to performance..."
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -f "$cpu" ] && echo performance > "$cpu"
done

# ── 3. Keep Turbo Boost enabled ───────────────────────────────────────────────
echo "[3/11] Enabling Turbo Boost..."
TURBO_FILE="/sys/devices/system/cpu/intel_pstate/no_turbo"
[ -f "$TURBO_FILE" ] && echo 0 > "$TURBO_FILE"

# ── 4. Disable Transparent Huge Pages ────────────────────────────────────────
echo "[4/11] Disabling Transparent Huge Pages..."
echo never > /sys/kernel/mm/transparent_hugepage/enabled
echo never > /sys/kernel/mm/transparent_hugepage/defrag
echo 0 > /sys/kernel/mm/transparent_hugepage/khugepaged/defrag 2>/dev/null || true

# ── 5. Allocate 2 MB huge pages ───────────────────────────────────────────────
echo "[5/11] Allocating 2 MB huge pages..."
echo 512 > /proc/sys/vm/nr_hugepages
ACTUAL=$(cat /proc/sys/vm/nr_hugepages)
echo "    Allocated: $ACTUAL pages ($(( ACTUAL * 2 )) MB)"

# ── 6. sysctl tuning ─────────────────────────────────────────────────────────
echo "[6/11] Applying sysctl parameters..."
sysctl -q -w kernel.numa_balancing=0
sysctl -q -w vm.swappiness=0
sysctl -q -w kernel.randomize_va_space=0
sysctl -q -w kernel.sched_rt_runtime_us=-1
sysctl -q -w net.core.rmem_max=134217728
sysctl -q -w net.core.wmem_max=134217728
sysctl -q -w net.core.rmem_default=16777216
sysctl -q -w net.core.wmem_default=16777216
sysctl -q -w net.core.busy_poll=50
sysctl -q -w net.core.busy_read=50
sysctl -q -w net.ipv4.tcp_timestamps=0
sysctl -q -w "net.ipv4.tcp_rmem=4096 87380 134217728"
sysctl -q -w "net.ipv4.tcp_wmem=4096 65536 134217728"

# ── 7. Set default IRQ affinity (cores 0-1 only) ─────────────────────────────
echo "[7/11] Setting default IRQ affinity to cores 0-1..."
echo "${OS_CORES_MASK}" > /proc/irq/default_smp_affinity

# ── 8. Move all existing IRQs to cores 0-1 (except NIC IRQs) ─────────────────
echo "[8/11] Moving non-NIC IRQs to cores 0-1..."
for irq_dir in /proc/irq/*/; do
    irq=$(basename "$irq_dir")
    [ -f "$irq_dir/smp_affinity" ] || continue
    [[ "$irq" =~ ^[0-9]+$ ]] || continue
    # Skip NIC IRQs (handled separately in step 9)
    desc=$(grep "^ *${irq}:" /proc/interrupts 2>/dev/null | tail -1 || echo "")
    if ! echo "$desc" | grep -qi "sfc\|sfn\|solarflare"; then
        echo "${OS_CORES_MASK}" > "$irq_dir/smp_affinity" 2>/dev/null || true
    fi
done

# ── 9. Pin Solarflare NIC IRQs to core 1 ─────────────────────────────────────
echo "[9/11] Pinning Solarflare NIC IRQs to CPU $IRQ_CORE..."
NIC_IRQS=$(grep -i "sfc\|sfn\|solarflare" /proc/interrupts 2>/dev/null \
           | awk -F: '{print $1}' | tr -d ' ' || echo "")
if [ -z "$NIC_IRQS" ]; then
    echo "    WARNING: No Solarflare IRQs found in /proc/interrupts."
    echo "    Ensure the sfc driver is loaded: modprobe sfc"
else
    for irq in $NIC_IRQS; do
        echo "${IRQ_CORE_MASK}" > "/proc/irq/${irq}/smp_affinity" 2>/dev/null \
            && echo "    IRQ $irq → CPU $IRQ_CORE" \
            || echo "    WARNING: Could not set affinity for IRQ $irq"
    done
fi

# ── 10. NIC queue configuration ───────────────────────────────────────────────
echo "[10/11] Configuring NIC queues..."
if [ -d "/sys/class/net/${IFACE}" ]; then
    # Reduce to 1 combined queue directed at feed handler core (core 2)
    ethtool -L "${IFACE}" combined 1 2>/dev/null \
        && echo "    Set ${IFACE} to 1 combined queue" \
        || echo "    INFO: Could not set queue count (may not be supported)"
    # Increase NIC receive buffer
    ethtool -G "${IFACE}" rx 4096 2>/dev/null || true
else
    echo "    WARNING: Interface $IFACE not found. Update IFACE variable."
fi

# ── 11. Memory locking and RT priority limits ─────────────────────────────────
echo "[11/11] Configuring ulimits for memory locking and RT priority..."
LIMITS_FILE="/etc/security/limits.d/99-optionmm.conf"
if [ ! -f "$LIMITS_FILE" ]; then
    cat > "$LIMITS_FILE" << 'EOF'
# OptionMM HFT: allow memory locking and real-time priority
*    hard    memlock    unlimited
*    soft    memlock    unlimited
*    hard    rtprio     99
*    soft    rtprio     99
EOF
    echo "    Created $LIMITS_FILE"
else
    echo "    $LIMITS_FILE already exists, skipping"
fi

echo ""
echo "=== Tuning complete. Run scripts/verify_tuning.sh to confirm. ==="

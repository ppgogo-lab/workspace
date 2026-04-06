# OptionMM — Linux & CPU Optimization Guide

Target hardware: **Intel Xeon Gold 6544Y** (Emerald Rapids, 5th Gen Xeon Scalable)
- 16 physical cores / 32 logical threads (HyperThreading)
- Base 3.6 GHz / Turbo 4.1 GHz
- L1D 48 KB · L2 2 MB · L3 45 MB (shared)
- DDR5 5200 MT/s, 8-channel · PCIe 5.0
- AVX-512 (VNNI, BF16, AMX)
- Optional SNC2: splits 16 cores into 2 NUMA domains of 8 (BIOS setting)

---

## Section 1 — CPU Topology Discovery

Run these **before applying any configuration** to understand the actual layout of your machine.

```bash
# ── Physical core / HT sibling map ──────────────────────────────────────────
# Each line is a pair of logical CPUs sharing the same physical core (L1/L2)
cat /sys/devices/system/cpu/cpu*/topology/thread_siblings_list | sort -u
# On Xeon 6544Y with HT: expect pairs like 0,16 / 1,17 / ... / 15,31
# RULE: Never pin two latency-critical threads to the same physical core pair

# ── NUMA topology ────────────────────────────────────────────────────────────
numactl --hardware
# If SNC2 is OFF (default HEX mode): "available: 1 nodes (0)"  → all 16 cores on node 0
# If SNC2 is ON:                     "available: 2 nodes (0-1)" → cores split across 2 domains

# Visual CPU/cache/NUMA map (requires hwloc package)
lstopo --of ascii

# ── NIC PCIe NUMA affinity ───────────────────────────────────────────────────
# Determines which NUMA node is "near" the Solarflare NIC
lspci | grep -i "solarflare\|sfn\|sfc"
# Note the BDF (e.g., 41:00.0), then:
cat /sys/bus/pci/devices/0000:41:00.0/numa_node
# Output: 0 or 1 → pin all trading threads to this NUMA node

# ── Per-core NUMA node assignment ────────────────────────────────────────────
for c in $(seq 0 31); do
    numa=$(cat /sys/devices/system/cpu/cpu${c}/node*/cpumap 2>/dev/null \
           | head -1 | xargs -I{} echo "node:{}")
    echo "logical cpu $c → $numa"
done

# ── Check if isolated CPUs are active (after reboot with grub params) ────────
cat /sys/devices/system/cpu/isolated
cat /sys/devices/system/cpu/nohz_full
```

**Recommended core layout for 16-core Xeon 6544Y:**

```
Logical CPU  Physical Core  Role
───────────  ─────────────  ────────────────────────────────
0,  16       0              OS / kernel / system tasks
1,  17       1              Solarflare NIC IRQ (pin to CPU 1)
2,  18       2              Feed handler thread       (use CPU 2 only)
3,  19       3              Pricer thread             (use CPU 3 only)
4,  20       4              Strategy thread 0         (use CPU 4 only)
5,  21       5              Strategy thread 1         (use CPU 5 only)
6,  22       6              Strategy thread 2         (use CPU 6 only)
7,  23       7              Strategy thread 3         (use CPU 7 only)
8,  24       8              Strategy thread 4         (use CPU 8 only)
9,  25       9              Strategy thread 5         (use CPU 9 only)
10, 26       10             Strategy thread 6         (use CPU 10 only)
11, 27       11             Strategy thread 7         (use CPU 11 only)
12, 28       12             Gateway dispatcher thread (use CPU 12 only)
13, 29       13             Vol surface fitter thread (use CPU 13 only)
14, 30       14             Risk monitor thread       (use CPU 14 only)
15, 31       15             gRPC server thread        (use CPU 15 only)
```
HT siblings (e.g. CPU 18 when CPU 2 is the feed thread) are left idle on isolated cores.

---

## Section 2 — GRUB Kernel Boot Parameters

Edit `/etc/default/grub` and add the following to `GRUB_CMDLINE_LINUX_DEFAULT`:

```
isolcpus=2-15 nohz_full=2-15 rcu_nocbs=2-15 intel_idle.max_cstate=1 processor.max_cstate=1 hugepagesz=2M hugepages=512 hugepagesz=1G hugepages=4 transparent_hugepage=never
```

**Parameter rationale:**

| Parameter | Value | Reason |
|-----------|-------|--------|
| `isolcpus=2-15` | cores 2–15 | Remove from kernel scheduler. Normal processes stay on cores 0–1. |
| `nohz_full=2-15` | same | Disable periodic timer tick on isolated cores when 1 runnable task present. Eliminates ~50–100 μs timer interruptions. |
| `rcu_nocbs=2-15` | same | Offload RCU callbacks from isolated cores to kthreads on cores 0–1. Required alongside `nohz_full`. |
| `intel_idle.max_cstate=1` | C1 max | Prevent deep C-state entry (C6 wake-up on Emerald Rapids ≈ 100 μs). C1 exit latency < 1 μs. |
| `processor.max_cstate=1` | C1 max | Fallback for ACPI idle driver. Belt-and-suspenders with `intel_idle`. |
| `hugepagesz=2M hugepages=512` | 1 GB total | 2 MB huge pages for ring buffers, pricing arrays. Allocated at boot (guaranteed contiguous). |
| `hugepagesz=1G hugepages=4` | 4 GB total | 1 GB huge pages for large allocations. **Boot-time only** — cannot allocate at runtime. Order matters: `hugepagesz` must immediately precede its `hugepages`. |
| `transparent_hugepage=never` | disabled | Prevents `khugepaged` from causing unpredictable multi-ms stalls during THP collapse/defrag. |

**Apply and reboot:**
```bash
# RHEL / CentOS / Rocky
sudo grub2-mkconfig -o /boot/grub2/grub.cfg
# Ubuntu / Debian
sudo update-grub
sudo reboot
```

---

## Section 3 — Boot-Time Verification

Run after every reboot to confirm kernel parameters are active:

```bash
# Isolated and tickless cores
cat /sys/devices/system/cpu/isolated        # expect: 2-15
cat /sys/devices/system/cpu/nohz_full       # expect: 2-15

# All boot parameters applied
cat /proc/cmdline | tr ' ' '\n' | grep -E "isolcpus|nohz_full|rcu_nocbs|cstate|hugepage"

# C-state limit
cat /sys/module/intel_idle/parameters/max_cstate   # expect: 1

# 1 GB huge pages (only verifiable after boot, cannot be allocated at runtime)
grep -E "^HugePages_|^Hugepagesize" /proc/meminfo

# THP disabled
cat /sys/kernel/mm/transparent_hugepage/enabled   # expect: always [never]
cat /sys/kernel/mm/transparent_hugepage/defrag    # expect: always [never]
```

---

## Section 4 — Runtime Tuning Script

See `scripts/tune_system.sh`. Run as root before starting the trading engine.

**What it does:**
1. Stops and disables `irqbalance`
2. Sets all CPU frequency governors to `performance`
3. Keeps Turbo Boost enabled (for max single-core throughput)
4. Disables Transparent Huge Pages (belt-and-suspenders)
5. Allocates 512 × 2 MB huge pages at runtime
6. Applies all sysctl tuning (NUMA balancing, swappiness, network buffers, RT scheduling, ASLR)
7. Pins Solarflare NIC IRQs to CPU 1
8. Sets default IRQ affinity to CPUs 0–1 for all future IRQs
9. Configures NIC RX queue count and affinity

```bash
sudo bash scripts/tune_system.sh enp65s0f0   # pass your NIC interface name
```

---

## Section 5 — OpenOnload / Solarflare Configuration

```bash
# ── Load drivers ─────────────────────────────────────────────────────────────
modprobe sfc
onload_tool reload

# ── Verify acceleration is active ────────────────────────────────────────────
onload_tool status
onload_stackdump                       # empty until process starts

# ── Launch trading engine with OpenOnload ─────────────────────────────────────
# EF_POLL_USEC=100     : busy-poll for 100 μs before yielding (key for <2μs latency)
# EF_SPIN_USEC=100     : spin on socket ops for 100 μs
# EF_INT_DRIVEN=0      : disable interrupt-driven mode — use spinning exclusively
# EF_STACK_PER_THREAD=1: separate Onload stack per thread (better isolation)
# EF_UDP_RCVBUF        : 8 MB UDP receive buffer per Onload stack

NIC_NUMA=$(cat /sys/bus/pci/devices/$(lspci | grep -i solarflare | awk '{print $1}' | head -1)/numa_node)

EF_POLL_USEC=100 \
EF_SPIN_USEC=100 \
EF_INT_DRIVEN=0 \
EF_STACK_PER_THREAD=1 \
EF_UDP_RCVBUF=8388608 \
onload numactl --cpunodebind=${NIC_NUMA} --membind=${NIC_NUMA} \
  ./optionMM config/config.yaml

# ── Post-launch diagnostics ───────────────────────────────────────────────────
onload_stackdump lots | grep -E "poll|rx|tx|drops|errors"
watch -n 1 'onload_stackdump lots | grep -i drop'   # monitor for packet drops
```

---

## Section 6 — NUMA Configuration

```bash
# ── Determine NUMA layout ─────────────────────────────────────────────────────
numactl --hardware
# Default (SNC2 OFF, HEX mode): 1 node — all 16 cores, all memory on node 0
# SNC2 ON (Hemisphere mode):    2 nodes — cores 0-7 on node 0, cores 8-15 on node 1

# ── Find NIC's NUMA node ──────────────────────────────────────────────────────
NIC_BDF=$(lspci | grep -i "solarflare\|sfc" | awk '{print $1}' | head -1)
NIC_NUMA=$(cat /sys/bus/pci/devices/0000:${NIC_BDF}/numa_node 2>/dev/null || echo 0)
echo "Solarflare NIC NUMA node: ${NIC_NUMA}"

# ── Allocate 2 MB huge pages on the NIC's NUMA node ──────────────────────────
echo 256 > /sys/devices/system/node/node${NIC_NUMA}/hugepages/hugepages-2048kB/nr_hugepages

# ── Bind process to NIC's NUMA node ──────────────────────────────────────────
numactl --cpunodebind=${NIC_NUMA} --membind=${NIC_NUMA} ./optionMM config/config.yaml

# ── Verify memory locality after launch ──────────────────────────────────────
numastat -p $(pgrep optionMM)
# Goal: column for node ${NIC_NUMA} should show nearly all allocated memory
```

**If SNC2 is enabled:**
- Confirm NIC is on NUMA node 0 (typical for first PCIe slot)
- Use cores 0–7 for all trading threads (feed, pricer, strategies, gateway dispatcher)
- Move vol fitter, risk monitor, gRPC server to cores 0–7 as well, OR accept cross-NUMA latency for non-critical threads

---

## Section 7 — HyperThreading / SMT Management

```bash
# ── Check current SMT status ─────────────────────────────────────────────────
cat /sys/devices/system/cpu/smt/control     # on | off | forceoff | notsupported
cat /sys/devices/system/cpu/smt/active      # 1 = enabled, 0 = disabled

# ── Map physical cores to HT sibling pairs ───────────────────────────────────
# Run this to see actual pairs on YOUR machine before configuring core assignments
for c in $(seq 0 15); do
    siblings=$(cat /sys/devices/system/cpu/cpu${c}/topology/thread_siblings_list)
    echo "Physical core $c → logical CPUs: $siblings"
done
# Example output on Xeon 6544Y: Physical core 0 → logical CPUs: 0,16
# IMPORTANT: Verify this matches your config.yaml core assignments

# ── Recommended: Keep SMT ON, use only thread 0 of each physical core ────────
# This avoids the reboot required for SMT disable.
# The HT sibling (e.g., CPU 18 when CPU 2 is the feed thread) is kept idle
# by isolcpus — it won't be scheduled by the kernel.
# Net effect: feed thread on CPU 2 gets full exclusive L1/L2 of physical core 2.

# ── Alternative: Disable SMT entirely ────────────────────────────────────────
# WARNING: Cannot re-enable without reboot.
echo off > /sys/devices/system/cpu/smt/control
nproc   # should drop from 32 → 16

# ── Verify SMT is disabled ────────────────────────────────────────────────────
cat /sys/devices/system/cpu/smt/active      # expect: 0
cat /proc/cpuinfo | grep -c "^processor"    # expect: 16 (was 32)
```

---

## Section 8 — C-State and Latency Verification

```bash
# ── Monitor C-state residency with turbostat (10-second sample) ───────────────
sudo turbostat --quiet \
  --show Core,CPU,Avg_MHz,Busy%,Bzy_MHz,IRQ,Pkg%pc2,Pkg%pc6,Pkg%pc7 \
  sleep 10
# Targets for isolated cores 2–15:
#   Busy%     → near 100% (busy-polling loop)
#   Pkg%pc2   → ≈ 0% (not entering package C2)
#   Pkg%pc6   → ≈ 0% (not entering package C6)
#   Avg_MHz   → near 4100 (turbo, no throttling)

# ── Measure actual wake-up latency with cyclictest ───────────────────────────
# Install: sudo yum install rt-tests  OR  sudo apt install rt-tests
cyclictest \
  --mlockall \
  --smp \
  --priority=99 \
  --interval=200 \
  --distance=0 \
  --affinity=2-15 \
  --duration=30s \
  --histogram=100
# Targets:
#   Max latency < 10 μs on isolated cores
#   P99        < 5 μs
# If max > 50 μs: C-state still active OR timer tick not fully suppressed

# ── Quick spot-check: current CPU frequencies ─────────────────────────────────
grep "cpu MHz" /proc/cpuinfo | sort -t: -k2 -n
# Isolated cores should show ~4100 MHz (at turbo, busy-polling)

# ── Verify C-state limit from kernel side ─────────────────────────────────────
cat /sys/module/intel_idle/parameters/max_cstate    # expect: 1
```

---

## Section 9 — Process Launch & Thread Affinity Verification

```bash
# ── Launch the trading engine (NUMA-aware) ────────────────────────────────────
NIC_NUMA=$(cat /sys/bus/pci/devices/0000:$(lspci | grep -i solarflare | \
           awk '{print $1}' | head -1)/numa_node 2>/dev/null || echo 0)

EF_POLL_USEC=100 EF_SPIN_USEC=100 EF_INT_DRIVEN=0 EF_STACK_PER_THREAD=1 \
onload numactl --cpunodebind=${NIC_NUMA} --membind=${NIC_NUMA} \
  ./optionMM config/config.yaml &
TRADING_PID=$!
echo "Trading engine PID: $TRADING_PID"

# Wait for threads to start
sleep 2

# ── Verify per-thread CPU pinning ─────────────────────────────────────────────
echo "=== Thread CPU assignments for PID $TRADING_PID ==="
for tid in $(ls /proc/$TRADING_PID/task/ 2>/dev/null); do
    name=$(cat /proc/$TRADING_PID/task/$tid/comm 2>/dev/null || echo "?")
    cpu=$(grep "Cpus_allowed_list" /proc/$TRADING_PID/task/$tid/status 2>/dev/null \
          | awk '{print $2}')
    echo "  TID $tid  core=$cpu  name=$name"
done

# ── Real-time thread placement monitor ───────────────────────────────────────
watch -n 0.5 "ps -eLo tid,psr,comm --no-headers | grep -E 'optionMM|feed|pricer|strat|gateway|vol|risk|grpc'"
# psr = processor the thread is CURRENTLY running on (refreshes each line)

# ── Verify no trading threads on non-isolated cores ──────────────────────────
ISOLATED_CORES="2,3,4,5,6,7,8,9,10,11,12,13,14,15"
for tid in $(ls /proc/$TRADING_PID/task/ 2>/dev/null); do
    cpu=$(grep "Cpus_allowed_list" /proc/$TRADING_PID/task/$tid/status 2>/dev/null \
          | awk '{print $2}')
    name=$(cat /proc/$TRADING_PID/task/$tid/comm 2>/dev/null)
    if [[ ! "$ISOLATED_CORES" == *"$cpu"* ]]; then
        echo "WARNING: TID $tid ($name) on non-isolated core $cpu"
    fi
done
```

---

## Section 10 — Verification Checklist Script

See `scripts/verify_tuning.sh`. Runs a pass/fail check of all tuning settings.

```bash
sudo bash scripts/verify_tuning.sh
```

Expected output on a correctly tuned system:
```
=== OptionMM System Tuning Verification ===

── Kernel Parameters ──
  [PASS] isolcpus=2-15 active
  [PASS] nohz_full=2-15 active
  [PASS] C-state max=1
  [PASS] THP disabled

── Memory ──
  [PASS] 2MB huge pages allocated
  [PASS] Swappiness=0
  [PASS] NUMA balancing disabled

── Scheduling ──
  [PASS] irqbalance stopped
  [PASS] CPU governor=performance
  [PASS] RT unlimited (sched_rt=-1)

── Network ──
  [PASS] rmem_max >= 128MB
  [PASS] busy_poll=50

── ASLR ──
  [PASS] randomize_va_space=0

Results: 13 passed, 0 failed
System ready for trading.
```

---

## Section 11 — Persistent sysctl Configuration

Install as `/etc/sysctl.d/99-optionmm.conf` to survive reboots:

```bash
sudo cp scripts/99-optionmm.conf /etc/sysctl.d/
sudo sysctl --system   # apply without reboot
```

Contents:
```ini
# OptionMM HFT system tuning
kernel.numa_balancing         = 0
kernel.sched_rt_runtime_us    = -1
kernel.randomize_va_space     = 0
vm.swappiness                 = 0
vm.nr_hugepages               = 512
net.core.rmem_max             = 134217728
net.core.wmem_max             = 134217728
net.core.rmem_default         = 16777216
net.core.wmem_default         = 16777216
net.core.busy_poll            = 50
net.core.busy_read            = 50
net.ipv4.tcp_timestamps       = 0
net.ipv4.tcp_rmem             = 4096 87380 134217728
net.ipv4.tcp_wmem             = 4096 65536 134217728
```

---

## Section 12 — IRQ Affinity Deep Dive

```bash
# ── Show all IRQ assignments ───────────────────────────────────────────────────
for irq in $(seq 0 255); do
    [ -f /proc/irq/$irq/smp_affinity_list ] || continue
    aff=$(cat /proc/irq/$irq/smp_affinity_list)
    desc=$(grep "^ *$irq:" /proc/interrupts | awk '{for(i=NF;i>0;i--) if($i~/[a-zA-Z]/) {print $i; break}}')
    echo "IRQ $irq  affinity=$aff  desc=$desc"
done

# ── Check if Solarflare is fighting your affinity settings ────────────────────
# Solarflare's sfptpd/sfcaffinity_config may reset /proc/irq/*/smp_affinity
# Monitor for drift:
watch -n 2 'for irq in $(grep -i "sfc\|sfn" /proc/interrupts | awk -F: "{print \$1}" | tr -d " "); do
    echo "IRQ $irq: $(cat /proc/irq/$irq/smp_affinity_list)"; done'

# ── If Solarflare resets affinity, disable their daemon ──────────────────────
systemctl stop sfptpd 2>/dev/null || true
systemctl disable sfptpd 2>/dev/null || true

# ── Set default affinity for new IRQs (prevents future IRQs landing on isolated cores)
echo 3 > /proc/irq/default_smp_affinity     # 0x3 = CPUs 0 and 1

# ── Verify final NIC IRQ placement ────────────────────────────────────────────
grep -i "sfc\|sfn" /proc/interrupts | while read line; do
    irq=$(echo $line | awk -F: '{print $1}' | tr -d ' ')
    aff=$(cat /proc/irq/$irq/smp_affinity_list 2>/dev/null)
    echo "SFC IRQ $irq → CPU(s) $aff"
done
# All SFC IRQs should show: 1 (pinned to CPU 1 only)
```

---

## Quick Reference: Complete Boot-to-Run Sequence

```bash
# Step 1 (once, requires reboot): set GRUB params → reboot
# Step 2 (each boot, as root):
sudo bash scripts/tune_system.sh enp65s0f0

# Step 3 (verify):
sudo bash scripts/verify_tuning.sh

# Step 4 (launch):
NIC_NUMA=$(cat /sys/bus/pci/devices/0000:$(lspci | grep -i solarflare | \
           awk '{print $1}' | head -1)/numa_node 2>/dev/null || echo 0)
EF_POLL_USEC=100 EF_SPIN_USEC=100 EF_INT_DRIVEN=0 EF_STACK_PER_THREAD=1 \
onload numactl --cpunodebind=${NIC_NUMA} --membind=${NIC_NUMA} \
  ./optionMM config/config.yaml
```

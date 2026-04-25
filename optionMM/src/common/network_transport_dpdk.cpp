#include "common/network_transport.h"
#include "logger/logger.h"

#ifdef OMM_ENABLE_DPDK
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#endif

namespace omm {

#ifdef OMM_ENABLE_DPDK

// ─── DPDK Transport ───────────────────────────────────────────────────────────
// Uses DPDK poll-mode drivers for kernel-bypass networking.
// Provides 10-50μs lower latency compared to standard sockets.
//
// Requirements:
//   - DPDK installed and configured
//   - NIC bound to DPDK driver (igb_uio, vfio-pci, etc.)
//   - Huge pages configured
//   - Run as root or with CAP_NET_ADMIN

class DPDKTransport : public INetworkTransport {
public:
    DPDKTransport(const char* interface,
                  const char* multicast_group,
                  uint16_t port) noexcept
        : interface_(interface ? interface : "")
        , multicast_group_(multicast_group ? multicast_group : "")
        , port_(port) {}

    ~DPDKTransport() override {
        shutdown();
    }

    bool initialize() noexcept override {
        // Initialize DPDK EAL (Environment Abstraction Layer)
        const char* argv[] = {
            "optionmm",
            "-l", "0-1",           // Use cores 0-1
            "-n", "4",             // 4 memory channels
            "--proc-type=primary",
            "--file-prefix=omm",
            nullptr
        };
        int argc = 7;

        if (rte_eal_init(argc, const_cast<char**>(argv)) < 0) {
            OMM_LOG_ERROR("dpdk", "Failed to initialize DPDK EAL");
            return false;
        }

        // Get number of available ports
        uint16_t nb_ports = rte_eth_dev_count_avail();
        if (nb_ports == 0) {
            OMM_LOG_ERROR("dpdk", "No DPDK-compatible NICs found");
            return false;
        }

        OMM_LOG_INFO("dpdk", "Found {} DPDK-compatible ports", nb_ports);

        // Use first available port
        port_id_ = 0;

        // Create mbuf pool
        mbuf_pool_ = rte_pktmbuf_pool_create("mbuf_pool", kNumMbufs,
                                             kMbufCacheSize, 0,
                                             RTE_MBUF_DEFAULT_BUF_SIZE,
                                             rte_socket_id());
        if (!mbuf_pool_) {
            OMM_LOG_ERROR("dpdk", "Failed to create mbuf pool");
            return false;
        }

        // Configure port
        struct rte_eth_conf port_conf{};
        port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
        port_conf.rxmode.offloads = RTE_ETH_RX_OFFLOAD_CHECKSUM;

        if (rte_eth_dev_configure(port_id_, 1, 0, &port_conf) < 0) {
            OMM_LOG_ERROR("dpdk", "Failed to configure port {}", port_id_);
            return false;
        }

        // Setup RX queue
        if (rte_eth_rx_queue_setup(port_id_, 0, kRxRingSize,
                                   rte_eth_dev_socket_id(port_id_),
                                   nullptr, mbuf_pool_) < 0) {
            OMM_LOG_ERROR("dpdk", "Failed to setup RX queue");
            return false;
        }

        // Start port
        if (rte_eth_dev_start(port_id_) < 0) {
            OMM_LOG_ERROR("dpdk", "Failed to start port {}", port_id_);
            return false;
        }

        // Enable promiscuous mode for multicast
        if (!multicast_group_.empty()) {
            if (rte_eth_promiscuous_enable(port_id_) < 0) {
                OMM_LOG_WARN("dpdk", "Failed to enable promiscuous mode");
            }
        }

        ready_ = true;
        OMM_LOG_INFO("dpdk", "DPDK transport initialized on port {}", port_id_);
        return true;
    }

    void shutdown() noexcept override {
        if (ready_ && port_id_ != UINT16_MAX) {
            rte_eth_dev_stop(port_id_);
            rte_eth_dev_close(port_id_);
        }
        ready_ = false;
    }

    int receive_batch(NetworkPacket* packets, int max_packets) noexcept override {
        if (!ready_) return 0;

        // Receive burst of packets
        struct rte_mbuf* mbufs[max_packets];
        uint16_t nb_rx = rte_eth_rx_burst(port_id_, 0, mbufs, max_packets);

        if (nb_rx == 0) return 0;

        // Filter for UDP packets on our port
        int valid_count = 0;
        for (uint16_t i = 0; i < nb_rx; ++i) {
            struct rte_mbuf* mbuf = mbufs[i];

            // Parse Ethernet header
            struct rte_ether_hdr* eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr*);
            if (rte_be_to_cpu_16(eth_hdr->ether_type) != RTE_ETHER_TYPE_IPV4) {
                rte_pktmbuf_free(mbuf);
                continue;
            }

            // Parse IP header
            struct rte_ipv4_hdr* ip_hdr = (struct rte_ipv4_hdr*)(eth_hdr + 1);
            if (ip_hdr->next_proto_id != IPPROTO_UDP) {
                rte_pktmbuf_free(mbuf);
                continue;
            }

            // Parse UDP header
            struct rte_udp_hdr* udp_hdr = (struct rte_udp_hdr*)((uint8_t*)ip_hdr + sizeof(struct rte_ipv4_hdr));
            if (rte_be_to_cpu_16(udp_hdr->dst_port) != port_) {
                rte_pktmbuf_free(mbuf);
                continue;
            }

            // Extract payload
            uint8_t* payload = (uint8_t*)(udp_hdr + 1);
            uint32_t payload_len = rte_be_to_cpu_16(udp_hdr->dgram_len) - sizeof(struct rte_udp_hdr);

            packets[valid_count].data = payload;
            packets[valid_count].length = payload_len;
            packets[valid_count].timestamp = mbuf->timestamp;  // Hardware timestamp if available
            packets[valid_count].metadata = mbuf;  // Store mbuf for later release

            valid_count++;
        }

        rx_packets_ += valid_count;
        for (int i = 0; i < valid_count; ++i) {
            rx_bytes_ += packets[i].length;
        }

        return valid_count;
    }

    void release_packets(NetworkPacket* packets, int count) noexcept override {
        // Free mbufs back to pool
        for (int i = 0; i < count; ++i) {
            if (packets[i].metadata) {
                rte_pktmbuf_free(static_cast<struct rte_mbuf*>(packets[i].metadata));
            }
        }
    }

    [[nodiscard]] NetworkBackend backend() const noexcept override {
        return NetworkBackend::DPDK;
    }

    [[nodiscard]] bool is_ready() const noexcept override {
        return ready_;
    }

    [[nodiscard]] uint64_t rx_packets() const noexcept override { return rx_packets_; }
    [[nodiscard]] uint64_t rx_bytes() const noexcept override { return rx_bytes_; }
    [[nodiscard]] uint64_t rx_errors() const noexcept override {
        if (!ready_) return 0;
        struct rte_eth_stats stats{};
        rte_eth_stats_get(port_id_, &stats);
        return stats.ierrors;
    }
    [[nodiscard]] uint64_t rx_dropped() const noexcept override {
        if (!ready_) return 0;
        struct rte_eth_stats stats{};
        rte_eth_stats_get(port_id_, &stats);
        return stats.imissed;
    }

private:
    static constexpr int kNumMbufs = 8192;
    static constexpr int kMbufCacheSize = 250;
    static constexpr int kRxRingSize = 1024;

    std::string interface_;
    std::string multicast_group_;
    uint16_t port_;
    uint16_t port_id_{UINT16_MAX};
    bool ready_{false};

    struct rte_mempool* mbuf_pool_{nullptr};

    uint64_t rx_packets_{0};
    uint64_t rx_bytes_{0};
};

// Update factory to support DPDK
INetworkTransport* create_dpdk_transport(const char* interface,
                                         const char* multicast_group,
                                         uint16_t port) noexcept {
    return new DPDKTransport(interface, multicast_group, port);
}

bool is_dpdk_available() noexcept {
    // Check if DPDK is initialized
    return rte_eal_process_type() != RTE_PROC_INVALID;
}

#else

// Stub implementations when DPDK is not enabled
INetworkTransport* create_dpdk_transport(const char*, const char*, uint16_t) noexcept {
    OMM_LOG_ERROR("dpdk", "DPDK support not compiled in");
    return nullptr;
}

bool is_dpdk_available() noexcept {
    return false;
}

#endif // OMM_ENABLE_DPDK

} // namespace omm

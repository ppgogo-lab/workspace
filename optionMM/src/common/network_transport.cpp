#include "common/network_transport.h"
#include "logger/logger.h"

#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

namespace omm {

// ─── Standard Socket Transport ────────────────────────────────────────────────
// Uses standard BSD sockets with recvmmsg for batch receives.
// This is the fallback implementation that works on all systems.

class StandardSocketTransport : public INetworkTransport {
public:
    StandardSocketTransport(const char* interface,
                           const char* multicast_group,
                           uint16_t port) noexcept
        : interface_(interface ? interface : "")
        , multicast_group_(multicast_group ? multicast_group : "")
        , port_(port) {}

    ~StandardSocketTransport() override {
        shutdown();
    }

    bool initialize() noexcept override {
        // Create UDP socket
        sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ < 0) {
            OMM_LOG_ERROR("network", "Failed to create socket: {}", strerror(errno));
            return false;
        }

        // Set non-blocking
        int flags = fcntl(sock_, F_GETFL, 0);
        if (flags < 0 || fcntl(sock_, F_SETFL, flags | O_NONBLOCK) < 0) {
            OMM_LOG_ERROR("network", "Failed to set non-blocking: {}", strerror(errno));
            close(sock_);
            sock_ = -1;
            return false;
        }

        // Set SO_REUSEADDR
        int reuse = 1;
        if (setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
            OMM_LOG_WARN("network", "Failed to set SO_REUSEADDR: {}", strerror(errno));
        }

        // Bind to port
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (bind(sock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            OMM_LOG_ERROR("network", "Failed to bind to port {}: {}", port_, strerror(errno));
            close(sock_);
            sock_ = -1;
            return false;
        }

        // Join multicast group
        if (!multicast_group_.empty()) {
            struct ip_mreq mreq{};
            if (inet_pton(AF_INET, multicast_group_.c_str(), &mreq.imr_multiaddr) <= 0) {
                OMM_LOG_ERROR("network", "Invalid multicast address: {}", multicast_group_);
                close(sock_);
                sock_ = -1;
                return false;
            }
            mreq.imr_interface.s_addr = INADDR_ANY;

            if (setsockopt(sock_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
                OMM_LOG_ERROR("network", "Failed to join multicast group {}: {}",
                            multicast_group_, strerror(errno));
                close(sock_);
                sock_ = -1;
                return false;
            }
        }

        // Increase receive buffer size
        int bufsize = 16 * 1024 * 1024;  // 16 MB
        if (setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize)) < 0) {
            OMM_LOG_WARN("network", "Failed to set receive buffer size: {}", strerror(errno));
        }

        ready_ = true;
        OMM_LOG_INFO("network", "Standard socket transport initialized on port {}", port_);
        return true;
    }

    void shutdown() noexcept override {
        if (sock_ >= 0) {
            close(sock_);
            sock_ = -1;
        }
        ready_ = false;
    }

    int receive_batch(NetworkPacket* packets, int max_packets) noexcept override {
        if (!ready_ || sock_ < 0) return 0;

        // Use recvmmsg for batch receives (Linux-specific)
#ifdef __linux__
        struct mmsghdr msgs[max_packets];
        struct iovec iovecs[max_packets];

        for (int i = 0; i < max_packets; ++i) {
            iovecs[i].iov_base = recv_buffers_[i];
            iovecs[i].iov_len = kMaxPacketSize;

            std::memset(&msgs[i], 0, sizeof(msgs[i]));
            msgs[i].msg_hdr.msg_iov = &iovecs[i];
            msgs[i].msg_hdr.msg_iovlen = 1;
        }

        int received = recvmmsg(sock_, msgs, max_packets, MSG_DONTWAIT, nullptr);
        if (received < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                rx_errors_++;
            }
            return 0;
        }

        // Fill packet structures
        for (int i = 0; i < received; ++i) {
            packets[i].data = recv_buffers_[i];
            packets[i].length = msgs[i].msg_len;
            packets[i].timestamp = 0;  // No hardware timestamp
            packets[i].metadata = nullptr;
        }

        rx_packets_ += received;
        for (int i = 0; i < received; ++i) {
            rx_bytes_ += packets[i].length;
        }

        return received;
#else
        // Fallback: single recvfrom
        struct sockaddr_in src_addr{};
        socklen_t addr_len = sizeof(src_addr);

        ssize_t len = recvfrom(sock_, recv_buffers_[0], kMaxPacketSize, MSG_DONTWAIT,
                              (struct sockaddr*)&src_addr, &addr_len);
        if (len < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                rx_errors_++;
            }
            return 0;
        }

        packets[0].data = recv_buffers_[0];
        packets[0].length = static_cast<uint32_t>(len);
        packets[0].timestamp = 0;
        packets[0].metadata = nullptr;

        rx_packets_++;
        rx_bytes_ += len;
        return 1;
#endif
    }

    void release_packets(NetworkPacket* packets, int count) noexcept override {
        // No-op for standard sockets (buffers are reused)
        (void)packets;
        (void)count;
    }

    [[nodiscard]] NetworkBackend backend() const noexcept override {
        return NetworkBackend::StandardSocket;
    }

    [[nodiscard]] bool is_ready() const noexcept override {
        return ready_;
    }

    [[nodiscard]] uint64_t rx_packets() const noexcept override { return rx_packets_; }
    [[nodiscard]] uint64_t rx_bytes() const noexcept override { return rx_bytes_; }
    [[nodiscard]] uint64_t rx_errors() const noexcept override { return rx_errors_; }
    [[nodiscard]] uint64_t rx_dropped() const noexcept override { return rx_dropped_; }

private:
    static constexpr int kMaxPacketSize = 9000;  // Jumbo frame
    static constexpr int kMaxBatchSize = 32;

    std::string interface_;
    std::string multicast_group_;
    uint16_t port_;
    int sock_{-1};
    bool ready_{false};

    uint64_t rx_packets_{0};
    uint64_t rx_bytes_{0};
    uint64_t rx_errors_{0};
    uint64_t rx_dropped_{0};

    // Pre-allocated receive buffers
    alignas(64) uint8_t recv_buffers_[kMaxBatchSize][kMaxPacketSize];
};

// ─── Factory Implementation ───────────────────────────────────────────────────

// Forward declaration for DPDK transport (implemented in network_transport_dpdk.cpp)
INetworkTransport* create_dpdk_transport(const char* interface,
                                         const char* multicast_group,
                                         uint16_t port) noexcept;
bool is_dpdk_available() noexcept;

INetworkTransport* create_network_transport(NetworkBackend backend,
                                            const char* interface,
                                            const char* multicast_group,
                                            uint16_t port) noexcept {
    switch (backend) {
    case NetworkBackend::StandardSocket:
        return new StandardSocketTransport(interface, multicast_group, port);

    case NetworkBackend::DPDK:
        if (is_dpdk_available()) {
            return create_dpdk_transport(interface, multicast_group, port);
        } else {
            OMM_LOG_WARN("network", "DPDK not available, falling back to standard sockets");
            return new StandardSocketTransport(interface, multicast_group, port);
        }

    case NetworkBackend::OpenOnload:
        // OpenOnload uses standard sockets with LD_PRELOAD
        OMM_LOG_INFO("network", "OpenOnload backend uses standard sockets with LD_PRELOAD");
        return new StandardSocketTransport(interface, multicast_group, port);

    default:
        OMM_LOG_ERROR("network", "Unknown network backend");
        return nullptr;
    }
}

bool is_network_backend_available(NetworkBackend backend) noexcept {
    switch (backend) {
    case NetworkBackend::StandardSocket:
        return true;  // Always available

    case NetworkBackend::DPDK:
        return is_dpdk_available();

    case NetworkBackend::OpenOnload:
        // Check if OpenOnload is loaded
        return getenv("EF_POLL_USEC") != nullptr;

    default:
        return false;
    }
}

const char* network_backend_name(NetworkBackend backend) noexcept {
    switch (backend) {
    case NetworkBackend::StandardSocket: return "StandardSocket";
    case NetworkBackend::DPDK: return "DPDK";
    case NetworkBackend::OpenOnload: return "OpenOnload";
    default: return "Unknown";
    }
}

} // namespace omm

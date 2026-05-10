#pragma once

#include <cstddef>
#include <cstdint>

namespace omm {

// ─── Network Transport Abstraction ────────────────────────────────────────────
// Provides a unified interface for different network I/O backends:
//   - Standard sockets (kernel network stack)
//   - DPDK (kernel bypass, poll-mode drivers)
//   - OpenOnload (Solarflare kernel bypass)
//
// This abstraction allows the feed handler to work with any backend without
// code changes, enabling deployment flexibility.

enum class NetworkBackend {
    StandardSocket,  // Standard BSD sockets (kernel network stack)
    DPDK,           // DPDK poll-mode drivers (kernel bypass)
    OpenOnload      // Solarflare OpenOnload (kernel bypass)
};

// Network packet buffer for zero-copy receive
struct NetworkPacket {
    uint8_t*  data;        // Packet data pointer
    uint32_t  length;      // Packet length in bytes
    uint64_t  timestamp;   // Hardware timestamp (if available)
    void*     metadata;    // Backend-specific metadata (mbuf, etc.)
};

// Abstract network transport interface
class INetworkTransport {
public:
    /**
     * @brief INetworkTransport.
     * @return None.
     */
    virtual ~INetworkTransport() = default;

    // Initialize the transport (open socket, setup DPDK port, etc.)
    // Returns true on success, false on failure.
    /**
     * @brief Initialize.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    virtual bool initialize() noexcept = 0;

    // Cleanup and release resources
    /**
     * @brief Shutdown.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    virtual void shutdown() noexcept = 0;

    // Receive packets in batch (non-blocking)
    // Returns the number of packets received (0 if none available)
    // packets: output array to store received packets
    // max_packets: maximum number of packets to receive
    /**
     * @brief Receive batch.
     * @param packets Parameter supplied by the caller.
     * @param max_packets Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    virtual int receive_batch(NetworkPacket* packets, int max_packets) noexcept = 0;

    // Release packet buffers back to the transport
    // Must be called after processing packets to avoid memory leaks
    /**
     * @brief Release packets.
     * @param packets Parameter supplied by the caller.
     * @param count Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    virtual void release_packets(NetworkPacket* packets, int count) noexcept = 0;

    // Get the backend type
    /**
     * @brief Backend.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] virtual NetworkBackend backend() const noexcept = 0;

    // Check if transport is ready
    /**
     * @brief Is ready.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] virtual bool is_ready() const noexcept = 0;

    // Get statistics
    /**
     * @brief Rx packets.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] virtual uint64_t rx_packets() const noexcept = 0;
    /**
     * @brief Rx bytes.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] virtual uint64_t rx_bytes() const noexcept = 0;
    /**
     * @brief Rx errors.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] virtual uint64_t rx_errors() const noexcept = 0;
    /**
     * @brief Rx dropped.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] virtual uint64_t rx_dropped() const noexcept = 0;
};

// Factory function to create the appropriate transport based on configuration
// Returns nullptr if the requested backend is not available
/**
 * @brief Create network transport.
 * @param backend Parameter supplied by the caller.
 * @param interface Parameter supplied by the caller.
 * @param multicast_group Parameter supplied by the caller.
 * @param port Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
INetworkTransport* create_network_transport(NetworkBackend backend,
                                            const char* interface,
                                            const char* multicast_group,
                                            uint16_t port) noexcept;

// Check if a specific backend is available on this system
/**
 * @brief Is network backend available.
 * @param backend Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
bool is_network_backend_available(NetworkBackend backend) noexcept;

// Get the name of a backend (for logging)
/**
 * @brief Network backend name.
 * @param backend Parameter supplied by the caller.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
const char* network_backend_name(NetworkBackend backend) noexcept;

} // namespace omm

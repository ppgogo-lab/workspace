#include "feed/multicast_feed.h"
#include "common/thread_utils.h"

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cmath>

namespace omm {

/**
 * @brief Implements Setup socket.
 * @return Return value produced by the operation.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
bool MulticastFeedHandler::setup_socket() noexcept {
    sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ < 0) { err_count_++; return false; }

    // Reuse address for multiple subscribers on same host
    int opt = 1;
    ::setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Set receive buffer to cfg_.rcvbuf_mb MB
    int rcvbuf = cfg_.rcvbuf_mb * 1024 * 1024;
    ::setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    // Bind to the multicast port
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(cfg_.port);
    if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        err_count_++;
        ::close(sock_);
        sock_ = -1;
        return false;
    }

    // Join each multicast group
    for (int g = 0; g < cfg_.group_count; ++g) {
        ip_mreq mreq{};
        ::inet_pton(AF_INET, cfg_.groups[g], &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = INADDR_ANY;
        // If interface name is specified, resolve to IP
        if (cfg_.interface[0] != '\0') {
            ifreq ifr{};
            std::strncpy(ifr.ifr_name, cfg_.interface, IFNAMSIZ - 1);
            if (::ioctl(sock_, SIOCGIFADDR, &ifr) == 0)
                mreq.imr_interface =
                    reinterpret_cast<sockaddr_in*>(&ifr.ifr_addr)->sin_addr;
        }
        ::setsockopt(sock_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    }

    connected_.store(true, std::memory_order_release);
    return true;
}

/**
 * @brief Implements Decode and push.
 * @param buf Parameter supplied by the caller.
 * @param len Parameter supplied by the caller.
 * @param recv_ts_ns Parameter supplied by the caller.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void MulticastFeedHandler::decode_and_push(const uint8_t* buf, int len,
                                             int64_t recv_ts_ns) noexcept {
    // Wire format: simplified fixed-layout binary matching CTP MdApi tick.
    // In production this would be the actual exchange binary protocol.
    // Here we define a minimal testable layout.
    //
    // Layout (little-endian):
    //   [0..31]  instrument_code char[32]
    //   [32..39] exchange_ts_ns  int64
    //   [40..47] last_price      double
    //   [48..87] bid_price[5]    double[5]
    //   [88..127] ask_price[5]   double[5]
    //   [128..147] bid_vol[5]    int32[5]
    //   [148..167] ask_vol[5]    int32[5]
    //   [168..175] open_interest double
    //   [176..183] volume        int64
    //   [184..191] sequence_no   uint64

    static constexpr int MIN_LEN = 192;
    if (len < MIN_LEN) { err_count_++; return; }

    char code[33]{};
    std::memcpy(code, buf, 32);

    uint16_t id = resolve_instrument(code);
    if (id == INVALID_INSTRUMENT_ID) return;

    MarketTick tick{};
    tick.recv_ts_ns     = recv_ts_ns;
    tick.instrument_id  = id;
    std::memcpy(&tick.exchange_ts_ns, buf + 32,  8);
    std::memcpy(&tick.last_price,     buf + 40,  8);
    std::memcpy(tick.bid_price,       buf + 48,  40);
    std::memcpy(tick.ask_price,       buf + 88,  40);
    std::memcpy(tick.bid_volume,      buf + 128, 20);
    std::memcpy(tick.ask_volume,      buf + 148, 20);
    std::memcpy(&tick.open_interest,  buf + 168, 8);
    std::memcpy(&tick.volume,         buf + 176, 8);
    std::memcpy(&tick.sequence_no,    buf + 184, 8);

    if (!tick_buf_->try_push(to_top_of_book_tick(tick)))
        dropped_count_.fetch_add(1, std::memory_order_relaxed);
    else
        msg_count_.fetch_add(1, std::memory_order_relaxed);
}

/**
 * @brief Implements Run loop.
 * @return None.
 * @note Noexcept API preserves hot-path failure and latency invariants.
 */
void MulticastFeedHandler::run_loop() noexcept {
    // recvmmsg batch: amortise syscall overhead
    static constexpr int BATCH = 32;
    static constexpr int BUF_SIZE = 256;

    uint8_t bufs[BATCH][BUF_SIZE];
    iovec   iovecs[BATCH];
    mmsghdr msgs[BATCH];
    std::memset(msgs, 0, sizeof(msgs));

    for (int i = 0; i < BATCH; ++i) {
        iovecs[i].iov_base = bufs[i];
        iovecs[i].iov_len  = BUF_SIZE;
        msgs[i].msg_hdr.msg_iov    = &iovecs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }

    timespec timeout{0, 100'000};  // 100µs timeout so we can check stop_flag_

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        int n = ::recvmmsg(sock_, msgs, BATCH, MSG_DONTWAIT, &timeout);
        if (n <= 0) {
            spin_pause();
            continue;
        }
        int64_t recv_ts = get_monotonic_ns();
        for (int i = 0; i < n; ++i)
            decode_and_push(bufs[i],
                            static_cast<int>(msgs[i].msg_len), recv_ts);
    }
}

/**
 * @brief Implements Start.
 * @return None.
 */
void MulticastFeedHandler::start() {
    if (!setup_socket()) return;
    stop_flag_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this] { run_loop(); });
}

/**
 * @brief Implements Stop.
 * @return None.
 */
void MulticastFeedHandler::stop() {
    stop_flag_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    if (sock_ >= 0) { ::close(sock_); sock_ = -1; }
    connected_.store(false, std::memory_order_release);
}

} // namespace omm

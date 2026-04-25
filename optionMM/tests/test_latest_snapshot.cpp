#include <gtest/gtest.h>

#include "common/latest_snapshot.h"
#include "common/ring_buffer.h"
#include "common/types.h"

#include <array>
#include <atomic>
#include <thread>
#include <vector>

using namespace omm;

namespace {

TopOfBookTick make_consistent_tick(uint64_t seq) {
    TopOfBookTick tick{};
    tick.recv_ts_ns = static_cast<int64_t>(seq);
    tick.exchange_ts_ns = static_cast<int64_t>(seq);
    tick.instrument_id = static_cast<uint16_t>(seq & 0x3FFu);
    tick.last_price = static_cast<double>(seq);
    tick.bid_price[0] = static_cast<double>(seq + 1);
    tick.ask_price[0] = static_cast<double>(seq + 2);
    tick.bid_volume[0] = static_cast<int32_t>(seq & 0x7FFFu);
    tick.ask_volume[0] = static_cast<int32_t>((seq + 3) & 0x7FFFu);
    tick.sequence_no = seq;
    return tick;
}

bool tick_is_consistent(const TopOfBookTick& tick) {
    const uint64_t seq = tick.sequence_no;
    return tick.recv_ts_ns == static_cast<int64_t>(seq)
        && tick.exchange_ts_ns == static_cast<int64_t>(seq)
        && tick.instrument_id == static_cast<uint16_t>(seq & 0x3FFu)
        && tick.last_price == static_cast<double>(seq)
        && tick.bid_price[0] == static_cast<double>(seq + 1)
        && tick.ask_price[0] == static_cast<double>(seq + 2)
        && tick.bid_volume[0] == static_cast<int32_t>(seq & 0x7FFFu)
        && tick.ask_volume[0] == static_cast<int32_t>((seq + 3) & 0x7FFFu);
}

} // namespace

TEST(LatestSnapshotTest, ReadsPublishedValue) {
    LatestSnapshot<TopOfBookTick> snapshot;

    TopOfBookTick tick = make_consistent_tick(42);
    snapshot.publish(tick);

    TopOfBookTick out{};
    ASSERT_TRUE(snapshot.read(&out));
    EXPECT_TRUE(tick_is_consistent(out));
    EXPECT_EQ(out.sequence_no, 42u);
}

TEST(LatestSnapshotTest, ConcurrentReadersNeverAcceptTornTicks) {
    SnapshotArray<TopOfBookTick, 4> snapshots;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> bad_reads{0};
    std::atomic<uint64_t> good_reads{0};

    std::thread writer([&] {
        for (uint64_t seq = 1; seq <= 200000; ++seq) {
            snapshots.publish(0, make_consistent_tick(seq));
        }
        stop.store(true, std::memory_order_release);
    });

    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                TopOfBookTick tick{};
                if (!snapshots.read(0, &tick)) {
                    spin_pause();
                    continue;
                }
                if (!tick_is_consistent(tick)) {
                    bad_reads.fetch_add(1, std::memory_order_relaxed);
                } else {
                    good_reads.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    writer.join();
    for (auto& reader : readers) reader.join();

    EXPECT_EQ(bad_reads.load(std::memory_order_relaxed), 0u);
    EXPECT_GT(good_reads.load(std::memory_order_relaxed), 0u);
}


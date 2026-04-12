#include <gtest/gtest.h>

#include "monitoring/topic.h"

namespace {

struct Sample {
    uint64_t id;
    double value;
};

} // namespace

TEST(MonitoringTopic, PublishReadSequence) {
    omm::MonitoringTopic<Sample, 8> topic;

    Sample a{1, 10.0};
    Sample b{2, 20.0};
    topic.publish(a);
    topic.publish(b);

    uint64_t cursor = 0;
    Sample out{};
    ASSERT_TRUE(topic.read_next(cursor, out));
    EXPECT_EQ(out.id, 1u);
    ASSERT_TRUE(topic.read_next(cursor, out));
    EXPECT_EQ(out.id, 2u);
    EXPECT_FALSE(topic.read_next(cursor, out));
}

TEST(MonitoringTopic, ReaderSkipsOverwrittenEntries) {
    omm::MonitoringTopic<Sample, 4> topic;
    for (uint64_t i = 1; i <= 6; ++i) {
        topic.publish(Sample{i, static_cast<double>(i)});
    }

    uint64_t cursor = 0;
    Sample out{};
    ASSERT_TRUE(topic.read_next(cursor, out));
    EXPECT_GE(out.id, 3u);
    EXPECT_LE(out.id, 6u);
}

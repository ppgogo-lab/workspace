#include <gtest/gtest.h>

#include "common/instrument_lookup.h"

#include <array>
#include <string>
#include <unordered_map>
#include <utility>

using namespace omm;

namespace {

Instrument make_instrument(uint16_t id, std::string_view code) {
    Instrument instr{};
    instr.instrument_id = id;
    instr.kind = InstrumentKind::Future;
    instr.product_index = 0;
    instr.tick_size = 1.0;
    instr.multiplier = 1.0;
    instr.code = InstrumentCode(code);
    instr.underlying_code = InstrumentCode(code);
    return instr;
}

uint64_t hash_code(std::string_view code) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : code) {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    return hash == 0 ? 1ULL : hash;
}

std::pair<std::string, std::string> find_collision_pair() {
    std::unordered_map<std::size_t, std::string> seen;
    for (int i = 0; i < 50'000; ++i) {
        std::string code = "OPT" + std::to_string(i);
        const std::size_t bucket = hash_code(code) & (InstrumentLookup::capacity() - 1);
        auto [it, inserted] = seen.emplace(bucket, code);
        if (!inserted && it->second != code) {
            return {it->second, code};
        }
    }
    return {};
}

} // namespace

TEST(InstrumentLookupTest, BuildsAndResolvesQueriedInstruments) {
    std::array<Instrument, 4> instruments{};
    instruments[0] = make_instrument(0, "au2506");
    instruments[1] = make_instrument(1, "au2506-C-580");
    instruments[2] = make_instrument(2, "au2506-P-580");

    InstrumentLookup lookup;
    lookup.build(instruments.data(), 3);

    EXPECT_EQ(lookup.find("au2506"), 0);
    EXPECT_EQ(lookup.find("au2506-C-580"), 1);
    EXPECT_EQ(lookup.find("au2506-P-580"), 2);
    EXPECT_EQ(lookup.find("missing"), INVALID_INSTRUMENT_ID);
}

TEST(InstrumentLookupTest, RebuildDropsOldMappings) {
    std::array<Instrument, 2> first{};
    first[0] = make_instrument(0, "old0");
    first[1] = make_instrument(1, "old1");

    std::array<Instrument, 2> second{};
    second[0] = make_instrument(0, "new0");
    second[1] = make_instrument(1, "new1");

    InstrumentLookup lookup;
    lookup.build(first.data(), 2);
    ASSERT_EQ(lookup.find("old0"), 0);

    lookup.build(second.data(), 2);
    EXPECT_EQ(lookup.find("old0"), INVALID_INSTRUMENT_ID);
    EXPECT_EQ(lookup.find("new0"), 0);
    EXPECT_EQ(lookup.find("new1"), 1);
}

TEST(InstrumentLookupTest, ResolvesCodesThatShareTheSameBucket) {
    const auto [first_code, second_code] = find_collision_pair();
    ASSERT_FALSE(first_code.empty());
    ASSERT_FALSE(second_code.empty());

    std::array<Instrument, 2> instruments{};
    instruments[0] = make_instrument(0, first_code);
    instruments[1] = make_instrument(1, second_code);

    InstrumentLookup lookup;
    lookup.build(instruments.data(), 2);

    EXPECT_EQ(lookup.find(first_code), 0);
    EXPECT_EQ(lookup.find(second_code), 1);
}

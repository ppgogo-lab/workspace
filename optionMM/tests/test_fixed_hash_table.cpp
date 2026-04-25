#include "common/fixed_hash_table.h"

#include <gtest/gtest.h>

#include <cstring>

using namespace omm;

TEST(FixedHashTableTest, NumericInsertUpdateFindErase) {
    FixedHashTable<uint64_t, int, 8> table;

    EXPECT_FALSE(table.insert(0, 1));
    EXPECT_TRUE(table.insert(10, 1));
    EXPECT_TRUE(table.insert(18, 2));
    EXPECT_EQ(table.size(), 2U);

    ASSERT_NE(table.find(10), nullptr);
    EXPECT_EQ(*table.find(10), 1);
    EXPECT_TRUE(table.insert(10, 3));
    EXPECT_EQ(table.size(), 2U);
    ASSERT_NE(table.find(10), nullptr);
    EXPECT_EQ(*table.find(10), 3);

    EXPECT_TRUE(table.erase(10));
    EXPECT_EQ(table.find(10), nullptr);
    EXPECT_EQ(table.size(), 1U);
    EXPECT_FALSE(table.erase(10));

    EXPECT_TRUE(table.insert(26, 4));
    ASSERT_NE(table.find(26), nullptr);
    EXPECT_EQ(*table.find(26), 4);
}

TEST(FixedHashTableTest, NumericFullTableRejectsExtraKey) {
    FixedHashTable<uint64_t, int, 4> table;

    EXPECT_TRUE(table.insert(1, 1));
    EXPECT_TRUE(table.insert(2, 2));
    EXPECT_TRUE(table.insert(3, 3));
    EXPECT_TRUE(table.insert(4, 4));
    EXPECT_FALSE(table.insert(5, 5));

    EXPECT_TRUE(table.erase(2));
    EXPECT_TRUE(table.insert(5, 5));
    ASSERT_NE(table.find(5), nullptr);
    EXPECT_EQ(*table.find(5), 5);
}

TEST(FixedHashTableTest, StringInsertCopiesKeyAndUpdates) {
    FixedStringHashTable<8, int, 8> table;
    char key[8]{};
    std::strncpy(key, "ABC123", sizeof(key) - 1);

    EXPECT_FALSE(table.insert("", 1));
    EXPECT_TRUE(table.insert(key, 7));
    key[0] = 'Z';

    ASSERT_NE(table.find("ABC123"), nullptr);
    EXPECT_EQ(*table.find("ABC123"), 7);
    EXPECT_EQ(table.find("ZBC123"), nullptr);

    EXPECT_TRUE(table.insert("ABC123", 9));
    ASSERT_NE(table.find("ABC123"), nullptr);
    EXPECT_EQ(*table.find("ABC123"), 9);

    EXPECT_TRUE(table.erase("ABC123"));
    EXPECT_EQ(table.find("ABC123"), nullptr);
    EXPECT_TRUE(table.insert("ABC123", 11));
    ASSERT_NE(table.find("ABC123"), nullptr);
    EXPECT_EQ(*table.find("ABC123"), 11);
}

TEST(FixedHashTableTest, StringFullTableRejectsExtraKey) {
    FixedStringHashTable<8, int, 4> table;

    EXPECT_TRUE(table.insert("A", 1));
    EXPECT_TRUE(table.insert("B", 2));
    EXPECT_TRUE(table.insert("C", 3));
    EXPECT_TRUE(table.insert("D", 4));
    EXPECT_FALSE(table.insert("E", 5));

    EXPECT_TRUE(table.erase("B"));
    EXPECT_TRUE(table.insert("E", 5));
    ASSERT_NE(table.find("E"), nullptr);
    EXPECT_EQ(*table.find("E"), 5);
}


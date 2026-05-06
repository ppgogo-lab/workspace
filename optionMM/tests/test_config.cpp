#include <gtest/gtest.h>

#include "common/config.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace omm;

namespace {

SystemConfig load_temp_config(const std::string& yaml) {
    const auto unique_id = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path()
                    / ("optionmm_test_config_" + std::to_string(unique_id) + ".yaml");
    {
        std::ofstream out(path);
        out << yaml;
    }
    try {
        SystemConfig cfg = load_config(path.string());
        std::filesystem::remove(path);
        return cfg;
    } catch (...) {
        std::filesystem::remove(path);
        throw;
    }
}

std::string minimal_config_with_product_pricing(const std::string& pricing_block) {
    return std::string(
        "instance:\n"
        "  exchange_id: \"SHFE\"\n"
        "  account_id: \"TEST\"\n"
        "feed:\n"
        "  type: sim\n"
        "gateway:\n"
        "  type: sim\n"
        "products:\n"
        "  - underlying_id: \"cu2501\"\n"
        "    exchange_id: \"SHFE\"\n"
        "    strategy_core: 4\n")
        + pricing_block;
}

} // namespace

TEST(BaseOffsetConfig, AppliesTickPriceAndPercentageOffsets) {
    ProductPricingConfig cfg{};

    cfg.base_offset_type = BaseOffsetType::Tick;
    cfg.base_offset_value = 2.5;
    EXPECT_DOUBLE_EQ(apply_base_offset(100.0, 0.2, cfg), 100.5);

    cfg.base_offset_type = BaseOffsetType::Price;
    cfg.base_offset_value = -1.25;
    EXPECT_DOUBLE_EQ(apply_base_offset(100.0, 0.2, cfg), 98.75);

    cfg.base_offset_type = BaseOffsetType::Percentage;
    cfg.base_offset_value = 0.015;
    EXPECT_DOUBLE_EQ(apply_base_offset(100.0, 0.2, cfg), 101.5);
}

TEST(ConfigParsing, ProductPricingDefaultsToNeutralPriceOffset) {
    const SystemConfig cfg = load_temp_config(minimal_config_with_product_pricing(""));

    ASSERT_EQ(cfg.product_count, 1);
    EXPECT_EQ(cfg.products[0].pricing.base_offset_type, BaseOffsetType::Price);
    EXPECT_DOUBLE_EQ(cfg.products[0].pricing.base_offset_value, 0.0);
    EXPECT_DOUBLE_EQ(apply_base_offset(100.0, 0.5, cfg.products[0].pricing), 100.0);
}

TEST(ConfigParsing, ParsesProductBaseOffsetTypes) {
    const SystemConfig tick_cfg = load_temp_config(minimal_config_with_product_pricing(
        "    pricing:\n"
        "      base_offset_type: tick\n"
        "      base_offset_value: 3.0\n"));
    EXPECT_EQ(tick_cfg.products[0].pricing.base_offset_type, BaseOffsetType::Tick);
    EXPECT_DOUBLE_EQ(tick_cfg.products[0].pricing.base_offset_value, 3.0);

    const SystemConfig price_cfg = load_temp_config(minimal_config_with_product_pricing(
        "    pricing:\n"
        "      base_offset_type: price\n"
        "      base_offset_value: -2.0\n"));
    EXPECT_EQ(price_cfg.products[0].pricing.base_offset_type, BaseOffsetType::Price);
    EXPECT_DOUBLE_EQ(price_cfg.products[0].pricing.base_offset_value, -2.0);

    const SystemConfig pct_cfg = load_temp_config(minimal_config_with_product_pricing(
        "    pricing:\n"
        "      base_offset_type: percentage\n"
        "      base_offset_value: 0.01\n"));
    EXPECT_EQ(pct_cfg.products[0].pricing.base_offset_type, BaseOffsetType::Percentage);
    EXPECT_DOUBLE_EQ(pct_cfg.products[0].pricing.base_offset_value, 0.01);
}

TEST(ConfigParsing, RejectsInvalidProductBaseOffsetType) {
    EXPECT_THROW(
        (void)load_temp_config(minimal_config_with_product_pricing(
            "    pricing:\n"
            "      base_offset_type: points\n"
            "      base_offset_value: 1.0\n")),
        std::runtime_error);
}

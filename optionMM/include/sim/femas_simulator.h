#pragma once

#include "femas/api_wrapper.h"

#include <string>
#include <string_view>
#include <vector>

namespace omm {

struct FemasSimFrontConfig {
    std::string session_name{"session"};
    std::string exchange;
    std::vector<std::string> products;
    std::string trading_day;
    std::string start_time;
    std::string end_time;
    std::string ddb;
    double replay_speed{1.0};
    bool max_speed{false};
};

[[nodiscard]] bool parse_femas_sim_front(std::string_view front_addr,
                                         FemasSimFrontConfig* out,
                                         std::string* error);

[[nodiscard]] IFemasMdApi* create_sim_femas_md_api(std::string_view front_addr);
[[nodiscard]] IFemasTraderApi* create_sim_femas_trader_api(std::string_view front_addr);

} // namespace omm

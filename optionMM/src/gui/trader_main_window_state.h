#pragma once

#include "trading.pb.h"
#include "trader_main_window_blotter_models.h"

#include <QString>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace omm::gui {

class GrpcTraderClient;

struct VolCurveSnapshot {
    uint32_t curve_id{0};
    uint32_t product_index{0};
    double expiry_t{0.0};
    std::vector<double> strikes;
    std::vector<double> vols;
};

struct InstrumentMeta {
    uint32_t instrument_id{0};
    uint32_t product_index{0};
    uint32_t underlying_id{0};
    int expiry_date{0};
    std::string code;
    std::string underlying_code;
    std::string exchange_id;
    std::string kind;
    std::string option_type;
    double strike{0.0};
    double tick_size{0.0};
    double multiplier{0.0};
};

struct SharedState {
    std::mutex mutex;
    bool connected{false};
    bool authenticated{false};
    bool login_required{false};
    std::string auth_error_text;
    omm::proto::UserInfo current_user;
    omm::proto::PortfolioGreeks portfolio;
    omm::proto::RiskState risk_state;
    std::unordered_map<uint32_t, InstrumentMeta> instruments;
    std::map<uint32_t, omm::proto::BookInfo> books;
    std::unordered_map<uint32_t, omm::proto::Tick> ticks;
    std::unordered_map<uint32_t, omm::proto::Greeks> greeks;
    std::unordered_map<uint32_t, omm::proto::Position> positions;
    std::vector<omm::proto::BookPosition> book_positions;
    std::vector<omm::proto::BookPortfolio> book_portfolios;
    std::deque<omm::proto::OrderUpdate> orders;
    std::deque<omm::proto::QuoteUpdate> quotes;
    std::deque<omm::proto::OrderUpdate> trades;
    uint64_t orders_seq{0};
    uint64_t quotes_seq{0};
    uint64_t trades_seq{0};
    std::deque<omm::proto::RiskAlert> alerts;
    std::map<uint32_t, omm::proto::MMParams> mm_params;
    std::map<uint32_t, omm::proto::ProductPricingParams> product_pricing_params;
    std::map<uint64_t, omm::proto::ArbParams> arb_params;
    std::unordered_map<uint32_t, omm::proto::ProductMMState> product_states;
    std::unordered_map<uint32_t, omm::proto::InstrumentMMState> instrument_states;
    std::map<uint64_t, omm::proto::ArbStrategyState> arb_strategy_states;
    std::vector<omm::proto::PcpOpportunityState> pcp_opportunities;
    std::map<uint32_t, VolCurveSnapshot> curves;
};

struct TraderMainWindow::Impl {
    SharedState state;
    std::unique_ptr<GrpcTraderClient> client;
    uint32_t selected_product_index{0};
    uint32_t selected_instrument_id{0};
    uint32_t selected_manual_book_id{0};
    uint32_t selected_order_depth_instrument_id{0};
    uint32_t selected_order_depth_book_id{0};
    uint32_t selected_pms_book_id{0};
    uint32_t selected_pms_product_index{0xFFFFFFFFu};
    int selected_arb_strategy_type{static_cast<int>(omm::proto::ARB_STRATEGY_NONE)};
    uint32_t selected_pcp_call_id{0};
    uint32_t selected_pcp_put_id{0};
    uint32_t selected_pcp_future_id{0};
    bool ui_state_restored{false};
    bool login_dialog_open{false};
    uint32_t params_editor_product_index{0xFFFFFFFFu};
    QString last_risk_action_text{"Risk thresholds not updated yet"};
    QString last_operator_status_text{"Waiting for login"};
    QString last_order_depth_message{"Waiting for order depth"};
    QString last_login_username;
    OrderBlotterModel* order_blotter_model{nullptr};
    QuoteBlotterModel* quote_blotter_model{nullptr};
    TradeBlotterModel* trade_blotter_model{nullptr};
    uint64_t displayed_trade_seq{0};
    uint32_t displayed_trade_product_index{0xFFFFFFFFu};
};

} // namespace omm::gui

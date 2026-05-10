#include "gui/trader_main_window.h"
#include "trader_main_window_ui_helpers.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace omm::gui {

/**
 * @brief Implements Build ticket panel.
 * @return Return value produced by the operation.
 */
QDockWidget* TraderMainWindow::build_ticket_panel() {
    auto* ticket_dock = new QDockWidget("Ticket", this);
    ticket_dock->setObjectName("ticketDock");
    ticket_dock->setFeatures(QDockWidget::DockWidgetMovable |
                             QDockWidget::DockWidgetFloatable |
                             QDockWidget::DockWidgetClosable);

    auto* ticket_panel = new QWidget();
    ticket_panel->setMinimumWidth(380);
    auto* ticket_layout = new QVBoxLayout(ticket_panel);
    ticket_layout->setContentsMargins(8, 8, 8, 8);
    ticket_layout->setSpacing(8);

    auto* order_box = new QGroupBox("Manual Order Ticket");
    auto* order_layout = new QGridLayout(order_box);
    order_layout->addWidget(new QLabel("Instrument"), 0, 0);
    instrument_selector_ = new QComboBox();
    order_layout->addWidget(instrument_selector_, 0, 1, 1, 2);
    order_layout->addWidget(new QLabel("Side"), 1, 0);
    side_selector_ = new QComboBox();
    side_selector_->addItems({"buy", "sell"});
    order_layout->addWidget(side_selector_, 1, 1, 1, 2);
    order_layout->addWidget(new QLabel("Book"), 2, 0);
    manual_book_selector_ = new QComboBox();
    order_layout->addWidget(manual_book_selector_, 2, 1, 1, 2);
    order_layout->addWidget(new QLabel("Price"), 3, 0);
    price_editor_ = new QDoubleSpinBox();
    price_editor_->setDecimals(4);
    price_editor_->setMaximum(1000000.0);
    order_layout->addWidget(price_editor_, 3, 1, 1, 2);
    order_layout->addWidget(new QLabel("Volume"), 4, 0);
    volume_editor_ = new QSpinBox();
    volume_editor_->setRange(1, 100000);
    volume_editor_->setValue(5);
    order_layout->addWidget(volume_editor_, 4, 1, 1, 2);
    buy_button_ = new QPushButton("Send Buy");
    sell_button_ = new QPushButton("Send Sell");
    order_layout->addWidget(buy_button_, 5, 0, 1, 3);
    order_layout->addWidget(sell_button_, 6, 0, 1, 3);
    ticket_layout->addWidget(order_box);

    ticket_layout->addStretch(1);

    ticket_dock->setWidget(ticket_panel);
    addDockWidget(Qt::RightDockWidgetArea, ticket_dock);
    prepare_floating_panel(ticket_dock, 430, 520);
    return ticket_dock;
}

/**
 * @brief Implements Build parameters panel.
 * @return Return value produced by the operation.
 */
QDockWidget* TraderMainWindow::build_parameters_panel() {
    auto* parameters_dock = new QDockWidget("Parameters", this);
    parameters_dock->setObjectName("parametersDock");
    parameters_dock->setFeatures(QDockWidget::DockWidgetMovable |
                                 QDockWidget::DockWidgetFloatable |
                                 QDockWidget::DockWidgetClosable);

    auto* parameters_panel = new QWidget();
    parameters_panel->setMinimumWidth(430);
    auto* parameters_layout = new QVBoxLayout(parameters_panel);
    parameters_layout->setContentsMargins(8, 8, 8, 8);
    parameters_layout->setSpacing(8);

    auto configure_double = [](QDoubleSpinBox* box,
                               int decimals,
                               double min_value,
                               double max_value,
                               double step) {
        box->setDecimals(decimals);
        box->setRange(min_value, max_value);
        box->setSingleStep(step);
    };
    auto configure_int = [](QSpinBox* box, int min_value, int max_value) {
        box->setRange(min_value, max_value);
    };

    auto* selection_box = new QGroupBox("Parameter Editor Status");
    auto* selection_layout = new QVBoxLayout(selection_box);
    selection_layout->setContentsMargins(8, 8, 8, 8);
    selection_layout->setSpacing(6);
    auto* params_state_row = new QHBoxLayout();
    params_state_row->addWidget(new QLabel("Param Editor"));
    params_state_label_ = new QLabel("No live params");
    params_state_label_->setAlignment(Qt::AlignCenter);
    style_pill(params_state_label_, QColor("#ececec"));
    params_state_row->addWidget(params_state_label_, 1);
    selection_layout->addLayout(params_state_row);
    parameters_layout->addWidget(selection_box);

    auto* params_box = new QWidget();
    auto* params_root_layout = new QVBoxLayout(params_box);
    params_root_layout->setContentsMargins(0, 0, 0, 0);
    params_root_layout->setSpacing(8);

    auto* params_hint = new QLabel(
        "MM controls are grouped by how traders think: shape the quote, manage inventory, then hedge and gate.");
    params_hint->setWordWrap(true);
    params_hint->setStyleSheet("padding:4px 2px; color:#6b5a3f;");
    params_root_layout->addWidget(params_hint);

    params_tabs_ = new QTabWidget();

    auto* quote_shape_tab = new QWidget();
    auto* quote_shape_layout = new QGridLayout(quote_shape_tab);
    quote_shape_layout->addWidget(new QLabel("Base Half"), 0, 0);
    bid_spread_editor_ = new QDoubleSpinBox();
    configure_double(bid_spread_editor_, 4, 0.0, 9999.0, 0.1);
    ask_spread_editor_ = new QDoubleSpinBox();
    configure_double(ask_spread_editor_, 4, 0.0, 9999.0, 0.1);
    base_half_spread_editor_ = new QDoubleSpinBox();
    configure_double(base_half_spread_editor_, 4, 0.0, 9999.0, 0.1);
    quote_shape_layout->addWidget(base_half_spread_editor_, 0, 1);
    quote_shape_layout->addWidget(new QLabel("Min Half"), 0, 2);
    min_half_spread_editor_ = new QDoubleSpinBox();
    configure_double(min_half_spread_editor_, 4, 0.0, 9999.0, 0.1);
    quote_shape_layout->addWidget(min_half_spread_editor_, 0, 3);
    quote_shape_layout->addWidget(new QLabel("Max Half"), 1, 0);
    max_half_spread_editor_ = new QDoubleSpinBox();
    configure_double(max_half_spread_editor_, 4, 0.0, 9999.0, 0.1);
    quote_shape_layout->addWidget(max_half_spread_editor_, 1, 1);
    quote_shape_layout->addWidget(new QLabel("Follow Weight"), 1, 2);
    follow_weight_editor_ = new QDoubleSpinBox();
    configure_double(follow_weight_editor_, 4, 0.0, 1.0, 0.05);
    quote_shape_layout->addWidget(follow_weight_editor_, 1, 3);
    quote_shape_layout->addWidget(new QLabel("Mkt Width Widen"), 2, 0);
    market_width_widen_editor_ = new QDoubleSpinBox();
    configure_double(market_width_widen_editor_, 4, 0.0, 9999.0, 0.1);
    quote_shape_layout->addWidget(market_width_widen_editor_, 2, 1);
    params_tabs_->addTab(quote_shape_tab, "Quote Shape");

    auto* inventory_tab = new QWidget();
    auto* inventory_layout = new QGridLayout(inventory_tab);
    inventory_layout->addWidget(new QLabel("Quote Volume"), 0, 0);
    quote_volume_editor_ = new QSpinBox();
    configure_int(quote_volume_editor_, 0, 100000);
    inventory_layout->addWidget(quote_volume_editor_, 0, 1);
    inventory_layout->addWidget(new QLabel("Warning Pos"), 0, 2);
    warning_position_editor_ = new QSpinBox();
    configure_int(warning_position_editor_, 1, 1000000);
    inventory_layout->addWidget(warning_position_editor_, 0, 3);
    inventory_layout->addWidget(new QLabel("Max Position"), 1, 0);
    max_position_editor_ = new QSpinBox();
    configure_int(max_position_editor_, 1, 1000000);
    inventory_layout->addWidget(max_position_editor_, 1, 1);
    inventory_layout->addWidget(new QLabel("Inv Skew / Lot"), 1, 2);
    inventory_skew_editor_ = new QDoubleSpinBox();
    configure_double(inventory_skew_editor_, 4, -9999.0, 9999.0, 0.01);
    inventory_layout->addWidget(inventory_skew_editor_, 1, 3);
    use_one_sided_editor_ = new QCheckBox("One-Sided At Limits");
    inventory_layout->addWidget(use_one_sided_editor_, 2, 0, 1, 2);
    params_tabs_->addTab(inventory_tab, "Inventory");

    auto* requote_tab = new QWidget();
    auto* requote_layout = new QGridLayout(requote_tab);
    requote_layout->addWidget(new QLabel("Requote Eps"), 0, 0);
    requote_epsilon_editor_ = new QDoubleSpinBox();
    configure_double(requote_epsilon_editor_, 4, 0.0, 9999.0, 0.1);
    requote_layout->addWidget(requote_epsilon_editor_, 0, 1);
    requote_layout->addWidget(new QLabel("Min Quote ms"), 0, 2);
    min_quote_interval_editor_ = new QDoubleSpinBox();
    configure_double(min_quote_interval_editor_, 3, 0.0, 60000.0, 1.0);
    requote_layout->addWidget(min_quote_interval_editor_, 0, 3);
    params_tabs_->addTab(requote_tab, "Requote");

    auto* hedge_tab = new QWidget();
    auto* hedge_layout = new QGridLayout(hedge_tab);
    hedge_layout->addWidget(new QLabel("Product Delta"), 0, 0);
    product_delta_threshold_editor_ = new QDoubleSpinBox();
    configure_double(product_delta_threshold_editor_, 4, 0.0, 999999.0, 1.0);
    hedge_layout->addWidget(product_delta_threshold_editor_, 0, 1);
    hedge_layout->addWidget(new QLabel("Product Vega"), 0, 2);
    product_vega_threshold_editor_ = new QDoubleSpinBox();
    configure_double(product_vega_threshold_editor_, 4, 0.0, 99999999.0, 10.0);
    hedge_layout->addWidget(product_vega_threshold_editor_, 0, 3);
    hedge_layout->addWidget(new QLabel("Underly Shock"), 1, 0);
    underlying_move_widen_editor_ = new QDoubleSpinBox();
    configure_double(underlying_move_widen_editor_, 4, 0.0, 9999.0, 0.1);
    hedge_layout->addWidget(underlying_move_widen_editor_, 1, 1);
    strategy_enabled_editor_ = new QCheckBox("Enabled");
    hedge_layout->addWidget(strategy_enabled_editor_, 1, 2, 1, 2);
    params_tabs_->addTab(hedge_tab, "Hedge / Gate");

    auto* advanced_tab = new QWidget();
    auto* advanced_layout = new QGridLayout(advanced_tab);
    advanced_layout->addWidget(new QLabel("Legacy Bid Spread"), 0, 0);
    advanced_layout->addWidget(bid_spread_editor_, 0, 1);
    advanced_layout->addWidget(new QLabel("Legacy Ask Spread"), 1, 0);
    advanced_layout->addWidget(ask_spread_editor_, 1, 1);
    params_tabs_->addTab(advanced_tab, "Advanced");

    params_root_layout->addWidget(params_tabs_);
    auto* params_actions_layout = new QGridLayout();
    reset_params_button_ = new QPushButton("Reset");
    revert_params_button_ = new QPushButton("Revert to Live");
    apply_params_button_ = new QPushButton("Apply Params");
    params_actions_layout->addWidget(reset_params_button_, 0, 0);
    params_actions_layout->addWidget(revert_params_button_, 0, 1);
    params_actions_layout->addWidget(apply_params_button_, 0, 2);
    params_root_layout->addLayout(params_actions_layout);

    auto* risk_box = new QGroupBox("Soft Risk Thresholds");
    auto* risk_layout = new QGridLayout(risk_box);
    risk_layout->addWidget(new QLabel("Soft Pos"), 0, 0);
    soft_position_limit_editor_ = new QSpinBox();
    configure_int(soft_position_limit_editor_, 1, 10000000);
    risk_layout->addWidget(soft_position_limit_editor_, 0, 1);
    risk_layout->addWidget(new QLabel("Soft Delta"), 1, 0);
    soft_delta_limit_editor_ = new QDoubleSpinBox();
    configure_double(soft_delta_limit_editor_, 2, 0.0, 1e9, 10.0);
    risk_layout->addWidget(soft_delta_limit_editor_, 1, 1);
    risk_layout->addWidget(new QLabel("Soft Gamma"), 2, 0);
    soft_gamma_limit_editor_ = new QDoubleSpinBox();
    configure_double(soft_gamma_limit_editor_, 2, 0.0, 1e9, 10.0);
    risk_layout->addWidget(soft_gamma_limit_editor_, 2, 1);
    risk_layout->addWidget(new QLabel("Soft Vega"), 3, 0);
    soft_vega_limit_editor_ = new QDoubleSpinBox();
    configure_double(soft_vega_limit_editor_, 2, 0.0, 1e9, 100.0);
    risk_layout->addWidget(soft_vega_limit_editor_, 3, 1);
    apply_risk_button_ = new QPushButton("Apply Risk Limits");
    risk_layout->addWidget(apply_risk_button_, 4, 0, 1, 2);
    risk_action_label_ = new QLabel("Risk thresholds not updated yet");
    risk_action_label_->setWordWrap(true);
    risk_action_label_->setStyleSheet(
        "padding:4px 8px; border-radius:8px; background:#f3f0e7; color:#4a4032;");
    risk_layout->addWidget(risk_action_label_, 5, 0, 1, 2);

    auto* parameters_tabs = new QTabWidget();
    parameters_tabs->setDocumentMode(true);

    auto* params_tab = new QWidget();
    auto* params_tab_layout = new QVBoxLayout(params_tab);
    params_tab_layout->setContentsMargins(0, 0, 0, 0);
    params_tab_layout->setSpacing(8);
    params_tab_layout->addWidget(params_box);
    parameters_tabs->addTab(params_tab, "MM Params");

    auto* risk_tab = new QWidget();
    auto* risk_tab_layout = new QVBoxLayout(risk_tab);
    risk_tab_layout->setContentsMargins(0, 0, 0, 0);
    risk_tab_layout->setSpacing(8);
    risk_tab_layout->addWidget(risk_box);
    risk_tab_layout->addStretch(1);
    parameters_tabs->addTab(risk_tab, "Risk");

    parameters_layout->addWidget(parameters_tabs, 1);
    parameters_dock->setWidget(parameters_panel);
    addDockWidget(Qt::RightDockWidgetArea, parameters_dock);
    prepare_floating_panel(parameters_dock, 560, 720);
    return parameters_dock;
}

} // namespace omm::gui

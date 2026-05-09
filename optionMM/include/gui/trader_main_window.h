#pragma once

#include <QMainWindow>

#include <memory>
#include <string>

class QDockWidget;
class QCloseEvent;
class QLabel;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QComboBox;
class QMainWindow;
class QPushButton;
class QTableView;
class QTableWidget;
class QTabWidget;
class QTreeWidget;
class QWidget;
class QAction;

namespace omm::proto {
class MMParams;
}

namespace omm::gui {

class TraderMainWindow final : public QMainWindow {
public:
    explicit TraderMainWindow(std::string grpc_endpoint, QWidget* parent = nullptr);
    ~TraderMainWindow() override;
    [[nodiscard]] bool initialize_session();

private:
    void closeEvent(QCloseEvent* event) override;
    void build_ui();
    void build_main_workspace_panel();
    QDockWidget* build_vol_curves_panel();
    QDockWidget* build_parameters_panel();
    QDockWidget* build_ticket_panel();
    QDockWidget* build_arbitrage_panel();
    QDockWidget* build_positions_panel();
    void connect_primary_interactions();
    void connect_panel_interactions();
    void show_floating_panel(QDockWidget* panel);
    void prepare_floating_panel(QDockWidget* panel, int width, int height);
    void update_blotter_cancel_controls(int tab_index);
    QWidget* create_vol_curve_grid_widget(QWidget* parent = nullptr);
    void refresh_ui();
    [[nodiscard]] bool prompt_for_login(const QString& reason = {});
    void perform_logout();
    void send_manual_order();
    void start_strategy(bool enabled);
    void start_arb_strategy(bool enabled);
    void apply_strategy_params();
    void reset_strategy_params_to_defaults();
    void revert_strategy_params_to_live();
    void apply_risk_thresholds();
    void show_instrument_panel();
    void cancel_selected_order();
    void cancel_selected_product_orders();
    void cancel_selected_quote();
    void cancel_selected_product_quotes();
    void ensure_vol_window();
    void restore_ui_state();
    void save_ui_state() const;
    void load_strategy_params_into_editors(const omm::proto::MMParams& params);
    omm::proto::MMParams collect_strategy_params_from_editors() const;

    std::string grpc_endpoint_;

    struct Impl;
    std::unique_ptr<Impl> impl_;

    QLabel* connection_label_{nullptr};
    QLabel* status_label_{nullptr};
    QLabel* user_label_{nullptr};
    QLabel* desk_state_label_{nullptr};
    QLabel* global_risk_label_{nullptr};
    QLabel* alert_banner_label_{nullptr};
    QComboBox* product_selector_{nullptr};
    QComboBox* instrument_selector_{nullptr};
    QComboBox* manual_book_selector_{nullptr};
    QComboBox* side_selector_{nullptr};
    QDoubleSpinBox* price_editor_{nullptr};
    QSpinBox* volume_editor_{nullptr};
    QPushButton* buy_button_{nullptr};
    QPushButton* sell_button_{nullptr};
    QPushButton* start_button_{nullptr};
    QPushButton* stop_button_{nullptr};
    QComboBox* arb_strategy_selector_{nullptr};
    QPushButton* arb_start_button_{nullptr};
    QPushButton* arb_stop_button_{nullptr};
    QLabel* arb_status_label_{nullptr};
    QLabel* arb_details_label_{nullptr};
    QLabel* arb_summary_label_{nullptr};
    QLabel* arb_legend_label_{nullptr};
    QTableWidget* arb_opportunity_table_{nullptr};
    QLabel* delta_label_{nullptr};
    QLabel* gamma_label_{nullptr};
    QLabel* vega_label_{nullptr};
    QDoubleSpinBox* bid_spread_editor_{nullptr};
    QDoubleSpinBox* ask_spread_editor_{nullptr};
    QDoubleSpinBox* base_half_spread_editor_{nullptr};
    QDoubleSpinBox* min_half_spread_editor_{nullptr};
    QDoubleSpinBox* max_half_spread_editor_{nullptr};
    QDoubleSpinBox* follow_weight_editor_{nullptr};
    QDoubleSpinBox* inventory_skew_editor_{nullptr};
    QDoubleSpinBox* market_width_widen_editor_{nullptr};
    QDoubleSpinBox* product_delta_threshold_editor_{nullptr};
    QDoubleSpinBox* product_vega_threshold_editor_{nullptr};
    QDoubleSpinBox* requote_epsilon_editor_{nullptr};
    QDoubleSpinBox* min_quote_interval_editor_{nullptr};
    QDoubleSpinBox* underlying_move_widen_editor_{nullptr};
    QSpinBox* quote_volume_editor_{nullptr};
    QSpinBox* warning_position_editor_{nullptr};
    QSpinBox* max_position_editor_{nullptr};
    QCheckBox* strategy_enabled_editor_{nullptr};
    QCheckBox* use_one_sided_editor_{nullptr};
    QLabel* strategy_status_label_{nullptr};
    QLabel* product_gate_label_{nullptr};
    QLabel* params_state_label_{nullptr};
    QTabWidget* params_tabs_{nullptr};
    QPushButton* apply_params_button_{nullptr};
    QPushButton* reset_params_button_{nullptr};
    QPushButton* revert_params_button_{nullptr};
    QSpinBox* soft_position_limit_editor_{nullptr};
    QDoubleSpinBox* soft_delta_limit_editor_{nullptr};
    QDoubleSpinBox* soft_gamma_limit_editor_{nullptr};
    QDoubleSpinBox* soft_vega_limit_editor_{nullptr};
    QPushButton* apply_risk_button_{nullptr};
    QLabel* risk_action_label_{nullptr};
    QPushButton* cancel_selected_order_button_{nullptr};
    QPushButton* cancel_product_orders_button_{nullptr};
    QPushButton* cancel_selected_quote_button_{nullptr};
    QPushButton* cancel_product_quotes_button_{nullptr};
    QLabel* execution_status_label_{nullptr};
    QLabel* pms_gate_label_{nullptr};
    QLabel* pms_greeks_label_{nullptr};
    QLabel* pms_limits_label_{nullptr};
    QLabel* pms_counts_label_{nullptr};
    QLabel* pms_alert_label_{nullptr};
    QComboBox* pms_book_filter_{nullptr};
    QComboBox* pms_product_filter_{nullptr};
    QTableWidget* t_table_{nullptr};
    QTreeWidget* positions_tree_{nullptr};
    QTableView* orders_table_{nullptr};
    QTableView* quotes_table_{nullptr};
    QTableView* trades_table_{nullptr};
    QTableWidget* alerts_table_{nullptr};
    QTabWidget* blotter_tabs_{nullptr};
    QWidget* order_cancel_panel_{nullptr};
    QWidget* quote_cancel_panel_{nullptr};
    QWidget* vol_widget_{nullptr};
    QDockWidget* vol_dock_{nullptr};
    QDockWidget* pms_dock_{nullptr};
    QDockWidget* parameters_dock_{nullptr};
    QDockWidget* ticket_dock_{nullptr};
    QDockWidget* arbitrage_dock_{nullptr};
    QMainWindow* vol_window_{nullptr};
    QWidget* vol_window_widget_{nullptr};
    QWidget* secondary_vol_widget_{nullptr};
    QAction* logout_action_{nullptr};
    QAction* instrument_action_{nullptr};
    QAction* pms_action_{nullptr};
    QAction* vol_action_{nullptr};
    QAction* parameters_action_{nullptr};
    QAction* ticket_action_{nullptr};
    QAction* arbitrage_action_{nullptr};
    QLabel* secondary_gate_label_{nullptr};
    QLabel* secondary_greeks_label_{nullptr};
    QLabel* secondary_limits_label_{nullptr};
    QLabel* secondary_counts_label_{nullptr};
    QLabel* secondary_alert_label_{nullptr};
    QTableWidget* secondary_risk_table_{nullptr};
    QTableWidget* secondary_alerts_table_{nullptr};
};

} // namespace omm::gui

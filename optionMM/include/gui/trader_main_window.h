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
class QTableWidget;
class QTreeWidget;
class QWidget;

namespace omm::gui {

class TraderMainWindow final : public QMainWindow {
public:
    explicit TraderMainWindow(std::string grpc_endpoint, QWidget* parent = nullptr);
    ~TraderMainWindow() override;

private:
    void closeEvent(QCloseEvent* event) override;
    void build_ui();
    void refresh_ui();
    void send_manual_order();
    void start_strategy(bool enabled);
    void apply_strategy_params();
    void ensure_vol_window();
    void restore_ui_state();
    void save_ui_state() const;

    std::string grpc_endpoint_;

    struct Impl;
    std::unique_ptr<Impl> impl_;

    QLabel* status_label_{nullptr};
    QComboBox* product_selector_{nullptr};
    QComboBox* instrument_selector_{nullptr};
    QComboBox* side_selector_{nullptr};
    QDoubleSpinBox* price_editor_{nullptr};
    QSpinBox* volume_editor_{nullptr};
    QPushButton* buy_button_{nullptr};
    QPushButton* sell_button_{nullptr};
    QPushButton* start_button_{nullptr};
    QPushButton* stop_button_{nullptr};
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
    QDoubleSpinBox* hedge_threshold_editor_{nullptr};
    QDoubleSpinBox* product_vega_threshold_editor_{nullptr};
    QDoubleSpinBox* requote_epsilon_editor_{nullptr};
    QDoubleSpinBox* min_quote_interval_editor_{nullptr};
    QDoubleSpinBox* underlying_move_widen_editor_{nullptr};
    QSpinBox* quote_volume_editor_{nullptr};
    QSpinBox* warning_position_editor_{nullptr};
    QSpinBox* max_position_editor_{nullptr};
    QCheckBox* use_one_sided_editor_{nullptr};
    QPushButton* apply_params_button_{nullptr};
    QTableWidget* t_table_{nullptr};
    QTreeWidget* positions_tree_{nullptr};
    QTableWidget* orders_table_{nullptr};
    QTableWidget* quotes_table_{nullptr};
    QTableWidget* trades_table_{nullptr};
    QWidget* vol_widget_{nullptr};
    QDockWidget* vol_dock_{nullptr};
    QMainWindow* vol_window_{nullptr};
    QWidget* vol_window_widget_{nullptr};
};

} // namespace omm::gui

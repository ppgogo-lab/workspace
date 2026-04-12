#pragma once

#include <QMainWindow>

#include <memory>
#include <string>

class QDockWidget;
class QLabel;
class QSpinBox;
class QDoubleSpinBox;
class QComboBox;
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
    void build_ui();
    void refresh_ui();
    void send_manual_order();
    void start_strategy(bool enabled);
    void apply_strategy_params();

    std::string grpc_endpoint_;

    struct Impl;
    std::unique_ptr<Impl> impl_;

    QLabel* status_label_{nullptr};
    QSpinBox* product_selector_{nullptr};
    QComboBox* instrument_selector_{nullptr};
    QComboBox* side_selector_{nullptr};
    QDoubleSpinBox* price_editor_{nullptr};
    QSpinBox* volume_editor_{nullptr};
    QPushButton* buy_button_{nullptr};
    QPushButton* sell_button_{nullptr};
    QPushButton* start_button_{nullptr};
    QPushButton* stop_button_{nullptr};
    QDoubleSpinBox* bid_spread_editor_{nullptr};
    QDoubleSpinBox* ask_spread_editor_{nullptr};
    QDoubleSpinBox* hedge_threshold_editor_{nullptr};
    QSpinBox* quote_volume_editor_{nullptr};
    QSpinBox* max_position_editor_{nullptr};
    QPushButton* apply_params_button_{nullptr};
    QTableWidget* t_table_{nullptr};
    QTreeWidget* positions_tree_{nullptr};
    QTableWidget* orders_table_{nullptr};
    QTableWidget* quotes_table_{nullptr};
    QTableWidget* trades_table_{nullptr};
    QWidget* vol_widget_{nullptr};
};

} // namespace omm::gui

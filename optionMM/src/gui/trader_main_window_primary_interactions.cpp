#include "gui/trader_main_window.h"
#include "trader_main_window_state.h"

#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTimer>
#include <QVariant>

namespace omm::gui {

/**
 * @brief Implements Connect primary interactions.
 * @return None.
 */
void TraderMainWindow::connect_primary_interactions() {
    connect(buy_button_, &QPushButton::clicked, this, [this] {
        side_selector_->setCurrentText("buy");
        send_manual_order();
    });
    connect(sell_button_, &QPushButton::clicked, this, [this] {
        side_selector_->setCurrentText("sell");
        send_manual_order();
    });
    connect(start_button_, &QPushButton::clicked, this, [this] { start_strategy(true); });
    connect(stop_button_, &QPushButton::clicked, this, [this] { start_strategy(false); });
    connect(arb_start_button_, &QPushButton::clicked, this, [this] { start_arb_strategy(true); });
    connect(arb_stop_button_, &QPushButton::clicked, this, [this] { start_arb_strategy(false); });
    connect(apply_params_button_, &QPushButton::clicked, this, [this] { apply_strategy_params(); });
    connect(cancel_selected_order_button_, &QPushButton::clicked, this, [this] {
        cancel_selected_order();
    });
    connect(cancel_product_orders_button_, &QPushButton::clicked, this, [this] {
        cancel_selected_product_orders();
    });
    connect(cancel_selected_quote_button_, &QPushButton::clicked, this, [this] {
        cancel_selected_quote();
    });
    connect(cancel_product_quotes_button_, &QPushButton::clicked, this, [this] {
        cancel_selected_product_quotes();
    });
    connect(reset_params_button_, &QPushButton::clicked, this, [this] {
        reset_strategy_params_to_defaults();
    });
    connect(revert_params_button_, &QPushButton::clicked, this, [this] {
        revert_strategy_params_to_live();
    });
    connect(apply_risk_button_, &QPushButton::clicked, this, [this] { apply_risk_thresholds(); });
    connect(product_selector_, &QComboBox::currentIndexChanged, this, [this](int) {
        const QVariant data = product_selector_->currentData();
        if (data.isValid()) impl_->selected_product_index = data.toUInt();
        refresh_ui();
    });
    connect(arb_strategy_selector_, &QComboBox::currentIndexChanged, this, [this](int) {
        const QVariant data = arb_strategy_selector_->currentData();
        if (data.isValid()) impl_->selected_arb_strategy_type = data.toInt();
        refresh_ui();
    });
    connect(instrument_selector_, &QComboBox::currentIndexChanged, this, [this](int) {
        const QVariant data = instrument_selector_->currentData();
        if (data.isValid()) impl_->selected_instrument_id = data.toUInt();
    });
    connect(t_table_, &QTableWidget::cellClicked, this, [this](int row, int col) {
        auto* item = t_table_->item(row, col);
        if (item == nullptr || !item->data(Qt::UserRole).isValid()) {
            item = t_table_->item(row, 8);
        }
        if (item == nullptr) return;
        const uint32_t instrument_id = item->data(Qt::UserRole).toUInt();
        if (instrument_id == 0) return;
        const int combo_index = instrument_selector_->findData(QVariant::fromValue(instrument_id));
        if (combo_index >= 0) instrument_selector_->setCurrentIndex(combo_index);

        auto parse_price = [this, row](int price_col) {
            auto* cell = t_table_->item(row, price_col);
            return cell != nullptr ? cell->text().toDouble() : 0.0;
        };
        if (col == 3 || col == 5) {
            side_selector_->setCurrentText(col == 3 ? "sell" : "buy");
            price_editor_->setValue(parse_price(col));
        } else if (col == 11 || col == 13) {
            side_selector_->setCurrentText(col == 11 ? "sell" : "buy");
            price_editor_->setValue(parse_price(col));
        } else if (col < 7) {
            side_selector_->setCurrentText("sell");
            price_editor_->setValue(parse_price(3));
        } else if (col > 9) {
            side_selector_->setCurrentText("buy");
            price_editor_->setValue(parse_price(13));
        } else {
            price_editor_->setValue(parse_price(4));
        }
    });
}

} // namespace omm::gui

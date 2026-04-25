#include "gui/trader_main_window.h"
#include "trader_main_window_state.h"

#include <QComboBox>
#include <QLabel>
#include <QTableWidget>
#include <QVariant>

namespace omm::gui {

void TraderMainWindow::connect_panel_interactions() {
    connect(orders_table_, &QTableWidget::cellClicked, this, [this](int row, int) {
        auto* order_item = orders_table_->item(row, 0);
        auto* instrument_item = orders_table_->item(row, 1);
        if (order_item == nullptr) return;
        execution_status_label_->setText(
            QString("Selected order %1 on %2")
                .arg(order_item->text(),
                     instrument_item != nullptr ? instrument_item->text() : QString("-")));
    });
    connect(quotes_table_, &QTableWidget::cellClicked, this, [this](int row, int) {
        auto* instrument_item = quotes_table_->item(row, 0);
        auto* state_item = quotes_table_->item(row, 6);
        if (instrument_item == nullptr) return;
        execution_status_label_->setText(
            QString("Selected quote on %1 [%2]")
                .arg(instrument_item->text(),
                     state_item != nullptr ? state_item->text() : QString("-")));
    });
    connect(arb_opportunity_table_, &QTableWidget::cellClicked, this, [this](int row, int) {
        auto* row_item = arb_opportunity_table_->item(row, 0);
        if (row_item == nullptr) return;
        impl_->selected_pcp_call_id = row_item->data(Qt::UserRole).toUInt();
        impl_->selected_pcp_put_id = row_item->data(Qt::UserRole + 1).toUInt();
        impl_->selected_pcp_future_id = row_item->data(Qt::UserRole + 2).toUInt();
        if (impl_->selected_pcp_call_id != 0) {
            const int idx = instrument_selector_->findData(QVariant::fromValue(impl_->selected_pcp_call_id));
            if (idx >= 0) instrument_selector_->setCurrentIndex(idx);
        }
    });
    connect(manual_book_selector_, &QComboBox::currentIndexChanged, this, [this](int) {
        const QVariant data = manual_book_selector_->currentData();
        impl_->selected_manual_book_id = data.isValid() ? data.toUInt() : 0u;
    });
    connect(pms_book_filter_, &QComboBox::currentIndexChanged, this, [this](int) {
        const QVariant data = pms_book_filter_->currentData();
        impl_->selected_pms_book_id = data.isValid() ? data.toUInt() : 0u;
        refresh_ui();
    });
    connect(pms_product_filter_, &QComboBox::currentIndexChanged, this, [this](int) {
        const QVariant data = pms_product_filter_->currentData();
        impl_->selected_pms_product_index = data.isValid() ? data.toUInt() : 0xFFFFFFFFu;
        refresh_ui();
    });
}

} // namespace omm::gui

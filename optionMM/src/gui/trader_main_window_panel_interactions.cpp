#include "gui/trader_main_window.h"
#include "trader_main_window_state.h"

#include <QComboBox>
#include <QLabel>
#include <QModelIndex>
#include <QTableView>
#include <QTableWidget>
#include <QVariant>

namespace omm::gui {

void TraderMainWindow::connect_panel_interactions() {
    connect(orders_table_, &QTableView::clicked, this, [this](const QModelIndex& index) {
        if (!index.isValid() || impl_->order_blotter_model == nullptr) return;
        const auto* row = impl_->order_blotter_model->row(index.row());
        if (row == nullptr) return;
        execution_status_label_->setText(
            QString("Selected order %1 on %2")
                .arg(QString::number(row->order.client_order_id()), row->instrument));
    });
    connect(quotes_table_, &QTableView::clicked, this, [this](const QModelIndex& index) {
        if (!index.isValid() || impl_->quote_blotter_model == nullptr) return;
        const auto* row = impl_->quote_blotter_model->row(index.row());
        if (row == nullptr) return;
        execution_status_label_->setText(
            QString("Selected quote on %1 [%2]")
                .arg(row->instrument, row->quote_state));
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

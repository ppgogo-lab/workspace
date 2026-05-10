#include "gui/trader_main_window.h"
#include "trader_main_window_state.h"
#include "trader_main_window_ui_helpers.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDockWidget>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>

namespace omm::gui {

/**
 * @brief Implements Build order depth panel.
 * @return Return value produced by the operation.
 */
QDockWidget* TraderMainWindow::build_order_depth_panel() {
    auto* depth_dock = new QDockWidget("OrderDepth", this);
    depth_dock->setObjectName("orderDepthDock");
    depth_dock->setFeatures(QDockWidget::DockWidgetMovable |
                            QDockWidget::DockWidgetFloatable |
                            QDockWidget::DockWidgetClosable);

    auto* panel = new QWidget();
    panel->setMinimumWidth(620);
    auto* root = new QVBoxLayout(panel);
    root->setContentsMargins(6, 8, 6, 6);
    root->setSpacing(8);

    auto* editor = new QWidget();
    auto* editor_layout = new QGridLayout(editor);
    editor_layout->setContentsMargins(0, 0, 0, 0);
    editor_layout->setHorizontalSpacing(10);
    editor_layout->setVerticalSpacing(6);

    order_depth_instrument_selector_ = new QComboBox();
    order_depth_instrument_selector_->setMinimumWidth(220);
    editor_layout->addWidget(new QLabel("Instrument"), 0, 0);
    editor_layout->addWidget(order_depth_instrument_selector_, 0, 1, 1, 2);

    order_depth_book_selector_ = new QComboBox();
    order_depth_book_selector_->setMinimumWidth(180);
    editor_layout->addWidget(new QLabel("Book"), 1, 0);
    editor_layout->addWidget(order_depth_book_selector_, 1, 1, 1, 2);

    order_depth_expand_by_selector_ = new QComboBox();
    order_depth_expand_by_selector_->addItems({"Tick", "FifthOrder"});
    editor_layout->addWidget(new QLabel("ExpandBy"), 1, 3);
    editor_layout->addWidget(order_depth_expand_by_selector_, 1, 4);

    order_depth_price_type_selector_ = new QComboBox();
    order_depth_price_type_selector_->addItem("Limit");
    order_depth_price_type_selector_->setEnabled(false);
    editor_layout->addWidget(new QLabel("PriceType"), 2, 0);
    editor_layout->addWidget(order_depth_price_type_selector_, 2, 1, 1, 2);

    order_depth_time_condition_selector_ = new QComboBox();
    order_depth_time_condition_selector_->addItems({"GFD", "FAK", "FOK"});
    editor_layout->addWidget(new QLabel("OrderType"), 2, 3);
    editor_layout->addWidget(order_depth_time_condition_selector_, 2, 4);

    order_depth_volume_editor_ = new QSpinBox();
    order_depth_volume_editor_->setRange(1, 100000);
    order_depth_volume_editor_->setValue(1);
    order_depth_volume_editor_->setMinimumWidth(120);
    editor_layout->addWidget(new QLabel("Volume"), 3, 0);
    editor_layout->addWidget(order_depth_volume_editor_, 3, 1);

    auto* quick_row = new QHBoxLayout();
    quick_row->setContentsMargins(0, 0, 0, 0);
    quick_row->setSpacing(8);
    for (int volume : {1, 5, 10, 20, 50}) {
        auto* button = new QPushButton(QString::number(volume));
        button->setFixedWidth(54);
        quick_row->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, volume] {
            if (order_depth_volume_editor_ != nullptr) order_depth_volume_editor_->setValue(volume);
        });
    }
    quick_row->addStretch(1);
    editor_layout->addLayout(quick_row, 3, 2, 1, 3);
    root->addWidget(editor);

    order_depth_table_ = make_table({"own bid", "market bid", "price", "market ask", "own ask"});
    order_depth_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    order_depth_table_->setSelectionBehavior(QAbstractItemView::SelectItems);
    order_depth_table_->setWordWrap(false);
    order_depth_table_->verticalHeader()->setDefaultSectionSize(30);
    order_depth_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    root->addWidget(order_depth_table_, 1);

    auto* cancel_row = new QHBoxLayout();
    cancel_row->setContentsMargins(0, 0, 0, 0);
    cancel_row->setSpacing(8);
    order_depth_cancel_bid_button_ = new QPushButton("Cancel Bid");
    order_depth_cancel_all_button_ = new QPushButton("Cancel All");
    order_depth_cancel_ask_button_ = new QPushButton("Cancel Ask");
    cancel_row->addWidget(order_depth_cancel_bid_button_);
    cancel_row->addWidget(order_depth_cancel_all_button_);
    cancel_row->addWidget(order_depth_cancel_ask_button_);
    root->addLayout(cancel_row);

    order_depth_message_label_ = new QLabel("Waiting for order depth");
    order_depth_message_label_->setWordWrap(true);
    order_depth_message_label_->setMinimumHeight(38);
    order_depth_message_label_->setStyleSheet("color:#d40000; font-weight:600;");
    root->addWidget(order_depth_message_label_);

    connect(order_depth_instrument_selector_, &QComboBox::currentIndexChanged, this, [this](int) {
        const QVariant data = order_depth_instrument_selector_->currentData();
        impl_->selected_order_depth_instrument_id = data.isValid() ? data.toUInt() : 0u;
        refresh_ui();
    });
    connect(order_depth_book_selector_, &QComboBox::currentIndexChanged, this, [this](int) {
        const QVariant data = order_depth_book_selector_->currentData();
        impl_->selected_order_depth_book_id = data.isValid() ? data.toUInt() : 0u;
        refresh_ui();
    });
    connect(order_depth_expand_by_selector_, &QComboBox::currentIndexChanged, this, [this](int) {
        refresh_ui();
    });
    connect(order_depth_table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int col) {
        if (col != 0 && col != 4) return;
        auto* price_item = order_depth_table_->item(row, 2);
        if (price_item == nullptr) return;
        const double price = price_item->data(Qt::UserRole).toDouble();
        if (price <= 0.0) return;
        send_order_depth_order(col == 0 ? "buy" : "sell", price);
    });
    connect(order_depth_cancel_bid_button_, &QPushButton::clicked, this, [this] {
        cancel_order_depth_orders("buy");
    });
    connect(order_depth_cancel_all_button_, &QPushButton::clicked, this, [this] {
        cancel_order_depth_orders({});
    });
    connect(order_depth_cancel_ask_button_, &QPushButton::clicked, this, [this] {
        cancel_order_depth_orders("sell");
    });

    depth_dock->setWidget(panel);
    addDockWidget(Qt::RightDockWidgetArea, depth_dock);
    prepare_floating_panel(depth_dock, 660, 900);
    return depth_dock;
}

} // namespace omm::gui

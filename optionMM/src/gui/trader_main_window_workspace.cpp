#include "gui/trader_main_window.h"
#include "trader_main_window_state.h"
#include "trader_main_window_blotter_models.h"
#include "trader_main_window_ui_helpers.h"

#include <QAbstractItemView>
#include <QAction>
#include <QComboBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenuBar>
#include <QSplitter>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace omm::gui {

void TraderMainWindow::build_main_workspace_panel() {
    auto* session_menu = menuBar()->addMenu("Session");
    logout_action_ = session_menu->addAction("Logout...");
    logout_action_->setEnabled(false);
    connect(logout_action_, &QAction::triggered, this, [this] { perform_logout(); });

    auto* central = new QWidget();
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    auto* header_panel = new QWidget();
    auto* header_layout = new QGridLayout(header_panel);
    header_layout->setContentsMargins(0, 0, 0, 0);
    header_layout->setHorizontalSpacing(8);
    header_layout->setVerticalSpacing(6);

    auto* hero = new QLabel("Desk / Risk Monitor");
    hero->setStyleSheet("font-size: 22px; font-weight: 700; color: #2b2418;");
    header_layout->addWidget(hero, 0, 0, 1, 4);

    auto* product_label = new QLabel("Product");
    product_label->setStyleSheet("font-weight: 700; color: #5d4f36;");
    header_layout->addWidget(product_label, 1, 0);
    product_selector_ = new QComboBox();
    product_selector_->setMinimumWidth(180);
    header_layout->addWidget(product_selector_, 1, 1);

    connection_label_ = new QLabel("Disconnected");
    connection_label_->setMinimumWidth(120);
    connection_label_->setAlignment(Qt::AlignCenter);
    style_pill(connection_label_, QColor("#ffb3b3"));
    header_layout->addWidget(connection_label_, 1, 2);

    status_label_ = new QLabel("Waiting for monitor data");
    status_label_->setStyleSheet(
        "padding:4px 10px; border-radius:10px; background:#f6edd4; color:#4c3d24;");
    header_layout->addWidget(status_label_, 1, 3, 1, 4);

    user_label_ = new QLabel("User --");
    user_label_->setAlignment(Qt::AlignCenter);
    style_pill(user_label_, QColor("#ececec"));
    header_layout->addWidget(user_label_, 1, 7);

    desk_state_label_ = new QLabel("Desk --");
    desk_state_label_->setAlignment(Qt::AlignCenter);
    style_pill(desk_state_label_, QColor("#ececec"));
    header_layout->addWidget(desk_state_label_, 2, 0);

    global_risk_label_ = new QLabel("Risk --");
    global_risk_label_->setAlignment(Qt::AlignCenter);
    style_pill(global_risk_label_, QColor("#ececec"));
    header_layout->addWidget(global_risk_label_, 2, 1);

    alert_banner_label_ = new QLabel("No live risk alerts");
    alert_banner_label_->setStyleSheet(
        "padding:4px 10px; border-radius:10px; background:#f3f0e7; color:#4a4032; font-weight:700;");
    header_layout->addWidget(alert_banner_label_, 2, 2, 1, 4);

    delta_label_ = new QLabel("Delta --");
    gamma_label_ = new QLabel("Gamma --");
    vega_label_ = new QLabel("Vega --");
    delta_label_->setAlignment(Qt::AlignCenter);
    gamma_label_->setAlignment(Qt::AlignCenter);
    vega_label_->setAlignment(Qt::AlignCenter);
    style_pill(delta_label_, QColor("#ececec"));
    style_pill(gamma_label_, QColor("#ececec"));
    style_pill(vega_label_, QColor("#ececec"));
    header_layout->addWidget(delta_label_, 3, 0);
    header_layout->addWidget(gamma_label_, 3, 1);
    header_layout->addWidget(vega_label_, 3, 2);
    layout->addWidget(header_panel);

    auto* desk_splitter = new QSplitter(Qt::Vertical, central);
    desk_splitter->setChildrenCollapsible(false);

    auto* quote_panel = new QWidget();
    auto* quote_layout = new QVBoxLayout(quote_panel);
    quote_layout->setContentsMargins(0, 0, 0, 0);
    quote_layout->setSpacing(6);

    auto* quote_title = new QLabel("Live Quote Board");
    quote_title->setStyleSheet("font-size: 16px; font-weight: 700; color: #2b2418;");
    quote_layout->addWidget(quote_title);

    auto* quote_hint = new QLabel(
        "Click bid / ask cells to stage orders. Quote state and suppression reasons stay visible inline.");
    quote_hint->setWordWrap(true);
    quote_hint->setStyleSheet("color:#6b5a3f; padding-left:2px;");
    quote_layout->addWidget(quote_hint);

    t_table_ = make_table({"C.Q", "C.Why", "C.BQty", "C.Bid", "C.Theo", "C.Ask", "C.AQty",
                           "Exp", "Strike", "Net",
                           "P.BQty", "P.Bid", "P.Theo", "P.Ask", "P.AQty", "P.Q", "P.Why"});
    t_table_->setShowGrid(false);
    t_table_->setWordWrap(false);
    t_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    t_table_->setMinimumHeight(420);
    auto* t_header = t_table_->horizontalHeader();
    t_header->setSectionResizeMode(QHeaderView::ResizeToContents);
    t_header->setSectionResizeMode(1, QHeaderView::Stretch);
    t_header->setSectionResizeMode(16, QHeaderView::Stretch);
    t_table_->verticalHeader()->setDefaultSectionSize(22);
    quote_layout->addWidget(t_table_, 1);
    desk_splitter->addWidget(quote_panel);

    auto configure_blotter_view = [](QTableView* table) {
        table->setAlternatingRowColors(true);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSortingEnabled(false);
        table->setWordWrap(false);
        table->verticalHeader()->setVisible(false);
        table->verticalHeader()->setDefaultSectionSize(22);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setStretchLastSection(true);
    };

    impl_->order_blotter_model = new OrderBlotterModel(this);
    impl_->quote_blotter_model = new QuoteBlotterModel(this);
    impl_->trade_blotter_model = new TradeBlotterModel(this);
    orders_table_ = new QTableView();
    quotes_table_ = new QTableView();
    trades_table_ = new QTableView();
    orders_table_->setModel(impl_->order_blotter_model);
    quotes_table_->setModel(impl_->quote_blotter_model);
    trades_table_->setModel(impl_->trade_blotter_model);
    configure_blotter_view(orders_table_);
    configure_blotter_view(quotes_table_);
    configure_blotter_view(trades_table_);
    alerts_table_ = make_table({"Ts", "Type", "Message"});
    alerts_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    alerts_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    auto* blotter_panel = new QWidget();
    auto* blotter_layout = new QVBoxLayout(blotter_panel);
    blotter_layout->setContentsMargins(0, 0, 0, 0);
    blotter_layout->setSpacing(6);

    auto* blotter_title = new QLabel("Desk Blotter");
    blotter_title->setStyleSheet("font-size: 16px; font-weight: 700; color: #2b2418;");
    blotter_layout->addWidget(blotter_title);

    auto* blotter_hint = new QLabel(
        "Orders, working quotes, fills, and alerts stay docked under the board for one-scan monitoring.");
    blotter_hint->setWordWrap(true);
    blotter_hint->setStyleSheet("color:#6b5a3f; padding-left:2px;");
    blotter_layout->addWidget(blotter_hint);

    auto* blotter_tabs = new QTabWidget();
    blotter_tabs->setDocumentMode(true);
    blotter_tabs->addTab(orders_table_, "Orders");
    blotter_tabs->addTab(quotes_table_, "Quotes");
    blotter_tabs->addTab(trades_table_, "Trades");
    blotter_tabs->addTab(alerts_table_, "Risk Alerts");
    blotter_layout->addWidget(blotter_tabs, 1);
    desk_splitter->addWidget(blotter_panel);
    desk_splitter->setStretchFactor(0, 5);
    desk_splitter->setStretchFactor(1, 2);
    layout->addWidget(desk_splitter, 1);
    setCentralWidget(central);
}

} // namespace omm::gui

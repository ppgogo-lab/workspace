#include "gui/trader_main_window.h"
#include "trader_main_window_ui_helpers.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDockWidget>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace omm::gui {

QDockWidget* TraderMainWindow::build_arbitrage_panel() {
    auto* arb_monitor_dock = new QDockWidget("Arbitrage", this);
    arb_monitor_dock->setObjectName("arbitrageDock");
    arb_monitor_dock->setFeatures(QDockWidget::DockWidgetMovable |
                                  QDockWidget::DockWidgetFloatable |
                                  QDockWidget::DockWidgetClosable);
    auto* arb_panel = new QWidget();
    auto* arb_layout = new QVBoxLayout(arb_panel);
    arb_layout->setContentsMargins(8, 8, 8, 8);
    arb_layout->setSpacing(6);

    auto* strategy_box = new QGroupBox("Arbitrage Control");
    auto* strategy_layout = new QGridLayout(strategy_box);
    strategy_layout->addWidget(new QLabel("Strategy"), 0, 0);
    arb_strategy_selector_ = new QComboBox();
    strategy_layout->addWidget(arb_strategy_selector_, 0, 1);
    arb_start_button_ = new QPushButton("Start Arb");
    arb_stop_button_ = new QPushButton("Stop Arb");
    strategy_layout->addWidget(arb_start_button_, 1, 0);
    strategy_layout->addWidget(arb_stop_button_, 1, 1);
    arb_status_label_ = new QLabel("No arbitrage strategy selected.");
    arb_status_label_->setWordWrap(true);
    arb_status_label_->setStyleSheet(
        "padding:4px 8px; border-radius:8px; background:#ececec; color:#353535; font-weight:700;");
    strategy_layout->addWidget(arb_status_label_, 2, 0, 1, 2);
    arb_details_label_ = new QLabel("Arbitrage state follows the live snapshot.");
    arb_details_label_->setWordWrap(true);
    arb_details_label_->setStyleSheet(
        "padding:4px 8px; border-radius:8px; background:#f3f0e7; color:#4a4032;");
    strategy_layout->addWidget(arb_details_label_, 3, 0, 1, 2);
    arb_layout->addWidget(strategy_box);

    arb_summary_label_ = new QLabel("Select a PCP strategy to inspect synthetic vs future opportunities.");
    arb_summary_label_->setWordWrap(true);
    arb_summary_label_->setStyleSheet(
        "padding:4px 8px; border-radius:8px; background:#f3f0e7; color:#4a4032;");
    arb_layout->addWidget(arb_summary_label_);
    arb_legend_label_ = new QLabel(
        "Rows highlight when the best executable edge meets the PCP trigger threshold. Left wing: synthetic bid vs future ask. Right wing: future bid vs synthetic ask.");
    arb_legend_label_->setWordWrap(true);
    arb_legend_label_->setStyleSheet(
        "padding:4px 8px; border-radius:8px; background:#eef6ff; color:#29415f;");
    arb_layout->addWidget(arb_legend_label_);
    arb_opportunity_table_ = make_table({
        "Synth Bid", "Fut Ask", "Syn Rich", "Exp", "Strike",
        "Fut Bid", "Synth Ask", "Fut Rich", "Best Dir", "Best Edge", "Qty", "State"
    });
    arb_opportunity_table_->setAlternatingRowColors(true);
    arb_opportunity_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    auto* arb_header = arb_opportunity_table_->horizontalHeader();
    arb_header->setSectionResizeMode(QHeaderView::Interactive);
    arb_header->setDefaultSectionSize(88);
    arb_header->setMinimumSectionSize(64);
    arb_header->setStretchLastSection(true);
    arb_opportunity_table_->setColumnWidth(8, 120);
    arb_opportunity_table_->setColumnWidth(11, 140);
    arb_opportunity_table_->setMinimumHeight(220);
    arb_layout->addWidget(arb_opportunity_table_, 1);
    arb_monitor_dock->setWidget(arb_panel);
    addDockWidget(Qt::BottomDockWidgetArea, arb_monitor_dock);
    prepare_floating_panel(arb_monitor_dock, 920, 620);
    return arb_monitor_dock;
}

} // namespace omm::gui

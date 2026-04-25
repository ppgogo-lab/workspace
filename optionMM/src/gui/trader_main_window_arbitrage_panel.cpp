#include "gui/trader_main_window.h"
#include "trader_main_window_ui_helpers.h"

#include <QAbstractItemView>
#include <QDockWidget>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace omm::gui {

QDockWidget* TraderMainWindow::build_arbitrage_panel() {
    auto* arb_monitor_dock = new QDockWidget("Arbitrage Monitor", this);
    arb_monitor_dock->setObjectName("arbitrageMonitorDock");
    arb_monitor_dock->setFeatures(QDockWidget::DockWidgetMovable |
                                  QDockWidget::DockWidgetFloatable |
                                  QDockWidget::DockWidgetClosable);
    auto* arb_panel = new QWidget();
    auto* arb_layout = new QVBoxLayout(arb_panel);
    arb_layout->setContentsMargins(8, 8, 8, 8);
    arb_layout->setSpacing(6);
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
    arb_opportunity_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    arb_opportunity_table_->horizontalHeader()->setSectionResizeMode(8, QHeaderView::Stretch);
    arb_opportunity_table_->horizontalHeader()->setSectionResizeMode(11, QHeaderView::Stretch);
    arb_opportunity_table_->setMinimumHeight(220);
    arb_layout->addWidget(arb_opportunity_table_, 1);
    arb_monitor_dock->setWidget(arb_panel);
    addDockWidget(Qt::BottomDockWidgetArea, arb_monitor_dock);
    return arb_monitor_dock;
}

} // namespace omm::gui

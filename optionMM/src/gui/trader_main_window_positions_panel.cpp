#include "gui/trader_main_window.h"
#include "trader_main_window_ui_helpers.h"

#include <QComboBox>
#include <QDockWidget>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace omm::gui {

/**
 * @brief Implements Build positions panel.
 * @return Return value produced by the operation.
 */
QDockWidget* TraderMainWindow::build_positions_panel() {
    auto* positions_panel = new QWidget();
    auto* positions_layout = new QVBoxLayout(positions_panel);
    positions_layout->setContentsMargins(6, 6, 6, 6);
    positions_layout->setSpacing(8);

    auto* pms_box = new QGroupBox("Book PMS Board");
    auto* pms_layout = new QVBoxLayout(pms_box);
    pms_layout->setContentsMargins(8, 8, 8, 8);
    pms_layout->setSpacing(6);
    auto* pms_filter_row = new QGridLayout();
    pms_filter_row->addWidget(new QLabel("Book"), 0, 0);
    pms_book_filter_ = new QComboBox();
    pms_filter_row->addWidget(pms_book_filter_, 0, 1);
    pms_filter_row->addWidget(new QLabel("Product"), 0, 2);
    pms_product_filter_ = new QComboBox();
    pms_filter_row->addWidget(pms_product_filter_, 0, 3);
    pms_layout->addLayout(pms_filter_row);
    pms_gate_label_ = new QLabel("PRODUCT --");
    pms_gate_label_->setAlignment(Qt::AlignCenter);
    style_pill(pms_gate_label_, QColor("#ececec"));
    pms_layout->addWidget(pms_gate_label_);
    pms_greeks_label_ = new QLabel("Delta --   Gamma --   Vega --");
    pms_greeks_label_->setWordWrap(true);
    pms_greeks_label_->setStyleSheet(
        "padding:4px 8px; border-radius:8px; background:#f6edd4; color:#4c3d24; font-weight:700;");
    pms_layout->addWidget(pms_greeks_label_);
    pms_limits_label_ = new QLabel("Soft limits --");
    pms_limits_label_->setWordWrap(true);
    pms_limits_label_->setStyleSheet(
        "padding:4px 8px; border-radius:8px; background:#f3f0e7; color:#4a4032;");
    pms_layout->addWidget(pms_limits_label_);
    pms_counts_label_ = new QLabel("Quoted --   Suppressed --   Positions --");
    pms_counts_label_->setWordWrap(true);
    pms_counts_label_->setStyleSheet(
        "padding:4px 8px; border-radius:8px; background:#eef6ff; color:#29415f;");
    pms_layout->addWidget(pms_counts_label_);
    pms_alert_label_ = new QLabel("Latest alert --");
    pms_alert_label_->setWordWrap(true);
    pms_alert_label_->setStyleSheet(
        "padding:4px 8px; border-radius:8px; background:#fff6dc; color:#574526;");
    pms_layout->addWidget(pms_alert_label_);
    positions_layout->addWidget(pms_box);

    positions_tree_ = new QTreeWidget();
    positions_tree_->setColumnCount(8);
    positions_tree_->setHeaderLabels({"Node", "Net", "Avg", "RPnL", "UPnL", "Delta", "Gamma", "Vega"});
    positions_tree_->setAlternatingRowColors(true);
    positions_tree_->setUniformRowHeights(true);
    positions_tree_->header()->setSectionResizeMode(QHeaderView::Interactive);
    positions_tree_->header()->setStretchLastSection(false);
    positions_tree_->header()->resizeSection(0, 280);
    for (int col = 1; col < positions_tree_->columnCount(); ++col) {
        positions_tree_->header()->resizeSection(col, 110);
    }
    positions_layout->addWidget(positions_tree_, 1);

    auto* positions_dock = new QDockWidget("Positions / Greeks", this);
    positions_dock->setObjectName("positionsGreeksDock");
    positions_dock->setFeatures(QDockWidget::DockWidgetMovable |
                                QDockWidget::DockWidgetFloatable |
                                QDockWidget::DockWidgetClosable);
    positions_panel->setMinimumWidth(320);
    positions_dock->setWidget(positions_panel);
    addDockWidget(Qt::LeftDockWidgetArea, positions_dock);
    prepare_floating_panel(positions_dock, 780, 680);
    return positions_dock;
}

} // namespace omm::gui

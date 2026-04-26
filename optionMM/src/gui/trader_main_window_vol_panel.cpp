#include "gui/trader_main_window.h"

#include <QDockWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace omm::gui {

QDockWidget* TraderMainWindow::build_vol_curves_panel() {
    vol_dock_ = new QDockWidget("ORC Wing / Vol Curves", this);
    vol_dock_->setObjectName("volCurvesDock");
    vol_dock_->setFeatures(QDockWidget::DockWidgetMovable |
                           QDockWidget::DockWidgetFloatable |
                           QDockWidget::DockWidgetClosable);
    auto* vol_panel = new QWidget();
    auto* vol_layout = new QVBoxLayout(vol_panel);
    vol_layout->setContentsMargins(8, 8, 8, 8);
    vol_widget_ = create_vol_curve_grid_widget();
    vol_layout->addWidget(vol_widget_);
    vol_dock_->setWidget(vol_panel);
    addDockWidget(Qt::RightDockWidgetArea, vol_dock_);
    ensure_vol_window();
    return vol_dock_;
}

} // namespace omm::gui

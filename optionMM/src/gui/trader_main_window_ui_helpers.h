#pragma once

#include "trading.pb.h"

#include <QColor>
#include <QStringList>

#include <cstdint>

class QLabel;
class QTableWidget;

namespace omm::gui {

QTableWidget* make_table(const QStringList& headers);
void style_pill(QLabel* label, const QColor& bg, const QColor& fg = QColor("#1d1d1d"));
uint64_t make_arb_key(uint32_t product_index,
                      omm::proto::ArbitrageStrategyType strategy_type);
QString arb_strategy_type_text(omm::proto::ArbitrageStrategyType type);
QString current_time_text();

} // namespace omm::gui

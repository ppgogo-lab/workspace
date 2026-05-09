#include "gui/trader_main_window.h"
#include "trader_main_window_state.h"
#include "trader_main_window_ui_helpers.h"

#include "trading.grpc.pb.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTime>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace omm::gui {

QTableWidget* make_table(const QStringList& headers) {
    auto* table = new QTableWidget();
    table->setColumnCount(headers.size());
    table->setHorizontalHeaderLabels(headers);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->setVisible(false);
    table->setAlternatingRowColors(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    return table;
}

void set_cell(QTableWidget* table, int row, int col, const QString& text, const QColor& bg) {
    auto* item = new QTableWidgetItem(text);
    item->setBackground(bg);
    item->setForeground(QColor("#1d1d1d"));
    item->setTextAlignment((col == 3 || col == 4 || col == 1 || col == 2 || col == 0)
        ? Qt::AlignCenter
        : Qt::AlignCenter);
    table->setItem(row, col, item);
}

QString infer_exchange_from_code(std::string_view code) {
    std::string upper;
    upper.reserve(code.size());
    for (char ch : code) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));

    auto starts = [&](std::string_view prefix) {
        return upper.rfind(prefix, 0) == 0;
    };

    if (starts("AU") || starts("AG") || starts("CU") || starts("AL") || starts("ZN")
        || starts("RB") || starts("HC") || starts("NI") || starts("SN") || starts("PB")
        || starts("SS") || starts("FU") || starts("BU") || starts("SP") || starts("BR")) {
        return "SHFE";
    }
    if (starts("IF") || starts("IH") || starts("IC") || starts("IM")
        || starts("T") || starts("TF") || starts("TS") || starts("TL")) {
        return "CFFEX";
    }
    if (starts("M") || starts("A") || starts("Y") || starts("P") || starts("C")
        || starts("CS") || starts("J") || starts("JM") || starts("I")
        || starts("EG") || starts("PP") || starts("EB") || starts("L")
        || starts("V") || starts("PG") || starts("RR")) {
        return "DCE";
    }
    if (starts("SR") || starts("CF") || starts("TA") || starts("MA") || starts("FG")
        || starts("RM") || starts("ZC") || starts("AP") || starts("CJ")
        || starts("UR") || starts("SA") || starts("PK") || starts("PF")
        || starts("SH") || starts("SF") || starts("SM") || starts("OI")) {
        return "CZCE";
    }
    return {};
}

QString format_expiry_date(int expiry_date) {
    if (expiry_date <= 0) return {};
    const int year = expiry_date / 10000;
    const int month = (expiry_date / 100) % 100;
    const int day = expiry_date % 100;
    return QString("%1-%2-%3")
        .arg(year, 4, 10, QChar('0'))
        .arg(month, 2, 10, QChar('0'))
        .arg(day, 2, 10, QChar('0'));
}

QString format_expiry_short(int expiry_date) {
    if (expiry_date <= 0) return "-";
    const int month = (expiry_date / 100) % 100;
    const int day = expiry_date % 100;
    return QString("%1/%2")
        .arg(month, 2, 10, QChar('0'))
        .arg(day, 2, 10, QChar('0'));
}

QString format_monotonic_ts(int64_t ts_ns) {
    if (ts_ns <= 0) return "-";

    static int64_t base_ts_ns = 0;
    if (base_ts_ns == 0 || ts_ns < base_ts_ns) base_ts_ns = ts_ns;

    const int64_t elapsed_ms = std::max<int64_t>(0, (ts_ns - base_ts_ns) / 1'000'000LL);
    const QTime time = QTime(0, 0).addMSecs(static_cast<int>(elapsed_ms % (24LL * 3600LL * 1000LL)));
    return QString("T+%1").arg(time.toString("hh:mm:ss.zzz"));
}

uint64_t make_arb_key(uint32_t product_index,
                      omm::proto::ArbitrageStrategyType strategy_type) {
    return (static_cast<uint64_t>(product_index) << 32)
        | static_cast<uint32_t>(strategy_type);
}

QColor risk_color(double value, double warning, double danger) {
    const double magnitude = std::abs(value);
    if (magnitude >= danger) return QColor("#ffb3b3");
    if (magnitude >= warning) return QColor("#ffe39b");
    return QColor("#dff4df");
}

QColor risk_alert_color(omm::proto::RiskAlert::AlertType type) {
    switch (type) {
    case omm::proto::RiskAlert::POSITION_BREACH:
    case omm::proto::RiskAlert::DELTA_BREACH:
    case omm::proto::RiskAlert::GAMMA_BREACH:
    case omm::proto::RiskAlert::VEGA_BREACH:
        return QColor("#ffb3b3");
    case omm::proto::RiskAlert::QUOTE_CANCEL_GIVE_UP:
        return QColor("#ffd7a8");
    }
    return QColor("#ececec");
}

QString arb_strategy_type_text(omm::proto::ArbitrageStrategyType type) {
    switch (type) {
    case omm::proto::ARB_STRATEGY_PCP:
        return "PCP";
    case omm::proto::ARB_STRATEGY_NONE:
    default:
        return "None";
    }
}

QString pcp_direction_text(omm::proto::PcpOpportunityDirection dir) {
    switch (dir) {
    case omm::proto::PCP_DIR_LONG_SYNTH_SHORT_FUTURE:
        return "LONG SYN / SHORT FUT";
    case omm::proto::PCP_DIR_SHORT_SYNTH_LONG_FUTURE:
        return "SHORT SYN / LONG FUT";
    case omm::proto::PCP_DIR_NONE:
    default:
        return "-";
    }
}

QColor pcp_direction_color(omm::proto::PcpOpportunityDirection dir, double edge_ticks) {
    if (edge_ticks <= 0.0 || dir == omm::proto::PCP_DIR_NONE) return QColor("#ececec");
    switch (dir) {
    case omm::proto::PCP_DIR_LONG_SYNTH_SHORT_FUTURE:
        return QColor("#cfe7ff");
    case omm::proto::PCP_DIR_SHORT_SYNTH_LONG_FUTURE:
        return QColor("#d6f0d5");
    case omm::proto::PCP_DIR_NONE:
    default:
        return QColor("#ececec");
    }
}

QColor blend_colors(const QColor& base, const QColor& overlay, double overlay_weight) {
    const double weight = std::clamp(overlay_weight, 0.0, 1.0);
    const auto mix = [weight](int base_channel, int overlay_channel) {
        return static_cast<int>(std::lround(
            static_cast<double>(base_channel) * (1.0 - weight)
            + static_cast<double>(overlay_channel) * weight));
    };
    return QColor(mix(base.red(), overlay.red()),
                  mix(base.green(), overlay.green()),
                  mix(base.blue(), overlay.blue()));
}

QColor pcp_row_highlight_color(omm::proto::PcpOpportunityDirection dir) {
    switch (dir) {
    case omm::proto::PCP_DIR_LONG_SYNTH_SHORT_FUTURE:
        return QColor("#cfe7ff");
    case omm::proto::PCP_DIR_SHORT_SYNTH_LONG_FUTURE:
        return QColor("#d6f0d5");
    case omm::proto::PCP_DIR_NONE:
    default:
        return QColor("#ffe39b");
    }
}

bool pcp_row_is_actionable(const omm::proto::PcpOpportunityState& row, double min_edge_ticks) {
    return row.market_valid()
        && row.best_volume() > 0
        && row.best_direction() != omm::proto::PCP_DIR_NONE
        && min_edge_ticks > 0.0
        && row.best_edge_ticks() >= min_edge_ticks;
}

QString risk_alert_type_text(omm::proto::RiskAlert::AlertType type) {
    switch (type) {
    case omm::proto::RiskAlert::POSITION_BREACH:
        return "POS";
    case omm::proto::RiskAlert::DELTA_BREACH:
        return "DELTA";
    case omm::proto::RiskAlert::GAMMA_BREACH:
        return "GAMMA";
    case omm::proto::RiskAlert::VEGA_BREACH:
        return "VEGA";
    case omm::proto::RiskAlert::QUOTE_CANCEL_GIVE_UP:
        return "CXL";
    }
    return "INFO";
}

QString mm_quote_state_text(omm::proto::MMQuoteState state) {
    switch (state) {
    case omm::proto::MM_QUOTE_LIVE:
        return "LIVE";
    case omm::proto::MM_QUOTE_ACK_PENDING:
        return "ACK";
    case omm::proto::MM_QUOTE_CANCEL_PENDING:
        return "CANCEL";
    case omm::proto::MM_QUOTE_CANCEL_FAILED:
        return "CXL FAIL";
    case omm::proto::MM_QUOTE_SUPPRESSED:
        return "SUPP";
    case omm::proto::MM_QUOTE_IDLE:
    default:
        return "IDLE";
    }
}

QColor mm_quote_state_color(omm::proto::MMQuoteState state) {
    switch (state) {
    case omm::proto::MM_QUOTE_LIVE:
        return QColor("#cdeccf");
    case omm::proto::MM_QUOTE_ACK_PENDING:
        return QColor("#cfe7ff");
    case omm::proto::MM_QUOTE_CANCEL_PENDING:
        return QColor("#ffd7a8");
    case omm::proto::MM_QUOTE_CANCEL_FAILED:
        return QColor("#ff9e9e");
    case omm::proto::MM_QUOTE_SUPPRESSED:
        return QColor("#e8e0cf");
    case omm::proto::MM_QUOTE_IDLE:
    default:
        return QColor("#ececec");
    }
}

template<typename RepeatedReasons>
bool has_suppress_reason(const RepeatedReasons& reasons, omm::proto::MMSuppressReason reason) {
    for (const int value : reasons) {
        if (value == static_cast<int>(reason)) return true;
    }
    return false;
}

template<typename RepeatedReasons>
QString suppress_reason_text(const RepeatedReasons& reasons, uint32_t cancel_attempts = 0) {
    QStringList parts;
    auto add_part = [&parts](const QString& text) {
        if (!parts.contains(text)) parts.push_back(text);
    };

    for (const int value : reasons) {
        switch (static_cast<omm::proto::MMSuppressReason>(value)) {
        case omm::proto::MM_REASON_DISABLED:
            add_part("DISABLED");
            break;
        case omm::proto::MM_REASON_SESSION_CLOSED:
            add_part("SESSION");
            break;
        case omm::proto::MM_REASON_STALE_THEO:
            add_part("STALE");
            break;
        case omm::proto::MM_REASON_INVALID_MARKET:
            add_part("MARKET");
            break;
        case omm::proto::MM_REASON_POSITION_LIMIT:
            add_part("POSITION");
            break;
        case omm::proto::MM_REASON_RISK_LIMIT:
            add_part("RISK");
            break;
        case omm::proto::MM_REASON_THROTTLE:
            add_part("THROTTLE");
            break;
        case omm::proto::MM_REASON_UNDERLYING_SHOCK:
            add_part("SHOCK");
            break;
        case omm::proto::MM_REASON_PRODUCT_EXPOSURE:
            add_part("EXPOSURE");
            break;
        case omm::proto::MM_REASON_CANCEL_STUCK:
            add_part(cancel_attempts > 0
                         ? QString("CXL %1").arg(cancel_attempts)
                         : QString("CXL FAIL"));
            break;
        case omm::proto::MM_REASON_NONE:
        default:
            break;
        }
    }
    return parts.isEmpty() ? "-" : parts.join(" / ");
}

template<typename RepeatedReasons>
QColor suppress_reason_color(const RepeatedReasons& reasons) {
    if (has_suppress_reason(reasons, omm::proto::MM_REASON_CANCEL_STUCK)
        || has_suppress_reason(reasons, omm::proto::MM_REASON_RISK_LIMIT)) {
        return QColor("#ffb3b3");
    }
    if (has_suppress_reason(reasons, omm::proto::MM_REASON_PRODUCT_EXPOSURE)
        || has_suppress_reason(reasons, omm::proto::MM_REASON_POSITION_LIMIT)
        || has_suppress_reason(reasons, omm::proto::MM_REASON_UNDERLYING_SHOCK)) {
        return QColor("#ffd7a8");
    }
    if (has_suppress_reason(reasons, omm::proto::MM_REASON_DISABLED)
        || has_suppress_reason(reasons, omm::proto::MM_REASON_SESSION_CLOSED)) {
        return QColor("#e0e0e0");
    }
    if (has_suppress_reason(reasons, omm::proto::MM_REASON_STALE_THEO)
        || has_suppress_reason(reasons, omm::proto::MM_REASON_INVALID_MARKET)
        || has_suppress_reason(reasons, omm::proto::MM_REASON_THROTTLE)) {
        return QColor("#fff0b3");
    }
    return QColor("#ececec");
}

template<typename RepeatedReasons>
bool has_arb_reason(const RepeatedReasons& reasons, omm::proto::ArbSuppressReason reason) {
    for (const int value : reasons) {
        if (value == static_cast<int>(reason)) return true;
    }
    return false;
}

template<typename RepeatedReasons>
QString arb_suppress_reason_text(const RepeatedReasons& reasons) {
    QStringList parts;
    auto add_part = [&parts](const QString& text) {
        if (!parts.contains(text)) parts.push_back(text);
    };

    for (const int value : reasons) {
        switch (static_cast<omm::proto::ArbSuppressReason>(value)) {
        case omm::proto::ARB_REASON_DISABLED:
            add_part("DISABLED");
            break;
        case omm::proto::ARB_REASON_NO_PAIRS:
            add_part("NO PAIRS");
            break;
        case omm::proto::ARB_REASON_INVALID_MARKET:
            add_part("MARKET");
            break;
        case omm::proto::ARB_REASON_COOLDOWN:
            add_part("COOLDOWN");
            break;
        case omm::proto::ARB_REASON_INTENT_BACKPRESSURE:
            add_part("INTENT");
            break;
        case omm::proto::ARB_REASON_LIVE_ORDERS:
            add_part("LIVE ORDERS");
            break;
        case omm::proto::ARB_REASON_CLEANUP_PENDING:
            add_part("CLEANUP");
            break;
        case omm::proto::ARB_REASON_NONE:
        default:
            break;
        }
    }
    return parts.isEmpty() ? "-" : parts.join(" / ");
}

template<typename RepeatedReasons>
QColor arb_suppress_reason_color(const RepeatedReasons& reasons) {
    if (has_arb_reason(reasons, omm::proto::ARB_REASON_INTENT_BACKPRESSURE)) {
        return QColor("#ffb3b3");
    }
    if (has_arb_reason(reasons, omm::proto::ARB_REASON_LIVE_ORDERS)
        || has_arb_reason(reasons, omm::proto::ARB_REASON_CLEANUP_PENDING)) {
        return QColor("#ffd7a8");
    }
    if (has_arb_reason(reasons, omm::proto::ARB_REASON_INVALID_MARKET)
        || has_arb_reason(reasons, omm::proto::ARB_REASON_COOLDOWN)) {
        return QColor("#fff0b3");
    }
    if (has_arb_reason(reasons, omm::proto::ARB_REASON_DISABLED)
        || has_arb_reason(reasons, omm::proto::ARB_REASON_NO_PAIRS)) {
        return QColor("#e0e0e0");
    }
    return QColor("#ececec");
}

void style_pill(QLabel* label, const QColor& bg, const QColor& fg) {
    if (label == nullptr) return;
    label->setStyleSheet(QString(
        "padding:4px 10px; border-radius:10px; font-weight:700; background:%1; color:%2;")
            .arg(bg.name(), fg.name()));
}

QString current_time_text() {
    return QTime::currentTime().toString("hh:mm:ss");
}

QString book_label_from_map(const std::map<uint32_t, omm::proto::BookInfo>& books, uint32_t book_id) {
    if (book_id == 0) return "-";
    auto it = books.find(book_id);
    if (it == books.end()) return QString("Book %1").arg(book_id);
    const auto& book = it->second;
    const QString display_name = QString::fromStdString(book.display_name());
    if (!display_name.isEmpty()) return display_name;
    const QString code = QString::fromStdString(book.book_code());
    return code.isEmpty() ? QString("Book %1").arg(book_id) : code;
}

bool combo_matches(QComboBox* combo,
                   const std::vector<std::pair<QString, QVariant>>& items) {
    if (combo == nullptr) return false;
    if (combo->count() != static_cast<int>(items.size())) return false;
    for (int i = 0; i < combo->count(); ++i) {
        if (combo->itemText(i) != items[static_cast<std::size_t>(i)].first) return false;
        if (combo->itemData(i) != items[static_cast<std::size_t>(i)].second) return false;
    }
    return true;
}

void TraderMainWindow::refresh_ui() {
    std::lock_guard<std::mutex> lock(impl_->state.mutex);
    const bool connected = impl_->state.connected;
    connection_label_->setText(connected ? "LIVE" : "DOWN");
    style_pill(connection_label_, connected ? QColor("#cdeccf") : QColor("#ffb3b3"));
    status_label_->setText(impl_->last_operator_status_text);
    buy_button_->setEnabled(connected);
    sell_button_->setEnabled(connected);
    start_button_->setEnabled(connected);
    stop_button_->setEnabled(connected);
    arb_strategy_selector_->setEnabled(connected);
    arb_start_button_->setEnabled(connected);
    arb_stop_button_->setEnabled(connected);
    apply_params_button_->setEnabled(connected);
    apply_risk_button_->setEnabled(connected);
    cancel_selected_order_button_->setEnabled(connected);
    cancel_product_orders_button_->setEnabled(connected);
    cancel_selected_quote_button_->setEnabled(connected);
    cancel_product_quotes_button_->setEnabled(connected);

    struct SideView {
        uint32_t instrument_id{0};
        QString code;
        double bid_px{0.0};
        int bid_qty{0};
        double theo{0.0};
        double ask_px{0.0};
        int ask_qty{0};
        int pos{0};
        omm::proto::MMQuoteState quote_state{omm::proto::MM_QUOTE_IDLE};
        QString quote_state_label{"IDLE"};
        QString reason_label{"-"};
        QColor quote_state_color{"#ececec"};
        QColor reason_color{"#ececec"};
        QColor bid_color{"#fff2a8"};
        QColor ask_color{"#ffd7b6"};
    };
    struct StrikeRow {
        int expiry_date{0};
        double strike{0.0};
        int net_pos{0};
        SideView call;
        SideView put;
    };

    std::vector<StrikeRow> rows;
    const QVariant selected_product_data = QVariant::fromValue(impl_->selected_product_index);
    std::vector<std::pair<QString, QVariant>> product_items;
    {
        std::map<uint32_t, QString> product_labels;
        for (const auto& [_, meta] : impl_->state.instruments) {
            QString label = QString("Product %1").arg(meta.product_index);
            if (meta.kind == "Future" && !meta.code.empty()) {
                label = QString::fromStdString(meta.code);
            } else if (!meta.underlying_code.empty()) {
                label = QString::fromStdString(meta.underlying_code);
            }
            auto it = product_labels.find(meta.product_index);
            if (it == product_labels.end() || (it->second.startsWith("Product ") && !label.startsWith("Product "))) {
                product_labels[meta.product_index] = label;
            }
        }
        if (product_labels.empty()) product_labels.emplace(0u, "Product 0");
        for (const auto& [product_index, label] : product_labels) {
            product_items.push_back({label, QVariant::fromValue(product_index)});
        }
    }
    if (!combo_matches(product_selector_, product_items)) {
        QSignalBlocker blocker(product_selector_);
        product_selector_->clear();
        for (const auto& [label, data] : product_items) {
            product_selector_->addItem(label, data);
        }
    }
    {
        QSignalBlocker blocker(product_selector_);
        int restore_index = product_selector_->findData(selected_product_data);
        if (restore_index >= 0) {
            product_selector_->setCurrentIndex(restore_index);
        } else if (product_selector_->count() > 0) {
            product_selector_->setCurrentIndex(0);
            impl_->selected_product_index = product_selector_->currentData().toUInt();
        }
    }
    const uint32_t selected_product = impl_->selected_product_index;

    const QVariant selected_arb_data = QVariant::fromValue(impl_->selected_arb_strategy_type);
    std::vector<std::pair<QString, QVariant>> arb_items;
    {
        std::map<int, QString> arb_labels;
        for (const auto& [key, arb_state] : impl_->state.arb_strategy_states) {
            const uint32_t product_index = static_cast<uint32_t>(key >> 32);
            if (product_index != selected_product) continue;
            arb_labels[static_cast<int>(arb_state.strategy_type())] =
                arb_strategy_type_text(arb_state.strategy_type());
        }
        for (const auto& [key, params] : impl_->state.arb_params) {
            const uint32_t product_index = static_cast<uint32_t>(key >> 32);
            if (product_index != selected_product) continue;
            const auto strategy_type =
                static_cast<omm::proto::ArbitrageStrategyType>(static_cast<uint32_t>(key));
            if (params.enabled()) {
                arb_labels[static_cast<int>(strategy_type)] = arb_strategy_type_text(strategy_type);
            } else if (arb_labels.find(static_cast<int>(strategy_type)) == arb_labels.end()) {
                arb_labels[static_cast<int>(strategy_type)] = arb_strategy_type_text(strategy_type);
            }
        }
        for (const auto& [strategy_type, label] : arb_labels) {
            if (strategy_type == static_cast<int>(omm::proto::ARB_STRATEGY_NONE)) continue;
            arb_items.push_back({label, QVariant::fromValue(strategy_type)});
        }
    }
    if (!combo_matches(arb_strategy_selector_, arb_items)) {
        QSignalBlocker blocker(arb_strategy_selector_);
        arb_strategy_selector_->clear();
        for (const auto& [label, data] : arb_items) {
            arb_strategy_selector_->addItem(label, data);
        }
    }
    {
        QSignalBlocker blocker(arb_strategy_selector_);
        int restore_index = arb_strategy_selector_->findData(selected_arb_data);
        if (restore_index >= 0) {
            arb_strategy_selector_->setCurrentIndex(restore_index);
        } else if (arb_strategy_selector_->count() > 0) {
            arb_strategy_selector_->setCurrentIndex(0);
            impl_->selected_arb_strategy_type = arb_strategy_selector_->currentData().toInt();
        } else {
            impl_->selected_arb_strategy_type = static_cast<int>(omm::proto::ARB_STRATEGY_NONE);
        }
    }

    const QVariant selected_instrument_data = QVariant::fromValue(impl_->selected_instrument_id);
    std::vector<std::pair<QString, QVariant>> instrument_items;
    {
        std::vector<const InstrumentMeta*> combo_items;
        combo_items.reserve(impl_->state.instruments.size());
        for (const auto& [_, meta] : impl_->state.instruments) {
            if (meta.product_index != selected_product) continue;
            combo_items.push_back(&meta);
        }
        std::sort(combo_items.begin(), combo_items.end(), [](const InstrumentMeta* lhs, const InstrumentMeta* rhs) {
            if (lhs->kind != rhs->kind) return lhs->kind < rhs->kind;
            if (lhs->expiry_date != rhs->expiry_date) return lhs->expiry_date < rhs->expiry_date;
            if (lhs->strike != rhs->strike) return lhs->strike < rhs->strike;
            return lhs->code < rhs->code;
        });
        std::unordered_map<std::string, int> label_counts;
        for (const InstrumentMeta* meta : combo_items) {
            std::string base_label = meta->code;
            if (meta->kind == "Future") base_label += "  Future";
            ++label_counts[base_label];
        }
        for (const InstrumentMeta* meta : combo_items) {
            QString label = QString::fromStdString(meta->code);
            if (meta->kind == "Future") {
                label = QString("%1  Future").arg(QString::fromStdString(meta->code));
            } else {
                const std::string base_label = meta->code;
                if (label_counts[base_label] > 1) {
                    const QString expiry = format_expiry_date(meta->expiry_date);
                    if (!expiry.isEmpty()) {
                        label = QString("%1  %2").arg(label, expiry);
                    } else {
                        label = QString("%1  #%2").arg(label).arg(meta->instrument_id);
                    }
                }
            }
            instrument_items.push_back({label, QVariant::fromValue(meta->instrument_id)});
        }
    }
    if (!combo_matches(instrument_selector_, instrument_items)) {
        QSignalBlocker blocker(instrument_selector_);
        instrument_selector_->clear();
        for (const auto& [label, data] : instrument_items) {
            instrument_selector_->addItem(label, data);
        }
    }
    {
        QSignalBlocker blocker(instrument_selector_);
        int restore_index = instrument_selector_->findData(selected_instrument_data);
        if (restore_index >= 0) {
            instrument_selector_->setCurrentIndex(restore_index);
            impl_->selected_instrument_id = instrument_selector_->currentData().toUInt();
        } else if (instrument_selector_->count() > 0) {
            instrument_selector_->setCurrentIndex(0);
            impl_->selected_instrument_id = instrument_selector_->currentData().toUInt();
        }
    }

    bool have_live_params = false;
    omm::proto::MMParams live_params;
    if (auto params_it = impl_->state.mm_params.find(selected_product);
        params_it != impl_->state.mm_params.end()) {
        have_live_params = true;
        live_params = params_it->second;
    }

    auto approx_equal = [](double lhs, double rhs, double eps = 1e-6) {
        return std::abs(lhs - rhs) <= eps;
    };

    bool params_dirty = false;
    if (have_live_params) {
        if (impl_->params_editor_product_index != selected_product) {
            load_strategy_params_into_editors(live_params);
            impl_->params_editor_product_index = selected_product;
        } else {
            const omm::proto::MMParams editor_params = collect_strategy_params_from_editors();
            params_dirty =
                !approx_equal(editor_params.bid_spread(), live_params.bid_spread()) ||
                !approx_equal(editor_params.ask_spread(), live_params.ask_spread()) ||
                !approx_equal(editor_params.base_half_spread_ticks(), live_params.base_half_spread_ticks()) ||
                !approx_equal(editor_params.min_half_spread_ticks(), live_params.min_half_spread_ticks()) ||
                !approx_equal(editor_params.max_half_spread_ticks(), live_params.max_half_spread_ticks()) ||
                !approx_equal(editor_params.follow_weight(), live_params.follow_weight()) ||
                !approx_equal(editor_params.inventory_skew_per_lot_ticks(),
                              live_params.inventory_skew_per_lot_ticks()) ||
                !approx_equal(editor_params.market_width_widen_threshold_ticks(),
                              live_params.market_width_widen_threshold_ticks()) ||
                !approx_equal(editor_params.product_delta_threshold(), live_params.product_delta_threshold()) ||
                !approx_equal(editor_params.product_vega_threshold(), live_params.product_vega_threshold()) ||
                editor_params.quote_volume() != live_params.quote_volume() ||
                editor_params.warning_position() != live_params.warning_position() ||
                editor_params.max_position() != live_params.max_position() ||
                !approx_equal(editor_params.requote_price_epsilon_ticks(),
                              live_params.requote_price_epsilon_ticks()) ||
                !approx_equal(editor_params.min_quote_interval_ms(), live_params.min_quote_interval_ms()) ||
                !approx_equal(editor_params.underlying_move_widen_threshold_ticks(),
                              live_params.underlying_move_widen_threshold_ticks()) ||
                editor_params.enabled() != live_params.enabled() ||
                editor_params.use_one_sided_at_limits() != live_params.use_one_sided_at_limits();
            if (!params_dirty) {
                load_strategy_params_into_editors(live_params);
            }
        }
    } else {
        impl_->params_editor_product_index = 0xFFFFFFFFu;
    }

    revert_params_button_->setEnabled(have_live_params);
    if (!have_live_params) {
        params_state_label_->setText("NO SNAPSHOT");
        style_pill(params_state_label_, QColor("#ececec"));
    } else if (params_dirty) {
        params_state_label_->setText("DIRTY");
        style_pill(params_state_label_, QColor("#ffe39b"));
    } else {
        params_state_label_->setText("LIVE");
        style_pill(params_state_label_, QColor("#cdeccf"));
    }
    auto sync_risk_int = [](QSpinBox* editor, int value) {
        if (editor != nullptr && !editor->hasFocus()) editor->setValue(value);
    };
    auto sync_risk_double = [](QDoubleSpinBox* editor, double value) {
        if (editor != nullptr && !editor->hasFocus()) editor->setValue(value);
    };
    const auto& risk_state = impl_->state.risk_state;
    if (risk_state.has_threshold()) {
        sync_risk_int(soft_position_limit_editor_,
                      static_cast<int>(risk_state.threshold().max_net_position()));
        sync_risk_double(soft_delta_limit_editor_, risk_state.threshold().max_delta());
        sync_risk_double(soft_gamma_limit_editor_, risk_state.threshold().max_gamma());
        sync_risk_double(soft_vega_limit_editor_, risk_state.threshold().max_vega());
    }
    risk_action_label_->setText(impl_->last_risk_action_text);

    const double soft_delta_limit = risk_state.has_threshold() && risk_state.threshold().max_delta() > 0.0
        ? risk_state.threshold().max_delta()
        : 2500.0;
    const double soft_gamma_limit = risk_state.has_threshold() && risk_state.threshold().max_gamma() > 0.0
        ? risk_state.threshold().max_gamma()
        : 1200.0;
    const double soft_vega_limit = risk_state.has_threshold() && risk_state.threshold().max_vega() > 0.0
        ? risk_state.threshold().max_vega()
        : 25000.0;

    delta_label_->setText(QString("Delta %1").arg(QString::number(impl_->state.portfolio.total_delta(), 'f', 1)));
    gamma_label_->setText(QString("Gamma %1").arg(QString::number(impl_->state.portfolio.total_gamma(), 'f', 1)));
    vega_label_->setText(QString("Vega %1").arg(QString::number(impl_->state.portfolio.total_vega(), 'f', 1)));
    style_pill(delta_label_, risk_color(impl_->state.portfolio.total_delta(),
                                        0.75 * soft_delta_limit, soft_delta_limit));
    style_pill(gamma_label_, risk_color(impl_->state.portfolio.total_gamma(),
                                        0.75 * soft_gamma_limit, soft_gamma_limit));
    style_pill(vega_label_, risk_color(impl_->state.portfolio.total_vega(),
                                       0.75 * soft_vega_limit, soft_vega_limit));

    const bool any_breach = risk_state.position_breach()
        || risk_state.delta_breach()
        || risk_state.gamma_breach()
        || risk_state.vega_breach();
    const bool watch_risk =
        std::abs(impl_->state.portfolio.total_delta()) >= 0.75 * soft_delta_limit
        || std::abs(impl_->state.portfolio.total_gamma()) >= 0.75 * soft_gamma_limit
        || std::abs(impl_->state.portfolio.total_vega()) >= 0.75 * soft_vega_limit;
    global_risk_label_->setText(any_breach ? "RISK BREACH" : watch_risk ? "RISK WATCH" : "RISK OK");
    style_pill(global_risk_label_,
               any_breach ? QColor("#ff8f8f") : watch_risk ? QColor("#ffe39b") : QColor("#cdeccf"));

    bool strategy_enabled = true;
    bool have_product_state = false;
    omm::proto::ProductMMState product_state;
    if (auto params_it = impl_->state.mm_params.find(selected_product);
        params_it != impl_->state.mm_params.end()) {
        strategy_enabled = params_it->second.enabled();
    }
    if (auto product_it = impl_->state.product_states.find(selected_product);
        product_it != impl_->state.product_states.end()) {
        have_product_state = true;
        product_state = product_it->second;
        strategy_enabled = product_state.strategy_enabled();
    }

    QString gate_label = "PRODUCT RUNNING";
    QColor gate_color("#cdeccf");
    if (have_product_state) {
        const QString gate_reason = suppress_reason_text(product_state.reasons());
        if (product_state.product_suppressed()) {
            gate_label = QString("PRODUCT GATED  %1").arg(gate_reason);
            gate_color = suppress_reason_color(product_state.reasons());
        } else if (!product_state.strategy_enabled()) {
            gate_label = "PRODUCT DISABLED";
            gate_color = QColor("#e0e0e0");
        } else {
            gate_label = "PRODUCT RUNNING";
            gate_color = QColor("#cdeccf");
        }
    }
    product_gate_label_->setText(gate_label);
    product_gate_label_->setStyleSheet(QString(
        "padding:4px 8px; border-radius:8px; background:%1; color:#2a261f; font-weight:700;")
            .arg(gate_color.name()));

    strategy_status_label_->setText(
        QString("Selected product %1 is %2. Param editor is %3.")
            .arg(product_selector_->currentText())
            .arg(strategy_enabled ? "enabled" : "disabled")
            .arg(have_live_params ? (params_dirty ? "dirty" : "live") : "waiting for snapshot"));
    desk_state_label_->setText(!connected ? "DISCONNECTED"
                             : !strategy_enabled ? "MM OFF"
                             : (have_product_state && product_state.product_suppressed()) ? "GATED"
                             : any_breach ? "RISK HALT"
                             : "RUNNING");
    style_pill(desk_state_label_,
               !connected ? QColor("#ffb3b3")
               : !strategy_enabled ? QColor("#e0e0e0")
               : (have_product_state && product_state.product_suppressed()) ? suppress_reason_color(product_state.reasons())
               : any_breach ? QColor("#ffb3b3")
               : QColor("#cdeccf"));

    const auto selected_arb_type =
        static_cast<omm::proto::ArbitrageStrategyType>(impl_->selected_arb_strategy_type);
    const bool have_arb_selection =
        selected_arb_type != omm::proto::ARB_STRATEGY_NONE && !arb_items.empty();
    bool have_arb_state = false;
    bool have_arb_params = false;
    omm::proto::ArbStrategyState arb_state;
    omm::proto::ArbParams arb_params;
    if (have_arb_selection) {
        const uint64_t arb_key = make_arb_key(selected_product, selected_arb_type);
        if (auto state_it = impl_->state.arb_strategy_states.find(arb_key);
            state_it != impl_->state.arb_strategy_states.end()) {
            have_arb_state = true;
            arb_state = state_it->second;
        }
        if (auto params_it = impl_->state.arb_params.find(arb_key);
            params_it != impl_->state.arb_params.end()) {
            have_arb_params = true;
            arb_params = params_it->second;
        }
    }

    if (selected_arb_type == omm::proto::ARB_STRATEGY_PCP) {
        const double edge_threshold = have_arb_params ? std::max(0.0, arb_params.min_edge_ticks()) : 0.0;
        arb_legend_label_->setText(
            edge_threshold > 0.0
                ? QString("Rows highlight when best edge >= %1 ticks. Left wing: synthetic bid vs future ask. Right wing: future bid vs synthetic ask.")
                      .arg(QString::number(edge_threshold, 'f', 2))
                : QString("Rows highlight when the best executable edge meets the PCP trigger threshold. Left wing: synthetic bid vs future ask. Right wing: future bid vs synthetic ask."));
    } else {
        arb_legend_label_->setText(
            "Select PCP to view the synthetic-vs-future T-table. Highlighting follows the live PCP trigger threshold.");
    }

    if (!have_arb_selection) {
        arb_strategy_selector_->setEnabled(connected && !arb_items.empty());
        arb_start_button_->setEnabled(false);
        arb_stop_button_->setEnabled(false);
        arb_status_label_->setText("No arbitrage strategy configured for the selected product.");
        arb_status_label_->setStyleSheet(
            "padding:4px 8px; border-radius:8px; background:#ececec; color:#353535; font-weight:700;");
        arb_details_label_->setText("Arbitrage control is idle until the selected product exposes a strategy.");
    } else {
        const bool arb_enabled = have_arb_state
            ? arb_state.enabled()
            : (have_arb_params && arb_params.enabled());
        const QString arb_name = arb_strategy_type_text(selected_arb_type);
        const QString arb_reason = have_arb_state ? arb_suppress_reason_text(arb_state.reasons())
                                                  : (arb_enabled ? "-" : "DISABLED");

        QString arb_status = QString("%1 READY").arg(arb_name);
        QColor arb_color("#cdeccf");
        if (!arb_enabled) {
            arb_status = QString("%1 STOPPED").arg(arb_name);
            arb_color = QColor("#e0e0e0");
        } else if (have_arb_state && arb_state.cleanup_active()) {
            arb_status = QString("%1 CLEANUP").arg(arb_name);
            arb_color = QColor("#ffd7a8");
        } else if (have_arb_state && arb_state.live_orders() > 0) {
            arb_status = QString("%1 WORKING").arg(arb_name);
            arb_color = QColor("#cfe7ff");
        } else if (have_arb_state && arb_reason != "-") {
            arb_status = QString("%1 GATED  %2").arg(arb_name, arb_reason);
            arb_color = arb_suppress_reason_color(arb_state.reasons());
        } else if (have_arb_state && arb_state.running()) {
            arb_status = QString("%1 RUNNING").arg(arb_name);
            arb_color = QColor("#cdeccf");
        }

        arb_strategy_selector_->setEnabled(connected);
        arb_start_button_->setEnabled(connected);
        arb_stop_button_->setEnabled(connected);
        arb_status_label_->setText(arb_status);
        arb_status_label_->setStyleSheet(QString(
            "padding:4px 8px; border-radius:8px; background:%1; color:#2a261f; font-weight:700;")
                .arg(arb_color.name()));

        const QString pair_count = have_arb_state
            ? QString::number(arb_state.pair_count())
            : QString("-");
        const QString live_orders = have_arb_state
            ? QString::number(arb_state.live_orders())
            : QString("-");
        const QString last_edge = have_arb_state
            ? QString::number(arb_state.last_edge_ticks(), 'f', 2)
            : QString("-");
        const QString last_trigger = have_arb_state
            ? QString::number(arb_state.last_trigger_edge_ticks(), 'f', 2)
            : QString("-");
        const QString eval_ts = have_arb_state
            ? format_monotonic_ts(arb_state.last_eval_ts_ns())
            : QString("-");
        arb_details_label_->setText(
            QString("%1 pairs %2 | live %3 | edge %4t | trigger %5t | eval %6 | reason %7")
                .arg(arb_name)
                .arg(pair_count)
                .arg(live_orders)
                .arg(last_edge)
                .arg(last_trigger)
                .arg(eval_ts)
                .arg(arb_reason));
    }

    std::vector<omm::proto::PcpOpportunityState> pcp_rows;
    if (selected_arb_type == omm::proto::ARB_STRATEGY_PCP) {
        for (const auto& row : impl_->state.pcp_opportunities) {
            if (row.product_index() != selected_product) continue;
            if (row.strategy_type() != selected_arb_type) continue;
            pcp_rows.push_back(row);
        }
        std::sort(pcp_rows.begin(), pcp_rows.end(),
                  [](const auto& lhs, const auto& rhs) {
                      if (lhs.expiry_date() != rhs.expiry_date()) {
                          return lhs.expiry_date() < rhs.expiry_date();
                      }
                      return lhs.strike() < rhs.strike();
                  });
    }

    if (selected_arb_type != omm::proto::ARB_STRATEGY_PCP || pcp_rows.empty()) {
        if (arb_opportunity_table_->rowCount() != 0) {
            arb_opportunity_table_->setRowCount(0);
        }
        arb_summary_label_->setText(
            selected_arb_type == omm::proto::ARB_STRATEGY_PCP
                ? QString("No live PCP opportunities are available for %1.").arg(product_selector_->currentText())
                : QString("Opportunity monitor activates when PCP is selected on %1.").arg(product_selector_->currentText()));
    } else {
        const double edge_highlight_threshold = have_arb_params
            ? std::max(0.0, arb_params.min_edge_ticks())
            : 0.0;
        int selected_pcp_row = -1;
        arb_opportunity_table_->setRowCount(static_cast<int>(pcp_rows.size()));
        for (int row = 0; row < static_cast<int>(pcp_rows.size()); ++row) {
            const auto& item = pcp_rows[static_cast<std::size_t>(row)];
            const bool actionable = pcp_row_is_actionable(item, edge_highlight_threshold);
            const QColor row_overlay = pcp_row_highlight_color(
                actionable ? item.best_direction() : omm::proto::PCP_DIR_NONE);
            const double row_tint_weight = actionable ? (item.selected() ? 0.54 : 0.34)
                                                      : (item.selected() ? 0.22 : 0.0);
            const auto tint_row = [&](const QColor& base) {
                return row_tint_weight > 0.0 ? blend_colors(base, row_overlay, row_tint_weight) : base;
            };

            const QColor future_rich_bg = tint_row(
                item.long_synth_edge_ticks() > 0.0 ? QColor("#d8ecff") : QColor("#ececec"));
            const QColor synth_rich_bg = tint_row(
                item.short_synth_edge_ticks() > 0.0 ? QColor("#dff4df") : QColor("#ececec"));
            const QColor dir_bg = tint_row(pcp_direction_color(item.best_direction(), item.best_edge_ticks()));
            QColor state_bg = item.market_valid() ? QColor("#ececec") : QColor("#ffd7a8");
            QString state_text = item.market_valid() ? "LIVE" : "MARKET";
            if (item.selected()) {
                state_text = "SELECTED";
                state_bg = QColor("#ffe39b");
            } else if (actionable) {
                state_text = "EDGE";
                state_bg = pcp_row_highlight_color(item.best_direction());
            }

            set_cell(arb_opportunity_table_, row, 0,
                     item.synthetic_bid() > 0.0 ? QString::number(item.synthetic_bid(), 'f', 2) : "-",
                     tint_row(QColor("#f4fbf4")));
            set_cell(arb_opportunity_table_, row, 1,
                     item.future_ask() > 0.0 ? QString::number(item.future_ask(), 'f', 2) : "-",
                     tint_row(QColor("#f7f7f7")));
            set_cell(arb_opportunity_table_, row, 2,
                     QString::number(item.short_synth_edge_ticks(), 'f', 2),
                     synth_rich_bg);
            set_cell(arb_opportunity_table_, row, 3,
                     format_expiry_short(item.expiry_date()),
                     tint_row(QColor("#ecf0f6")));
            set_cell(arb_opportunity_table_, row, 4,
                     QString::number(item.strike(), 'f', 0),
                     tint_row(QColor("#e2d7ff")));
            set_cell(arb_opportunity_table_, row, 5,
                     item.future_bid() > 0.0 ? QString::number(item.future_bid(), 'f', 2) : "-",
                     tint_row(QColor("#f7f7f7")));
            set_cell(arb_opportunity_table_, row, 6,
                     item.synthetic_ask() > 0.0 ? QString::number(item.synthetic_ask(), 'f', 2) : "-",
                     tint_row(QColor("#f4fbf4")));
            set_cell(arb_opportunity_table_, row, 7,
                     QString::number(item.long_synth_edge_ticks(), 'f', 2),
                     future_rich_bg);
            set_cell(arb_opportunity_table_, row, 8,
                     pcp_direction_text(item.best_direction()),
                     dir_bg);
            set_cell(arb_opportunity_table_, row, 9,
                     QString::number(item.best_edge_ticks(), 'f', 2),
                     dir_bg);
            set_cell(arb_opportunity_table_, row, 10,
                     item.best_volume() > 0 ? QString::number(item.best_volume()) : "-",
                     QColor("#f3f0e7"));
            set_cell(arb_opportunity_table_, row, 11, state_text, state_bg);

            const auto call_it = impl_->state.instruments.find(item.call_id());
            const auto put_it = impl_->state.instruments.find(item.put_id());
            const auto fut_it = impl_->state.instruments.find(item.future_id());
            const QString call_code = call_it != impl_->state.instruments.end()
                ? QString::fromStdString(call_it->second.code) : QString("-");
            const QString put_code = put_it != impl_->state.instruments.end()
                ? QString::fromStdString(put_it->second.code) : QString("-");
            const QString fut_code = fut_it != impl_->state.instruments.end()
                ? QString::fromStdString(fut_it->second.code) : QString("-");
            const QString tooltip = QString(
                "Call %1\nPut %2\nFuture %3\nDF %4\nBest edge %5 ticks\nTrigger %6 ticks\nEval %7")
                .arg(call_code)
                .arg(put_code)
                .arg(fut_code)
                .arg(QString::number(item.discount_factor(), 'f', 6))
                .arg(QString::number(item.best_edge_ticks(), 'f', 2))
                .arg(QString::number(edge_highlight_threshold, 'f', 2))
                .arg(format_monotonic_ts(item.eval_ts_ns()));
            for (int col = 0; col < arb_opportunity_table_->columnCount(); ++col) {
                if (auto* cell = arb_opportunity_table_->item(row, col); cell != nullptr) {
                    cell->setData(Qt::UserRole, QVariant::fromValue(item.call_id()));
                    cell->setData(Qt::UserRole + 1, QVariant::fromValue(item.put_id()));
                    cell->setData(Qt::UserRole + 2, QVariant::fromValue(item.future_id()));
                    cell->setToolTip(tooltip);
                    QFont font = cell->font();
                    font.setBold(actionable || item.selected());
                    cell->setFont(font);
                }
            }

            const bool restore_match =
                impl_->selected_pcp_call_id == item.call_id()
                && impl_->selected_pcp_put_id == item.put_id()
                && impl_->selected_pcp_future_id == item.future_id();
            if (restore_match || (selected_pcp_row < 0 && item.selected())) {
                selected_pcp_row = row;
            }
        }

        if (selected_pcp_row >= 0) {
            arb_opportunity_table_->setCurrentCell(selected_pcp_row, 8);
            const auto& selected_row = pcp_rows[static_cast<std::size_t>(selected_pcp_row)];
            impl_->selected_pcp_call_id = selected_row.call_id();
            impl_->selected_pcp_put_id = selected_row.put_id();
            impl_->selected_pcp_future_id = selected_row.future_id();

            const auto call_it = impl_->state.instruments.find(selected_row.call_id());
            const auto put_it = impl_->state.instruments.find(selected_row.put_id());
            const auto fut_it = impl_->state.instruments.find(selected_row.future_id());
            const QString call_code = call_it != impl_->state.instruments.end()
                ? QString::fromStdString(call_it->second.code) : QString("-");
            const QString put_code = put_it != impl_->state.instruments.end()
                ? QString::fromStdString(put_it->second.code) : QString("-");
            const QString fut_code = fut_it != impl_->state.instruments.end()
                ? QString::fromStdString(fut_it->second.code) : QString("-");

            arb_summary_label_->setText(
                QString("%1 | %2 / %3 / %4 | synth bid %5 vs fut ask %6 (%7t) | fut bid %8 vs synth ask %9 (%10t) | best %11 %12t qty %13")
                    .arg(product_selector_->currentText())
                    .arg(call_code)
                    .arg(put_code)
                    .arg(fut_code)
                    .arg(selected_row.synthetic_bid() > 0.0 ? QString::number(selected_row.synthetic_bid(), 'f', 2) : QString("-"))
                    .arg(selected_row.future_ask() > 0.0 ? QString::number(selected_row.future_ask(), 'f', 2) : QString("-"))
                    .arg(QString::number(selected_row.short_synth_edge_ticks(), 'f', 2))
                    .arg(selected_row.future_bid() > 0.0 ? QString::number(selected_row.future_bid(), 'f', 2) : QString("-"))
                    .arg(selected_row.synthetic_ask() > 0.0 ? QString::number(selected_row.synthetic_ask(), 'f', 2) : QString("-"))
                    .arg(QString::number(selected_row.long_synth_edge_ticks(), 'f', 2))
                    .arg(pcp_direction_text(selected_row.best_direction()))
                    .arg(QString::number(selected_row.best_edge_ticks(), 'f', 2))
                    .arg(selected_row.best_volume() > 0 ? QString::number(selected_row.best_volume()) : QString("-")));
        } else {
            arb_summary_label_->setText(
                QString("%1 PCP monitor loaded with %2 pairs.")
                    .arg(product_selector_->currentText())
                    .arg(pcp_rows.size()));
        }
    }

    if (!impl_->state.alerts.empty()) {
        const auto& alert = impl_->state.alerts.front();
        alert_banner_label_->setText(
            QString("%1  %2")
                .arg(risk_alert_type_text(alert.type()))
                .arg(QString::fromStdString(alert.message())));
        alert_banner_label_->setStyleSheet(QString(
            "padding:4px 10px; border-radius:10px; background:%1; color:#40211b; font-weight:700;")
                .arg(risk_alert_color(alert.type()).name()));
    } else {
        alert_banner_label_->setText("No live risk alerts");
        alert_banner_label_->setStyleSheet(
            "padding:4px 10px; border-radius:10px; background:#f3f0e7; color:#4a4032; font-weight:700;");
    }

    std::unordered_map<uint32_t, const omm::proto::QuoteUpdate*> latest_quotes;
    for (auto it = impl_->state.quotes.rbegin(); it != impl_->state.quotes.rend(); ++it) {
        latest_quotes.emplace(it->instrument_id(), &(*it));
    }

    auto fill_side = [&](SideView& side, const InstrumentMeta& meta) {
        side.instrument_id = meta.instrument_id;
        side.code = QString::fromStdString(meta.code);
        if (auto greek_it = impl_->state.greeks.find(meta.instrument_id);
            greek_it != impl_->state.greeks.end()) {
            side.theo = greek_it->second.theo_price();
        }
        if (auto tick_it = impl_->state.ticks.find(meta.instrument_id);
            tick_it != impl_->state.ticks.end()) {
            side.bid_px = tick_it->second.bid_price();
            side.bid_qty = tick_it->second.bid_volume();
            side.ask_px = tick_it->second.ask_price();
            side.ask_qty = tick_it->second.ask_volume();
        }
        if (auto pos_it = impl_->state.positions.find(meta.instrument_id);
            pos_it != impl_->state.positions.end()) {
            side.pos = pos_it->second.net_position();
        }
        if (auto state_it = impl_->state.instrument_states.find(meta.instrument_id);
            state_it != impl_->state.instrument_states.end()) {
            const auto& mm_state = state_it->second;
            side.quote_state = mm_state.quote_state();
            side.quote_state_label = mm_quote_state_text(mm_state.quote_state());
            side.quote_state_color = mm_quote_state_color(mm_state.quote_state());
            side.reason_label = suppress_reason_text(mm_state.reasons(), mm_state.cancel_attempts());
            side.reason_color = suppress_reason_color(mm_state.reasons());
        }
        if (auto quote_it = latest_quotes.find(meta.instrument_id); quote_it != latest_quotes.end()) {
            const auto* quote = quote_it->second;
            const bool has_bid = quote->bid_volume() > 0;
            const bool has_ask = quote->ask_volume() > 0;
            if (has_bid) side.bid_color = QColor("#f3f8a6");
            if (has_ask) side.ask_color = QColor("#ffddb7");
            if (side.reason_label == "-") {
                if (quote->status().find("Reject") != std::string::npos) {
                    side.reason_label = "REJECT";
                    side.reason_color = QColor("#ffcccc");
                } else if (quote->status().find("Fill") != std::string::npos) {
                    side.reason_label = "FILL";
                    side.reason_color = QColor("#ffe3b3");
                } else if (!has_bid && !has_ask) {
                    side.reason_label = "OFF";
                    side.reason_color = QColor("#ececec");
                } else if (has_bid != has_ask) {
                    side.reason_label = "1-WAY";
                    side.reason_color = QColor("#dcefff");
                }
            }
            if (side.quote_state == omm::proto::MM_QUOTE_IDLE && (has_bid || has_ask)) {
                side.quote_state = omm::proto::MM_QUOTE_LIVE;
                side.quote_state_label = has_bid && has_ask ? "LIVE" : "1-WAY";
                side.quote_state_color = has_bid && has_ask ? QColor("#cdeccf") : QColor("#dcefff");
            }
        }
    };

    struct RowKey {
        int expiry_date{0};
        int strike_bp{0};
        bool operator<(const RowKey& other) const noexcept {
            if (expiry_date != other.expiry_date) return expiry_date < other.expiry_date;
            return strike_bp < other.strike_bp;
        }
    };

    std::map<RowKey, StrikeRow> grouped_rows;
    for (const auto& [instrument_id, meta] : impl_->state.instruments) {
        if (meta.product_index != selected_product) continue;
        if (meta.kind != "Option") continue;

        RowKey key{meta.expiry_date, static_cast<int>(std::lround(meta.strike * 100.0))};
        auto& row = grouped_rows[key];
        row.expiry_date = meta.expiry_date;
        row.strike = meta.strike;
        if (meta.option_type == "Call") {
            fill_side(row.call, meta);
        } else if (meta.option_type == "Put") {
            fill_side(row.put, meta);
        }
    }
    rows.reserve(grouped_rows.size());
    for (auto& [_, row] : grouped_rows) {
        row.net_pos = row.call.pos + row.put.pos;
        rows.push_back(row);
    }
    t_table_->setRowCount(static_cast<int>(rows.size()));
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        const StrikeRow& item = rows[row];
        set_cell(t_table_, row, 0, item.call.quote_state_label, item.call.quote_state_color);
        set_cell(t_table_, row, 1, item.call.reason_label, item.call.reason_color);
        set_cell(t_table_, row, 2, item.call.bid_qty > 0 ? QString::number(item.call.bid_qty) : "-", item.call.bid_color.darker(104));
        set_cell(t_table_, row, 3, item.call.bid_px > 0.0 ? QString::number(item.call.bid_px, 'f', 2) : "-", item.call.bid_color);
        set_cell(t_table_, row, 4, item.call.theo > 0.0 ? QString::number(item.call.theo, 'f', 2) : "-", QColor("#d5f0d9"));
        set_cell(t_table_, row, 5, item.call.ask_px > 0.0 ? QString::number(item.call.ask_px, 'f', 2) : "-", item.call.ask_color);
        set_cell(t_table_, row, 6, item.call.ask_qty > 0 ? QString::number(item.call.ask_qty) : "-", item.call.ask_color.darker(104));
        set_cell(t_table_, row, 7, format_expiry_short(item.expiry_date), QColor("#ecf0f6"));
        set_cell(t_table_, row, 8, item.strike > 0.0 ? QString::number(item.strike, 'f', 0) : "-", QColor("#e2d7ff"));
        set_cell(t_table_, row, 9, QString::number(item.net_pos), QColor(item.net_pos != 0 ? "#ffb6b6" : "#eaeaea"));
        set_cell(t_table_, row, 10, item.put.bid_qty > 0 ? QString::number(item.put.bid_qty) : "-", item.put.bid_color.darker(104));
        set_cell(t_table_, row, 11, item.put.bid_px > 0.0 ? QString::number(item.put.bid_px, 'f', 2) : "-", item.put.bid_color);
        set_cell(t_table_, row, 12, item.put.theo > 0.0 ? QString::number(item.put.theo, 'f', 2) : "-", QColor("#d5f0d9"));
        set_cell(t_table_, row, 13, item.put.ask_px > 0.0 ? QString::number(item.put.ask_px, 'f', 2) : "-", item.put.ask_color);
        set_cell(t_table_, row, 14, item.put.ask_qty > 0 ? QString::number(item.put.ask_qty) : "-", item.put.ask_color.darker(104));
        set_cell(t_table_, row, 15, item.put.quote_state_label, item.put.quote_state_color);
        set_cell(t_table_, row, 16, item.put.reason_label, item.put.reason_color);

        for (int col : {0, 1, 2, 3, 4, 5, 6}) {
            if (auto* cell = t_table_->item(row, col); cell != nullptr && item.call.instrument_id != 0) {
                cell->setData(Qt::UserRole, QVariant::fromValue(item.call.instrument_id));
                cell->setToolTip(QString("%1\n%2\n%3")
                    .arg(item.call.code)
                    .arg(item.call.quote_state_label)
                    .arg(item.call.reason_label));
            }
        }
        for (int col : {10, 11, 12, 13, 14, 15, 16}) {
            if (auto* cell = t_table_->item(row, col); cell != nullptr && item.put.instrument_id != 0) {
                cell->setData(Qt::UserRole, QVariant::fromValue(item.put.instrument_id));
                cell->setToolTip(QString("%1\n%2\n%3")
                    .arg(item.put.code)
                    .arg(item.put.quote_state_label)
                    .arg(item.put.reason_label));
            }
        }
        if (auto* strike_item = t_table_->item(row, 8); strike_item != nullptr) {
            const uint32_t focus_id = item.call.instrument_id != 0 ? item.call.instrument_id : item.put.instrument_id;
            strike_item->setData(Qt::UserRole, QVariant::fromValue(focus_id));
            strike_item->setToolTip(QString("Call: %1\nPut: %2")
                .arg(item.call.code.isEmpty() ? "-" : item.call.code)
                .arg(item.put.code.isEmpty() ? "-" : item.put.code));
        }
    }

    struct GroupSummary {
        int net{0};
        double rpnl{0.0};
        double upnl{0.0};
        double delta{0.0};
        double gamma{0.0};
        double vega{0.0};
    };

    struct RiskBoardRow {
        QString code;
        int net{0};
        double delta{0.0};
        double gamma{0.0};
        double vega{0.0};
        QString quote_state{"IDLE"};
        QString why{"-"};
        QColor quote_color{"#ececec"};
        QColor why_color{"#ececec"};
        double severity{0.0};
    };

    std::map<QString, std::vector<uint32_t>> grouped_positions;
    std::vector<RiskBoardRow> risk_board_rows;
    double selected_rpnl = 0.0;
    double selected_upnl = 0.0;
    double selected_delta = 0.0;
    double selected_gamma = 0.0;
    double selected_vega = 0.0;
    int selected_net = 0;
    int selected_position_count = 0;
    int quoted_instruments = 0;
    int suppressed_instruments = 0;
    int monitored_instruments = 0;

    for (const auto& [instrument_id, meta] : impl_->state.instruments) {
        if (meta.product_index != selected_product) continue;

        int net = 0;
        double rpnl = 0.0;
        double upnl = 0.0;
        double delta_contrib = 0.0;
        double gamma_contrib = 0.0;
        double vega_contrib = 0.0;
        if (auto pos_it = impl_->state.positions.find(instrument_id);
            pos_it != impl_->state.positions.end()) {
            net = pos_it->second.net_position();
            rpnl = pos_it->second.realized_pnl();
            upnl = pos_it->second.unrealized_pnl();
            if (net != 0) {
                ++selected_position_count;
                selected_net += net;
                selected_rpnl += rpnl;
                selected_upnl += upnl;
            }
            if (net != 0) {
                QString group = !meta.underlying_code.empty()
                    ? QString::fromStdString(meta.underlying_code)
                    : QString("Product %1").arg(meta.product_index);
                grouped_positions[group].push_back(instrument_id);
            }
        }
        if (auto greek_it = impl_->state.greeks.find(instrument_id);
            greek_it != impl_->state.greeks.end()) {
            delta_contrib = greek_it->second.delta() * static_cast<double>(net);
            gamma_contrib = greek_it->second.gamma() * static_cast<double>(net);
            vega_contrib = greek_it->second.vega() * static_cast<double>(net);
            selected_delta += delta_contrib;
            selected_gamma += gamma_contrib;
            selected_vega += vega_contrib;
        }

        QString quote_state_text = "IDLE";
        QString why_text = "-";
        QColor quote_state_bg("#ececec");
        QColor why_bg("#ececec");
        bool include_risk_row = net != 0;
        if (meta.kind == "Option") {
            if (auto mm_it = impl_->state.instrument_states.find(instrument_id);
                mm_it != impl_->state.instrument_states.end()) {
                ++monitored_instruments;
                quote_state_text = mm_quote_state_text(mm_it->second.quote_state());
                why_text = suppress_reason_text(mm_it->second.reasons(), mm_it->second.cancel_attempts());
                quote_state_bg = mm_quote_state_color(mm_it->second.quote_state());
                why_bg = suppress_reason_color(mm_it->second.reasons());
                include_risk_row = include_risk_row
                    || mm_it->second.quote_state() != omm::proto::MM_QUOTE_IDLE
                    || mm_it->second.reasons_size() > 0;

                switch (mm_it->second.quote_state()) {
                case omm::proto::MM_QUOTE_LIVE:
                case omm::proto::MM_QUOTE_ACK_PENDING:
                case omm::proto::MM_QUOTE_CANCEL_PENDING:
                    ++quoted_instruments;
                    break;
                case omm::proto::MM_QUOTE_CANCEL_FAILED:
                case omm::proto::MM_QUOTE_SUPPRESSED:
                    ++suppressed_instruments;
                    break;
                case omm::proto::MM_QUOTE_IDLE:
                default:
                    if (mm_it->second.reasons_size() > 0) ++suppressed_instruments;
                    break;
                }
            }
        }

        if (include_risk_row) {
            RiskBoardRow row;
            row.code = QString::fromStdString(meta.code);
            row.net = net;
            row.delta = delta_contrib;
            row.gamma = gamma_contrib;
            row.vega = vega_contrib;
            row.quote_state = quote_state_text;
            row.why = why_text;
            row.quote_color = quote_state_bg;
            row.why_color = why_bg;
            row.severity = std::abs(delta_contrib)
                + 2.0 * std::abs(gamma_contrib)
                + 0.05 * std::abs(vega_contrib)
                + 10.0 * std::abs(static_cast<double>(net));
            risk_board_rows.push_back(std::move(row));
        }
    }
    std::sort(risk_board_rows.begin(), risk_board_rows.end(), [](const RiskBoardRow& lhs, const RiskBoardRow& rhs) {
        if (lhs.severity != rhs.severity) return lhs.severity > rhs.severity;
        return lhs.code < rhs.code;
    });

    const QString limits_text = QString("Soft Pos %1   Delta %2   Gamma %3   Vega %4")
        .arg(static_cast<int>(risk_state.has_threshold() ? risk_state.threshold().max_net_position() : 0))
        .arg(QString::number(soft_delta_limit, 'f', 0))
        .arg(QString::number(soft_gamma_limit, 'f', 0))
        .arg(QString::number(soft_vega_limit, 'f', 0));
    const QString counts_text = QString("Quoted %1 / %2   Suppressed %3   Positions %4")
        .arg(quoted_instruments)
        .arg(monitored_instruments)
        .arg(suppressed_instruments)
        .arg(selected_position_count);
    const QString greeks_text = QString("Delta %1   Gamma %2   Vega %3")
        .arg(QString::number(selected_delta, 'f', 1))
        .arg(QString::number(selected_gamma, 'f', 1))
        .arg(QString::number(selected_vega, 'f', 1));
    const QString latest_alert_text = !impl_->state.alerts.empty()
        ? QString("%1  %2")
              .arg(risk_alert_type_text(impl_->state.alerts.front().type()))
              .arg(QString::fromStdString(impl_->state.alerts.front().message()))
        : QString("No live alerts");
    const QColor latest_alert_color = !impl_->state.alerts.empty()
        ? risk_alert_color(impl_->state.alerts.front().type())
        : QColor("#fff6dc");

    auto apply_summary_panel = [&](QLabel* gate,
                                   QLabel* greeks,
                                   QLabel* limits,
                                   QLabel* counts,
                                   QLabel* alert) {
        if (gate != nullptr) style_pill(gate, gate_color);
        if (gate != nullptr) gate->setText(gate_label);
        if (greeks != nullptr) greeks->setText(greeks_text);
        if (limits != nullptr) limits->setText(limits_text);
        if (counts != nullptr) counts->setText(counts_text);
        if (alert != nullptr) {
            alert->setText(latest_alert_text);
            alert->setStyleSheet(QString(
                "padding:4px 8px; border-radius:8px; background:%1; color:#4a4032;")
                    .arg(latest_alert_color.lighter(112).name()));
        }
    };
    apply_summary_panel(pms_gate_label_, pms_greeks_label_, pms_limits_label_, pms_counts_label_, pms_alert_label_);
    apply_summary_panel(secondary_gate_label_, secondary_greeks_label_, secondary_limits_label_,
                        secondary_counts_label_, secondary_alert_label_);
    if (vol_window_ != nullptr) {
        vol_window_->setWindowTitle(
            QString("Secondary Risk / Vol Workspace - %1").arg(product_selector_->currentText()));
    }

    positions_tree_->clear();
    auto* root = new QTreeWidgetItem(positions_tree_);
    root->setText(0, QString("%1 Product Summary").arg(product_selector_->currentText()));
    root->setText(1, QString::number(selected_net));
    root->setText(3, QString::number(selected_rpnl, 'f', 2));
    root->setText(4, QString::number(selected_upnl, 'f', 2));
    root->setText(5, QString::number(selected_delta, 'f', 2));
    root->setText(6, QString::number(selected_gamma, 'f', 2));
    root->setText(7, QString::number(selected_vega, 'f', 2));
    root->setBackground(0, QColor("#d7c4ff"));
    root->setBackground(5, risk_color(selected_delta, 0.75 * soft_delta_limit, soft_delta_limit));
    root->setBackground(6, risk_color(selected_gamma, 0.75 * soft_gamma_limit, soft_gamma_limit));
    root->setBackground(7, risk_color(selected_vega, 0.75 * soft_vega_limit, soft_vega_limit));

    for (auto& [group_name, instrument_ids] : grouped_positions) {
        std::sort(instrument_ids.begin(), instrument_ids.end(), [&](uint32_t lhs, uint32_t rhs) {
            const auto& a = impl_->state.instruments.at(lhs);
            const auto& b = impl_->state.instruments.at(rhs);
            if (a.kind != b.kind) return a.kind < b.kind;
            if (a.strike != b.strike) return a.strike < b.strike;
            return a.code < b.code;
        });

        GroupSummary summary;
        auto* parent = new QTreeWidgetItem(root);
        for (uint32_t instrument_id : instrument_ids) {
            const auto& pos = impl_->state.positions.at(instrument_id);
            const auto& meta = impl_->state.instruments.at(instrument_id);
            const auto greek_it = impl_->state.greeks.find(instrument_id);

            summary.net += pos.net_position();
            summary.rpnl += pos.realized_pnl();
            summary.upnl += pos.unrealized_pnl();
            if (greek_it != impl_->state.greeks.end()) {
                summary.delta += greek_it->second.delta() * static_cast<double>(pos.net_position());
                summary.gamma += greek_it->second.gamma() * static_cast<double>(pos.net_position());
                summary.vega += greek_it->second.vega() * static_cast<double>(pos.net_position());
            }

            auto* child = new QTreeWidgetItem(parent);
            child->setText(0, QString::fromStdString(meta.code));
            child->setText(1, QString::number(pos.net_position()));
            child->setText(2, QString::number(pos.avg_price(), 'f', 2));
            child->setText(3, QString::number(pos.realized_pnl(), 'f', 2));
            child->setText(4, QString::number(pos.unrealized_pnl(), 'f', 2));
            if (greek_it != impl_->state.greeks.end()) {
                child->setText(5, QString::number(greek_it->second.delta(), 'f', 3));
                child->setText(6, QString::number(greek_it->second.gamma(), 'f', 3));
                child->setText(7, QString::number(greek_it->second.vega(), 'f', 3));
            }
            child->setBackground(0, meta.kind == "Future" ? QColor("#d6efff") : QColor("#f9f1bf"));
        }
        parent->setText(0, group_name);
        parent->setText(1, QString::number(summary.net));
        parent->setText(3, QString::number(summary.rpnl, 'f', 2));
        parent->setText(4, QString::number(summary.upnl, 'f', 2));
        parent->setText(5, QString::number(summary.delta, 'f', 2));
        parent->setText(6, QString::number(summary.gamma, 'f', 2));
        parent->setText(7, QString::number(summary.vega, 'f', 2));
        parent->setBackground(0, QColor("#f8d687"));
        parent->setBackground(5, risk_color(summary.delta, 0.35 * soft_delta_limit, 0.6 * soft_delta_limit));
        parent->setBackground(6, risk_color(summary.gamma, 0.35 * soft_gamma_limit, 0.6 * soft_gamma_limit));
        parent->setBackground(7, risk_color(summary.vega, 0.35 * soft_vega_limit, 0.6 * soft_vega_limit));
    }
    positions_tree_->expandAll();

    if (secondary_risk_table_ != nullptr) {
        const int max_rows = std::min<int>(18, risk_board_rows.size());
        secondary_risk_table_->setRowCount(max_rows);
        for (int i = 0; i < max_rows; ++i) {
            const auto& row = risk_board_rows[static_cast<std::size_t>(i)];
            set_cell(secondary_risk_table_, i, 0, row.code, QColor("#eaeaea"));
            set_cell(secondary_risk_table_, i, 1, QString::number(row.net), QColor(row.net != 0 ? "#ffdfdf" : "#f7f7f7"));
            set_cell(secondary_risk_table_, i, 2, QString::number(row.delta, 'f', 2),
                     risk_color(row.delta, 0.35 * soft_delta_limit, 0.6 * soft_delta_limit));
            set_cell(secondary_risk_table_, i, 3, QString::number(row.gamma, 'f', 2),
                     risk_color(row.gamma, 0.35 * soft_gamma_limit, 0.6 * soft_gamma_limit));
            set_cell(secondary_risk_table_, i, 4, QString::number(row.vega, 'f', 2),
                     risk_color(row.vega, 0.35 * soft_vega_limit, 0.6 * soft_vega_limit));
            set_cell(secondary_risk_table_, i, 5, row.quote_state, row.quote_color);
            set_cell(secondary_risk_table_, i, 6, row.why, row.why_color);
        }
    }

    auto in_selected_product = [&](uint32_t instrument_id) {
        auto meta_it = impl_->state.instruments.find(instrument_id);
        return meta_it != impl_->state.instruments.end() && meta_it->second.product_index == selected_product;
    };
    auto instrument_label_for = [&](uint32_t instrument_id) {
        auto meta_it = impl_->state.instruments.find(instrument_id);
        return meta_it != impl_->state.instruments.end()
            ? QString::fromStdString(meta_it->second.code)
            : QString::number(instrument_id);
    };
    auto exchange_label_for = [&](const omm::proto::OrderUpdate& update) {
        QString exchange_label = update.exchange_id().empty()
            ? "-"
            : QString::fromStdString(update.exchange_id());
        if (exchange_label != "-") return exchange_label;
        auto meta_it = impl_->state.instruments.find(update.instrument_id());
        if (meta_it == impl_->state.instruments.end()) return exchange_label;
        if (!meta_it->second.exchange_id.empty()) return QString::fromStdString(meta_it->second.exchange_id);
        const QString inferred = infer_exchange_from_code(meta_it->second.code);
        return inferred.isEmpty() ? exchange_label : inferred;
    };

    struct AggregatedOrder {
        omm::proto::OrderUpdate latest;
        int cumulative_fill_qty{0};
        double cumulative_fill_notional{0.0};
        bool seen_reject{false};
        bool seen_cancel{false};
        bool seen_new{false};
    };

    std::vector<AggregatedOrder> orders;
    orders.reserve(impl_->state.orders.size());
    std::unordered_map<uint64_t, std::size_t> latest_order_index;
    for (auto it = impl_->state.orders.rbegin(); it != impl_->state.orders.rend(); ++it) {
        const auto& order = *it;
        if (!in_selected_product(order.instrument_id())) continue;

        auto pos = latest_order_index.find(order.client_order_id());
        if (pos == latest_order_index.end()) {
            AggregatedOrder agg;
            agg.latest = order;
            if (order.status() == "Filled") {
                agg.cumulative_fill_qty = order.fill_volume();
                agg.cumulative_fill_notional = order.fill_price() * static_cast<double>(order.fill_volume());
            }
            agg.seen_reject = order.status() == "Rejected";
            agg.seen_cancel = order.status() == "Cancelled";
            agg.seen_new = order.status() == "New";
            latest_order_index.emplace(order.client_order_id(), orders.size());
            orders.push_back(std::move(agg));
            continue;
        }

        auto& agg = orders[pos->second];
        auto& merged = agg.latest;
        if (merged.instrument_id() == 0 && order.instrument_id() != 0) merged.set_instrument_id(order.instrument_id());
        if (merged.exchange_id().empty() && !order.exchange_id().empty()) merged.set_exchange_id(order.exchange_id());
        if (merged.side().empty() && !order.side().empty()) merged.set_side(order.side());
        if (merged.price() <= 0.0 && order.price() > 0.0) merged.set_price(order.price());
        if (merged.volume() <= 0 && order.volume() > 0) merged.set_volume(order.volume());
        if (order.ts_ns() > merged.ts_ns()) merged.set_ts_ns(order.ts_ns());

        if (order.status() == "Filled") {
            agg.cumulative_fill_qty += order.fill_volume();
            agg.cumulative_fill_notional += order.fill_price() * static_cast<double>(order.fill_volume());
        } else if (!order.status().empty()) {
            merged.set_status(order.status());
            agg.seen_reject = agg.seen_reject || order.status() == "Rejected";
            agg.seen_cancel = agg.seen_cancel || order.status() == "Cancelled";
            agg.seen_new = agg.seen_new || order.status() == "New";
        }
    }
    std::vector<OrderBlotterRow> order_rows;
    order_rows.reserve(std::min<std::size_t>(orders.size(), 1000));
    for (int i = 0; i < static_cast<int>(orders.size()); ++i) {
        auto& agg = orders[i];
        auto& order = agg.latest;
        if (agg.cumulative_fill_qty > 0) {
            order.set_fill_volume(agg.cumulative_fill_qty);
            order.set_fill_price(agg.cumulative_fill_notional / static_cast<double>(agg.cumulative_fill_qty));
        }
        if (agg.seen_reject) {
            order.set_status("Rejected");
        } else if (order.volume() > 0 && agg.cumulative_fill_qty >= order.volume()) {
            order.set_status("Filled");
        } else if (agg.cumulative_fill_qty > 0) {
            order.set_status("PartialFilled");
        } else if (agg.seen_cancel) {
            order.set_status("Cancelled");
        } else if (agg.seen_new) {
            order.set_status("New");
        }

        if (order_rows.size() >= 1000) break;
        OrderBlotterRow row;
        row.order = order;
        row.instrument = instrument_label_for(order.instrument_id());
        row.book = book_label_from_map(impl_->state.books, order.book_id());
        row.exchange = exchange_label_for(order);
        row.side = order.side().empty() ? "-" : QString::fromStdString(order.side());
        row.price = order.price() > 0.0 ? QString::number(order.price(), 'f', 2) : "-";
        row.volume = order.volume() > 0 ? QString::number(order.volume()) : "-";
        row.status = QString::fromStdString(order.status());
        row.fill_price = QString::number(order.fill_price(), 'f', 2);
        row.fill_volume = QString::number(order.fill_volume());
        row.ts = format_monotonic_ts(order.ts_ns());
        row.status_color = order.status().find("Reject") != std::string::npos ? QColor("#ffb6b6") :
                           order.status().find("Fill") != std::string::npos ? QColor("#c9f4d5") :
                           QColor("#fff0b3");
        order_rows.push_back(std::move(row));
    }
    if (impl_->order_blotter_model != nullptr) {
        impl_->order_blotter_model->replace_rows(std::move(order_rows));
    }

    std::vector<QuoteBlotterRow> quote_rows;
    quote_rows.reserve(std::min<std::size_t>(impl_->state.quotes.size(), 1000));
    for (const auto& quote : impl_->state.quotes) {
        if (!in_selected_product(quote.instrument_id())) continue;
        if (quote_rows.size() >= 1000) break;
        QuoteBlotterRow row;
        row.quote = quote;
        row.instrument = instrument_label_for(quote.instrument_id());
        row.book = book_label_from_map(impl_->state.books, quote.book_id());
        row.bid_price = QString::number(quote.bid_price(), 'f', 2);
        row.bid_volume = QString::number(quote.bid_volume());
        row.ask_price = QString::number(quote.ask_price(), 'f', 2);
        row.ask_volume = QString::number(quote.ask_volume());
        row.quote_state = "IDLE";
        row.reason = "-";
        row.status = QString::fromStdString(quote.status());
        if (auto mm_it = impl_->state.instrument_states.find(quote.instrument_id());
            mm_it != impl_->state.instrument_states.end()) {
            const auto& mm_state = mm_it->second;
            row.quote_state = mm_quote_state_text(mm_state.quote_state());
            row.reason = suppress_reason_text(mm_state.reasons(), mm_state.cancel_attempts());
            row.quote_state_color = mm_quote_state_color(mm_state.quote_state());
            row.reason_color = suppress_reason_color(mm_state.reasons());
        }
        quote_rows.push_back(std::move(row));
    }
    if (impl_->quote_blotter_model != nullptr) {
        impl_->quote_blotter_model->replace_rows(std::move(quote_rows));
    }

    auto make_trade_row = [&](const omm::proto::OrderUpdate& trade) {
        TradeBlotterRow row;
        row.trade = trade;
        row.trade_id = QString::number(trade.exchange_trade_id());
        row.order_id = QString::number(trade.client_order_id());
        row.instrument = instrument_label_for(trade.instrument_id());
        row.book = book_label_from_map(impl_->state.books, trade.book_id());
        row.exchange = exchange_label_for(trade);
        row.side = trade.side().empty() ? "-" : QString::fromStdString(trade.side());
        row.price = QString::number(trade.fill_price(), 'f', 2);
        row.qty = QString::number(trade.fill_volume());
        row.ts = format_monotonic_ts(trade.ts_ns());
        return row;
    };
    if (impl_->trade_blotter_model != nullptr) {
        const bool rebuild_trades =
            impl_->displayed_trade_product_index != selected_product ||
            impl_->displayed_trade_seq > impl_->state.trades_seq ||
            impl_->displayed_trade_seq == 0 ||
            (impl_->state.trades.empty() && impl_->displayed_trade_seq != impl_->state.trades_seq);
        if (rebuild_trades) {
            std::vector<TradeBlotterRow> trade_rows;
            trade_rows.reserve(std::min<std::size_t>(impl_->state.trades.size(), 100000));
            for (const auto& trade : impl_->state.trades) {
                if (!in_selected_product(trade.instrument_id())) continue;
                if (trade_rows.size() >= 100000) break;
                trade_rows.push_back(make_trade_row(trade));
            }
            impl_->trade_blotter_model->replace_rows(std::move(trade_rows));
        } else if (impl_->state.trades_seq > impl_->displayed_trade_seq) {
            const uint64_t delta_seq = impl_->state.trades_seq - impl_->displayed_trade_seq;
            const std::size_t scan_count =
                std::min<std::size_t>(impl_->state.trades.size(), static_cast<std::size_t>(delta_seq));
            std::vector<TradeBlotterRow> trade_rows;
            trade_rows.reserve(scan_count);
            for (std::size_t i = 0; i < scan_count; ++i) {
                const auto& trade = impl_->state.trades[i];
                if (in_selected_product(trade.instrument_id())) trade_rows.push_back(make_trade_row(trade));
            }
            impl_->trade_blotter_model->prepend_rows(std::move(trade_rows), 100000);
        }
        impl_->displayed_trade_product_index = selected_product;
        impl_->displayed_trade_seq = impl_->state.trades_seq;
    }

    auto populate_alert_table = [&](QTableWidget* table) {
        if (table == nullptr) return;
        table->setRowCount(static_cast<int>(impl_->state.alerts.size()));
        for (int i = 0; i < static_cast<int>(impl_->state.alerts.size()); ++i) {
            const auto& alert = impl_->state.alerts[static_cast<std::size_t>(i)];
            const QColor alert_color = risk_alert_color(alert.type());
            set_cell(table, i, 0, format_monotonic_ts(alert.ts_ns()), QColor("#f7f7f7"));
            set_cell(table, i, 1, risk_alert_type_text(alert.type()), alert_color);
            set_cell(table, i, 2, QString::fromStdString(alert.message()), alert_color.lighter(112));
        }
    };
    populate_alert_table(alerts_table_);
    populate_alert_table(secondary_alerts_table_);

    if (vol_widget_ != nullptr && vol_dock_ != nullptr && vol_dock_->isVisible()) {
        vol_widget_->update();
    }
    if (vol_window_ != nullptr && vol_window_->isVisible() && vol_window_widget_ != nullptr) {
        vol_window_widget_->update();
    }
}


} // namespace omm::gui

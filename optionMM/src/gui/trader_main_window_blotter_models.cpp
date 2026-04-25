#include "trader_main_window_blotter_models.h"

#include <QBrush>
#include <QStringList>

#include <algorithm>
#include <iterator>

namespace omm::gui {

namespace {

const QColor kDefaultBg{"#f7f7f7"};
const QColor kIdBg{"#eaeaea"};
const QColor kBidBg{"#ffe08a"};
const QColor kAskBg{"#ffc59c"};
const QColor kFillBg{"#e8f4ff"};
const QColor kBookBg{"#eef6ff"};

QVariant aligned(int role) {
    if (role == Qt::TextAlignmentRole) return Qt::AlignCenter;
    if (role == Qt::ForegroundRole) return QBrush(QColor("#1d1d1d"));
    return {};
}

template<typename Rows>
const typename Rows::value_type* container_row(const Rows& rows, int index) {
    if (index < 0 || index >= static_cast<int>(rows.size())) return nullptr;
    return &rows[static_cast<std::size_t>(index)];
}

} // namespace

OrderBlotterModel::OrderBlotterModel(QObject* parent) : QAbstractTableModel(parent) {}

int OrderBlotterModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int OrderBlotterModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : 11;
}

QVariant OrderBlotterModel::data(const QModelIndex& index, int role) const {
    const auto* row = this->row(index.row());
    if (row == nullptr || !index.isValid()) return {};
    if (const QVariant common = aligned(role); common.isValid()) return common;
    if (role == Qt::UserRole) return QVariant::fromValue(row->order.client_order_id());
    if (role == Qt::UserRole + 1) return QVariant::fromValue(row->order.instrument_id());
    if (role == Qt::BackgroundRole) {
        if (index.column() <= 1) return QBrush(kIdBg);
        if (index.column() == 2) return QBrush(kBookBg);
        if (index.column() == 7) return QBrush(row->status_color);
        return QBrush(kDefaultBg);
    }
    if (role != Qt::DisplayRole) return {};
    switch (index.column()) {
    case 0: return QString::number(row->order.client_order_id());
    case 1: return row->instrument;
    case 2: return row->book;
    case 3: return row->exchange;
    case 4: return row->side;
    case 5: return row->price;
    case 6: return row->volume;
    case 7: return row->status;
    case 8: return row->fill_price;
    case 9: return row->fill_volume;
    case 10: return row->ts;
    default: return {};
    }
}

QVariant OrderBlotterModel::headerData(int section, Qt::Orientation orientation, int role) const {
    static const QStringList headers{
        "OrderId", "Instrument", "Book", "Exchange", "Side", "Price",
        "Volume", "Status", "FillPx", "FillQty", "Ts"};
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    return section >= 0 && section < headers.size() ? headers[section] : QVariant{};
}

void OrderBlotterModel::replace_rows(std::vector<OrderBlotterRow> rows) {
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

const OrderBlotterRow* OrderBlotterModel::row(int index) const {
    return container_row(rows_, index);
}

QuoteBlotterModel::QuoteBlotterModel(QObject* parent) : QAbstractTableModel(parent) {}

int QuoteBlotterModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int QuoteBlotterModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : 9;
}

QVariant QuoteBlotterModel::data(const QModelIndex& index, int role) const {
    const auto* row = this->row(index.row());
    if (row == nullptr || !index.isValid()) return {};
    if (const QVariant common = aligned(role); common.isValid()) return common;
    if (role == Qt::UserRole) return QVariant::fromValue(row->quote.client_quote_id());
    if (role == Qt::UserRole + 1) return QVariant::fromValue(row->quote.instrument_id());
    if (role == Qt::BackgroundRole) {
        switch (index.column()) {
        case 0: return QBrush(kIdBg);
        case 1: return QBrush(kBookBg);
        case 2:
        case 3: return QBrush(kBidBg);
        case 4:
        case 5: return QBrush(kAskBg);
        case 6: return QBrush(row->quote_state_color);
        case 7: return QBrush(row->reason_color);
        case 8: return QBrush(QColor("#e4f8f0"));
        default: return QBrush(kDefaultBg);
        }
    }
    if (role != Qt::DisplayRole) return {};
    switch (index.column()) {
    case 0: return row->instrument;
    case 1: return row->book;
    case 2: return row->bid_price;
    case 3: return row->bid_volume;
    case 4: return row->ask_price;
    case 5: return row->ask_volume;
    case 6: return row->quote_state;
    case 7: return row->reason;
    case 8: return row->status;
    default: return {};
    }
}

QVariant QuoteBlotterModel::headerData(int section, Qt::Orientation orientation, int role) const {
    static const QStringList headers{
        "Instrument", "Book", "BidPx", "BidQty", "AskPx", "AskQty", "QState", "Why", "Status"};
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    return section >= 0 && section < headers.size() ? headers[section] : QVariant{};
}

void QuoteBlotterModel::replace_rows(std::vector<QuoteBlotterRow> rows) {
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

const QuoteBlotterRow* QuoteBlotterModel::row(int index) const {
    return container_row(rows_, index);
}

TradeBlotterModel::TradeBlotterModel(QObject* parent) : QAbstractTableModel(parent) {}

int TradeBlotterModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int TradeBlotterModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : 9;
}

QVariant TradeBlotterModel::data(const QModelIndex& index, int role) const {
    const auto* row = this->row(index.row());
    if (row == nullptr || !index.isValid()) return {};
    if (const QVariant common = aligned(role); common.isValid()) return common;
    if (role == Qt::BackgroundRole) {
        if (index.column() == 0) return QBrush(kFillBg);
        if (index.column() == 1) return QBrush(QColor("#f1f1f1"));
        if (index.column() == 2) return QBrush(kIdBg);
        if (index.column() == 3) return QBrush(kBookBg);
        if (index.column() == 5) return QBrush(row->side == "Buy" ? QColor("#dff4df") : QColor("#ffd9d1"));
        return QBrush(kDefaultBg);
    }
    if (role != Qt::DisplayRole) return {};
    switch (index.column()) {
    case 0: return row->trade_id;
    case 1: return row->order_id;
    case 2: return row->instrument;
    case 3: return row->book;
    case 4: return row->exchange;
    case 5: return row->side;
    case 6: return row->price;
    case 7: return row->qty;
    case 8: return row->ts;
    default: return {};
    }
}

QVariant TradeBlotterModel::headerData(int section, Qt::Orientation orientation, int role) const {
    static const QStringList headers{
        "TradeId", "OrderId", "Instrument", "Book", "Exchange", "Side", "Price", "Qty", "Ts"};
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    return section >= 0 && section < headers.size() ? headers[section] : QVariant{};
}

void TradeBlotterModel::replace_rows(std::vector<TradeBlotterRow> rows) {
    beginResetModel();
    rows_.clear();
    rows_.insert(rows_.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
    endResetModel();
}

void TradeBlotterModel::prepend_rows(std::vector<TradeBlotterRow> rows, int max_rows) {
    if (rows.empty()) return;
    const int insert_count = std::min<int>(static_cast<int>(rows.size()), max_rows);
    if (insert_count <= 0) return;
    beginInsertRows({}, 0, insert_count - 1);
    for (int i = insert_count - 1; i >= 0; --i) {
        rows_.push_front(std::move(rows[static_cast<std::size_t>(i)]));
    }
    endInsertRows();

    if (max_rows >= 0 && static_cast<int>(rows_.size()) > max_rows) {
        const int first = max_rows;
        const int last = static_cast<int>(rows_.size()) - 1;
        beginRemoveRows({}, first, last);
        while (static_cast<int>(rows_.size()) > max_rows) rows_.pop_back();
        endRemoveRows();
    }
}

const TradeBlotterRow* TradeBlotterModel::row(int index) const {
    return container_row(rows_, index);
}

} // namespace omm::gui

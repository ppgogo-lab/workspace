#pragma once

#include "trading.pb.h"

#include <QAbstractTableModel>
#include <QColor>
#include <QString>

#include <cstdint>
#include <deque>
#include <vector>

namespace omm::gui {

struct OrderBlotterRow {
    omm::proto::OrderUpdate order;
    QString instrument;
    QString book;
    QString exchange;
    QString side;
    QString price;
    QString volume;
    QString status;
    QString fill_price;
    QString fill_volume;
    QString ts;
    QColor status_color{"#fff0b3"};
};

struct QuoteBlotterRow {
    omm::proto::QuoteUpdate quote;
    QString instrument;
    QString book;
    QString bid_price;
    QString bid_volume;
    QString ask_price;
    QString ask_volume;
    QString quote_state;
    QString reason;
    QString status;
    QColor quote_state_color{"#ececec"};
    QColor reason_color{"#ececec"};
};

struct TradeBlotterRow {
    omm::proto::OrderUpdate trade;
    QString trade_id;
    QString order_id;
    QString instrument;
    QString book;
    QString exchange;
    QString side;
    QString price;
    QString qty;
    QString ts;
};

class OrderBlotterModel final : public QAbstractTableModel {
public:
    explicit OrderBlotterModel(QObject* parent = nullptr);
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void replace_rows(std::vector<OrderBlotterRow> rows);
    [[nodiscard]] const OrderBlotterRow* row(int index) const;

private:
    std::vector<OrderBlotterRow> rows_;
};

class QuoteBlotterModel final : public QAbstractTableModel {
public:
    explicit QuoteBlotterModel(QObject* parent = nullptr);
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void replace_rows(std::vector<QuoteBlotterRow> rows);
    [[nodiscard]] const QuoteBlotterRow* row(int index) const;

private:
    std::vector<QuoteBlotterRow> rows_;
};

class TradeBlotterModel final : public QAbstractTableModel {
public:
    explicit TradeBlotterModel(QObject* parent = nullptr);
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void replace_rows(std::vector<TradeBlotterRow> rows);
    void prepend_rows(std::vector<TradeBlotterRow> rows, int max_rows);
    [[nodiscard]] const TradeBlotterRow* row(int index) const;

private:
    std::deque<TradeBlotterRow> rows_;
};

} // namespace omm::gui

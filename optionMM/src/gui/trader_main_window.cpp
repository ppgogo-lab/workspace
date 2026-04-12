#include "gui/trader_main_window.h"

#include "proto/gen/trading.grpc.pb.h"

#include <grpcpp/channel.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/client_context.h>

#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace omm::gui {

namespace {

struct VolCurveSnapshot {
    uint32_t product_index{0};
    std::vector<double> strikes;
    std::vector<double> vols;
};

struct InstrumentMeta {
    uint32_t instrument_id{0};
    uint32_t product_index{0};
    uint32_t underlying_id{0};
    std::string code;
    std::string underlying_code;
    std::string kind;
    std::string option_type;
    double strike{0.0};
};

struct SharedState {
    std::mutex mutex;
    bool connected{false};
    omm::proto::PortfolioGreeks portfolio;
    std::unordered_map<uint32_t, InstrumentMeta> instruments;
    std::unordered_map<uint32_t, omm::proto::Tick> ticks;
    std::unordered_map<uint32_t, omm::proto::Greeks> greeks;
    std::unordered_map<uint32_t, omm::proto::Position> positions;
    std::deque<omm::proto::OrderUpdate> orders;
    std::deque<omm::proto::QuoteUpdate> quotes;
    std::deque<omm::proto::OrderUpdate> trades;
    std::map<uint32_t, omm::proto::MMParams> mm_params;
    std::map<uint32_t, VolCurveSnapshot> curves;
};

class VolCurveGridWidget final : public QWidget {
public:
    explicit VolCurveGridWidget(SharedState* state, QWidget* parent = nullptr)
        : QWidget(parent), state_(state) {
        setMinimumHeight(340);
        setAutoFillBackground(true);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor("#fbf6ea"));
        painter.setRenderHint(QPainter::Antialiasing, true);

        std::vector<VolCurveSnapshot> curves;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            for (const auto& [_, curve] : state_->curves) curves.push_back(curve);
        }

        if (curves.empty()) {
            painter.setPen(QColor("#715b36"));
            painter.drawText(rect().adjusted(16, 16, -16, -16),
                             Qt::AlignCenter,
                             "Waiting for live vol surfaces");
            return;
        }

        const int panel_count = std::min<int>(9, curves.size());
        const int cols = 3;
        const int rows = (panel_count + cols - 1) / cols;
        const int gap = 12;
        const int panel_w = std::max(180, (width() - gap * (cols + 1)) / cols);
        const int panel_h = std::max(140, (height() - gap * (rows + 1)) / rows);

        for (int i = 0; i < panel_count; ++i) {
            const int row = i / cols;
            const int col = i % cols;
            QRect panel(gap + col * (panel_w + gap),
                        gap + row * (panel_h + gap),
                        panel_w, panel_h);
            draw_panel(&painter, panel, curves[i]);
        }
    }

private:
    QString curve_label(uint32_t product_index) const {
        QString label = QString("Product %1").arg(product_index);
        std::lock_guard<std::mutex> lock(state_->mutex);
        for (const auto& [_, meta] : state_->instruments) {
            if (meta.product_index != product_index) continue;
            if (meta.kind == "Future") return QString::fromStdString(meta.code);
            if (!meta.underlying_code.empty()) {
                label = QString("%1 / P%2")
                    .arg(QString::fromStdString(meta.underlying_code))
                    .arg(product_index);
            }
        }
        return label;
    }

    void draw_panel(QPainter* painter, const QRect& panel, const VolCurveSnapshot& curve) const {
        painter->save();
        painter->setPen(QColor("#d69f2f"));
        painter->setBrush(QColor("#fffdf8"));
        painter->drawRoundedRect(panel, 8, 8);

        painter->setPen(QColor("#3a3123"));
        painter->drawText(panel.adjusted(12, 10, -12, -10),
                          Qt::AlignTop | Qt::AlignLeft,
                          curve_label(curve.product_index));

        QRect plot = panel.adjusted(16, 32, -16, -16);
        painter->setPen(QColor("#d8c9b0"));
        painter->drawRect(plot);

        if (curve.strikes.size() > 1 && curve.strikes.size() == curve.vols.size()) {
            const auto [min_x_it, max_x_it] = std::minmax_element(curve.strikes.begin(), curve.strikes.end());
            const auto [min_y_it, max_y_it] = std::minmax_element(curve.vols.begin(), curve.vols.end());
            const double min_x = *min_x_it;
            const double max_x = *max_x_it;
            const double min_y = *min_y_it;
            const double max_y = std::max(min_y + 1e-6, *max_y_it);

            QPainterPath path;
            for (std::size_t i = 0; i < curve.strikes.size(); ++i) {
                const double x_alpha = (curve.strikes[i] - min_x) / std::max(1e-9, max_x - min_x);
                const double y_alpha = (curve.vols[i] - min_y) / std::max(1e-9, max_y - min_y);
                QPointF point(plot.left() + x_alpha * plot.width(),
                              plot.bottom() - y_alpha * plot.height());
                if (i == 0) path.moveTo(point);
                else path.lineTo(point);
            }
            painter->setPen(QPen(QColor("#008f8c"), 2.0));
            painter->drawPath(path);
        }
        painter->restore();
    }

    SharedState* state_;
};

class GrpcTraderClient final {
public:
    explicit GrpcTraderClient(std::string endpoint, SharedState* state)
        : endpoint_(std::move(endpoint)),
          state_(state),
          stub_(omm::proto::TradingMonitor::NewStub(
              grpc::CreateChannel(endpoint_, grpc::InsecureChannelCredentials()))) {}

    ~GrpcTraderClient() {
        stop();
    }

    void start() {
        stop_requested_ = false;
        snapshot_thread_ = std::thread([this] { snapshot_loop(); });
        stream_threads_.emplace_back([this] { ticks_loop(); });
        stream_threads_.emplace_back([this] { greeks_loop(); });
        stream_threads_.emplace_back([this] { positions_loop(); });
        stream_threads_.emplace_back([this] { orders_loop(); });
        stream_threads_.emplace_back([this] { trades_loop(); });
        stream_threads_.emplace_back([this] { quotes_loop(); });
        stream_threads_.emplace_back([this] { vol_loop(); });
    }

    void stop() {
        stop_requested_ = true;
        if (snapshot_thread_.joinable()) snapshot_thread_.join();
        for (auto& thread : stream_threads_) {
            if (thread.joinable()) thread.join();
        }
        stream_threads_.clear();
    }

    bool send_manual_order(uint32_t instrument_id, std::string side, double price, int volume) {
        grpc::ClientContext ctx;
        omm::proto::ManualOrderRequest req;
        omm::proto::ManualOrderResponse resp;
        req.set_instrument_id(instrument_id);
        req.set_side(side);
        req.set_price(price);
        req.set_volume(volume);
        grpc::Status status = stub_->SendManualOrder(&ctx, req, &resp);
        return status.ok() && resp.ok();
    }

    bool set_strategy_enabled(int product_index, bool enabled) {
        grpc::ClientContext ctx;
        omm::proto::StartStopRequest req;
        omm::proto::StartStopResponse resp;
        req.set_product_index(product_index);
        grpc::Status status = enabled
            ? stub_->StartStrategy(&ctx, req, &resp)
            : stub_->StopStrategy(&ctx, req, &resp);
        return status.ok() && resp.ok();
    }

    bool set_strategy_params(uint32_t product_index, const omm::proto::MMParams& params) {
        grpc::ClientContext ctx;
        omm::proto::SetStrategyParamsRequest req;
        omm::proto::SetStrategyParamsResponse resp;
        req.set_product_index(product_index);
        *req.mutable_params() = params;
        grpc::Status status = stub_->SetStrategyParams(&ctx, req, &resp);
        return status.ok() && resp.ok();
    }

private:
    template<typename Message, typename ReaderFn, typename HandlerFn>
    void run_stream(ReaderFn reader_fn, HandlerFn handler) {
        while (!stop_requested_) {
            grpc::ClientContext ctx;
            omm::proto::StreamRequest req;
            req.set_product_index(0xFF);
            auto reader = reader_fn(&ctx, req);
            Message msg;
            while (!stop_requested_ && reader->Read(&msg)) {
                {
                    std::lock_guard<std::mutex> lock(state_->mutex);
                    state_->connected = true;
                    handler(msg);
                }
            }
            reader->Finish();
            if (!stop_requested_) {
                {
                    std::lock_guard<std::mutex> lock(state_->mutex);
                    state_->connected = false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        }
    }

    void snapshot_loop() {
        while (!stop_requested_) {
            grpc::ClientContext ctx;
            omm::proto::SnapshotRequest req;
            omm::proto::SnapshotResponse resp;
            grpc::Status status = stub_->GetSnapshot(&ctx, req, &resp);
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                state_->connected = status.ok();
                if (status.ok()) {
                    state_->instruments.clear();
                    for (const auto& instrument : resp.instruments()) {
                        InstrumentMeta meta;
                        meta.instrument_id = instrument.instrument_id();
                        meta.product_index = instrument.product_index();
                        meta.underlying_id = instrument.underlying_id();
                        meta.code = instrument.code();
                        meta.underlying_code = instrument.underlying_code();
                        meta.kind = instrument.kind();
                        meta.option_type = instrument.option_type();
                        meta.strike = instrument.strike();
                        state_->instruments[meta.instrument_id] = std::move(meta);
                    }
                    state_->greeks.clear();
                    for (const auto& g : resp.greeks()) state_->greeks[g.instrument_id()] = g;
                    state_->positions.clear();
                    for (const auto& p : resp.positions()) state_->positions[p.instrument_id()] = p;
                    state_->portfolio = resp.portfolio();
                    state_->mm_params.clear();
                    for (int i = 0; i < resp.mm_params_size(); ++i) {
                        state_->mm_params[static_cast<uint32_t>(i)] = resp.mm_params(i);
                    }
                }
            }
            for (int i = 0; i < 20 && !stop_requested_; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        }
    }

    void ticks_loop() {
        run_stream<omm::proto::Tick>(
            [this](grpc::ClientContext* ctx, const omm::proto::StreamRequest& req) {
                return stub_->StreamTicks(ctx, req);
            },
            [this](const omm::proto::Tick& tick) { state_->ticks[tick.instrument_id()] = tick; });
    }

    void greeks_loop() {
        run_stream<omm::proto::Greeks>(
            [this](grpc::ClientContext* ctx, const omm::proto::StreamRequest& req) {
                return stub_->StreamGreeks(ctx, req);
            },
            [this](const omm::proto::Greeks& greeks) { state_->greeks[greeks.instrument_id()] = greeks; });
    }

    void positions_loop() {
        run_stream<omm::proto::Position>(
            [this](grpc::ClientContext* ctx, const omm::proto::StreamRequest& req) {
                return stub_->StreamPositions(ctx, req);
            },
            [this](const omm::proto::Position& pos) { state_->positions[pos.instrument_id()] = pos; });
    }

    void orders_loop() {
        run_stream<omm::proto::OrderUpdate>(
            [this](grpc::ClientContext* ctx, const omm::proto::StreamRequest& req) {
                return stub_->StreamOrders(ctx, req);
            },
            [this](const omm::proto::OrderUpdate& order) {
                state_->orders.push_front(order);
                while (state_->orders.size() > 200) state_->orders.pop_back();
            });
    }

    void trades_loop() {
        run_stream<omm::proto::OrderUpdate>(
            [this](grpc::ClientContext* ctx, const omm::proto::StreamRequest& req) {
                return stub_->StreamTrades(ctx, req);
            },
            [this](const omm::proto::OrderUpdate& trade) {
                state_->trades.push_front(trade);
                while (state_->trades.size() > 200) state_->trades.pop_back();
            });
    }

    void quotes_loop() {
        run_stream<omm::proto::QuoteUpdate>(
            [this](grpc::ClientContext* ctx, const omm::proto::StreamRequest& req) {
                return stub_->StreamQuotes(ctx, req);
            },
            [this](const omm::proto::QuoteUpdate& quote) {
                state_->quotes.push_front(quote);
                while (state_->quotes.size() > 200) state_->quotes.pop_back();
            });
    }

    void vol_loop() {
        run_stream<omm::proto::VolSurface>(
            [this](grpc::ClientContext* ctx, const omm::proto::StreamRequest& req) {
                return stub_->StreamVolSurface(ctx, req);
            },
            [this](const omm::proto::VolSurface& surface) {
                VolCurveSnapshot curve;
                curve.product_index = surface.product_index();
                for (const auto& slice : surface.slices()) {
                    for (int i = 0; i < slice.strikes_size() && i < slice.vols_size(); ++i) {
                        curve.strikes.push_back(slice.strikes(i));
                        curve.vols.push_back(slice.vols(i));
                    }
                }
                state_->curves[surface.product_index()] = std::move(curve);
            });
    }

    std::string endpoint_;
    SharedState* state_;
    std::unique_ptr<omm::proto::TradingMonitor::Stub> stub_;
    std::atomic<bool> stop_requested_{false};
    std::thread snapshot_thread_;
    std::vector<std::thread> stream_threads_;
};

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
    table->setItem(row, col, item);
}

} // namespace

struct TraderMainWindow::Impl {
    SharedState state;
    std::unique_ptr<GrpcTraderClient> client;
};

TraderMainWindow::TraderMainWindow(std::string grpc_endpoint, QWidget* parent)
    : QMainWindow(parent),
      grpc_endpoint_(std::move(grpc_endpoint)),
      impl_(std::make_unique<Impl>()) {
    setWindowTitle(QString("optionMM Trader Dashboard - %1").arg(QString::fromStdString(grpc_endpoint_)));
    build_ui();
    impl_->client = std::make_unique<GrpcTraderClient>(grpc_endpoint_, &impl_->state);
    impl_->client->start();

    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] { refresh_ui(); });
    timer->start(120);
}

TraderMainWindow::~TraderMainWindow() = default;

void TraderMainWindow::build_ui() {
    auto* toolbar = addToolBar("Desk");
    toolbar->setMovable(false);
    toolbar->addWidget(new QLabel("Product"));
    product_selector_ = new QSpinBox();
    product_selector_->setRange(0, 31);
    toolbar->addWidget(product_selector_);
    toolbar->addSeparator();
    status_label_ = new QLabel("Connecting");
    toolbar->addWidget(status_label_);

    auto* central = new QWidget();
    auto* layout = new QVBoxLayout(central);
    auto* hero = new QLabel("T-Table");
    hero->setStyleSheet("font-size: 22px; font-weight: 700; color: #2b2418;");
    layout->addWidget(hero);

    t_table_ = make_table({"BidQty", "BidPx", "Theo", "Strike/Inst", "AskPx", "AskQty", "Delta", "Pos"});
    layout->addWidget(t_table_);

    auto* vol_box = new QGroupBox("ORC Wing / Vol Curves");
    auto* vol_layout = new QVBoxLayout(vol_box);
    vol_widget_ = new VolCurveGridWidget(&impl_->state);
    vol_layout->addWidget(vol_widget_);
    layout->addWidget(vol_box);
    setCentralWidget(central);

    auto* quick_dock = new QDockWidget("Quick Order / Strategy", this);
    auto* quick_panel = new QWidget();
    auto* quick_layout = new QGridLayout(quick_panel);
    quick_layout->addWidget(new QLabel("Instrument"), 0, 0);
    instrument_selector_ = new QComboBox();
    quick_layout->addWidget(instrument_selector_, 0, 1, 1, 2);
    quick_layout->addWidget(new QLabel("Side"), 1, 0);
    side_selector_ = new QComboBox();
    side_selector_->addItems({"buy", "sell"});
    quick_layout->addWidget(side_selector_, 1, 1, 1, 2);
    quick_layout->addWidget(new QLabel("Price"), 2, 0);
    price_editor_ = new QDoubleSpinBox();
    price_editor_->setDecimals(4);
    price_editor_->setMaximum(1000000.0);
    quick_layout->addWidget(price_editor_, 2, 1, 1, 2);
    quick_layout->addWidget(new QLabel("Volume"), 3, 0);
    volume_editor_ = new QSpinBox();
    volume_editor_->setRange(1, 100000);
    volume_editor_->setValue(5);
    quick_layout->addWidget(volume_editor_, 3, 1, 1, 2);
    buy_button_ = new QPushButton("Send Buy");
    sell_button_ = new QPushButton("Send Sell");
    start_button_ = new QPushButton("Start MM");
    stop_button_ = new QPushButton("Stop MM");
    quick_layout->addWidget(buy_button_, 4, 0, 1, 3);
    quick_layout->addWidget(sell_button_, 5, 0, 1, 3);
    quick_layout->addWidget(start_button_, 6, 0, 1, 3);
    quick_layout->addWidget(stop_button_, 7, 0, 1, 3);
    auto* params_box = new QGroupBox("Strategy Params");
    auto* params_layout = new QGridLayout(params_box);
    params_layout->addWidget(new QLabel("Bid Spread"), 0, 0);
    bid_spread_editor_ = new QDoubleSpinBox();
    bid_spread_editor_->setDecimals(4);
    bid_spread_editor_->setMaximum(9999.0);
    params_layout->addWidget(bid_spread_editor_, 0, 1);
    params_layout->addWidget(new QLabel("Ask Spread"), 1, 0);
    ask_spread_editor_ = new QDoubleSpinBox();
    ask_spread_editor_->setDecimals(4);
    ask_spread_editor_->setMaximum(9999.0);
    params_layout->addWidget(ask_spread_editor_, 1, 1);
    params_layout->addWidget(new QLabel("Hedge Delta"), 2, 0);
    hedge_threshold_editor_ = new QDoubleSpinBox();
    hedge_threshold_editor_->setDecimals(4);
    hedge_threshold_editor_->setMaximum(999999.0);
    params_layout->addWidget(hedge_threshold_editor_, 2, 1);
    params_layout->addWidget(new QLabel("Quote Volume"), 3, 0);
    quote_volume_editor_ = new QSpinBox();
    quote_volume_editor_->setRange(1, 100000);
    params_layout->addWidget(quote_volume_editor_, 3, 1);
    params_layout->addWidget(new QLabel("Max Position"), 4, 0);
    max_position_editor_ = new QSpinBox();
    max_position_editor_->setRange(1, 1000000);
    params_layout->addWidget(max_position_editor_, 4, 1);
    apply_params_button_ = new QPushButton("Apply Params");
    params_layout->addWidget(apply_params_button_, 5, 0, 1, 2);
    quick_layout->addWidget(params_box, 8, 0, 1, 3);
    quick_dock->setWidget(quick_panel);
    addDockWidget(Qt::RightDockWidgetArea, quick_dock);

    connect(buy_button_, &QPushButton::clicked, this, [this] {
        side_selector_->setCurrentText("buy");
        send_manual_order();
    });
    connect(sell_button_, &QPushButton::clicked, this, [this] {
        side_selector_->setCurrentText("sell");
        send_manual_order();
    });
    connect(start_button_, &QPushButton::clicked, this, [this] { start_strategy(true); });
    connect(stop_button_, &QPushButton::clicked, this, [this] { start_strategy(false); });
    connect(apply_params_button_, &QPushButton::clicked, this, [this] { apply_strategy_params(); });
    connect(product_selector_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        refresh_ui();
    });
    connect(t_table_, &QTableWidget::cellClicked, this, [this](int row, int col) {
        auto* item = t_table_->item(row, 3);
        if (item == nullptr) return;
        const uint32_t instrument_id = item->data(Qt::UserRole).toUInt();
        const int combo_index = instrument_selector_->findData(QVariant::fromValue(instrument_id));
        if (combo_index >= 0) instrument_selector_->setCurrentIndex(combo_index);

        auto parse_price = [this, row](int price_col) {
            auto* cell = t_table_->item(row, price_col);
            return cell != nullptr ? cell->text().toDouble() : 0.0;
        };
        if (col <= 1) {
            side_selector_->setCurrentText("sell");
            price_editor_->setValue(parse_price(1));
        } else if (col >= 4 && col <= 5) {
            side_selector_->setCurrentText("buy");
            price_editor_->setValue(parse_price(4));
        } else {
            price_editor_->setValue(parse_price(2));
        }
    });

    positions_tree_ = new QTreeWidget();
    positions_tree_->setColumnCount(7);
    positions_tree_->setHeaderLabels({"Node", "Net", "Avg", "UPnL", "Delta", "Gamma", "Vega"});
    positions_tree_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    auto* positions_dock = new QDockWidget("Positions / Greeks", this);
    positions_dock->setWidget(positions_tree_);
    addDockWidget(Qt::LeftDockWidgetArea, positions_dock);

    orders_table_ = make_table({"Instrument", "Status", "FillPx", "FillQty", "Ts"});
    auto* orders_dock = new QDockWidget("Orders", this);
    orders_dock->setWidget(orders_table_);
    addDockWidget(Qt::BottomDockWidgetArea, orders_dock);

    quotes_table_ = make_table({"Instrument", "BidPx", "BidQty", "AskPx", "AskQty", "Status"});
    auto* quotes_dock = new QDockWidget("Quotes", this);
    quotes_dock->setWidget(quotes_table_);
    addDockWidget(Qt::BottomDockWidgetArea, quotes_dock);

    trades_table_ = make_table({"Instrument", "Status", "FillPx", "FillQty", "Ts"});
    auto* trades_dock = new QDockWidget("Trades", this);
    trades_dock->setWidget(trades_table_);
    addDockWidget(Qt::BottomDockWidgetArea, trades_dock);
    tabifyDockWidget(orders_dock, quotes_dock);
    tabifyDockWidget(quotes_dock, trades_dock);
}

void TraderMainWindow::refresh_ui() {
    std::lock_guard<std::mutex> lock(impl_->state.mutex);
    status_label_->setText(impl_->state.connected ? "Live" : "Disconnected");
    status_label_->setStyleSheet(QString("font-weight: 700; color: %1;")
                                     .arg(impl_->state.connected ? "#007f5f" : "#a61b1b"));

    struct Row {
        uint32_t instrument_id{0};
        QString label;
        double bid_px{0.0};
        int bid_qty{0};
        double theo{0.0};
        double ask_px{0.0};
        int ask_qty{0};
        double delta{0.0};
        int pos{0};
    };

    std::vector<Row> rows;
    const uint32_t selected_product = static_cast<uint32_t>(product_selector_->value());
    uint32_t max_product = 0;
    for (const auto& [_, meta] : impl_->state.instruments) {
        max_product = std::max(max_product, meta.product_index);
    }
    product_selector_->setMaximum(static_cast<int>(std::max<uint32_t>(31, max_product)));

    const QVariant selected_instrument_data = instrument_selector_->currentData();
    {
        QSignalBlocker blocker(instrument_selector_);
        instrument_selector_->clear();
        std::vector<const InstrumentMeta*> combo_items;
        combo_items.reserve(impl_->state.instruments.size());
        for (const auto& [_, meta] : impl_->state.instruments) {
            if (meta.product_index != selected_product) continue;
            combo_items.push_back(&meta);
        }
        std::sort(combo_items.begin(), combo_items.end(), [](const InstrumentMeta* lhs, const InstrumentMeta* rhs) {
            if (lhs->kind != rhs->kind) return lhs->kind < rhs->kind;
            if (lhs->strike != rhs->strike) return lhs->strike < rhs->strike;
            return lhs->code < rhs->code;
        });
        for (const InstrumentMeta* meta : combo_items) {
            QString label = QString::fromStdString(meta->code);
            if (meta->kind == "Option") {
                label = QString("%1  %2 %3")
                    .arg(QString::fromStdString(meta->code))
                    .arg(QString::number(meta->strike, 'f', 0))
                    .arg(QString::fromStdString(meta->option_type));
            }
            instrument_selector_->addItem(label, QVariant::fromValue(meta->instrument_id));
        }
        int restore_index = instrument_selector_->findData(selected_instrument_data);
        if (restore_index >= 0) {
            instrument_selector_->setCurrentIndex(restore_index);
        } else if (instrument_selector_->count() > 0) {
            instrument_selector_->setCurrentIndex(0);
        }
    }

    if (auto params_it = impl_->state.mm_params.find(selected_product);
        params_it != impl_->state.mm_params.end()) {
        const auto& params = params_it->second;
        if (!bid_spread_editor_->hasFocus()) bid_spread_editor_->setValue(params.bid_spread());
        if (!ask_spread_editor_->hasFocus()) ask_spread_editor_->setValue(params.ask_spread());
        if (!hedge_threshold_editor_->hasFocus()) {
            hedge_threshold_editor_->setValue(params.hedge_delta_threshold());
        }
        if (!quote_volume_editor_->hasFocus()) quote_volume_editor_->setValue(params.quote_volume());
        if (!max_position_editor_->hasFocus()) max_position_editor_->setValue(params.max_position());
    }

    for (const auto& [instrument_id, greek] : impl_->state.greeks) {
        auto meta_it = impl_->state.instruments.find(instrument_id);
        if (meta_it == impl_->state.instruments.end()) continue;
        const InstrumentMeta& meta = meta_it->second;
        if (meta.product_index != selected_product) continue;
        if (meta.kind != "Option") continue;

        Row row;
        row.instrument_id = instrument_id;
        row.label = QString("%1 %2")
            .arg(meta.strike > 0.0 ? QString::number(meta.strike, 'f', 0) : "-")
            .arg(QString::fromStdString(meta.code));
        row.theo = greek.theo_price();
        row.delta = greek.delta();
        auto tick_it = impl_->state.ticks.find(instrument_id);
        if (tick_it != impl_->state.ticks.end()) {
            row.bid_px = tick_it->second.bid_price();
            row.bid_qty = tick_it->second.bid_volume();
            row.ask_px = tick_it->second.ask_price();
            row.ask_qty = tick_it->second.ask_volume();
        }
        auto pos_it = impl_->state.positions.find(instrument_id);
        if (pos_it != impl_->state.positions.end()) row.pos = pos_it->second.net_position();
        rows.push_back(row);
    }
    std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
        const InstrumentMeta& ma = impl_->state.instruments.at(a.instrument_id);
        const InstrumentMeta& mb = impl_->state.instruments.at(b.instrument_id);
        if (ma.strike != mb.strike) return ma.strike < mb.strike;
        return a.instrument_id < b.instrument_id;
    });
    t_table_->setRowCount(static_cast<int>(rows.size()));
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        const Row& item = rows[row];
        set_cell(t_table_, row, 0, QString::number(item.bid_qty), QColor("#f6dc7d"));
        set_cell(t_table_, row, 1, QString::number(item.bid_px, 'f', 2), QColor("#ffd88a"));
        set_cell(t_table_, row, 2, QString::number(item.theo, 'f', 2), QColor("#c9f4d5"));
        set_cell(t_table_, row, 3, item.label, QColor("#d7c4ff"));
        if (auto* strike_item = t_table_->item(row, 3); strike_item != nullptr) {
            strike_item->setData(Qt::UserRole, QVariant::fromValue(item.instrument_id));
        }
        set_cell(t_table_, row, 4, QString::number(item.ask_px, 'f', 2), QColor("#ffcc8a"));
        set_cell(t_table_, row, 5, QString::number(item.ask_qty), QColor("#ffc16f"));
        set_cell(t_table_, row, 6, QString::number(item.delta, 'f', 3), QColor("#cde8ff"));
        set_cell(t_table_, row, 7, QString::number(item.pos), QColor(item.pos != 0 ? "#ffb6b6" : "#eaeaea"));
    }

    struct GroupSummary {
        int net{0};
        double avg{0.0};
        double upnl{0.0};
        double delta{0.0};
        double gamma{0.0};
        double vega{0.0};
    };

    std::map<QString, std::vector<uint32_t>> grouped_positions;
    for (const auto& [instrument_id, pos] : impl_->state.positions) {
        auto meta_it = impl_->state.instruments.find(instrument_id);
        if (meta_it == impl_->state.instruments.end()) continue;
        if (meta_it->second.product_index != selected_product) continue;
        QString group = !meta_it->second.underlying_code.empty()
            ? QString::fromStdString(meta_it->second.underlying_code)
            : QString("Product %1").arg(meta_it->second.product_index);
        grouped_positions[group].push_back(instrument_id);
    }

    positions_tree_->clear();
    auto* root = new QTreeWidgetItem(positions_tree_);
    root->setText(0, QString("Portfolio P%1").arg(selected_product));
    root->setText(4, QString::number(impl_->state.portfolio.total_delta(), 'f', 2));
    root->setText(5, QString::number(impl_->state.portfolio.total_gamma(), 'f', 2));
    root->setText(6, QString::number(impl_->state.portfolio.total_vega(), 'f', 2));
    root->setBackground(0, QColor("#d7c4ff"));

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
            child->setText(3, QString::number(pos.unrealized_pnl(), 'f', 2));
            if (greek_it != impl_->state.greeks.end()) {
                child->setText(4, QString::number(greek_it->second.delta(), 'f', 3));
                child->setText(5, QString::number(greek_it->second.gamma(), 'f', 3));
                child->setText(6, QString::number(greek_it->second.vega(), 'f', 3));
            }
            child->setBackground(0, meta.kind == "Future" ? QColor("#d6efff") : QColor("#f9f1bf"));
        }
        parent->setText(0, group_name);
        parent->setText(1, QString::number(summary.net));
        parent->setText(3, QString::number(summary.upnl, 'f', 2));
        parent->setText(4, QString::number(summary.delta, 'f', 2));
        parent->setText(5, QString::number(summary.gamma, 'f', 2));
        parent->setText(6, QString::number(summary.vega, 'f', 2));
        parent->setBackground(0, QColor("#f8d687"));
    }
    positions_tree_->expandAll();

    auto in_selected_product = [&](uint32_t instrument_id) {
        auto meta_it = impl_->state.instruments.find(instrument_id);
        return meta_it != impl_->state.instruments.end() && meta_it->second.product_index == selected_product;
    };

    std::vector<omm::proto::OrderUpdate> orders;
    orders.reserve(impl_->state.orders.size());
    for (const auto& order : impl_->state.orders) {
        if (in_selected_product(order.instrument_id())) orders.push_back(order);
    }
    orders_table_->setRowCount(static_cast<int>(orders.size()));
    for (int i = 0; i < static_cast<int>(orders.size()); ++i) {
        const auto& order = orders[i];
        QString instrument_label = QString::number(order.instrument_id());
        if (auto meta_it = impl_->state.instruments.find(order.instrument_id());
            meta_it != impl_->state.instruments.end()) {
            instrument_label = QString::fromStdString(meta_it->second.code);
        }
        QColor status_color = order.status().find("Reject") != std::string::npos ? QColor("#ffb6b6") :
                              order.status().find("Fill") != std::string::npos ? QColor("#c9f4d5") :
                              QColor("#fff0b3");
        set_cell(orders_table_, i, 0, instrument_label, QColor("#eaeaea"));
        set_cell(orders_table_, i, 1, QString::fromStdString(order.status()), status_color);
        set_cell(orders_table_, i, 2, QString::number(order.fill_price(), 'f', 2), QColor("#f7f7f7"));
        set_cell(orders_table_, i, 3, QString::number(order.fill_volume()), QColor("#f7f7f7"));
        set_cell(orders_table_, i, 4, QString::number(order.ts_ns()), QColor("#f7f7f7"));
    }

    std::vector<omm::proto::QuoteUpdate> quotes;
    quotes.reserve(impl_->state.quotes.size());
    for (const auto& quote : impl_->state.quotes) {
        if (in_selected_product(quote.instrument_id())) quotes.push_back(quote);
    }
    quotes_table_->setRowCount(static_cast<int>(quotes.size()));
    for (int i = 0; i < static_cast<int>(quotes.size()); ++i) {
        const auto& quote = quotes[i];
        QString instrument_label = QString::number(quote.instrument_id());
        if (auto meta_it = impl_->state.instruments.find(quote.instrument_id());
            meta_it != impl_->state.instruments.end()) {
            instrument_label = QString::fromStdString(meta_it->second.code);
        }
        set_cell(quotes_table_, i, 0, instrument_label, QColor("#eaeaea"));
        set_cell(quotes_table_, i, 1, QString::number(quote.bid_price(), 'f', 2), QColor("#ffe08a"));
        set_cell(quotes_table_, i, 2, QString::number(quote.bid_volume()), QColor("#ffe08a"));
        set_cell(quotes_table_, i, 3, QString::number(quote.ask_price(), 'f', 2), QColor("#ffc59c"));
        set_cell(quotes_table_, i, 4, QString::number(quote.ask_volume()), QColor("#ffc59c"));
        set_cell(quotes_table_, i, 5, QString::fromStdString(quote.status()), QColor("#e4f8f0"));
    }

    std::vector<omm::proto::OrderUpdate> trades;
    trades.reserve(impl_->state.trades.size());
    for (const auto& trade : impl_->state.trades) {
        if (in_selected_product(trade.instrument_id())) trades.push_back(trade);
    }
    trades_table_->setRowCount(static_cast<int>(trades.size()));
    for (int i = 0; i < static_cast<int>(trades.size()); ++i) {
        const auto& trade = trades[i];
        QString instrument_label = QString::number(trade.instrument_id());
        if (auto meta_it = impl_->state.instruments.find(trade.instrument_id());
            meta_it != impl_->state.instruments.end()) {
            instrument_label = QString::fromStdString(meta_it->second.code);
        }
        set_cell(trades_table_, i, 0, instrument_label, QColor("#eaeaea"));
        set_cell(trades_table_, i, 1, QString::fromStdString(trade.status()), QColor("#c9f4d5"));
        set_cell(trades_table_, i, 2, QString::number(trade.fill_price(), 'f', 2), QColor("#f7f7f7"));
        set_cell(trades_table_, i, 3, QString::number(trade.fill_volume()), QColor("#f7f7f7"));
        set_cell(trades_table_, i, 4, QString::number(trade.ts_ns()), QColor("#f7f7f7"));
    }

    vol_widget_->update();
}

void TraderMainWindow::send_manual_order() {
    const QVariant instrument_data = instrument_selector_->currentData();
    if (!instrument_data.isValid()) {
        status_label_->setText("Select an instrument");
        return;
    }
    const bool ok = impl_->client->send_manual_order(
        instrument_data.toUInt(),
        side_selector_->currentText().toStdString(),
        price_editor_->value(),
        volume_editor_->value());
    status_label_->setText(ok ? "Order sent" : "Order rejected");
}

void TraderMainWindow::start_strategy(bool enabled) {
    const bool ok = impl_->client->set_strategy_enabled(product_selector_->value(), enabled);
    status_label_->setText(ok
        ? (enabled ? "Strategy started" : "Strategy stopped")
        : "Strategy action failed");
}

void TraderMainWindow::apply_strategy_params() {
    omm::proto::MMParams params;
    params.set_bid_spread(bid_spread_editor_->value());
    params.set_ask_spread(ask_spread_editor_->value());
    params.set_hedge_delta_threshold(hedge_threshold_editor_->value());
    params.set_quote_volume(quote_volume_editor_->value());
    params.set_max_position(max_position_editor_->value());
    params.set_enabled(true);
    const bool ok = impl_->client->set_strategy_params(
        static_cast<uint32_t>(product_selector_->value()), params);
    status_label_->setText(ok ? "Params applied" : "Params update failed");
}

} // namespace omm::gui

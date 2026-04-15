#include "gui/trader_main_window.h"

#include "proto/gen/trading.grpc.pb.h"

#include <grpcpp/channel.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/client_context.h>

#include <QApplication>
#include <QCheckBox>
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
#include <QSettings>
#include <QScreen>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTime>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <deque>
#include <cctype>
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
    uint32_t curve_id{0};
    uint32_t product_index{0};
    double expiry_t{0.0};
    std::vector<double> strikes;
    std::vector<double> vols;
};

struct InstrumentMeta {
    uint32_t instrument_id{0};
    uint32_t product_index{0};
    uint32_t underlying_id{0};
    int expiry_date{0};
    std::string code;
    std::string underlying_code;
    std::string exchange_id;
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
    QString curve_label(const VolCurveSnapshot& curve) const {
        const uint32_t product_index = curve.product_index;
        QString label = QString("Product %1").arg(product_index);
        std::lock_guard<std::mutex> lock(state_->mutex);
        for (const auto& [_, meta] : state_->instruments) {
            if (meta.product_index != product_index) continue;
            if (meta.kind == "Future") {
                return QString("%1  T=%2")
                    .arg(QString::fromStdString(meta.code))
                    .arg(QString::number(curve.expiry_t, 'f', 3));
            }
            if (!meta.underlying_code.empty()) {
                label = QString("%1 / P%2  T=%3")
                    .arg(QString::fromStdString(meta.underlying_code))
                    .arg(product_index)
                    .arg(QString::number(curve.expiry_t, 'f', 3));
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
                           curve_label(curve));

        QRect plot = panel.adjusted(52, 32, -16, -34);
        painter->setPen(QColor("#d8c9b0"));
        painter->drawRect(plot);

        if (curve.strikes.size() > 1 && curve.strikes.size() == curve.vols.size()) {
            const auto [min_x_it, max_x_it] = std::minmax_element(curve.strikes.begin(), curve.strikes.end());
            const auto [min_y_it, max_y_it] = std::minmax_element(curve.vols.begin(), curve.vols.end());
            const double min_x = *min_x_it;
            const double max_x = *max_x_it;
            const double min_y = *min_y_it;
            const double max_y = std::max(min_y + 1e-6, *max_y_it);
            const double x_span = std::max(1e-9, max_x - min_x);
            const double y_span = std::max(1e-9, max_y - min_y);

            painter->setPen(QColor("#ece2cc"));
            for (int tick = 0; tick < 4; ++tick) {
                const double alpha = static_cast<double>(tick) / 3.0;
                const int y = plot.bottom() - static_cast<int>(alpha * plot.height());
                painter->drawLine(plot.left(), y, plot.right(), y);
            }
            for (int tick = 0; tick < 4; ++tick) {
                const double alpha = static_cast<double>(tick) / 3.0;
                const int x = plot.left() + static_cast<int>(alpha * plot.width());
                painter->drawLine(x, plot.top(), x, plot.bottom());
            }

            painter->setPen(QColor("#6f6044"));
            for (int tick = 0; tick < 4; ++tick) {
                const double alpha = static_cast<double>(tick) / 3.0;
                const double vol = min_y + alpha * y_span;
                const int y = plot.bottom() - static_cast<int>(alpha * plot.height());
                QRect label_rect(panel.left() + 6, y - 10, 40, 20);
                painter->drawText(label_rect,
                                  Qt::AlignRight | Qt::AlignVCenter,
                                  QString::number(vol, 'f', 3));
            }

            for (int tick = 0; tick < 4; ++tick) {
                const double alpha = static_cast<double>(tick) / 3.0;
                const double strike = min_x + alpha * x_span;
                const int x = plot.left() + static_cast<int>(alpha * plot.width());
                QRect label_rect(x - 28, plot.bottom() + 6, 56, 18);
                painter->drawText(label_rect,
                                  Qt::AlignHCenter | Qt::AlignTop,
                                  QString::number(strike, 'f', 0));
            }

            QPainterPath path;
            for (std::size_t i = 0; i < curve.strikes.size(); ++i) {
                const double x_alpha = (curve.strikes[i] - min_x) / x_span;
                const double y_alpha = (curve.vols[i] - min_y) / y_span;
                QPointF point(plot.left() + x_alpha * plot.width(),
                              plot.bottom() - y_alpha * plot.height());
                if (i == 0) path.moveTo(point);
                else path.lineTo(point);
            }
            painter->setPen(QPen(QColor("#008f8c"), 2.0));
            painter->drawPath(path);

            painter->setPen(QPen(QColor("#008f8c"), 4.0));
            for (std::size_t i = 0; i < curve.strikes.size(); ++i) {
                const double x_alpha = (curve.strikes[i] - min_x) / x_span;
                const double y_alpha = (curve.vols[i] - min_y) / y_span;
                QPointF point(plot.left() + x_alpha * plot.width(),
                              plot.bottom() - y_alpha * plot.height());
                painter->drawPoint(point);
            }
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
                        meta.expiry_date = instrument.expiry_date();
                        meta.code = instrument.code();
                        meta.underlying_code = instrument.underlying_code();
                        meta.exchange_id = instrument.exchange_id();
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
                for (int slice_index = 0; slice_index < surface.slices_size(); ++slice_index) {
                    const auto& slice = surface.slices(slice_index);
                    VolCurveSnapshot curve;
                    curve.curve_id = surface.product_index() * 100u + static_cast<uint32_t>(slice_index);
                    curve.product_index = surface.product_index();
                    curve.expiry_t = slice.expiry_t();
                    for (int i = 0; i < slice.strikes_size() && i < slice.vols_size(); ++i) {
                        curve.strikes.push_back(slice.strikes(i));
                        curve.vols.push_back(slice.vols(i));
                    }
                    state_->curves[curve.curve_id] = std::move(curve);
                }
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

QColor risk_color(double value, double warning, double danger) {
    const double magnitude = std::abs(value);
    if (magnitude >= danger) return QColor("#ffb3b3");
    if (magnitude >= warning) return QColor("#ffe39b");
    return QColor("#dff4df");
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

} // namespace

struct TraderMainWindow::Impl {
    SharedState state;
    std::unique_ptr<GrpcTraderClient> client;
    uint32_t selected_product_index{0};
    uint32_t selected_instrument_id{0};
    bool ui_state_restored{false};
};

TraderMainWindow::TraderMainWindow(std::string grpc_endpoint, QWidget* parent)
    : QMainWindow(parent),
      grpc_endpoint_(std::move(grpc_endpoint)),
      impl_(std::make_unique<Impl>()) {
    setWindowTitle(QString("optionMM Trader Dashboard - %1").arg(QString::fromStdString(grpc_endpoint_)));
    build_ui();
    restore_ui_state();
    impl_->client = std::make_unique<GrpcTraderClient>(grpc_endpoint_, &impl_->state);
    impl_->client->start();

    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] { refresh_ui(); });
    timer->start(120);
}

TraderMainWindow::~TraderMainWindow() = default;

void TraderMainWindow::closeEvent(QCloseEvent* event) {
    save_ui_state();
    QMainWindow::closeEvent(event);
}

void TraderMainWindow::build_ui() {
    auto* toolbar = addToolBar("Desk");
    toolbar->setMovable(false);
    toolbar->addWidget(new QLabel("Product"));
    product_selector_ = new QComboBox();
    toolbar->addWidget(product_selector_);
    toolbar->addSeparator();
    status_label_ = new QLabel("Connecting");
    toolbar->addWidget(status_label_);
    toolbar->addSeparator();
    delta_label_ = new QLabel("Delta --");
    gamma_label_ = new QLabel("Gamma --");
    vega_label_ = new QLabel("Vega --");
    toolbar->addWidget(delta_label_);
    toolbar->addWidget(gamma_label_);
    toolbar->addWidget(vega_label_);

    auto* central = new QWidget();
    auto* layout = new QVBoxLayout(central);
    auto* hero = new QLabel("T-Table");
    hero->setStyleSheet("font-size: 22px; font-weight: 700; color: #2b2418;");
    layout->addWidget(hero);

    t_table_ = make_table({"C.MM", "C.St", "C.BQty", "C.Bid", "C.Theo", "C.Ask", "C.AQty",
                           "Exp", "Strike", "Net",
                           "P.BQty", "P.Bid", "P.Theo", "P.Ask", "P.AQty", "P.St", "P.MM"});
    t_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    t_table_->verticalHeader()->setDefaultSectionSize(22);
    layout->addWidget(t_table_);
    setCentralWidget(central);

    vol_dock_ = new QDockWidget("ORC Wing / Vol Curves", this);
    vol_dock_->setFeatures(QDockWidget::DockWidgetMovable |
                           QDockWidget::DockWidgetFloatable |
                           QDockWidget::DockWidgetClosable);
    auto* vol_panel = new QWidget();
    auto* vol_layout = new QVBoxLayout(vol_panel);
    vol_layout->setContentsMargins(8, 8, 8, 8);
    vol_widget_ = new VolCurveGridWidget(&impl_->state);
    vol_layout->addWidget(vol_widget_);
    vol_dock_->setWidget(vol_panel);
    addDockWidget(Qt::RightDockWidgetArea, vol_dock_);
    ensure_vol_window();

    auto* quick_dock = new QDockWidget("Quick Order / Strategy", this);
    quick_dock->setFeatures(QDockWidget::DockWidgetMovable |
                            QDockWidget::DockWidgetFloatable |
                            QDockWidget::DockWidgetClosable);
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

    auto configure_double = [](QDoubleSpinBox* box,
                               int decimals,
                               double min_value,
                               double max_value,
                               double step) {
        box->setDecimals(decimals);
        box->setRange(min_value, max_value);
        box->setSingleStep(step);
    };
    auto configure_int = [](QSpinBox* box, int min_value, int max_value) {
        box->setRange(min_value, max_value);
    };

    params_layout->addWidget(new QLabel("Bid Spread"), 0, 0);
    bid_spread_editor_ = new QDoubleSpinBox();
    configure_double(bid_spread_editor_, 4, 0.0, 9999.0, 0.1);
    params_layout->addWidget(bid_spread_editor_, 0, 1);

    params_layout->addWidget(new QLabel("Ask Spread"), 0, 2);
    ask_spread_editor_ = new QDoubleSpinBox();
    configure_double(ask_spread_editor_, 4, 0.0, 9999.0, 0.1);
    params_layout->addWidget(ask_spread_editor_, 0, 3);

    params_layout->addWidget(new QLabel("Base Half"), 1, 0);
    base_half_spread_editor_ = new QDoubleSpinBox();
    configure_double(base_half_spread_editor_, 4, 0.0, 9999.0, 0.1);
    params_layout->addWidget(base_half_spread_editor_, 1, 1);

    params_layout->addWidget(new QLabel("Min Half"), 1, 2);
    min_half_spread_editor_ = new QDoubleSpinBox();
    configure_double(min_half_spread_editor_, 4, 0.0, 9999.0, 0.1);
    params_layout->addWidget(min_half_spread_editor_, 1, 3);

    params_layout->addWidget(new QLabel("Max Half"), 2, 0);
    max_half_spread_editor_ = new QDoubleSpinBox();
    configure_double(max_half_spread_editor_, 4, 0.0, 9999.0, 0.1);
    params_layout->addWidget(max_half_spread_editor_, 2, 1);

    params_layout->addWidget(new QLabel("Follow Weight"), 2, 2);
    follow_weight_editor_ = new QDoubleSpinBox();
    configure_double(follow_weight_editor_, 4, 0.0, 1.0, 0.05);
    params_layout->addWidget(follow_weight_editor_, 2, 3);

    params_layout->addWidget(new QLabel("Inv Skew / Lot"), 3, 0);
    inventory_skew_editor_ = new QDoubleSpinBox();
    configure_double(inventory_skew_editor_, 4, -9999.0, 9999.0, 0.01);
    params_layout->addWidget(inventory_skew_editor_, 3, 1);

    params_layout->addWidget(new QLabel("Mkt Width Widen"), 3, 2);
    market_width_widen_editor_ = new QDoubleSpinBox();
    configure_double(market_width_widen_editor_, 4, 0.0, 9999.0, 0.1);
    params_layout->addWidget(market_width_widen_editor_, 3, 3);

    params_layout->addWidget(new QLabel("Hedge Delta"), 4, 0);
    hedge_threshold_editor_ = new QDoubleSpinBox();
    configure_double(hedge_threshold_editor_, 4, 0.0, 999999.0, 1.0);
    params_layout->addWidget(hedge_threshold_editor_, 4, 1);

    params_layout->addWidget(new QLabel("Product Vega"), 4, 2);
    product_vega_threshold_editor_ = new QDoubleSpinBox();
    configure_double(product_vega_threshold_editor_, 4, 0.0, 99999999.0, 10.0);
    params_layout->addWidget(product_vega_threshold_editor_, 4, 3);

    params_layout->addWidget(new QLabel("Quote Volume"), 5, 0);
    quote_volume_editor_ = new QSpinBox();
    configure_int(quote_volume_editor_, 0, 100000);
    params_layout->addWidget(quote_volume_editor_, 5, 1);

    params_layout->addWidget(new QLabel("Warning Pos"), 5, 2);
    warning_position_editor_ = new QSpinBox();
    configure_int(warning_position_editor_, 1, 1000000);
    params_layout->addWidget(warning_position_editor_, 5, 3);

    params_layout->addWidget(new QLabel("Max Position"), 6, 0);
    max_position_editor_ = new QSpinBox();
    configure_int(max_position_editor_, 1, 1000000);
    params_layout->addWidget(max_position_editor_, 6, 1);

    params_layout->addWidget(new QLabel("Requote Eps"), 6, 2);
    requote_epsilon_editor_ = new QDoubleSpinBox();
    configure_double(requote_epsilon_editor_, 4, 0.0, 9999.0, 0.1);
    params_layout->addWidget(requote_epsilon_editor_, 6, 3);

    params_layout->addWidget(new QLabel("Min Quote ms"), 7, 0);
    min_quote_interval_editor_ = new QDoubleSpinBox();
    configure_double(min_quote_interval_editor_, 3, 0.0, 60000.0, 1.0);
    params_layout->addWidget(min_quote_interval_editor_, 7, 1);

    params_layout->addWidget(new QLabel("Underly Shock"), 7, 2);
    underlying_move_widen_editor_ = new QDoubleSpinBox();
    configure_double(underlying_move_widen_editor_, 4, 0.0, 9999.0, 0.1);
    params_layout->addWidget(underlying_move_widen_editor_, 7, 3);

    use_one_sided_editor_ = new QCheckBox("One-Sided At Limits");
    params_layout->addWidget(use_one_sided_editor_, 8, 0, 1, 2);

    apply_params_button_ = new QPushButton("Apply Params");
    params_layout->addWidget(apply_params_button_, 8, 2, 1, 2);
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
    connect(product_selector_, &QComboBox::currentIndexChanged, this, [this](int) {
        const QVariant data = product_selector_->currentData();
        if (data.isValid()) impl_->selected_product_index = data.toUInt();
        refresh_ui();
    });
    connect(instrument_selector_, &QComboBox::currentIndexChanged, this, [this](int) {
        const QVariant data = instrument_selector_->currentData();
        if (data.isValid()) impl_->selected_instrument_id = data.toUInt();
    });
    connect(vol_dock_, &QDockWidget::topLevelChanged, this, [this](bool floating) {
        if (!floating || vol_dock_ == nullptr) return;
        QTimer::singleShot(0, this, [this] {
            if (vol_dock_ == nullptr || !vol_dock_->isFloating()) return;
            ensure_vol_window();
            if (vol_window_ == nullptr) return;

            const QRect floating_geometry = vol_dock_->frameGeometry();
            {
                QSignalBlocker blocker(vol_dock_);
                vol_dock_->setFloating(false);
            }
            vol_dock_->hide();
            vol_window_->setGeometry(floating_geometry);
            vol_window_->show();
            vol_window_->raise();
            vol_window_->activateWindow();
        });
    });
    connect(t_table_, &QTableWidget::cellClicked, this, [this](int row, int col) {
        auto* item = t_table_->item(row, col);
        if (item == nullptr || !item->data(Qt::UserRole).isValid()) {
            item = t_table_->item(row, 8);
        }
        if (item == nullptr) return;
        const uint32_t instrument_id = item->data(Qt::UserRole).toUInt();
        if (instrument_id == 0) return;
        const int combo_index = instrument_selector_->findData(QVariant::fromValue(instrument_id));
        if (combo_index >= 0) instrument_selector_->setCurrentIndex(combo_index);

        auto parse_price = [this, row](int price_col) {
            auto* cell = t_table_->item(row, price_col);
            return cell != nullptr ? cell->text().toDouble() : 0.0;
        };
        if (col == 3 || col == 5) {
            side_selector_->setCurrentText(col == 3 ? "sell" : "buy");
            price_editor_->setValue(parse_price(col));
        } else if (col == 11 || col == 13) {
            side_selector_->setCurrentText(col == 11 ? "sell" : "buy");
            price_editor_->setValue(parse_price(col));
        } else if (col < 7) {
            side_selector_->setCurrentText("sell");
            price_editor_->setValue(parse_price(3));
        } else if (col > 9) {
            side_selector_->setCurrentText("buy");
            price_editor_->setValue(parse_price(13));
        } else {
            price_editor_->setValue(parse_price(4));
        }
    });

    positions_tree_ = new QTreeWidget();
    positions_tree_->setColumnCount(7);
    positions_tree_->setHeaderLabels({"Node", "Net", "Avg", "UPnL", "Delta", "Gamma", "Vega"});
    positions_tree_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    auto* positions_dock = new QDockWidget("Positions / Greeks", this);
    positions_dock->setFeatures(QDockWidget::DockWidgetMovable |
                                QDockWidget::DockWidgetFloatable |
                                QDockWidget::DockWidgetClosable);
    positions_dock->setWidget(positions_tree_);
    addDockWidget(Qt::LeftDockWidgetArea, positions_dock);

    orders_table_ = make_table({"OrderId", "Instrument", "Exchange", "Side", "Price", "Volume", "Status", "FillPx", "FillQty", "Ts"});
    auto* orders_dock = new QDockWidget("Orders", this);
    orders_dock->setFeatures(QDockWidget::DockWidgetMovable |
                             QDockWidget::DockWidgetFloatable |
                             QDockWidget::DockWidgetClosable);
    orders_dock->setWidget(orders_table_);
    addDockWidget(Qt::BottomDockWidgetArea, orders_dock);

    quotes_table_ = make_table({"Instrument", "BidPx", "BidQty", "AskPx", "AskQty", "Status"});
    auto* quotes_dock = new QDockWidget("Quotes", this);
    quotes_dock->setFeatures(QDockWidget::DockWidgetMovable |
                             QDockWidget::DockWidgetFloatable |
                             QDockWidget::DockWidgetClosable);
    quotes_dock->setWidget(quotes_table_);
    addDockWidget(Qt::BottomDockWidgetArea, quotes_dock);

    trades_table_ = make_table({"TradeId", "OrderId", "Instrument", "Exchange", "Side", "Price", "Qty", "Ts"});
    auto* trades_dock = new QDockWidget("Trades", this);
    trades_dock->setFeatures(QDockWidget::DockWidgetMovable |
                             QDockWidget::DockWidgetFloatable |
                             QDockWidget::DockWidgetClosable);
    trades_dock->setWidget(trades_table_);
    addDockWidget(Qt::BottomDockWidgetArea, trades_dock);
    tabifyDockWidget(orders_dock, quotes_dock);
    tabifyDockWidget(quotes_dock, trades_dock);
    resizeDocks({vol_dock_}, {520}, Qt::Horizontal);
}

void TraderMainWindow::ensure_vol_window() {
    if (vol_window_ != nullptr) return;

    const Qt::WindowFlags flags = Qt::Window |
                                  Qt::WindowTitleHint |
                                  Qt::WindowSystemMenuHint |
                                  Qt::WindowMinimizeButtonHint |
                                  Qt::WindowMaximizeButtonHint |
                                  Qt::WindowCloseButtonHint;
    vol_window_ = new QMainWindow(nullptr, flags);
    vol_window_->setAttribute(Qt::WA_DeleteOnClose, false);
    vol_window_->setWindowTitle("ORC Wing / Vol Curves");
    vol_window_->resize(1280, 900);

    vol_window_widget_ = new VolCurveGridWidget(&impl_->state, vol_window_);
    vol_window_->setCentralWidget(vol_window_widget_);
}

void TraderMainWindow::restore_ui_state() {
    QSettings settings;
    const QByteArray geometry = settings.value("main/geometry").toByteArray();
    const QByteArray state = settings.value("main/state").toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
    if (!state.isEmpty()) restoreState(state);
    impl_->selected_product_index = settings.value("selection/product", 0u).toUInt();
    impl_->selected_instrument_id = settings.value("selection/instrument", 0u).toUInt();

    ensure_vol_window();
    const QByteArray vol_geometry = settings.value("vol/geometry").toByteArray();
    if (!vol_geometry.isEmpty()) vol_window_->restoreGeometry(vol_geometry);
    const bool vol_window_visible = settings.value("vol/window_visible", false).toBool();
    if (vol_window_visible) {
        vol_dock_->hide();
        vol_window_->show();
    }
    impl_->ui_state_restored = true;
}

void TraderMainWindow::save_ui_state() const {
    QSettings settings;
    settings.setValue("main/geometry", saveGeometry());
    settings.setValue("main/state", saveState());
    settings.setValue("selection/product", impl_->selected_product_index);
    settings.setValue("selection/instrument", impl_->selected_instrument_id);
    if (vol_window_ != nullptr) {
        settings.setValue("vol/geometry", vol_window_->saveGeometry());
        settings.setValue("vol/window_visible", vol_window_->isVisible());
    }
}

void TraderMainWindow::refresh_ui() {
    std::lock_guard<std::mutex> lock(impl_->state.mutex);
    status_label_->setText(impl_->state.connected ? "Live" : "Disconnected");
    status_label_->setStyleSheet(QString("font-weight: 700; color: %1;")
                                     .arg(impl_->state.connected ? "#007f5f" : "#a61b1b"));
    const bool connected = impl_->state.connected;
    buy_button_->setEnabled(connected);
    sell_button_->setEnabled(connected);
    start_button_->setEnabled(connected);
    stop_button_->setEnabled(connected);
    apply_params_button_->setEnabled(connected);

    struct SideView {
        uint32_t instrument_id{0};
        QString code;
        double bid_px{0.0};
        int bid_qty{0};
        double theo{0.0};
        double ask_px{0.0};
        int ask_qty{0};
        int pos{0};
        QString mm_label{"OFF"};
        QString status_label{"Off"};
        QColor mm_color{"#e7e7e7"};
        QColor status_color{"#ececec"};
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

    if (auto params_it = impl_->state.mm_params.find(selected_product);
        params_it != impl_->state.mm_params.end()) {
        const auto& params = params_it->second;
        auto sync_double = [](QDoubleSpinBox* editor, double value) {
            if (editor != nullptr && !editor->hasFocus()) editor->setValue(value);
        };
        auto sync_int = [](QSpinBox* editor, int value) {
            if (editor != nullptr && !editor->hasFocus()) editor->setValue(value);
        };
        auto sync_check = [](QCheckBox* editor, bool value) {
            if (editor != nullptr && !editor->hasFocus()) editor->setChecked(value);
        };

        sync_double(bid_spread_editor_, params.bid_spread());
        sync_double(ask_spread_editor_, params.ask_spread());
        sync_double(base_half_spread_editor_, params.base_half_spread_ticks());
        sync_double(min_half_spread_editor_, params.min_half_spread_ticks());
        sync_double(max_half_spread_editor_, params.max_half_spread_ticks());
        sync_double(follow_weight_editor_, params.follow_weight());
        sync_double(inventory_skew_editor_, params.inventory_skew_per_lot_ticks());
        sync_double(market_width_widen_editor_, params.market_width_widen_threshold_ticks());
        sync_double(hedge_threshold_editor_, params.hedge_delta_threshold());
        sync_double(product_vega_threshold_editor_, params.product_vega_threshold());
        sync_double(requote_epsilon_editor_, params.requote_price_epsilon_ticks());
        sync_double(min_quote_interval_editor_, params.min_quote_interval_ms());
        sync_double(underlying_move_widen_editor_, params.underlying_move_widen_threshold_ticks());
        sync_int(quote_volume_editor_, params.quote_volume());
        sync_int(warning_position_editor_, params.warning_position());
        sync_int(max_position_editor_, params.max_position());
        sync_check(use_one_sided_editor_, params.use_one_sided_at_limits());
    }
    delta_label_->setText(QString("Delta %1").arg(QString::number(impl_->state.portfolio.total_delta(), 'f', 1)));
    gamma_label_->setText(QString("Gamma %1").arg(QString::number(impl_->state.portfolio.total_gamma(), 'f', 1)));
    vega_label_->setText(QString("Vega %1").arg(QString::number(impl_->state.portfolio.total_vega(), 'f', 1)));
    delta_label_->setStyleSheet(QString("padding:2px 8px; border-radius:4px; background:%1;")
        .arg(risk_color(impl_->state.portfolio.total_delta(), 500.0, 2500.0).name()));
    gamma_label_->setStyleSheet(QString("padding:2px 8px; border-radius:4px; background:%1;")
        .arg(risk_color(impl_->state.portfolio.total_gamma(), 250.0, 1200.0).name()));
    vega_label_->setStyleSheet(QString("padding:2px 8px; border-radius:4px; background:%1;")
        .arg(risk_color(impl_->state.portfolio.total_vega(), 5000.0, 25000.0).name()));

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
        if (auto quote_it = latest_quotes.find(meta.instrument_id); quote_it != latest_quotes.end()) {
            const auto* quote = quote_it->second;
            const bool has_bid = quote->bid_volume() > 0;
            const bool has_ask = quote->ask_volume() > 0;
            side.mm_label = has_bid && has_ask ? "2W" : has_bid ? "BID" : has_ask ? "ASK" : "OFF";
            side.status_label = has_bid && has_ask ? "Live" : has_bid || has_ask ? "1-Way" : "Off";
            side.mm_color = has_bid || has_ask ? QColor("#d7f0cf") : QColor("#ececec");
            side.status_color = QColor("#dcefff");
            if (quote->status().find("Ack") != std::string::npos) {
                side.status_label = "Ack";
                side.status_color = QColor("#cfeecf");
            } else if (quote->status().find("Fill") != std::string::npos) {
                side.status_label = "Fill";
                side.status_color = QColor("#ffe3b3");
            } else if (quote->status().find("Reject") != std::string::npos) {
                side.status_label = "Reject";
                side.status_color = QColor("#ffcccc");
            } else if (quote->status().find("New") != std::string::npos) {
                side.status_label = "New";
                side.status_color = QColor("#dcefff");
            }
            if (has_bid) side.bid_color = QColor("#f3f8a6");
            if (has_ask) side.ask_color = QColor("#ffddb7");
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
        set_cell(t_table_, row, 0, item.call.mm_label, item.call.mm_color);
        set_cell(t_table_, row, 1, item.call.status_label, item.call.status_color);
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
        set_cell(t_table_, row, 15, item.put.status_label, item.put.status_color);
        set_cell(t_table_, row, 16, item.put.mm_label, item.put.mm_color);

        for (int col : {0, 1, 2, 3, 4, 5, 6}) {
            if (auto* cell = t_table_->item(row, col); cell != nullptr && item.call.instrument_id != 0) {
                cell->setData(Qt::UserRole, QVariant::fromValue(item.call.instrument_id));
                cell->setToolTip(item.call.code);
            }
        }
        for (int col : {10, 11, 12, 13, 14, 15, 16}) {
            if (auto* cell = t_table_->item(row, col); cell != nullptr && item.put.instrument_id != 0) {
                cell->setData(Qt::UserRole, QVariant::fromValue(item.put.instrument_id));
                cell->setToolTip(item.put.code);
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
    root->setText(0, QString("Portfolio %1").arg(product_selector_->currentText()));
    root->setText(4, QString::number(impl_->state.portfolio.total_delta(), 'f', 2));
    root->setText(5, QString::number(impl_->state.portfolio.total_gamma(), 'f', 2));
    root->setText(6, QString::number(impl_->state.portfolio.total_vega(), 'f', 2));
    root->setBackground(0, QColor("#d7c4ff"));
    root->setBackground(4, risk_color(impl_->state.portfolio.total_delta(), 500.0, 2500.0));
    root->setBackground(5, risk_color(impl_->state.portfolio.total_gamma(), 250.0, 1200.0));
    root->setBackground(6, risk_color(impl_->state.portfolio.total_vega(), 5000.0, 25000.0));

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
        parent->setBackground(4, risk_color(summary.delta, 200.0, 1000.0));
        parent->setBackground(5, risk_color(summary.gamma, 100.0, 600.0));
        parent->setBackground(6, risk_color(summary.vega, 2000.0, 10000.0));
    }
    positions_tree_->expandAll();

    auto in_selected_product = [&](uint32_t instrument_id) {
        auto meta_it = impl_->state.instruments.find(instrument_id);
        return meta_it != impl_->state.instruments.end() && meta_it->second.product_index == selected_product;
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
    orders_table_->setRowCount(static_cast<int>(orders.size()));
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

        QString instrument_label = QString::number(order.instrument_id());
        if (auto meta_it = impl_->state.instruments.find(order.instrument_id());
            meta_it != impl_->state.instruments.end()) {
            instrument_label = QString::fromStdString(meta_it->second.code);
        }
        QColor status_color = order.status().find("Reject") != std::string::npos ? QColor("#ffb6b6") :
                              order.status().find("Fill") != std::string::npos ? QColor("#c9f4d5") :
                              QColor("#fff0b3");
        QString exchange_label = order.exchange_id().empty()
            ? "-"
            : QString::fromStdString(order.exchange_id());
        if (exchange_label == "-") {
            if (auto meta_it = impl_->state.instruments.find(order.instrument_id());
                meta_it != impl_->state.instruments.end() && !meta_it->second.exchange_id.empty()) {
                exchange_label = QString::fromStdString(meta_it->second.exchange_id);
            } else if (auto meta_it = impl_->state.instruments.find(order.instrument_id());
                       meta_it != impl_->state.instruments.end()) {
                const QString inferred = infer_exchange_from_code(meta_it->second.code);
                if (!inferred.isEmpty()) exchange_label = inferred;
            }
        }
        const QString side_label = order.side().empty()
            ? "-"
            : QString::fromStdString(order.side());
        const QString price_label = order.price() > 0.0
            ? QString::number(order.price(), 'f', 2)
            : "-";
        const QString volume_label = order.volume() > 0
            ? QString::number(order.volume())
            : "-";
        const QString ts_label = format_monotonic_ts(order.ts_ns());
        set_cell(orders_table_, i, 0, QString::number(order.client_order_id()), QColor("#eaeaea"));
        set_cell(orders_table_, i, 1, instrument_label, QColor("#eaeaea"));
        set_cell(orders_table_, i, 2, exchange_label, QColor("#f7f7f7"));
        set_cell(orders_table_, i, 3, side_label, QColor("#f7f7f7"));
        set_cell(orders_table_, i, 4, price_label, QColor("#f7f7f7"));
        set_cell(orders_table_, i, 5, volume_label, QColor("#f7f7f7"));
        set_cell(orders_table_, i, 6, QString::fromStdString(order.status()), status_color);
        set_cell(orders_table_, i, 7, QString::number(order.fill_price(), 'f', 2), QColor("#f7f7f7"));
        set_cell(orders_table_, i, 8, QString::number(order.fill_volume()), QColor("#f7f7f7"));
        set_cell(orders_table_, i, 9, ts_label, QColor("#f7f7f7"));
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
        QString exchange_label = trade.exchange_id().empty() ? "-" : QString::fromStdString(trade.exchange_id());
        if (exchange_label == "-") {
            if (auto meta_it = impl_->state.instruments.find(trade.instrument_id());
                meta_it != impl_->state.instruments.end() && !meta_it->second.exchange_id.empty()) {
                exchange_label = QString::fromStdString(meta_it->second.exchange_id);
            }
        }
        const QString side_label = trade.side().empty() ? "-" : QString::fromStdString(trade.side());
        set_cell(trades_table_, i, 0, QString::number(trade.exchange_trade_id()), QColor("#e8f4ff"));
        set_cell(trades_table_, i, 1, QString::number(trade.client_order_id()), QColor("#f1f1f1"));
        set_cell(trades_table_, i, 2, instrument_label, QColor("#eaeaea"));
        set_cell(trades_table_, i, 3, exchange_label, QColor("#f7f7f7"));
        set_cell(trades_table_, i, 4, side_label, QColor(side_label == "Buy" ? "#dff4df" : "#ffd9d1"));
        set_cell(trades_table_, i, 5, QString::number(trade.fill_price(), 'f', 2), QColor("#f7f7f7"));
        set_cell(trades_table_, i, 6, QString::number(trade.fill_volume()), QColor("#f7f7f7"));
        set_cell(trades_table_, i, 7, format_monotonic_ts(trade.ts_ns()), QColor("#f7f7f7"));
    }

    if (vol_widget_ != nullptr && vol_dock_ != nullptr && vol_dock_->isVisible()) {
        vol_widget_->update();
    }
    if (vol_window_ != nullptr && vol_window_->isVisible() && vol_window_widget_ != nullptr) {
        vol_window_widget_->update();
    }
    if (vol_window_ != nullptr && !vol_window_->isVisible() && vol_dock_ != nullptr && !vol_dock_->isVisible()) {
        vol_dock_->show();
    }
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
    const bool ok = impl_->client->set_strategy_enabled(product_selector_->currentData().toInt(), enabled);
    status_label_->setText(ok
        ? (enabled ? "Strategy started" : "Strategy stopped")
        : "Strategy action failed");
}

void TraderMainWindow::apply_strategy_params() {
    omm::proto::MMParams params;
    params.set_bid_spread(bid_spread_editor_->value());
    params.set_ask_spread(ask_spread_editor_->value());
    params.set_base_half_spread_ticks(base_half_spread_editor_->value());
    params.set_min_half_spread_ticks(min_half_spread_editor_->value());
    params.set_max_half_spread_ticks(max_half_spread_editor_->value());
    params.set_follow_weight(follow_weight_editor_->value());
    params.set_inventory_skew_per_lot_ticks(inventory_skew_editor_->value());
    params.set_market_width_widen_threshold_ticks(market_width_widen_editor_->value());
    params.set_hedge_delta_threshold(hedge_threshold_editor_->value());
    params.set_product_vega_threshold(product_vega_threshold_editor_->value());
    params.set_quote_volume(quote_volume_editor_->value());
    params.set_warning_position(warning_position_editor_->value());
    params.set_max_position(max_position_editor_->value());
    params.set_requote_price_epsilon_ticks(requote_epsilon_editor_->value());
    params.set_min_quote_interval_ms(min_quote_interval_editor_->value());
    params.set_underlying_move_widen_threshold_ticks(underlying_move_widen_editor_->value());
    params.set_use_one_sided_at_limits(use_one_sided_editor_->isChecked());
    const bool ok = impl_->client->set_strategy_params(
        product_selector_->currentData().toUInt(), params);
    status_label_->setText(ok ? "Params applied" : "Params update failed");
}

} // namespace omm::gui

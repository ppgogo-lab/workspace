#include "common/config.h"
#include "gui/trader_main_window.h"

#include "trading.grpc.pb.h"

#include <grpcpp/channel.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/client_context.h>

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QScreen>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QStringList>
#include <QTime>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cmath>
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

constexpr int kWorkspaceStateVersion = 1;

uint64_t make_arb_key(uint32_t product_index,
                      omm::proto::ArbitrageStrategyType strategy_type);

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
    omm::proto::RiskState risk_state;
    std::unordered_map<uint32_t, InstrumentMeta> instruments;
    std::unordered_map<uint32_t, omm::proto::Tick> ticks;
    std::unordered_map<uint32_t, omm::proto::Greeks> greeks;
    std::unordered_map<uint32_t, omm::proto::Position> positions;
    std::deque<omm::proto::OrderUpdate> orders;
    std::deque<omm::proto::QuoteUpdate> quotes;
    std::deque<omm::proto::OrderUpdate> trades;
    std::deque<omm::proto::RiskAlert> alerts;
    std::map<uint32_t, omm::proto::MMParams> mm_params;
    std::map<uint64_t, omm::proto::ArbParams> arb_params;
    std::unordered_map<uint32_t, omm::proto::ProductMMState> product_states;
    std::unordered_map<uint32_t, omm::proto::InstrumentMMState> instrument_states;
    std::map<uint64_t, omm::proto::ArbStrategyState> arb_strategy_states;
    std::vector<omm::proto::PcpOpportunityState> pcp_opportunities;
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
        stream_threads_.emplace_back([this] { alerts_loop(); });
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

    bool set_arb_strategy_enabled(uint32_t product_index,
                                  omm::proto::ArbitrageStrategyType strategy_type,
                                  bool enabled) {
        grpc::ClientContext ctx;
        omm::proto::ArbStartStopRequest req;
        omm::proto::ArbStartStopResponse resp;
        req.mutable_id()->set_product_index(product_index);
        req.mutable_id()->set_strategy_type(strategy_type);
        grpc::Status status = enabled
            ? stub_->StartArbStrategy(&ctx, req, &resp)
            : stub_->StopArbStrategy(&ctx, req, &resp);
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

    bool set_risk_thresholds(int max_net_position,
                             double max_delta,
                             double max_gamma,
                             double max_vega) {
        grpc::ClientContext ctx;
        omm::proto::SetRiskThresholdRequest req;
        omm::proto::SetRiskThresholdResponse resp;
        auto* threshold = req.mutable_threshold();
        threshold->set_max_net_position(max_net_position);
        threshold->set_max_delta(max_delta);
        threshold->set_max_gamma(max_gamma);
        threshold->set_max_vega(max_vega);
        grpc::Status status = stub_->SetRiskThreshold(&ctx, req, &resp);
        return status.ok() && resp.ok();
    }

    bool cancel_order(uint64_t order_id, uint32_t instrument_id) {
        grpc::ClientContext ctx;
        omm::proto::CancelOrderRequest req;
        omm::proto::CancelOrderResponse resp;
        req.set_order_id(order_id);
        req.set_instrument_id(instrument_id);
        grpc::Status status = stub_->CancelOrder(&ctx, req, &resp);
        return status.ok() && resp.ok();
    }

    bool cancel_quote(uint64_t quote_id, uint32_t instrument_id) {
        grpc::ClientContext ctx;
        omm::proto::CancelQuoteRequest req;
        omm::proto::CancelQuoteResponse resp;
        req.set_quote_id(quote_id);
        req.set_instrument_id(instrument_id);
        grpc::Status status = stub_->CancelQuote(&ctx, req, &resp);
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
                    state_->risk_state = resp.risk_state();
                    state_->mm_params.clear();
                    for (int i = 0; i < resp.mm_params_size(); ++i) {
                        state_->mm_params[static_cast<uint32_t>(i)] = resp.mm_params(i);
                    }
                    state_->arb_params.clear();
                    for (const auto& entry : resp.arb_params()) {
                        state_->arb_params[make_arb_key(entry.product_index(), entry.strategy_type())] =
                            entry.params();
                    }
                    state_->product_states.clear();
                    for (const auto& product_state : resp.product_states()) {
                        state_->product_states[product_state.product_index()] = product_state;
                    }
                    state_->instrument_states.clear();
                    for (const auto& instrument_state : resp.instrument_states()) {
                        state_->instrument_states[instrument_state.instrument_id()] = instrument_state;
                    }
                    state_->arb_strategy_states.clear();
                    for (const auto& arb_state : resp.arb_strategy_states()) {
                        state_->arb_strategy_states[make_arb_key(
                            arb_state.product_index(), arb_state.strategy_type())] = arb_state;
                    }
                    state_->pcp_opportunities.clear();
                    state_->pcp_opportunities.reserve(resp.pcp_opportunities_size());
                    for (const auto& row : resp.pcp_opportunities()) {
                        state_->pcp_opportunities.push_back(row);
                    }
                }
            }
            for (int i = 0; i < 2 && !stop_requested_; ++i) {
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

    void alerts_loop() {
        run_stream<omm::proto::RiskAlert>(
            [this](grpc::ClientContext* ctx, const omm::proto::StreamRequest& req) {
                return stub_->StreamRiskAlerts(ctx, req);
            },
            [this](const omm::proto::RiskAlert& alert) {
                switch (alert.type()) {
                case omm::proto::RiskAlert::POSITION_BREACH:
                    state_->risk_state.set_position_breach(true);
                    break;
                case omm::proto::RiskAlert::DELTA_BREACH:
                    state_->risk_state.set_delta_breach(true);
                    break;
                case omm::proto::RiskAlert::GAMMA_BREACH:
                    state_->risk_state.set_gamma_breach(true);
                    break;
                case omm::proto::RiskAlert::VEGA_BREACH:
                    state_->risk_state.set_vega_breach(true);
                    break;
                default:
                    break;
                }
                state_->alerts.push_front(alert);
                while (state_->alerts.size() > 100) state_->alerts.pop_back();
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

void style_pill(QLabel* label, const QColor& bg, const QColor& fg = QColor("#1d1d1d")) {
    if (label == nullptr) return;
    label->setStyleSheet(QString(
        "padding:4px 10px; border-radius:10px; font-weight:700; background:%1; color:%2;")
            .arg(bg.name(), fg.name()));
}

QString current_time_text() {
    return QTime::currentTime().toString("hh:mm:ss");
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
    int selected_arb_strategy_type{static_cast<int>(omm::proto::ARB_STRATEGY_NONE)};
    uint32_t selected_pcp_call_id{0};
    uint32_t selected_pcp_put_id{0};
    uint32_t selected_pcp_future_id{0};
    bool ui_state_restored{false};
    uint32_t params_editor_product_index{0xFFFFFFFFu};
    QString last_risk_action_text{"Risk thresholds not updated yet"};
    QString last_operator_status_text{"Waiting for monitor data"};
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
    auto* central = new QWidget();
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    auto* header_panel = new QWidget();
    auto* header_layout = new QGridLayout(header_panel);
    header_layout->setContentsMargins(0, 0, 0, 0);
    header_layout->setHorizontalSpacing(8);
    header_layout->setVerticalSpacing(6);

    auto* hero = new QLabel("Desk / Risk Monitor");
    hero->setStyleSheet("font-size: 22px; font-weight: 700; color: #2b2418;");
    header_layout->addWidget(hero, 0, 0, 1, 4);

    auto* product_label = new QLabel("Product");
    product_label->setStyleSheet("font-weight: 700; color: #5d4f36;");
    header_layout->addWidget(product_label, 1, 0);
    product_selector_ = new QComboBox();
    product_selector_->setMinimumWidth(180);
    header_layout->addWidget(product_selector_, 1, 1);

    connection_label_ = new QLabel("Disconnected");
    connection_label_->setMinimumWidth(120);
    connection_label_->setAlignment(Qt::AlignCenter);
    style_pill(connection_label_, QColor("#ffb3b3"));
    header_layout->addWidget(connection_label_, 1, 2);

    status_label_ = new QLabel("Waiting for monitor data");
    status_label_->setStyleSheet(
        "padding:4px 10px; border-radius:10px; background:#f6edd4; color:#4c3d24;");
    header_layout->addWidget(status_label_, 1, 3, 1, 5);

    desk_state_label_ = new QLabel("Desk --");
    desk_state_label_->setAlignment(Qt::AlignCenter);
    style_pill(desk_state_label_, QColor("#ececec"));
    header_layout->addWidget(desk_state_label_, 2, 0);

    global_risk_label_ = new QLabel("Risk --");
    global_risk_label_->setAlignment(Qt::AlignCenter);
    style_pill(global_risk_label_, QColor("#ececec"));
    header_layout->addWidget(global_risk_label_, 2, 1);

    alert_banner_label_ = new QLabel("No live risk alerts");
    alert_banner_label_->setStyleSheet(
        "padding:4px 10px; border-radius:10px; background:#f3f0e7; color:#4a4032; font-weight:700;");
    header_layout->addWidget(alert_banner_label_, 2, 2, 1, 4);

    delta_label_ = new QLabel("Delta --");
    gamma_label_ = new QLabel("Gamma --");
    vega_label_ = new QLabel("Vega --");
    delta_label_->setAlignment(Qt::AlignCenter);
    gamma_label_->setAlignment(Qt::AlignCenter);
    vega_label_->setAlignment(Qt::AlignCenter);
    style_pill(delta_label_, QColor("#ececec"));
    style_pill(gamma_label_, QColor("#ececec"));
    style_pill(vega_label_, QColor("#ececec"));
    header_layout->addWidget(delta_label_, 3, 0);
    header_layout->addWidget(gamma_label_, 3, 1);
    header_layout->addWidget(vega_label_, 3, 2);
    layout->addWidget(header_panel);

    auto* desk_splitter = new QSplitter(Qt::Vertical, central);
    desk_splitter->setChildrenCollapsible(false);

    auto* quote_panel = new QWidget();
    auto* quote_layout = new QVBoxLayout(quote_panel);
    quote_layout->setContentsMargins(0, 0, 0, 0);
    quote_layout->setSpacing(6);

    auto* quote_title = new QLabel("Live Quote Board");
    quote_title->setStyleSheet("font-size: 16px; font-weight: 700; color: #2b2418;");
    quote_layout->addWidget(quote_title);

    auto* quote_hint = new QLabel(
        "Click bid / ask cells to stage orders. Quote state and suppression reasons stay visible inline.");
    quote_hint->setWordWrap(true);
    quote_hint->setStyleSheet("color:#6b5a3f; padding-left:2px;");
    quote_layout->addWidget(quote_hint);

    t_table_ = make_table({"C.Q", "C.Why", "C.BQty", "C.Bid", "C.Theo", "C.Ask", "C.AQty",
                           "Exp", "Strike", "Net",
                           "P.BQty", "P.Bid", "P.Theo", "P.Ask", "P.AQty", "P.Q", "P.Why"});
    t_table_->setShowGrid(false);
    t_table_->setWordWrap(false);
    t_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    t_table_->setMinimumHeight(420);
    auto* t_header = t_table_->horizontalHeader();
    t_header->setSectionResizeMode(QHeaderView::ResizeToContents);
    t_header->setSectionResizeMode(1, QHeaderView::Stretch);
    t_header->setSectionResizeMode(16, QHeaderView::Stretch);
    t_table_->verticalHeader()->setDefaultSectionSize(22);
    quote_layout->addWidget(t_table_, 1);
    desk_splitter->addWidget(quote_panel);

    orders_table_ = make_table({"OrderId", "Instrument", "Exchange", "Side", "Price", "Volume", "Status", "FillPx", "FillQty", "Ts"});
    quotes_table_ = make_table({"Instrument", "BidPx", "BidQty", "AskPx", "AskQty", "QState", "Why", "Status"});
    trades_table_ = make_table({"TradeId", "OrderId", "Instrument", "Exchange", "Side", "Price", "Qty", "Ts"});
    alerts_table_ = make_table({"Ts", "Type", "Message"});
    alerts_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    alerts_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    auto* blotter_panel = new QWidget();
    auto* blotter_layout = new QVBoxLayout(blotter_panel);
    blotter_layout->setContentsMargins(0, 0, 0, 0);
    blotter_layout->setSpacing(6);

    auto* blotter_title = new QLabel("Desk Blotter");
    blotter_title->setStyleSheet("font-size: 16px; font-weight: 700; color: #2b2418;");
    blotter_layout->addWidget(blotter_title);

    auto* blotter_hint = new QLabel(
        "Orders, working quotes, fills, and alerts stay docked under the board for one-scan monitoring.");
    blotter_hint->setWordWrap(true);
    blotter_hint->setStyleSheet("color:#6b5a3f; padding-left:2px;");
    blotter_layout->addWidget(blotter_hint);

    auto* blotter_tabs = new QTabWidget();
    blotter_tabs->setDocumentMode(true);
    blotter_tabs->addTab(orders_table_, "Orders");
    blotter_tabs->addTab(quotes_table_, "Quotes");
    blotter_tabs->addTab(trades_table_, "Trades");
    blotter_tabs->addTab(alerts_table_, "Risk Alerts");
    blotter_layout->addWidget(blotter_tabs, 1);
    desk_splitter->addWidget(blotter_panel);
    desk_splitter->setStretchFactor(0, 5);
    desk_splitter->setStretchFactor(1, 2);
    layout->addWidget(desk_splitter, 1);
    setCentralWidget(central);

    vol_dock_ = new QDockWidget("ORC Wing / Vol Curves", this);
    vol_dock_->setObjectName("volCurvesDock");
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

    auto* controls_dock = new QDockWidget("Trader Controls", this);
    controls_dock->setObjectName("traderControlsDock");
    controls_dock->setFeatures(QDockWidget::DockWidgetMovable |
                               QDockWidget::DockWidgetFloatable |
                               QDockWidget::DockWidgetClosable);
    auto* controls_panel = new QWidget();
    controls_panel->setMinimumWidth(410);
    auto* controls_layout = new QVBoxLayout(controls_panel);
    controls_layout->setContentsMargins(8, 8, 8, 8);
    controls_layout->setSpacing(8);

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

    auto* selection_box = new QGroupBox("Selected Product Status");
    auto* selection_layout = new QVBoxLayout(selection_box);
    selection_layout->setContentsMargins(8, 8, 8, 8);
    selection_layout->setSpacing(6);
    strategy_status_label_ = new QLabel("Selected product strategy state will follow the live snapshot.");
    strategy_status_label_->setWordWrap(true);
    strategy_status_label_->setStyleSheet(
        "padding:4px 8px; border-radius:8px; background:#f3f0e7; color:#4a4032;");
    selection_layout->addWidget(strategy_status_label_);
    product_gate_label_ = new QLabel("Product gate follows live MM state.");
    product_gate_label_->setWordWrap(true);
    product_gate_label_->setStyleSheet(
        "padding:4px 8px; border-radius:8px; background:#ececec; color:#353535; font-weight:700;");
    selection_layout->addWidget(product_gate_label_);
    auto* params_state_row = new QHBoxLayout();
    params_state_row->addWidget(new QLabel("Param Editor"));
    params_state_label_ = new QLabel("No live params");
    params_state_label_->setAlignment(Qt::AlignCenter);
    style_pill(params_state_label_, QColor("#ececec"));
    params_state_row->addWidget(params_state_label_, 1);
    selection_layout->addLayout(params_state_row);
    controls_layout->addWidget(selection_box);

    auto* order_box = new QGroupBox("Manual Order Ticket");
    auto* order_layout = new QGridLayout(order_box);
    order_layout->addWidget(new QLabel("Instrument"), 0, 0);
    instrument_selector_ = new QComboBox();
    order_layout->addWidget(instrument_selector_, 0, 1, 1, 2);
    order_layout->addWidget(new QLabel("Side"), 1, 0);
    side_selector_ = new QComboBox();
    side_selector_->addItems({"buy", "sell"});
    order_layout->addWidget(side_selector_, 1, 1, 1, 2);
    order_layout->addWidget(new QLabel("Price"), 2, 0);
    price_editor_ = new QDoubleSpinBox();
    price_editor_->setDecimals(4);
    price_editor_->setMaximum(1000000.0);
    order_layout->addWidget(price_editor_, 2, 1, 1, 2);
    order_layout->addWidget(new QLabel("Volume"), 3, 0);
    volume_editor_ = new QSpinBox();
    volume_editor_->setRange(1, 100000);
    volume_editor_->setValue(5);
    order_layout->addWidget(volume_editor_, 3, 1, 1, 2);
    buy_button_ = new QPushButton("Send Buy");
    sell_button_ = new QPushButton("Send Sell");
    order_layout->addWidget(buy_button_, 4, 0, 1, 3);
    order_layout->addWidget(sell_button_, 5, 0, 1, 3);
    auto* execution_box = new QGroupBox("Execution / Cancel");
    auto* execution_layout = new QGridLayout(execution_box);
    cancel_selected_order_button_ = new QPushButton("Cancel Selected");
    cancel_product_orders_button_ = new QPushButton("Cancel Product Working");
    cancel_selected_quote_button_ = new QPushButton("Cancel Selected Quote");
    cancel_product_quotes_button_ = new QPushButton("Cancel Product Quotes");
    execution_layout->addWidget(cancel_selected_order_button_, 0, 0, 1, 2);
    execution_layout->addWidget(cancel_product_orders_button_, 1, 0, 1, 2);
    execution_layout->addWidget(cancel_selected_quote_button_, 2, 0, 1, 2);
    execution_layout->addWidget(cancel_product_quotes_button_, 3, 0, 1, 2);
    execution_status_label_ = new QLabel(
        "Select an order or quote row to cancel, or use product-wide order / quote sweeps.");
    execution_status_label_->setWordWrap(true);
    execution_status_label_->setStyleSheet(
        "padding:4px 8px; border-radius:8px; background:#f3f0e7; color:#4a4032;");
    execution_layout->addWidget(execution_status_label_, 4, 0, 1, 2);

    auto* strategy_box = new QGroupBox("Strategy Control");
    auto* strategy_layout = new QGridLayout(strategy_box);
    start_button_ = new QPushButton("Start MM");
    stop_button_ = new QPushButton("Stop MM");
    strategy_layout->addWidget(start_button_, 0, 0);
    strategy_layout->addWidget(stop_button_, 0, 1);
    auto* strategy_note = new QLabel(
        "MM start / stop only affects the selected product. Arbitrage runs independently below.");
    strategy_note->setWordWrap(true);
    strategy_note->setStyleSheet(
        "padding:4px 8px; border-radius:8px; background:#f3f0e7; color:#4a4032;");
    strategy_layout->addWidget(strategy_note, 1, 0, 1, 2);
    strategy_layout->addWidget(new QLabel("Arb Strategy"), 2, 0);
    arb_strategy_selector_ = new QComboBox();
    strategy_layout->addWidget(arb_strategy_selector_, 2, 1);
    arb_start_button_ = new QPushButton("Start Arb");
    arb_stop_button_ = new QPushButton("Stop Arb");
    strategy_layout->addWidget(arb_start_button_, 3, 0);
    strategy_layout->addWidget(arb_stop_button_, 3, 1);
    arb_status_label_ = new QLabel("No arbitrage strategy selected.");
    arb_status_label_->setWordWrap(true);
    arb_status_label_->setStyleSheet(
        "padding:4px 8px; border-radius:8px; background:#ececec; color:#353535; font-weight:700;");
    strategy_layout->addWidget(arb_status_label_, 4, 0, 1, 2);
    arb_details_label_ = new QLabel("Arbitrage state follows the live snapshot.");
    arb_details_label_->setWordWrap(true);
    arb_details_label_->setStyleSheet(
        "padding:4px 8px; border-radius:8px; background:#f3f0e7; color:#4a4032;");
    strategy_layout->addWidget(arb_details_label_, 5, 0, 1, 2);

    auto* params_box = new QWidget();
    auto* params_root_layout = new QVBoxLayout(params_box);
    params_root_layout->setContentsMargins(0, 0, 0, 0);
    params_root_layout->setSpacing(8);

    auto* params_hint = new QLabel(
        "MM controls are grouped by how traders think: shape the quote, manage inventory, then hedge and gate.");
    params_hint->setWordWrap(true);
    params_hint->setStyleSheet("padding:4px 2px; color:#6b5a3f;");
    params_root_layout->addWidget(params_hint);

    params_tabs_ = new QTabWidget();

    auto* quote_shape_tab = new QWidget();
    auto* quote_shape_layout = new QGridLayout(quote_shape_tab);
    quote_shape_layout->addWidget(new QLabel("Base Half"), 0, 0);
    bid_spread_editor_ = new QDoubleSpinBox();
    configure_double(bid_spread_editor_, 4, 0.0, 9999.0, 0.1);
    ask_spread_editor_ = new QDoubleSpinBox();
    configure_double(ask_spread_editor_, 4, 0.0, 9999.0, 0.1);
    base_half_spread_editor_ = new QDoubleSpinBox();
    configure_double(base_half_spread_editor_, 4, 0.0, 9999.0, 0.1);
    quote_shape_layout->addWidget(base_half_spread_editor_, 0, 1);
    quote_shape_layout->addWidget(new QLabel("Min Half"), 0, 2);
    min_half_spread_editor_ = new QDoubleSpinBox();
    configure_double(min_half_spread_editor_, 4, 0.0, 9999.0, 0.1);
    quote_shape_layout->addWidget(min_half_spread_editor_, 0, 3);
    quote_shape_layout->addWidget(new QLabel("Max Half"), 1, 0);
    max_half_spread_editor_ = new QDoubleSpinBox();
    configure_double(max_half_spread_editor_, 4, 0.0, 9999.0, 0.1);
    quote_shape_layout->addWidget(max_half_spread_editor_, 1, 1);
    quote_shape_layout->addWidget(new QLabel("Follow Weight"), 1, 2);
    follow_weight_editor_ = new QDoubleSpinBox();
    configure_double(follow_weight_editor_, 4, 0.0, 1.0, 0.05);
    quote_shape_layout->addWidget(follow_weight_editor_, 1, 3);
    quote_shape_layout->addWidget(new QLabel("Mkt Width Widen"), 2, 0);
    market_width_widen_editor_ = new QDoubleSpinBox();
    configure_double(market_width_widen_editor_, 4, 0.0, 9999.0, 0.1);
    quote_shape_layout->addWidget(market_width_widen_editor_, 2, 1);
    params_tabs_->addTab(quote_shape_tab, "Quote Shape");

    auto* inventory_tab = new QWidget();
    auto* inventory_layout = new QGridLayout(inventory_tab);
    inventory_layout->addWidget(new QLabel("Quote Volume"), 0, 0);
    quote_volume_editor_ = new QSpinBox();
    configure_int(quote_volume_editor_, 0, 100000);
    inventory_layout->addWidget(quote_volume_editor_, 0, 1);
    inventory_layout->addWidget(new QLabel("Warning Pos"), 0, 2);
    warning_position_editor_ = new QSpinBox();
    configure_int(warning_position_editor_, 1, 1000000);
    inventory_layout->addWidget(warning_position_editor_, 0, 3);
    inventory_layout->addWidget(new QLabel("Max Position"), 1, 0);
    max_position_editor_ = new QSpinBox();
    configure_int(max_position_editor_, 1, 1000000);
    inventory_layout->addWidget(max_position_editor_, 1, 1);
    inventory_layout->addWidget(new QLabel("Inv Skew / Lot"), 1, 2);
    inventory_skew_editor_ = new QDoubleSpinBox();
    configure_double(inventory_skew_editor_, 4, -9999.0, 9999.0, 0.01);
    inventory_layout->addWidget(inventory_skew_editor_, 1, 3);
    use_one_sided_editor_ = new QCheckBox("One-Sided At Limits");
    inventory_layout->addWidget(use_one_sided_editor_, 2, 0, 1, 2);
    params_tabs_->addTab(inventory_tab, "Inventory");

    auto* requote_tab = new QWidget();
    auto* requote_layout = new QGridLayout(requote_tab);
    requote_layout->addWidget(new QLabel("Requote Eps"), 0, 0);
    requote_epsilon_editor_ = new QDoubleSpinBox();
    configure_double(requote_epsilon_editor_, 4, 0.0, 9999.0, 0.1);
    requote_layout->addWidget(requote_epsilon_editor_, 0, 1);
    requote_layout->addWidget(new QLabel("Min Quote ms"), 0, 2);
    min_quote_interval_editor_ = new QDoubleSpinBox();
    configure_double(min_quote_interval_editor_, 3, 0.0, 60000.0, 1.0);
    requote_layout->addWidget(min_quote_interval_editor_, 0, 3);
    params_tabs_->addTab(requote_tab, "Requote");

    auto* hedge_tab = new QWidget();
    auto* hedge_layout = new QGridLayout(hedge_tab);
    hedge_layout->addWidget(new QLabel("Product Delta"), 0, 0);
    product_delta_threshold_editor_ = new QDoubleSpinBox();
    configure_double(product_delta_threshold_editor_, 4, 0.0, 999999.0, 1.0);
    hedge_layout->addWidget(product_delta_threshold_editor_, 0, 1);
    hedge_layout->addWidget(new QLabel("Product Vega"), 0, 2);
    product_vega_threshold_editor_ = new QDoubleSpinBox();
    configure_double(product_vega_threshold_editor_, 4, 0.0, 99999999.0, 10.0);
    hedge_layout->addWidget(product_vega_threshold_editor_, 0, 3);
    hedge_layout->addWidget(new QLabel("Underly Shock"), 1, 0);
    underlying_move_widen_editor_ = new QDoubleSpinBox();
    configure_double(underlying_move_widen_editor_, 4, 0.0, 9999.0, 0.1);
    hedge_layout->addWidget(underlying_move_widen_editor_, 1, 1);
    strategy_enabled_editor_ = new QCheckBox("Enabled");
    hedge_layout->addWidget(strategy_enabled_editor_, 1, 2, 1, 2);
    params_tabs_->addTab(hedge_tab, "Hedge / Gate");

    auto* advanced_tab = new QWidget();
    auto* advanced_layout = new QGridLayout(advanced_tab);
    advanced_layout->addWidget(new QLabel("Legacy Bid Spread"), 0, 0);
    advanced_layout->addWidget(bid_spread_editor_, 0, 1);
    advanced_layout->addWidget(new QLabel("Legacy Ask Spread"), 1, 0);
    advanced_layout->addWidget(ask_spread_editor_, 1, 1);
    params_tabs_->addTab(advanced_tab, "Advanced");

    params_root_layout->addWidget(params_tabs_);
    auto* params_actions_layout = new QGridLayout();
    reset_params_button_ = new QPushButton("Reset");
    revert_params_button_ = new QPushButton("Revert to Live");
    apply_params_button_ = new QPushButton("Apply Params");
    params_actions_layout->addWidget(reset_params_button_, 0, 0);
    params_actions_layout->addWidget(revert_params_button_, 0, 1);
    params_actions_layout->addWidget(apply_params_button_, 0, 2);
    params_root_layout->addLayout(params_actions_layout);

    auto* risk_box = new QGroupBox("Soft Risk Thresholds");
    auto* risk_layout = new QGridLayout(risk_box);
    risk_layout->addWidget(new QLabel("Soft Pos"), 0, 0);
    soft_position_limit_editor_ = new QSpinBox();
    configure_int(soft_position_limit_editor_, 1, 10000000);
    risk_layout->addWidget(soft_position_limit_editor_, 0, 1);
    risk_layout->addWidget(new QLabel("Soft Delta"), 1, 0);
    soft_delta_limit_editor_ = new QDoubleSpinBox();
    configure_double(soft_delta_limit_editor_, 2, 0.0, 1e9, 10.0);
    risk_layout->addWidget(soft_delta_limit_editor_, 1, 1);
    risk_layout->addWidget(new QLabel("Soft Gamma"), 2, 0);
    soft_gamma_limit_editor_ = new QDoubleSpinBox();
    configure_double(soft_gamma_limit_editor_, 2, 0.0, 1e9, 10.0);
    risk_layout->addWidget(soft_gamma_limit_editor_, 2, 1);
    risk_layout->addWidget(new QLabel("Soft Vega"), 3, 0);
    soft_vega_limit_editor_ = new QDoubleSpinBox();
    configure_double(soft_vega_limit_editor_, 2, 0.0, 1e9, 100.0);
    risk_layout->addWidget(soft_vega_limit_editor_, 3, 1);
    apply_risk_button_ = new QPushButton("Apply Risk Limits");
    risk_layout->addWidget(apply_risk_button_, 4, 0, 1, 2);
    risk_action_label_ = new QLabel("Risk thresholds not updated yet");
    risk_action_label_->setWordWrap(true);
    risk_action_label_->setStyleSheet(
        "padding:4px 8px; border-radius:8px; background:#f3f0e7; color:#4a4032;");
    risk_layout->addWidget(risk_action_label_, 5, 0, 1, 2);

    auto* control_tabs = new QTabWidget();
    control_tabs->setDocumentMode(true);

    auto* ticket_tab = new QWidget();
    auto* ticket_layout = new QVBoxLayout(ticket_tab);
    ticket_layout->setContentsMargins(0, 0, 0, 0);
    ticket_layout->setSpacing(8);
    ticket_layout->addWidget(order_box);
    ticket_layout->addWidget(execution_box);
    ticket_layout->addStretch(1);
    control_tabs->addTab(ticket_tab, "Ticket");

    auto* strategy_tab = new QWidget();
    auto* strategy_tab_layout = new QVBoxLayout(strategy_tab);
    strategy_tab_layout->setContentsMargins(0, 0, 0, 0);
    strategy_tab_layout->setSpacing(8);
    strategy_tab_layout->addWidget(strategy_box);
    strategy_tab_layout->addStretch(1);
    control_tabs->addTab(strategy_tab, "Strategy");

    auto* params_tab = new QWidget();
    auto* params_tab_layout = new QVBoxLayout(params_tab);
    params_tab_layout->setContentsMargins(0, 0, 0, 0);
    params_tab_layout->setSpacing(8);
    params_tab_layout->addWidget(params_box);
    control_tabs->addTab(params_tab, "MM Params");

    auto* risk_tab = new QWidget();
    auto* risk_tab_layout = new QVBoxLayout(risk_tab);
    risk_tab_layout->setContentsMargins(0, 0, 0, 0);
    risk_tab_layout->setSpacing(8);
    risk_tab_layout->addWidget(risk_box);
    risk_tab_layout->addStretch(1);
    control_tabs->addTab(risk_tab, "Risk");

    controls_layout->addWidget(control_tabs, 1);
    controls_dock->setWidget(controls_panel);
    addDockWidget(Qt::RightDockWidgetArea, controls_dock);

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
    connect(arb_start_button_, &QPushButton::clicked, this, [this] { start_arb_strategy(true); });
    connect(arb_stop_button_, &QPushButton::clicked, this, [this] { start_arb_strategy(false); });
    connect(apply_params_button_, &QPushButton::clicked, this, [this] { apply_strategy_params(); });
    connect(cancel_selected_order_button_, &QPushButton::clicked, this, [this] {
        cancel_selected_order();
    });
    connect(cancel_product_orders_button_, &QPushButton::clicked, this, [this] {
        cancel_selected_product_orders();
    });
    connect(cancel_selected_quote_button_, &QPushButton::clicked, this, [this] {
        cancel_selected_quote();
    });
    connect(cancel_product_quotes_button_, &QPushButton::clicked, this, [this] {
        cancel_selected_product_quotes();
    });
    connect(reset_params_button_, &QPushButton::clicked, this, [this] {
        reset_strategy_params_to_defaults();
    });
    connect(revert_params_button_, &QPushButton::clicked, this, [this] {
        revert_strategy_params_to_live();
    });
    connect(apply_risk_button_, &QPushButton::clicked, this, [this] { apply_risk_thresholds(); });
    connect(product_selector_, &QComboBox::currentIndexChanged, this, [this](int) {
        const QVariant data = product_selector_->currentData();
        if (data.isValid()) impl_->selected_product_index = data.toUInt();
        refresh_ui();
    });
    connect(arb_strategy_selector_, &QComboBox::currentIndexChanged, this, [this](int) {
        const QVariant data = arb_strategy_selector_->currentData();
        if (data.isValid()) impl_->selected_arb_strategy_type = data.toInt();
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

    auto* positions_panel = new QWidget();
    auto* positions_layout = new QVBoxLayout(positions_panel);
    positions_layout->setContentsMargins(6, 6, 6, 6);
    positions_layout->setSpacing(8);

    auto* pms_box = new QGroupBox("Product Risk Board");
    auto* pms_layout = new QVBoxLayout(pms_box);
    pms_layout->setContentsMargins(8, 8, 8, 8);
    pms_layout->setSpacing(6);
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
    positions_tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int col = 1; col < positions_tree_->columnCount(); ++col) {
        positions_tree_->header()->setSectionResizeMode(col, QHeaderView::ResizeToContents);
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

    connect(orders_table_, &QTableWidget::cellClicked, this, [this](int row, int) {
        auto* order_item = orders_table_->item(row, 0);
        auto* instrument_item = orders_table_->item(row, 1);
        if (order_item == nullptr) return;
        execution_status_label_->setText(
            QString("Selected order %1 on %2")
                .arg(order_item->text(),
                     instrument_item != nullptr ? instrument_item->text() : QString("-")));
    });
    connect(quotes_table_, &QTableWidget::cellClicked, this, [this](int row, int) {
        auto* instrument_item = quotes_table_->item(row, 0);
        auto* state_item = quotes_table_->item(row, 5);
        if (instrument_item == nullptr) return;
        execution_status_label_->setText(
            QString("Selected quote on %1 [%2]")
                .arg(instrument_item->text(),
                     state_item != nullptr ? state_item->text() : QString("-")));
    });
    connect(arb_opportunity_table_, &QTableWidget::cellClicked, this, [this](int row, int) {
        auto* row_item = arb_opportunity_table_->item(row, 0);
        if (row_item == nullptr) return;
        impl_->selected_pcp_call_id = row_item->data(Qt::UserRole).toUInt();
        impl_->selected_pcp_put_id = row_item->data(Qt::UserRole + 1).toUInt();
        impl_->selected_pcp_future_id = row_item->data(Qt::UserRole + 2).toUInt();
        if (impl_->selected_pcp_call_id != 0) {
            const int idx = instrument_selector_->findData(QVariant::fromValue(impl_->selected_pcp_call_id));
            if (idx >= 0) instrument_selector_->setCurrentIndex(idx);
        }
    });
    resizeDocks({positions_dock, controls_dock, vol_dock_}, {350, 430, 520}, Qt::Horizontal);
    resizeDocks({arb_monitor_dock}, {280}, Qt::Vertical);
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
    vol_window_->setWindowTitle("Secondary Risk / Vol Workspace");
    vol_window_->resize(1440, 960);

    auto* secondary_root = new QWidget(vol_window_);
    auto* secondary_layout = new QVBoxLayout(secondary_root);
    secondary_layout->setContentsMargins(8, 8, 8, 8);
    secondary_layout->setSpacing(8);

    auto* secondary_header = new QLabel("Screen 2  Vol / PMS / Alerts");
    secondary_header->setStyleSheet("font-size: 18px; font-weight: 700; color: #2b2418;");
    secondary_layout->addWidget(secondary_header);

    auto* vertical_splitter = new QSplitter(Qt::Vertical, secondary_root);
    secondary_vol_widget_ = new VolCurveGridWidget(&impl_->state, vertical_splitter);
    vol_window_widget_ = secondary_vol_widget_;
    vertical_splitter->addWidget(secondary_vol_widget_);

    auto* lower_splitter = new QSplitter(Qt::Horizontal, vertical_splitter);
    auto* summary_panel = new QWidget(lower_splitter);
    auto* summary_layout = new QVBoxLayout(summary_panel);
    summary_layout->setContentsMargins(0, 0, 0, 0);
    summary_layout->setSpacing(8);

    auto* secondary_summary_box = new QGroupBox("Selected Product Risk");
    auto* secondary_summary_layout = new QVBoxLayout(secondary_summary_box);
    secondary_summary_layout->setContentsMargins(8, 8, 8, 8);
    secondary_summary_layout->setSpacing(6);
    secondary_gate_label_ = new QLabel("PRODUCT --");
    secondary_gate_label_->setAlignment(Qt::AlignCenter);
    style_pill(secondary_gate_label_, QColor("#ececec"));
    secondary_summary_layout->addWidget(secondary_gate_label_);
    secondary_greeks_label_ = new QLabel("Delta --   Gamma --   Vega --");
    secondary_greeks_label_->setWordWrap(true);
    secondary_greeks_label_->setStyleSheet(
        "padding:4px 8px; border-radius:8px; background:#f6edd4; color:#4c3d24; font-weight:700;");
    secondary_summary_layout->addWidget(secondary_greeks_label_);
    secondary_limits_label_ = new QLabel("Soft limits --");
    secondary_limits_label_->setWordWrap(true);
    secondary_limits_label_->setStyleSheet(
        "padding:4px 8px; border-radius:8px; background:#f3f0e7; color:#4a4032;");
    secondary_summary_layout->addWidget(secondary_limits_label_);
    secondary_counts_label_ = new QLabel("Quoted --   Suppressed --   Positions --");
    secondary_counts_label_->setWordWrap(true);
    secondary_counts_label_->setStyleSheet(
        "padding:4px 8px; border-radius:8px; background:#eef6ff; color:#29415f;");
    secondary_summary_layout->addWidget(secondary_counts_label_);
    secondary_alert_label_ = new QLabel("Latest alert --");
    secondary_alert_label_->setWordWrap(true);
    secondary_alert_label_->setStyleSheet(
        "padding:4px 8px; border-radius:8px; background:#fff6dc; color:#574526;");
    secondary_summary_layout->addWidget(secondary_alert_label_);
    summary_layout->addWidget(secondary_summary_box);

    secondary_risk_table_ = make_table({"Instrument", "Net", "Delta", "Gamma", "Vega", "Q", "Why"});
    secondary_risk_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    summary_layout->addWidget(secondary_risk_table_, 1);
    lower_splitter->addWidget(summary_panel);

    secondary_alerts_table_ = make_table({"Ts", "Type", "Message"});
    secondary_alerts_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    secondary_alerts_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    lower_splitter->addWidget(secondary_alerts_table_);
    lower_splitter->setStretchFactor(0, 3);
    lower_splitter->setStretchFactor(1, 2);

    vertical_splitter->addWidget(lower_splitter);
    vertical_splitter->setStretchFactor(0, 3);
    vertical_splitter->setStretchFactor(1, 2);
    secondary_layout->addWidget(vertical_splitter, 1);

    vol_window_->setCentralWidget(secondary_root);
}

void TraderMainWindow::restore_ui_state() {
    QSettings settings;
    const QByteArray geometry = settings.value("main/geometry").toByteArray();
    const QByteArray state = settings.value("main/state").toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
    if (!state.isEmpty()) restoreState(state, kWorkspaceStateVersion);
    impl_->selected_product_index = settings.value("selection/product", 0u).toUInt();
    impl_->selected_instrument_id = settings.value("selection/instrument", 0u).toUInt();

    ensure_vol_window();
    const QByteArray vol_geometry = settings.value("vol/geometry").toByteArray();
    if (!vol_geometry.isEmpty()) vol_window_->restoreGeometry(vol_geometry);
    const bool have_saved_secondary_visibility = settings.contains("vol/window_visible");
    const bool vol_window_visible = settings.value("vol/window_visible", false).toBool();
    if (vol_window_visible) {
        vol_dock_->hide();
        vol_window_->show();
    } else if (!settings.value("workspace/dual_screen_seeded", false).toBool()
               && QApplication::screens().size() > 1) {
        const QList<QScreen*> screens = QApplication::screens();
        if (geometry.isEmpty()) {
            setGeometry(screens[0]->availableGeometry().adjusted(16, 16, -16, -16));
        }
        if (!have_saved_secondary_visibility) {
            vol_window_->setGeometry(screens[1]->availableGeometry().adjusted(16, 16, -16, -16));
            vol_dock_->hide();
            vol_window_->show();
        }
        settings.setValue("workspace/dual_screen_seeded", true);
    }
    impl_->ui_state_restored = true;
}

void TraderMainWindow::save_ui_state() const {
    QSettings settings;
    settings.setValue("main/geometry", saveGeometry());
    settings.setValue("main/state", saveState(kWorkspaceStateVersion));
    settings.setValue("selection/product", impl_->selected_product_index);
    settings.setValue("selection/instrument", impl_->selected_instrument_id);
    if (vol_window_ != nullptr) {
        settings.setValue("vol/geometry", vol_window_->saveGeometry());
        settings.setValue("vol/window_visible", vol_window_->isVisible());
    }
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
        arb_opportunity_table_->setRowCount(0);
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
        if (auto* order_item = orders_table_->item(i, 0); order_item != nullptr) {
            order_item->setData(Qt::UserRole, QVariant::fromValue(order.client_order_id()));
            order_item->setData(Qt::UserRole + 1, QVariant::fromValue(order.instrument_id()));
        }
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
        QString quote_state_label = "IDLE";
        QString reason_label = "-";
        QColor quote_state_color = QColor("#ececec");
        QColor reason_color = QColor("#ececec");
        if (auto mm_it = impl_->state.instrument_states.find(quote.instrument_id());
            mm_it != impl_->state.instrument_states.end()) {
            const auto& mm_state = mm_it->second;
            quote_state_label = mm_quote_state_text(mm_state.quote_state());
            reason_label = suppress_reason_text(mm_state.reasons(), mm_state.cancel_attempts());
            quote_state_color = mm_quote_state_color(mm_state.quote_state());
            reason_color = suppress_reason_color(mm_state.reasons());
        }
        set_cell(quotes_table_, i, 0, instrument_label, QColor("#eaeaea"));
        set_cell(quotes_table_, i, 1, QString::number(quote.bid_price(), 'f', 2), QColor("#ffe08a"));
        set_cell(quotes_table_, i, 2, QString::number(quote.bid_volume()), QColor("#ffe08a"));
        set_cell(quotes_table_, i, 3, QString::number(quote.ask_price(), 'f', 2), QColor("#ffc59c"));
        set_cell(quotes_table_, i, 4, QString::number(quote.ask_volume()), QColor("#ffc59c"));
        set_cell(quotes_table_, i, 5, quote_state_label, quote_state_color);
        set_cell(quotes_table_, i, 6, reason_label, reason_color);
        set_cell(quotes_table_, i, 7, QString::fromStdString(quote.status()), QColor("#e4f8f0"));
        if (auto* quote_item = quotes_table_->item(i, 0); quote_item != nullptr) {
            quote_item->setData(Qt::UserRole, QVariant::fromValue(quote.client_quote_id()));
            quote_item->setData(Qt::UserRole + 1, QVariant::fromValue(quote.instrument_id()));
        }
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
    if (vol_window_ != nullptr && !vol_window_->isVisible() && vol_dock_ != nullptr && !vol_dock_->isVisible()) {
        vol_dock_->show();
    }
}

void TraderMainWindow::load_strategy_params_into_editors(const omm::proto::MMParams& params) {
    const MMParamsConfig defaults{};

    auto sync_double = [](QDoubleSpinBox* editor, double value) {
        if (editor == nullptr) return;
        QSignalBlocker blocker(editor);
        editor->setValue(value);
    };
    auto sync_int = [](QSpinBox* editor, int value) {
        if (editor == nullptr) return;
        QSignalBlocker blocker(editor);
        editor->setValue(value);
    };
    auto sync_check = [](QCheckBox* editor, bool value) {
        if (editor == nullptr) return;
        QSignalBlocker blocker(editor);
        editor->setChecked(value);
    };

    sync_double(bid_spread_editor_,
                params.has_bid_spread() ? params.bid_spread() : defaults.bid_spread);
    sync_double(ask_spread_editor_,
                params.has_ask_spread() ? params.ask_spread() : defaults.ask_spread);
    sync_double(base_half_spread_editor_,
                params.has_base_half_spread_ticks()
                    ? params.base_half_spread_ticks()
                    : defaults.base_half_spread_ticks);
    sync_double(min_half_spread_editor_,
                params.has_min_half_spread_ticks()
                    ? params.min_half_spread_ticks()
                    : defaults.min_half_spread_ticks);
    sync_double(max_half_spread_editor_,
                params.has_max_half_spread_ticks()
                    ? params.max_half_spread_ticks()
                    : defaults.max_half_spread_ticks);
    sync_double(follow_weight_editor_,
                params.has_follow_weight() ? params.follow_weight() : defaults.follow_weight);
    sync_double(inventory_skew_editor_,
                params.has_inventory_skew_per_lot_ticks()
                    ? params.inventory_skew_per_lot_ticks()
                    : defaults.inventory_skew_per_lot_ticks);
    sync_double(market_width_widen_editor_,
                params.has_market_width_widen_threshold_ticks()
                    ? params.market_width_widen_threshold_ticks()
                    : defaults.market_width_widen_threshold_ticks);
    sync_double(product_delta_threshold_editor_,
                params.has_product_delta_threshold()
                    ? params.product_delta_threshold()
                    : defaults.product_delta_threshold);
    sync_double(product_vega_threshold_editor_,
                params.has_product_vega_threshold()
                    ? params.product_vega_threshold()
                    : defaults.product_vega_threshold);
    sync_double(requote_epsilon_editor_,
                params.has_requote_price_epsilon_ticks()
                    ? params.requote_price_epsilon_ticks()
                    : defaults.requote_price_epsilon_ticks);
    sync_double(min_quote_interval_editor_,
                params.has_min_quote_interval_ms()
                    ? params.min_quote_interval_ms()
                    : defaults.min_quote_interval_ms);
    sync_double(underlying_move_widen_editor_,
                params.has_underlying_move_widen_threshold_ticks()
                    ? params.underlying_move_widen_threshold_ticks()
                    : defaults.underlying_move_widen_threshold_ticks);
    sync_int(quote_volume_editor_,
             params.has_quote_volume() ? params.quote_volume() : defaults.quote_volume);
    sync_int(warning_position_editor_,
             params.has_warning_position() ? params.warning_position() : defaults.warning_position);
    sync_int(max_position_editor_,
             params.has_max_position() ? params.max_position() : defaults.max_position);
    sync_check(strategy_enabled_editor_,
               params.has_enabled() ? params.enabled() : defaults.enabled);
    sync_check(use_one_sided_editor_,
               params.has_use_one_sided_at_limits()
                   ? params.use_one_sided_at_limits()
                   : defaults.use_one_sided_at_limits);
}

omm::proto::MMParams TraderMainWindow::collect_strategy_params_from_editors() const {
    omm::proto::MMParams params;
    params.set_bid_spread(bid_spread_editor_->value());
    params.set_ask_spread(ask_spread_editor_->value());
    params.set_base_half_spread_ticks(base_half_spread_editor_->value());
    params.set_min_half_spread_ticks(min_half_spread_editor_->value());
    params.set_max_half_spread_ticks(max_half_spread_editor_->value());
    params.set_follow_weight(follow_weight_editor_->value());
    params.set_inventory_skew_per_lot_ticks(inventory_skew_editor_->value());
    params.set_market_width_widen_threshold_ticks(market_width_widen_editor_->value());
    params.set_product_delta_threshold(product_delta_threshold_editor_->value());
    params.set_product_vega_threshold(product_vega_threshold_editor_->value());
    params.set_quote_volume(quote_volume_editor_->value());
    params.set_warning_position(warning_position_editor_->value());
    params.set_max_position(max_position_editor_->value());
    params.set_requote_price_epsilon_ticks(requote_epsilon_editor_->value());
    params.set_min_quote_interval_ms(min_quote_interval_editor_->value());
    params.set_underlying_move_widen_threshold_ticks(underlying_move_widen_editor_->value());
    params.set_enabled(strategy_enabled_editor_->isChecked());
    params.set_use_one_sided_at_limits(use_one_sided_editor_->isChecked());
    return params;
}

void TraderMainWindow::reset_strategy_params_to_defaults() {
    const MMParamsConfig defaults{};
    omm::proto::MMParams params;
    params.set_bid_spread(defaults.bid_spread);
    params.set_ask_spread(defaults.ask_spread);
    params.set_base_half_spread_ticks(defaults.base_half_spread_ticks);
    params.set_min_half_spread_ticks(defaults.min_half_spread_ticks);
    params.set_max_half_spread_ticks(defaults.max_half_spread_ticks);
    params.set_follow_weight(defaults.follow_weight);
    params.set_inventory_skew_per_lot_ticks(defaults.inventory_skew_per_lot_ticks);
    params.set_market_width_widen_threshold_ticks(defaults.market_width_widen_threshold_ticks);
    params.set_product_delta_threshold(defaults.product_delta_threshold);
    params.set_product_vega_threshold(defaults.product_vega_threshold);
    params.set_quote_volume(defaults.quote_volume);
    params.set_warning_position(defaults.warning_position);
    params.set_max_position(defaults.max_position);
    params.set_requote_price_epsilon_ticks(defaults.requote_price_epsilon_ticks);
    params.set_min_quote_interval_ms(defaults.min_quote_interval_ms);
    params.set_underlying_move_widen_threshold_ticks(defaults.underlying_move_widen_threshold_ticks);
    params.set_enabled(defaults.enabled);
    params.set_use_one_sided_at_limits(defaults.use_one_sided_at_limits);
    load_strategy_params_into_editors(params);
    impl_->last_operator_status_text = QString("Param editors reset to defaults at %1")
        .arg(current_time_text());
}

void TraderMainWindow::revert_strategy_params_to_live() {
    omm::proto::MMParams live_params;
    {
        std::lock_guard<std::mutex> lock(impl_->state.mutex);
        const auto it = impl_->state.mm_params.find(impl_->selected_product_index);
        if (it == impl_->state.mm_params.end()) {
            impl_->last_operator_status_text = QString("No live params to revert for product %1")
                .arg(impl_->selected_product_index);
            return;
        }
        live_params = it->second;
    }
    load_strategy_params_into_editors(live_params);
    impl_->last_operator_status_text = QString("Param editors reverted to live snapshot at %1")
        .arg(current_time_text());
}

void TraderMainWindow::send_manual_order() {
    const QVariant instrument_data = instrument_selector_->currentData();
    if (!instrument_data.isValid()) {
        impl_->last_operator_status_text = "Select an instrument before sending an order";
        return;
    }
    const bool ok = impl_->client->send_manual_order(
        instrument_data.toUInt(),
        side_selector_->currentText().toStdString(),
        price_editor_->value(),
        volume_editor_->value());
    impl_->last_operator_status_text = ok
        ? QString("Manual order sent at %1").arg(current_time_text())
        : QString("Manual order rejected at %1").arg(current_time_text());
}

void TraderMainWindow::start_strategy(bool enabled) {
    const uint32_t product_index = product_selector_->currentData().toUInt();
    const bool ok = impl_->client->set_strategy_enabled(static_cast<int>(product_index), enabled);
    if (ok) {
        std::lock_guard<std::mutex> lock(impl_->state.mutex);
        impl_->state.mm_params[product_index].set_enabled(enabled);
    }
    impl_->last_operator_status_text = ok
        ? QString("%1 at %2")
              .arg(enabled ? "Strategy started" : "Strategy stopped")
              .arg(current_time_text())
        : QString("Strategy action failed at %1").arg(current_time_text());
}

void TraderMainWindow::start_arb_strategy(bool enabled) {
    const QVariant product_data = product_selector_->currentData();
    const QVariant strategy_data = arb_strategy_selector_->currentData();
    if (!product_data.isValid() || !strategy_data.isValid()) {
        impl_->last_operator_status_text = "Select an arbitrage strategy before sending control";
        return;
    }

    const uint32_t product_index = product_data.toUInt();
    const auto strategy_type =
        static_cast<omm::proto::ArbitrageStrategyType>(strategy_data.toInt());
    const bool ok = impl_->client->set_arb_strategy_enabled(product_index, strategy_type, enabled);
    if (ok) {
        std::lock_guard<std::mutex> lock(impl_->state.mutex);
        const uint64_t arb_key = make_arb_key(product_index, strategy_type);
        impl_->state.arb_params[arb_key].set_enabled(enabled);
        impl_->state.arb_strategy_states[arb_key].set_product_index(product_index);
        impl_->state.arb_strategy_states[arb_key].set_strategy_type(strategy_type);
        impl_->state.arb_strategy_states[arb_key].set_enabled(enabled);
        impl_->state.arb_strategy_states[arb_key].set_running(enabled);
    }
    impl_->last_operator_status_text = ok
        ? QString("%1 %2 at %3")
              .arg(arb_strategy_type_text(strategy_type))
              .arg(enabled ? "started" : "stopped")
              .arg(current_time_text())
        : QString("Arbitrage action failed at %1").arg(current_time_text());
}

void TraderMainWindow::apply_strategy_params() {
    omm::proto::MMParams params = collect_strategy_params_from_editors();
    const uint32_t product_index = product_selector_->currentData().toUInt();
    const bool ok = impl_->client->set_strategy_params(product_index, params);
    if (ok) {
        std::lock_guard<std::mutex> lock(impl_->state.mutex);
        impl_->state.mm_params[product_index] = params;
    }
    impl_->last_operator_status_text = ok
        ? QString("MM params applied at %1").arg(current_time_text())
        : QString("MM params update failed at %1").arg(current_time_text());
}

void TraderMainWindow::cancel_selected_order() {
    const int row = orders_table_ != nullptr ? orders_table_->currentRow() : -1;
    if (row < 0) {
        impl_->last_operator_status_text = "Select an order row before sending cancel";
        execution_status_label_->setText("Select an order row before sending cancel.");
        return;
    }

    auto* order_item = orders_table_->item(row, 0);
    if (order_item == nullptr || !order_item->data(Qt::UserRole).isValid()) {
        impl_->last_operator_status_text = "Selected row has no cancelable order id";
        execution_status_label_->setText("Selected row has no cancelable order id.");
        return;
    }

    const uint64_t order_id = order_item->data(Qt::UserRole).toULongLong();
    const uint32_t instrument_id = order_item->data(Qt::UserRole + 1).toUInt();
    const bool ok = impl_->client->cancel_order(order_id, instrument_id);
    execution_status_label_->setText(
        ok ? QString("Cancel sent for order %1").arg(order_id)
           : QString("Cancel failed for order %1").arg(order_id));
    impl_->last_operator_status_text = ok
        ? QString("Cancel sent for order %1 at %2").arg(order_id).arg(current_time_text())
        : QString("Cancel failed for order %1 at %2").arg(order_id).arg(current_time_text());
}

void TraderMainWindow::cancel_selected_product_orders() {
    const uint32_t product_index = product_selector_->currentData().toUInt();
    std::vector<std::pair<uint64_t, uint32_t>> working_orders;
    {
        std::lock_guard<std::mutex> lock(impl_->state.mutex);
        std::unordered_map<uint64_t, std::pair<uint32_t, std::string>> latest;
        for (const auto& order : impl_->state.orders) {
            auto meta_it = impl_->state.instruments.find(order.instrument_id());
            if (meta_it == impl_->state.instruments.end()) continue;
            if (meta_it->second.product_index != product_index) continue;
            if (latest.find(order.client_order_id()) != latest.end()) continue;
            latest.emplace(order.client_order_id(),
                           std::make_pair(order.instrument_id(), order.status()));
        }
        for (const auto& [order_id, state] : latest) {
            const std::string& status = state.second;
            if (status == "Filled" || status == "Cancelled" || status == "Rejected") continue;
            working_orders.emplace_back(order_id, state.first);
        }
    }

    if (working_orders.empty()) {
        execution_status_label_->setText("No working orders found for the selected product.");
        impl_->last_operator_status_text = "No working orders found for selected product";
        return;
    }

    int sent = 0;
    for (const auto& [order_id, instrument_id] : working_orders) {
        if (impl_->client->cancel_order(order_id, instrument_id)) ++sent;
    }
    execution_status_label_->setText(
        QString("Product cancel sent for %1 / %2 working orders").arg(sent).arg(working_orders.size()));
    impl_->last_operator_status_text = QString(
        "Product cancel sent for %1 / %2 working orders at %3")
            .arg(sent)
            .arg(working_orders.size())
            .arg(current_time_text());
}

void TraderMainWindow::cancel_selected_quote() {
    const int row = quotes_table_ != nullptr ? quotes_table_->currentRow() : -1;
    if (row < 0) {
        impl_->last_operator_status_text = "Select a quote row before sending quote cancel";
        execution_status_label_->setText("Select a quote row before sending quote cancel.");
        return;
    }

    auto* quote_item = quotes_table_->item(row, 0);
    if (quote_item == nullptr || !quote_item->data(Qt::UserRole + 1).isValid()) {
        impl_->last_operator_status_text = "Selected quote row has no instrument id";
        execution_status_label_->setText("Selected quote row has no instrument id.");
        return;
    }

    const uint32_t instrument_id = quote_item->data(Qt::UserRole + 1).toUInt();
    const QString instrument_label = quote_item->text();

    uint64_t latest_quote_id = 0;
    bool quote_working = false;
    {
        std::lock_guard<std::mutex> lock(impl_->state.mutex);
        for (const auto& quote : impl_->state.quotes) {
            if (quote.instrument_id() != instrument_id) continue;
            latest_quote_id = quote.client_quote_id();
            break;
        }
        if (auto state_it = impl_->state.instrument_states.find(instrument_id);
            state_it != impl_->state.instrument_states.end()) {
            switch (state_it->second.quote_state()) {
            case omm::proto::MM_QUOTE_LIVE:
            case omm::proto::MM_QUOTE_ACK_PENDING:
            case omm::proto::MM_QUOTE_CANCEL_PENDING:
                quote_working = true;
                break;
            case omm::proto::MM_QUOTE_IDLE:
            case omm::proto::MM_QUOTE_CANCEL_FAILED:
            case omm::proto::MM_QUOTE_SUPPRESSED:
            default:
                break;
            }
        }
    }

    if (latest_quote_id == 0 || !quote_working) {
        impl_->last_operator_status_text = "No working quote found for the selected quote row";
        execution_status_label_->setText("No working quote found for the selected quote row.");
        return;
    }

    const bool ok = impl_->client->cancel_quote(latest_quote_id, instrument_id);
    execution_status_label_->setText(
        ok ? QString("Cancel sent for quote %1 on %2").arg(latest_quote_id).arg(instrument_label)
           : QString("Cancel failed for quote %1 on %2").arg(latest_quote_id).arg(instrument_label));
    impl_->last_operator_status_text = ok
        ? QString("Cancel sent for quote %1 at %2").arg(latest_quote_id).arg(current_time_text())
        : QString("Cancel failed for quote %1 at %2").arg(latest_quote_id).arg(current_time_text());
}

void TraderMainWindow::cancel_selected_product_quotes() {
    const uint32_t product_index = product_selector_->currentData().toUInt();
    std::vector<std::pair<uint64_t, uint32_t>> working_quotes;
    {
        std::lock_guard<std::mutex> lock(impl_->state.mutex);
        std::unordered_map<uint32_t, uint64_t> latest_quote_by_instrument;
        for (const auto& quote : impl_->state.quotes) {
            auto meta_it = impl_->state.instruments.find(quote.instrument_id());
            if (meta_it == impl_->state.instruments.end()) continue;
            if (meta_it->second.product_index != product_index) continue;
            if (latest_quote_by_instrument.find(quote.instrument_id()) != latest_quote_by_instrument.end()) continue;
            latest_quote_by_instrument.emplace(quote.instrument_id(), quote.client_quote_id());
        }
        for (const auto& [instrument_id, quote_id] : latest_quote_by_instrument) {
            auto state_it = impl_->state.instrument_states.find(instrument_id);
            if (state_it == impl_->state.instrument_states.end()) continue;
            switch (state_it->second.quote_state()) {
            case omm::proto::MM_QUOTE_LIVE:
            case omm::proto::MM_QUOTE_ACK_PENDING:
            case omm::proto::MM_QUOTE_CANCEL_PENDING:
                working_quotes.emplace_back(quote_id, instrument_id);
                break;
            case omm::proto::MM_QUOTE_IDLE:
            case omm::proto::MM_QUOTE_CANCEL_FAILED:
            case omm::proto::MM_QUOTE_SUPPRESSED:
            default:
                break;
            }
        }
    }

    if (working_quotes.empty()) {
        execution_status_label_->setText("No working quotes found for the selected product.");
        impl_->last_operator_status_text = "No working quotes found for selected product";
        return;
    }

    int sent = 0;
    for (const auto& [quote_id, instrument_id] : working_quotes) {
        if (impl_->client->cancel_quote(quote_id, instrument_id)) ++sent;
    }
    execution_status_label_->setText(
        QString("Product quote cancel sent for %1 / %2 working quotes").arg(sent).arg(working_quotes.size()));
    impl_->last_operator_status_text = QString(
        "Product quote cancel sent for %1 / %2 working quotes at %3")
            .arg(sent)
            .arg(working_quotes.size())
            .arg(current_time_text());
}

void TraderMainWindow::apply_risk_thresholds() {
    const int max_position = soft_position_limit_editor_->value();
    const double max_delta = soft_delta_limit_editor_->value();
    const double max_gamma = soft_gamma_limit_editor_->value();
    const double max_vega = soft_vega_limit_editor_->value();
    const bool ok = impl_->client->set_risk_thresholds(
        max_position, max_delta, max_gamma, max_vega);
    if (ok) {
        std::lock_guard<std::mutex> lock(impl_->state.mutex);
        auto* threshold = impl_->state.risk_state.mutable_threshold();
        threshold->set_max_net_position(max_position);
        threshold->set_max_delta(max_delta);
        threshold->set_max_gamma(max_gamma);
        threshold->set_max_vega(max_vega);
    }
    impl_->last_risk_action_text = ok
        ? QString("Applied %1  pos=%2 delta=%3 gamma=%4 vega=%5")
              .arg(current_time_text())
              .arg(max_position)
              .arg(QString::number(max_delta, 'f', 1))
              .arg(QString::number(max_gamma, 'f', 1))
              .arg(QString::number(max_vega, 'f', 1))
        : QString("Risk threshold update failed at %1").arg(current_time_text());
    impl_->last_operator_status_text = ok
        ? QString("Soft risk thresholds applied at %1").arg(current_time_text())
        : QString("Soft risk threshold update failed at %1").arg(current_time_text());
}

} // namespace omm::gui

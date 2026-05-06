#include "common/config.h"
#include "gui/trader_main_window.h"
#include "trader_main_window_state.h"
#include "trader_main_window_ui_helpers.h"

#include "trading.grpc.pb.h"

#include <grpcpp/channel.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/client_context.h>

#include <QAbstractItemView>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QScreen>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTableView>
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
#include <set>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace omm::gui {

constexpr int kWorkspaceStateVersion = 1;

uint64_t make_arb_key(uint32_t product_index,
                      omm::proto::ArbitrageStrategyType strategy_type);

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

class LoginDialog final : public QDialog {
public:
    explicit LoginDialog(const QString& endpoint,
                         const QString& reason = {},
                         QWidget* parent = nullptr)
        : QDialog(parent) {
        setWindowTitle("Trader Login");
        setModal(true);
        setMinimumWidth(380);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(14, 14, 14, 14);
        layout->setSpacing(10);

        auto* title = new QLabel(QString("Connect to %1").arg(endpoint), this);
        title->setStyleSheet("font-size: 18px; font-weight: 700; color: #2b2418;");
        layout->addWidget(title);

        if (!reason.trimmed().isEmpty()) {
            message_label_ = new QLabel(reason, this);
            message_label_->setWordWrap(true);
            message_label_->setStyleSheet(
                "padding:6px 8px; border-radius:8px; background:#fff3d6; color:#5b4020;");
            layout->addWidget(message_label_);
        } else {
            message_label_ = new QLabel("Enter your trader credentials to start live monitoring and order entry.",
                                        this);
            message_label_->setWordWrap(true);
            message_label_->setStyleSheet(
                "padding:6px 8px; border-radius:8px; background:#f3f0e7; color:#4a4032;");
            layout->addWidget(message_label_);
        }

        auto* form = new QFormLayout();
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        username_edit_ = new QLineEdit(this);
        username_edit_->setPlaceholderText("Username");
        password_edit_ = new QLineEdit(this);
        password_edit_->setPlaceholderText("Password");
        password_edit_->setEchoMode(QLineEdit::Password);
        form->addRow("Username", username_edit_);
        form->addRow("Password", password_edit_);
        layout->addLayout(form);

        error_label_ = new QLabel(this);
        error_label_->setWordWrap(true);
        error_label_->setVisible(false);
        error_label_->setStyleSheet(
            "padding:6px 8px; border-radius:8px; background:#ffd9d1; color:#6a241b;");
        layout->addWidget(error_label_);

        auto* buttons =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText("Login");
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);

        username_edit_->setFocus();
        connect(username_edit_, &QLineEdit::returnPressed, this, [this] {
            if (!password_edit_->text().isEmpty()) accept();
            else password_edit_->setFocus();
        });
        connect(password_edit_, &QLineEdit::returnPressed, this, &QDialog::accept);

    }

    [[nodiscard]] QString username() const { return username_edit_->text().trimmed(); }
    [[nodiscard]] QString password() const { return password_edit_->text(); }

    void set_username(const QString& value) { username_edit_->setText(value); }

    void show_error(const QString& text) {
        error_label_->setText(text);
        error_label_->setVisible(!text.trimmed().isEmpty());
    }

private:
    QLabel* message_label_{nullptr};
    QLabel* error_label_{nullptr};
    QLineEdit* username_edit_{nullptr};
    QLineEdit* password_edit_{nullptr};
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

    [[nodiscard]] std::string endpoint() const { return endpoint_; }

    bool login(const std::string& username,
               const std::string& password,
               std::string* out_message = nullptr) {
        grpc::ClientContext ctx;
        omm::proto::LoginRequest req;
        omm::proto::LoginResponse resp;
        req.set_username(username);
        req.set_password(password);
        grpc::Status status = stub_->Login(&ctx, req, &resp);
        if (!status.ok()) {
            if (out_message != nullptr) *out_message = status.error_message();
            return false;
        }
        if (!resp.ok()) {
            if (out_message != nullptr) *out_message = resp.message();
            return false;
        }

        {
            std::lock_guard<std::mutex> token_lock(session_mutex_);
            session_token_ = resp.session_token();
        }
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            reset_live_state_locked(false);
            state_->authenticated = true;
            state_->login_required = false;
            state_->connected = false;
            state_->auth_error_text.clear();
            state_->current_user = resp.user();
        }
        if (out_message != nullptr) {
            *out_message = !resp.message().empty() ? resp.message() : std::string("login successful");
        }
        return true;
    }

    bool logout(std::string* out_message = nullptr, bool* out_triggered_shutdown = nullptr) {
        grpc::ClientContext ctx;
        if (!prepare_authenticated_context(&ctx)) {
            if (out_message != nullptr) *out_message = "no active session";
            return false;
        }
        omm::proto::LogoutRequest req;
        omm::proto::LogoutResponse resp;
        grpc::Status status = stub_->Logout(&ctx, req, &resp);
        clear_session_token();
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            reset_live_state_locked(true);
            state_->authenticated = false;
            state_->login_required = false;
            state_->auth_error_text.clear();
        }
        if (out_triggered_shutdown != nullptr) {
            *out_triggered_shutdown = status.ok() && resp.ok() && resp.triggered_shutdown();
        }
        if (!status.ok()) {
            if (out_message != nullptr) *out_message = status.error_message();
            return false;
        }
        if (!resp.ok()) {
            if (out_message != nullptr) *out_message = resp.message();
            return false;
        }
        if (out_message != nullptr) {
            *out_message = !resp.message().empty() ? resp.message() : std::string("logout successful");
        }
        return true;
    }

    void start() {
        if (!has_session_token()) return;
        if (snapshot_thread_.joinable() || !stream_threads_.empty()) return;
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

    bool send_manual_order(uint32_t instrument_id,
                           std::string side,
                           double price,
                           int volume,
                           uint32_t book_id) {
        grpc::ClientContext ctx;
        if (!prepare_authenticated_context(&ctx)) return false;
        omm::proto::ManualOrderRequest req;
        omm::proto::ManualOrderResponse resp;
        req.set_instrument_id(instrument_id);
        req.set_side(side);
        req.set_price(price);
        req.set_volume(volume);
        req.set_book_id(book_id);
        grpc::Status status = stub_->SendManualOrder(&ctx, req, &resp);
        if (!status.ok()) {
            handle_authenticated_failure(status);
            return false;
        }
        return status.ok() && resp.ok();
    }

    bool set_strategy_enabled(int product_index, bool enabled) {
        grpc::ClientContext ctx;
        if (!prepare_authenticated_context(&ctx)) return false;
        omm::proto::StartStopRequest req;
        omm::proto::StartStopResponse resp;
        req.set_product_index(product_index);
        grpc::Status status = enabled
            ? stub_->StartStrategy(&ctx, req, &resp)
            : stub_->StopStrategy(&ctx, req, &resp);
        if (!status.ok()) {
            handle_authenticated_failure(status);
            return false;
        }
        return status.ok() && resp.ok();
    }

    bool set_arb_strategy_enabled(uint32_t product_index,
                                  omm::proto::ArbitrageStrategyType strategy_type,
                                  bool enabled) {
        grpc::ClientContext ctx;
        if (!prepare_authenticated_context(&ctx)) return false;
        omm::proto::ArbStartStopRequest req;
        omm::proto::ArbStartStopResponse resp;
        req.mutable_id()->set_product_index(product_index);
        req.mutable_id()->set_strategy_type(strategy_type);
        grpc::Status status = enabled
            ? stub_->StartArbStrategy(&ctx, req, &resp)
            : stub_->StopArbStrategy(&ctx, req, &resp);
        if (!status.ok()) {
            handle_authenticated_failure(status);
            return false;
        }
        return status.ok() && resp.ok();
    }

    bool set_strategy_params(uint32_t product_index, const omm::proto::MMParams& params) {
        grpc::ClientContext ctx;
        if (!prepare_authenticated_context(&ctx)) return false;
        omm::proto::SetStrategyParamsRequest req;
        omm::proto::SetStrategyParamsResponse resp;
        req.set_product_index(product_index);
        *req.mutable_params() = params;
        grpc::Status status = stub_->SetStrategyParams(&ctx, req, &resp);
        if (!status.ok()) {
            handle_authenticated_failure(status);
            return false;
        }
        return status.ok() && resp.ok();
    }

    bool set_product_pricing_params(const omm::proto::ProductPricingParams& params) {
        grpc::ClientContext ctx;
        if (!prepare_authenticated_context(&ctx)) return false;
        omm::proto::SetProductPricingParamsRequest req;
        omm::proto::SetProductPricingParamsResponse resp;
        *req.mutable_params() = params;
        grpc::Status status = stub_->SetProductPricingParams(&ctx, req, &resp);
        if (!status.ok()) {
            handle_authenticated_failure(status);
            return false;
        }
        return status.ok() && resp.ok();
    }

    bool set_risk_thresholds(int max_net_position,
                             double max_delta,
                             double max_gamma,
                             double max_vega) {
        grpc::ClientContext ctx;
        if (!prepare_authenticated_context(&ctx)) return false;
        omm::proto::SetRiskThresholdRequest req;
        omm::proto::SetRiskThresholdResponse resp;
        auto* threshold = req.mutable_threshold();
        threshold->set_max_net_position(max_net_position);
        threshold->set_max_delta(max_delta);
        threshold->set_max_gamma(max_gamma);
        threshold->set_max_vega(max_vega);
        grpc::Status status = stub_->SetRiskThreshold(&ctx, req, &resp);
        if (!status.ok()) {
            handle_authenticated_failure(status);
            return false;
        }
        return status.ok() && resp.ok();
    }

    bool cancel_order(uint64_t order_id, uint32_t instrument_id) {
        grpc::ClientContext ctx;
        if (!prepare_authenticated_context(&ctx)) return false;
        omm::proto::CancelOrderRequest req;
        omm::proto::CancelOrderResponse resp;
        req.set_order_id(order_id);
        req.set_instrument_id(instrument_id);
        grpc::Status status = stub_->CancelOrder(&ctx, req, &resp);
        if (!status.ok()) {
            handle_authenticated_failure(status);
            return false;
        }
        return status.ok() && resp.ok();
    }

    bool cancel_quote(uint64_t quote_id, uint32_t instrument_id) {
        grpc::ClientContext ctx;
        if (!prepare_authenticated_context(&ctx)) return false;
        omm::proto::CancelQuoteRequest req;
        omm::proto::CancelQuoteResponse resp;
        req.set_quote_id(quote_id);
        req.set_instrument_id(instrument_id);
        grpc::Status status = stub_->CancelQuote(&ctx, req, &resp);
        if (!status.ok()) {
            handle_authenticated_failure(status);
            return false;
        }
        return status.ok() && resp.ok();
    }

private:
    template<typename Message, typename ReaderFn, typename HandlerFn>
    void run_stream(ReaderFn reader_fn, HandlerFn handler) {
        while (!stop_requested_) {
            grpc::ClientContext ctx;
            if (!prepare_authenticated_context(&ctx)) return;
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
            const grpc::Status status = reader->Finish();
            if (!status.ok()) {
                handle_authenticated_failure(status);
            }
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
            if (!prepare_authenticated_context(&ctx)) return;
            omm::proto::SnapshotRequest req;
            omm::proto::SnapshotResponse resp;
            grpc::Status status = stub_->GetSnapshot(&ctx, req, &resp);
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                state_->connected = status.ok();
                if (status.ok()) {
                    state_->authenticated = true;
                    state_->login_required = false;
                    state_->auth_error_text.clear();
                    state_->current_user = resp.current_user();
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
                        meta.tick_size = instrument.tick_size();
                        meta.multiplier = instrument.multiplier();
                        state_->instruments[meta.instrument_id] = std::move(meta);
                    }
                    state_->books.clear();
                    for (const auto& book : resp.books()) {
                        state_->books[book.book_id()] = book;
                    }
                    state_->greeks.clear();
                    for (const auto& g : resp.greeks()) state_->greeks[g.instrument_id()] = g;
                    state_->positions.clear();
                    for (const auto& p : resp.positions()) state_->positions[p.instrument_id()] = p;
                    state_->book_positions.clear();
                    state_->book_positions.reserve(resp.book_positions_size());
                    for (const auto& p : resp.book_positions()) state_->book_positions.push_back(p);
                    state_->book_portfolios.clear();
                    state_->book_portfolios.reserve(resp.book_portfolios_size());
                    for (const auto& p : resp.book_portfolios()) state_->book_portfolios.push_back(p);
                    state_->portfolio = resp.portfolio();
                    state_->risk_state = resp.risk_state();
                    state_->mm_params.clear();
                    for (int i = 0; i < resp.mm_params_size(); ++i) {
                        state_->mm_params[static_cast<uint32_t>(i)] = resp.mm_params(i);
                    }
                    state_->product_pricing_params.clear();
                    for (const auto& params : resp.product_pricing_params()) {
                        state_->product_pricing_params[params.product_index()] = params;
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
                } else {
                    state_->connected = false;
                }
            }
            if (!status.ok()) {
                handle_authenticated_failure(status);
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
                ++state_->orders_seq;
                while (state_->orders.size() > 1000) state_->orders.pop_back();
            });
    }

    void trades_loop() {
        run_stream<omm::proto::OrderUpdate>(
            [this](grpc::ClientContext* ctx, const omm::proto::StreamRequest& req) {
                return stub_->StreamTrades(ctx, req);
            },
            [this](const omm::proto::OrderUpdate& trade) {
                state_->trades.push_front(trade);
                ++state_->trades_seq;
                while (state_->trades.size() > 100000) state_->trades.pop_back();
            });
    }

    void quotes_loop() {
        run_stream<omm::proto::QuoteUpdate>(
            [this](grpc::ClientContext* ctx, const omm::proto::StreamRequest& req) {
                return stub_->StreamQuotes(ctx, req);
            },
            [this](const omm::proto::QuoteUpdate& quote) {
                state_->quotes.push_front(quote);
                ++state_->quotes_seq;
                while (state_->quotes.size() > 1000) state_->quotes.pop_back();
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
    mutable std::mutex session_mutex_;
    std::string session_token_;

    [[nodiscard]] bool has_session_token() const {
        std::lock_guard<std::mutex> lock(session_mutex_);
        return !session_token_.empty();
    }

    [[nodiscard]] bool prepare_authenticated_context(grpc::ClientContext* ctx) const {
        if (ctx == nullptr) return false;
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (session_token_.empty()) return false;
        ctx->AddMetadata("authorization", "Bearer " + session_token_);
        return true;
    }

    void clear_session_token() {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_token_.clear();
    }

    void handle_authenticated_failure(const grpc::Status& status) {
        if (status.ok() || status.error_code() != grpc::StatusCode::UNAUTHENTICATED) return;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            reset_live_state_locked(true);
            state_->authenticated = false;
            state_->connected = false;
            state_->login_required = true;
            state_->auth_error_text = status.error_message().empty()
                ? std::string("Session expired. Please log in again.")
                : status.error_message();
        }
        clear_session_token();
        stop_requested_ = true;
    }

    void reset_live_state_locked(bool clear_identity) {
        state_->connected = false;
        state_->instruments.clear();
        state_->books.clear();
        state_->ticks.clear();
        state_->greeks.clear();
        state_->positions.clear();
        state_->book_positions.clear();
        state_->book_portfolios.clear();
        state_->orders.clear();
        state_->quotes.clear();
        state_->trades.clear();
        ++state_->orders_seq;
        ++state_->quotes_seq;
        ++state_->trades_seq;
        state_->alerts.clear();
        state_->mm_params.clear();
        state_->product_pricing_params.clear();
        state_->arb_params.clear();
        state_->product_states.clear();
        state_->instrument_states.clear();
        state_->arb_strategy_states.clear();
        state_->pcp_opportunities.clear();
        state_->curves.clear();
        state_->portfolio.Clear();
        state_->risk_state.Clear();
        if (clear_identity) {
            state_->current_user.Clear();
        }
    }
};

namespace {

enum InstrumentTreeRole {
    TreeRoleKind = Qt::UserRole + 1,
    TreeRoleInstrumentId,
};

enum InstrumentTreeKind {
    TreeKindGroup = 0,
    TreeKindInstrument = 1,
};

QString qstr(const std::string& value) {
    return QString::fromStdString(value);
}

QString product_class_for(const InstrumentMeta& meta) {
    const std::string& source = !meta.underlying_code.empty() ? meta.underlying_code : meta.code;
    QString product;
    for (char ch : source) {
        if (!std::isalpha(static_cast<unsigned char>(ch))) break;
        product.append(QChar(static_cast<char>(std::toupper(static_cast<unsigned char>(ch)))));
    }
    return product.isEmpty() ? QString("--") : product;
}

QString product_label_for(const InstrumentMeta& meta) {
    return QString("%1-%2").arg(product_class_for(meta), meta.kind == "Future" ? "F" : "O");
}

QString term_label_for(const InstrumentMeta& meta) {
    return QString("%1[%2]").arg(product_label_for(meta)).arg(meta.expiry_date);
}

QString format_double(double value, int decimals = 6) {
    QString text = QString::number(value, 'f', decimals);
    while (text.contains('.') && text.endsWith('0')) text.chop(1);
    if (text.endsWith('.')) text.chop(1);
    return text.isEmpty() ? QString("0") : text;
}

class InstrumentDialog final : public QDialog {
public:
    InstrumentDialog(SharedState* state, GrpcTraderClient* client, QWidget* parent)
        : QDialog(parent), state_(state), client_(client) {
        setWindowTitle("Instruments");
        resize(1180, 720);
        build_ui();
        reload_snapshot();
        populate_tree();
        if (hierarchy_->topLevelItemCount() > 0) {
            hierarchy_->setCurrentItem(hierarchy_->topLevelItem(0));
        }
    }

private:
    void build_ui() {
        auto* root_layout = new QVBoxLayout(this);
        root_layout->setContentsMargins(12, 12, 12, 12);
        root_layout->setSpacing(8);

        auto* title = new QLabel("Instruments");
        title->setStyleSheet("font-size:20px; font-weight:700;");
        root_layout->addWidget(title);

        auto* splitter = new QSplitter(Qt::Horizontal, this);
        splitter->setChildrenCollapsible(false);

        hierarchy_ = new QTreeWidget();
        hierarchy_->setColumnCount(4);
        hierarchy_->setHeaderLabels({"Exchange", "Product", "Term", "Strike"});
        hierarchy_->setRootIsDecorated(false);
        hierarchy_->setAlternatingRowColors(false);
        hierarchy_->setSelectionMode(QAbstractItemView::SingleSelection);
        hierarchy_->setUniformRowHeights(true);
        hierarchy_->setMinimumWidth(520);
        hierarchy_->header()->setSectionResizeMode(QHeaderView::Stretch);
        hierarchy_->header()->setStyleSheet("QHeaderView::section { background:#c8c8c8; padding:6px; }");
        splitter->addWidget(hierarchy_);

        auto* right = new QWidget();
        auto* right_layout = new QVBoxLayout(right);
        right_layout->setContentsMargins(12, 0, 0, 0);
        right_layout->setSpacing(12);

        selected_title_ = new QLabel("Select an instrument");
        selected_title_->setStyleSheet("font-size:20px; font-weight:700;");
        right_layout->addWidget(selected_title_);

        auto* details_group = new QGroupBox("Instrument Details");
        auto* details_layout = new QGridLayout(details_group);
        details_layout->setHorizontalSpacing(18);
        details_layout->setVerticalSpacing(8);
        details_layout->setContentsMargins(14, 14, 14, 14);

        add_detail(details_layout, 0, 0, "Product Class", "product_class");
        add_detail(details_layout, 0, 2, "Instrument Name", "instrument_name");
        add_detail(details_layout, 1, 0, "Instrument ID", "instrument_id");
        add_detail(details_layout, 1, 2, "Is Trading", "is_trading");
        add_detail(details_layout, 2, 0, "Expiry Date", "expiry_date");
        add_detail(details_layout, 2, 2, "Open Date", "open_date");
        add_detail(details_layout, 3, 0, "Tick Size", "tick_size");
        add_detail(details_layout, 3, 2, "Multiplier", "multiplier");
        add_detail(details_layout, 4, 0, "Underlying", "underlying");
        add_detail(details_layout, 4, 2, "Option Model", "option_model");
        add_detail(details_layout, 5, 0, "Option Type", "option_type");
        right_layout->addWidget(details_group);

        pricing_group_ = new QGroupBox("Pricing Parameter");
        auto* pricing_layout = new QGridLayout(pricing_group_);
        pricing_layout->setHorizontalSpacing(18);
        pricing_layout->setVerticalSpacing(10);
        pricing_layout->setContentsMargins(14, 14, 14, 14);

        auto* model = readonly_editor("Black-76");
        pricing_layout->addWidget(new QLabel("Pricing Model"), 0, 0);
        pricing_layout->addWidget(model, 0, 1);

        base_offset_type_ = new QComboBox();
        base_offset_type_->addItem("Tick", "tick");
        base_offset_type_->addItem("Price", "price");
        base_offset_type_->addItem("Percentage", "percentage");
        pricing_layout->addWidget(new QLabel("Base Offset Type"), 1, 0);
        pricing_layout->addWidget(base_offset_type_, 1, 1);

        base_offset_value_ = new QDoubleSpinBox();
        base_offset_value_->setRange(-1000000000.0, 1000000000.0);
        base_offset_value_->setDecimals(8);
        base_offset_value_->setSingleStep(0.01);
        pricing_layout->addWidget(new QLabel("Base Offset Value"), 1, 2);
        pricing_layout->addWidget(base_offset_value_, 1, 3);

        apply_button_ = new QPushButton("Apply");
        save_button_ = new QPushButton("Save");
        auto* button_row = new QWidget();
        auto* button_layout = new QHBoxLayout(button_row);
        button_layout->setContentsMargins(0, 0, 0, 0);
        button_layout->addStretch(1);
        button_layout->addWidget(apply_button_);
        button_layout->addWidget(save_button_);
        pricing_layout->addWidget(button_row, 2, 0, 1, 4);
        right_layout->addWidget(pricing_group_);

        status_ = new QLabel("Select a future instrument to edit pricing parameters.");
        status_->setStyleSheet("color:#5b5b5b;");
        right_layout->addWidget(status_);
        right_layout->addStretch(1);
        splitter->addWidget(right);
        splitter->setStretchFactor(0, 3);
        splitter->setStretchFactor(1, 2);
        root_layout->addWidget(splitter, 1);

        connect(hierarchy_, &QTreeWidget::currentItemChanged, this,
                [this](QTreeWidgetItem* current, QTreeWidgetItem*) { select_item(current); });
        connect(apply_button_, &QPushButton::clicked, this, [this] { apply_pricing("Applied"); });
        connect(save_button_, &QPushButton::clicked, this, [this] { apply_pricing("Saved"); });
        clear_details();
    }

    void add_detail(QGridLayout* layout, int row, int col, const QString& label, const QString& key) {
        layout->addWidget(new QLabel(label), row, col);
        auto* editor = readonly_editor("");
        detail_fields_[key] = editor;
        layout->addWidget(editor, row, col + 1);
    }

    QLineEdit* readonly_editor(const QString& text) {
        auto* editor = new QLineEdit(text);
        editor->setReadOnly(true);
        editor->setMinimumWidth(130);
        return editor;
    }

    void reload_snapshot() {
        instruments_.clear();
        pricing_.clear();
        ticks_.clear();
        states_.clear();
        if (state_ == nullptr) return;
        std::lock_guard<std::mutex> lock(state_->mutex);
        for (const auto& [id, meta] : state_->instruments) instruments_[id] = meta;
        for (const auto& [product, params] : state_->product_pricing_params) pricing_[product] = params;
        for (const auto& [id, tick] : state_->ticks) ticks_[id] = tick;
        for (const auto& [id, state] : state_->instrument_states) states_[id] = state;
    }

    void populate_tree() {
        hierarchy_->clear();
        std::vector<const InstrumentMeta*> rows;
        rows.reserve(instruments_.size());
        for (const auto& [_, meta] : instruments_) rows.push_back(&meta);
        std::sort(rows.begin(), rows.end(), [](const InstrumentMeta* lhs, const InstrumentMeta* rhs) {
            const auto lkey = std::tuple(
                qstr(lhs->exchange_id), product_label_for(*lhs), lhs->expiry_date, lhs->strike, qstr(lhs->code));
            const auto rkey = std::tuple(
                qstr(rhs->exchange_id), product_label_for(*rhs), rhs->expiry_date, rhs->strike, qstr(rhs->code));
            return lkey < rkey;
        });

        std::map<QString, QTreeWidgetItem*> exchange_items;
        std::map<QString, QTreeWidgetItem*> product_items;
        std::map<QString, QTreeWidgetItem*> term_items;
        for (const InstrumentMeta* meta : rows) {
            const QString exchange = qstr(meta->exchange_id);
            const QString product = product_label_for(*meta);
            const QString term = term_label_for(*meta);
            const QString exchange_key = exchange;
            const QString product_key = exchange + "|" + product;
            const QString term_key = product_key + "|" + term;

            QTreeWidgetItem* exchange_item = exchange_items[exchange_key];
            if (exchange_item == nullptr) {
                exchange_item = new QTreeWidgetItem(hierarchy_);
                exchange_item->setText(0, exchange);
                exchange_item->setData(0, TreeRoleKind, TreeKindGroup);
                exchange_items[exchange_key] = exchange_item;
            }
            QTreeWidgetItem* product_item = product_items[product_key];
            if (product_item == nullptr) {
                product_item = new QTreeWidgetItem(exchange_item);
                product_item->setText(1, product);
                product_item->setData(0, TreeRoleKind, TreeKindGroup);
                product_items[product_key] = product_item;
            }
            QTreeWidgetItem* term_item = term_items[term_key];
            if (term_item == nullptr) {
                term_item = new QTreeWidgetItem(product_item);
                term_item->setText(2, term);
                term_item->setData(0, TreeRoleKind, TreeKindGroup);
                term_items[term_key] = term_item;
            }

            if (meta->kind == "Future") {
                term_item->setData(0, TreeRoleKind, TreeKindInstrument);
                term_item->setData(0, TreeRoleInstrumentId, meta->instrument_id);
            } else {
                auto* strike_item = new QTreeWidgetItem(term_item);
                strike_item->setText(3, format_double(meta->strike, 4));
                strike_item->setData(0, TreeRoleKind, TreeKindInstrument);
                strike_item->setData(0, TreeRoleInstrumentId, meta->instrument_id);
            }
        }
        hierarchy_->expandAll();
        for (int col = 0; col < hierarchy_->columnCount(); ++col) hierarchy_->resizeColumnToContents(col);
    }

    void select_item(QTreeWidgetItem* item) {
        if (item == nullptr || item->data(0, TreeRoleKind).toInt() != TreeKindInstrument) {
            selected_instrument_id_ = 0;
            clear_details();
            return;
        }
        const uint32_t instrument_id = item->data(0, TreeRoleInstrumentId).toUInt();
        const auto it = instruments_.find(instrument_id);
        if (it == instruments_.end()) {
            selected_instrument_id_ = 0;
            clear_details();
            return;
        }
        selected_instrument_id_ = instrument_id;
        populate_details(it->second);
    }

    void clear_details() {
        selected_title_->setText("Select an instrument");
        for (auto& [_, editor] : detail_fields_) editor->clear();
        set_pricing_enabled(false);
    }

    void populate_details(const InstrumentMeta& meta) {
        selected_title_->setText(qstr(meta.code));
        set_detail("product_class", product_class_for(meta));
        set_detail("instrument_name", qstr(meta.code));
        set_detail("instrument_id", QString::number(meta.instrument_id));
        set_detail("is_trading", ticks_.find(meta.instrument_id) != ticks_.end() ? "Yes" : "No");
        set_detail("expiry_date", meta.expiry_date > 0 ? QString::number(meta.expiry_date) : "--");
        set_detail("open_date", "--");
        set_detail("tick_size", format_double(meta.tick_size));
        set_detail("multiplier", format_double(meta.multiplier));
        set_detail("underlying", meta.kind == "Option" ? qstr(meta.underlying_code) : "--");
        set_detail("option_model", meta.kind == "Option" ? "Euro" : "--");
        set_detail("option_type", meta.kind == "Option" ? qstr(meta.option_type) : "--");

        const bool future = meta.kind == "Future";
        set_pricing_enabled(future);
        if (!future) {
            status_->setText("Pricing parameters are editable only on future instruments.");
            return;
        }

        auto pricing_it = pricing_.find(meta.product_index);
        omm::proto::ProductPricingParams params;
        params.set_product_index(meta.product_index);
        params.set_base_offset_type("price");
        params.set_base_offset_value(0.0);
        if (pricing_it != pricing_.end()) params = pricing_it->second;

        const int type_index = base_offset_type_->findData(qstr(params.base_offset_type()));
        base_offset_type_->setCurrentIndex(type_index >= 0 ? type_index : 1);
        base_offset_value_->setValue(params.base_offset_value());
        status_->setText("Future pricing parameters loaded.");
    }

    void set_detail(const QString& key, const QString& value) {
        auto it = detail_fields_.find(key);
        if (it != detail_fields_.end()) it->second->setText(value);
    }

    void set_pricing_enabled(bool enabled) {
        pricing_group_->setEnabled(enabled);
        apply_button_->setEnabled(enabled);
        save_button_->setEnabled(enabled);
    }

    void apply_pricing(const QString& verb) {
        const auto it = instruments_.find(selected_instrument_id_);
        if (it == instruments_.end() || it->second.kind != "Future") {
            status_->setText("Select a future instrument before applying pricing parameters.");
            return;
        }
        if (client_ == nullptr) {
            status_->setText("No gRPC client is available.");
            return;
        }

        omm::proto::ProductPricingParams params;
        params.set_product_index(it->second.product_index);
        params.set_base_offset_type(base_offset_type_->currentData().toString().toStdString());
        params.set_base_offset_value(base_offset_value_->value());
        const bool ok = client_->set_product_pricing_params(params);
        if (ok) {
            pricing_[params.product_index()] = params;
            if (state_ != nullptr) {
                std::lock_guard<std::mutex> lock(state_->mutex);
                state_->product_pricing_params[params.product_index()] = params;
            }
            status_->setText(QString("%1 pricing parameters for %2.")
                                 .arg(verb, qstr(it->second.code)));
        } else {
            status_->setText(QString("%1 failed for %2.").arg(verb, qstr(it->second.code)));
        }
    }

    SharedState* state_{nullptr};
    GrpcTraderClient* client_{nullptr};
    std::map<uint32_t, InstrumentMeta> instruments_;
    std::map<uint32_t, omm::proto::ProductPricingParams> pricing_;
    std::unordered_map<uint32_t, omm::proto::Tick> ticks_;
    std::unordered_map<uint32_t, omm::proto::InstrumentMMState> states_;
    uint32_t selected_instrument_id_{0};
    QTreeWidget* hierarchy_{nullptr};
    QLabel* selected_title_{nullptr};
    QGroupBox* pricing_group_{nullptr};
    QComboBox* base_offset_type_{nullptr};
    QDoubleSpinBox* base_offset_value_{nullptr};
    QPushButton* apply_button_{nullptr};
    QPushButton* save_button_{nullptr};
    QLabel* status_{nullptr};
    std::map<QString, QLineEdit*> detail_fields_;
};

} // namespace

TraderMainWindow::TraderMainWindow(std::string grpc_endpoint, QWidget* parent)
    : QMainWindow(parent),
      grpc_endpoint_(std::move(grpc_endpoint)),
      impl_(std::make_unique<Impl>()) {
    setWindowTitle(QString("optionMM Trader Dashboard - %1")
                       .arg(QString::fromStdString(grpc_endpoint_)));
    build_ui();
    restore_ui_state();
    impl_->client = std::make_unique<GrpcTraderClient>(grpc_endpoint_, &impl_->state);

    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] { refresh_ui(); });
    timer->start(120);
    refresh_ui();
}

TraderMainWindow::~TraderMainWindow() = default;

bool TraderMainWindow::initialize_session() {
    return prompt_for_login();
}

void TraderMainWindow::closeEvent(QCloseEvent* event) {
    save_ui_state();
    QMainWindow::closeEvent(event);
}

bool TraderMainWindow::prompt_for_login(const QString& reason) {
    if (impl_->login_dialog_open) return false;
    impl_->login_dialog_open = true;

    LoginDialog dialog(QString::fromStdString(grpc_endpoint_), reason, this);
    dialog.set_username(impl_->last_login_username);
    for (;;) {
        if (dialog.exec() != QDialog::Accepted) {
            impl_->login_dialog_open = false;
            return false;
        }

        const QString username = dialog.username();
        const QString password = dialog.password();
        if (username.isEmpty() || password.isEmpty()) {
            dialog.show_error("Username and password are required.");
            continue;
        }

        std::string message;
        if (!impl_->client->login(username.toStdString(), password.toStdString(), &message)) {
            dialog.show_error(QString::fromStdString(message.empty()
                ? std::string("Login failed.")
                : message));
            continue;
        }

        impl_->last_login_username = username;
        impl_->client->start();
        impl_->last_operator_status_text = QString("Logged in as %1 at %2")
            .arg(username, current_time_text());
        impl_->login_dialog_open = false;
        refresh_ui();
        return true;
    }
}

void TraderMainWindow::perform_logout() {
    if (logout_action_ != nullptr && !logout_action_->isEnabled()) return;
    const auto answer = QMessageBox::question(
        this,
        "Confirm Logout",
        "Logging out will stop this GUI session and trigger the backend zero-session safety stop if this is the last trader session.\n\nDo you want to continue?",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    impl_->client->stop();
    std::string message;
    bool triggered_shutdown = false;
    const bool ok = impl_->client->logout(&message, &triggered_shutdown);
    impl_->last_operator_status_text = ok
        ? QString("Logged out at %1%2")
              .arg(current_time_text())
              .arg(triggered_shutdown ? "  backend stop-and-cancel triggered" : "")
        : QString("Logout completed locally at %1%2")
              .arg(current_time_text())
              .arg(message.empty() ? "" : QString("  (%1)").arg(QString::fromStdString(message)));
    refresh_ui();
    if (!prompt_for_login("Session closed. Log in again to resume monitoring and trading.")) {
        close();
    }
}

void TraderMainWindow::build_ui() {
    build_main_workspace_panel();
    auto* vol_dock = build_vol_curves_panel();
    auto* controls_dock = build_trader_controls_panel();
    auto* arb_monitor_dock = build_arbitrage_panel();
    connect_primary_interactions();
    auto* positions_dock = build_positions_panel();
    connect_panel_interactions();
    resizeDocks({positions_dock, controls_dock, vol_dock}, {350, 430, 520}, Qt::Horizontal);
    resizeDocks({arb_monitor_dock}, {280}, Qt::Vertical);
}

QWidget* TraderMainWindow::create_vol_curve_grid_widget(QWidget* parent) {
    return new VolCurveGridWidget(&impl_->state, parent);
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
    const QVariant book_data = manual_book_selector_ != nullptr
        ? manual_book_selector_->currentData()
        : QVariant{};
    if (!book_data.isValid() || book_data.toUInt() == 0) {
        impl_->last_operator_status_text = "Select a book before sending a manual order";
        return;
    }
    const bool ok = impl_->client->send_manual_order(
        instrument_data.toUInt(),
        side_selector_->currentText().toStdString(),
        price_editor_->value(),
        volume_editor_->value(),
        book_data.toUInt());
    impl_->last_operator_status_text = ok
        ? QString("Manual order sent to %1 at %2")
              .arg(manual_book_selector_->currentText(), current_time_text())
        : QString("Manual order rejected for %1 at %2")
              .arg(manual_book_selector_->currentText(), current_time_text());
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

void TraderMainWindow::show_instrument_panel() {
    InstrumentDialog dialog(&impl_->state, impl_->client.get(), this);
    dialog.exec();
}

void TraderMainWindow::cancel_selected_order() {
    const QModelIndex index = orders_table_ != nullptr ? orders_table_->currentIndex() : QModelIndex{};
    if (!index.isValid() || impl_->order_blotter_model == nullptr) {
        impl_->last_operator_status_text = "Select an order row before sending cancel";
        execution_status_label_->setText("Select an order row before sending cancel.");
        return;
    }

    const auto* row = impl_->order_blotter_model->row(index.row());
    if (row == nullptr || row->order.client_order_id() == 0) {
        impl_->last_operator_status_text = "Selected row has no cancelable order id";
        execution_status_label_->setText("Selected row has no cancelable order id.");
        return;
    }

    const uint64_t order_id = row->order.client_order_id();
    const uint32_t instrument_id = row->order.instrument_id();
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
    const QModelIndex index = quotes_table_ != nullptr ? quotes_table_->currentIndex() : QModelIndex{};
    if (!index.isValid() || impl_->quote_blotter_model == nullptr) {
        impl_->last_operator_status_text = "Select a quote row before sending quote cancel";
        execution_status_label_->setText("Select a quote row before sending quote cancel.");
        return;
    }

    const auto* row = impl_->quote_blotter_model->row(index.row());
    if (row == nullptr || row->quote.instrument_id() == 0) {
        impl_->last_operator_status_text = "Selected quote row has no instrument id";
        execution_status_label_->setText("Selected quote row has no instrument id.");
        return;
    }

    const uint32_t instrument_id = row->quote.instrument_id();
    const QString instrument_label = row->instrument;

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

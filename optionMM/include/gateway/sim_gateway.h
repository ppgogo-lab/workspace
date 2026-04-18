#pragma once

#include "gateway/gateway.h"
#include <array>
#include <atomic>
#include <mutex>
#include <random>
#include <thread>

namespace omm {

// ─── SimGateway ───────────────────────────────────────────────────────────────
// Simulation gateway for testing and CI — no exchange SDK required.
//
// Behaviour:
//   send_order:  immediately generates an OrderAck, then simulates a partial
//                or full fill based on last_price vs order price.
//   send_quote:  immediately generates a QuoteAck; fills simulated on both legs.
//   cancel_order: immediately generates a CancelAck.
//   query_instruments: returns a pre-populated instrument set (set via add_instrument).
//
// All callbacks are pushed into callback_buf (same as production path).

class SimGateway : public IGateway {
public:
    SimGateway() = default;
    ~SimGateway() override;

    void set_sim_config(const SimConfig& cfg) noexcept;

    // Add a simulated instrument (call before connect())
    void add_instrument(const Instrument& instr) noexcept {
        if (n_instruments_ < MAX_INSTRUMENTS)
            sim_instruments_[n_instruments_++] = instr;
    }

    // Set the simulated last price for an instrument (drives fill simulation)
    void set_last_price(uint16_t id, double price) noexcept {
        if (id < MAX_INSTRUMENTS) last_price_[id].store(price, std::memory_order_relaxed);
    }

    bool connect(const GatewayConfig&) override {
        stop_worker();
        connected_.store(true, std::memory_order_release);
        start_worker();
        return true;
    }
    void disconnect() override {
        stop_worker();
        connected_.store(false, std::memory_order_release);
    }
    [[nodiscard]] bool is_connected() const noexcept override {
        return connected_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool send_order(const Order& order) noexcept override;
    [[nodiscard]] bool send_quote(const Quote& quote) noexcept override;
    [[nodiscard]] bool cancel_order(OrderId id,
                                     uint16_t instrument_id) noexcept override;
    [[nodiscard]] bool supports_quote_replace() const noexcept override { return true; }

    bool query_instruments(Instrument* out, uint16_t* count,
                            uint16_t max_count) override {
        uint16_t n = (n_instruments_ < max_count) ? n_instruments_ : max_count;
        for (uint16_t i = 0; i < n; ++i) out[i] = sim_instruments_[i];
        *count = n;
        return true;
    }

    [[nodiscard]] uint64_t orders_sent()  const noexcept { return orders_sent_.load(); }
    [[nodiscard]] uint64_t quotes_sent()  const noexcept { return quotes_sent_.load(); }
    [[nodiscard]] uint64_t orders_filled()const noexcept { return orders_filled_.load();}

private:
    struct SimSettings {
        int ack_latency_ms{0};
        int cancel_latency_ms{0};
        int fill_interval_ms{25};
        double order_fill_probability{1.0};
        double quote_cross_fill_probability{1.0};
        double quote_passive_fill_probability{0.0};
        double partial_fill_probability{0.0};
        double reject_probability{0.0};
        Volume max_fill_size{0};
        int slippage_ticks{0};
        double quote_near_touch_ticks{0.5};
        uint32_t random_seed{42};
    };

    struct ActiveOrder {
        bool used{false};
        bool reject_pending{false};
        bool ack_sent{false};
        bool cancel_pending{false};
        Order order{};
        Volume remaining_volume{0};
        double fill_notional{0.0};
        uint64_t exchange_order_id{0};
        Timestamp ack_due_ns{0};
        Timestamp next_fill_due_ns{0};
        Timestamp cancel_due_ns{0};
    };

    struct ActiveQuote {
        bool used{false};
        bool ack_sent{false};
        Quote quote{};
        Volume remaining_bid{0};
        Volume remaining_ask{0};
        uint64_t exchange_quote_id{0};
        Timestamp ack_due_ns{0};
        Timestamp next_fill_due_ns{0};
    };

    void start_worker();
    void stop_worker();
    void worker_loop() noexcept;
    void process_orders(Timestamp now_ns, std::mt19937& rng) noexcept;
    void process_quotes(Timestamp now_ns, std::mt19937& rng) noexcept;
    [[nodiscard]] bool sample_probability(double p, std::mt19937& rng) noexcept;
    [[nodiscard]] Volume sample_fill_volume(Volume remaining, std::mt19937& rng) noexcept;
    [[nodiscard]] double market_price(uint16_t instrument_id) const noexcept;
    [[nodiscard]] double tick_size(uint16_t instrument_id) const noexcept;
    [[nodiscard]] double fill_price_for_order(const ActiveOrder& active) const noexcept;

    std::atomic<bool>     connected_{false};
    Instrument            sim_instruments_[MAX_INSTRUMENTS]{};
    uint16_t              n_instruments_{0};
    std::atomic<double>   last_price_[MAX_INSTRUMENTS]{};
    std::atomic<uint64_t> exchange_order_id_seq_{1};

    std::atomic<uint64_t> orders_sent_{0};
    std::atomic<uint64_t> quotes_sent_{0};
    std::atomic<uint64_t> orders_filled_{0};
    SimSettings           settings_{};
    std::array<ActiveOrder, MAX_OPEN_ORDERS> active_orders_{};
    std::array<ActiveQuote, MAX_INSTRUMENTS> active_quotes_{};
    std::mutex            state_mutex_;
    std::atomic<bool>     worker_running_{false};
    std::thread           worker_thread_;
};

} // namespace omm

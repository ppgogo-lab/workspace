#pragma once

#include "gateway/gateway.h"
#include <atomic>
#include <cstring>

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

    // Add a simulated instrument (call before connect())
    void add_instrument(const Instrument& instr) noexcept {
        if (n_instruments_ < MAX_INSTRUMENTS)
            sim_instruments_[n_instruments_++] = instr;
    }

    // Set the simulated last price for an instrument (drives fill simulation)
    void set_last_price(uint16_t id, double price) noexcept {
        if (id < MAX_INSTRUMENTS) last_price_[id] = price;
    }

    bool connect(const GatewayConfig&) override {
        connected_.store(true, std::memory_order_release);
        return true;
    }
    void disconnect() override {
        connected_.store(false, std::memory_order_release);
    }
    [[nodiscard]] bool is_connected() const noexcept override {
        return connected_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool send_order(const Order& order) noexcept override;
    [[nodiscard]] bool send_quote(const Quote& quote) noexcept override;
    [[nodiscard]] bool cancel_order(OrderId id,
                                     uint16_t instrument_id) noexcept override;

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
    std::atomic<bool>     connected_{false};
    Instrument            sim_instruments_[MAX_INSTRUMENTS]{};
    uint16_t              n_instruments_{0};
    double                last_price_[MAX_INSTRUMENTS]{};
    std::atomic<uint64_t> exchange_order_id_seq_{1};

    std::atomic<uint64_t> orders_sent_{0};
    std::atomic<uint64_t> quotes_sent_{0};
    std::atomic<uint64_t> orders_filled_{0};
};

} // namespace omm

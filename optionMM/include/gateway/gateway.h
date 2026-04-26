#pragma once

#include "common/instrument_lookup.h"
#include "common/types.h"
#include "common/config.h"
#include "common/ring_buffer.h"
#include <functional>
#include <vector>

namespace omm {

// ─── IGateway ─────────────────────────────────────────────────────────────────
// Abstract order routing interface.
// One concrete implementation per exchange (CTP, FEMAS) selected at startup.
//
// Thread model:
//   send_order / send_quote / cancel_order: called from gateway dispatcher thread.
//   Callbacks (on_order_ack etc.): called from the gateway's internal thread,
//     which pushes events into callback_buf_. The dispatcher thread drains this
//     buffer and routes events back to strategy threads.
//
// send_* methods must be noexcept, no dynamic allocation, no blocking.

// ─── Gateway event (callback → dispatcher) ───────────────────────────────────
enum class GatewayEventType : uint8_t {
    OrderAck,
    OrderFill,
    OrderCancel,
    OrderReject,
    QuoteAck,
    QuoteCancel,
    QuoteReject,
    QuoteFill,
    Connected,
    Disconnected,
};

struct GatewayEvent {
    GatewayEventType type;
    uint8_t          product_index;
    uint8_t          _pad[6];
    union {
        Order  order;
        Trade  trade;
        Quote  quote;
    };

    GatewayEvent() noexcept : type{}, product_index{}, _pad{}, order{} {}
};

class IGateway {
public:
    // ── Lifecycle ────────────────────────────────────────────────────────────
    virtual bool connect(const GatewayConfig& cfg)    = 0;
    virtual void disconnect()                          = 0;
    [[nodiscard]] virtual bool is_connected() const noexcept = 0;

    // ── Order routing (dispatcher thread, noexcept, no alloc) ────────────────
    [[nodiscard]] virtual bool send_order(const Order& order) noexcept = 0;
    [[nodiscard]] virtual bool send_quote(
            const Quote& quote,
            OrderId* bid_order_id_out = nullptr,
            OrderId* ask_order_id_out = nullptr) noexcept = 0;
    [[nodiscard]] virtual bool cancel_order(OrderId id,
                                            uint16_t instrument_id) noexcept = 0;
    [[nodiscard]] virtual bool cancel_quote(QuoteId id,
                                            uint16_t instrument_id) noexcept {
        return cancel_order(id, instrument_id);
    }
    [[nodiscard]] virtual bool supports_quote_replace() const noexcept {
        return false;
    }

    // ── Startup queries ───────────────────────────────────────────────────────
    // Populates instruments[] with all tradeable instruments for this exchange.
    // Blocking — called once at startup before trading begins.
    virtual bool query_instruments(Instrument* instruments,
                                    uint16_t*   count,
                                    uint16_t    max_count) = 0;

    virtual void set_instruments(const Instrument* instruments,
                                 uint16_t n) noexcept {
        instruments_ = instruments;
        n_instruments_ = n;
        instrument_lookup_.build(instruments_, n_instruments_);
    }

    // ── Callback ring buffer ──────────────────────────────────────────────────
    // The gateway's internal thread pushes GatewayEvents here.
    // The dispatcher thread drains it and routes to strategy threads.
    SPSCRingBuffer<GatewayEvent, 1024> callback_buf;

    virtual ~IGateway() = default;

protected:
    [[nodiscard]] const Instrument* instrument_by_id(uint16_t id) const noexcept {
        if (!instruments_ || id >= n_instruments_) return nullptr;
        return &instruments_[id];
    }

    [[nodiscard]] uint16_t find_instrument_id(std::string_view code) const noexcept {
        return instrument_lookup_.find(code);
    }

    const Instrument* instruments_{nullptr};
    uint16_t          n_instruments_{0};
    InstrumentLookup  instrument_lookup_{};
};

} // namespace omm

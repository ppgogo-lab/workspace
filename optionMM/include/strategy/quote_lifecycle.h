#pragma once

#include "common/types.h"
#include "strategy/mm_framework.h"

#include <algorithm>
#include <cmath>

namespace omm {

// Venue-level quote replace policy. Most venues allow direct replace, while
// GFEX requires cancel-first.
enum class QuoteReplacePolicy : uint8_t {
    DirectReplace = 0,
    CancelFirst,
};

// Price/size payload for a quote that has been sent and is awaiting ack.
struct QuoteLifecycleIntent {
    bool valid{false};
    double bid{0.0};
    double ask{0.0};
    Volume bid_vol{0};
    Volume ask_vol{0};
};

// Shared quote state machine used by strategy implementations.
struct QuoteLifecycleState {
    QuoteReplacePolicy replace_policy{QuoteReplacePolicy::DirectReplace};
    StrategyQuoteMonitorState status{StrategyQuoteMonitorState::Idle};
    double live_bid{0.0};
    double live_ask{0.0};
    Volume live_bid_vol{0};
    Volume live_ask_vol{0};
    QuoteId live_quote_id{0};
    QuoteId pending_quote_id{0};
    QuoteId cancel_target_quote_id{0};
    QuoteLifecycleIntent pending_quote{};
    Timestamp last_quote_ts{0};
    Timestamp live_since_ts{0};
    Timestamp cancel_last_send_ts{0};
    uint8_t cancel_attempts{0};
    bool reevaluate_after_quote_update{false};
    uint8_t _pad[6]{};
};

struct QuoteLifecycleConfig {
    int64_t quote_max_live_ns{0};
    int64_t cancel_retry_ns{0};
    uint8_t max_cancel_attempts{0};
};

struct QuoteLifecycleWork {
    bool block_new_quote{false};
    bool send_cancel{false};
    bool publish_cancel_failed_alert{false};
    QuoteId cancel_target_quote_id{0};
};

class QuoteLifecycleController {
public:
    /**
     * @brief Policy for exchange.
     * @param exchange Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static constexpr QuoteReplacePolicy policy_for_exchange(Exchange exchange) noexcept {
        return exchange == Exchange::GFEX
            ? QuoteReplacePolicy::CancelFirst
            : QuoteReplacePolicy::DirectReplace;
    }

    /**
     * @brief Has live quote.
     * @param state Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static constexpr bool has_live_quote(const QuoteLifecycleState& state) noexcept {
        return state.live_quote_id != 0;
    }

    /**
     * @brief Has pending quote.
     * @param state Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static constexpr bool has_pending_quote(const QuoteLifecycleState& state) noexcept {
        return state.pending_quote_id != 0;
    }

    /**
     * @brief Has tracked quote.
     * @param state Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static constexpr bool has_tracked_quote(const QuoteLifecycleState& state) noexcept {
        return state.live_quote_id != 0
            || state.pending_quote_id != 0
            || state.cancel_target_quote_id != 0;
    }

    /**
     * @brief Waiting for quote update.
     * @param state Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static constexpr bool waiting_for_quote_update(
            const QuoteLifecycleState& state) noexcept {
        return state.status == StrategyQuoteMonitorState::AckPending
            || state.status == StrategyQuoteMonitorState::CancelPending;
    }

    /**
     * @brief Should cancel current quote.
     * @param state Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static constexpr bool should_cancel_current_quote(
            const QuoteLifecycleState& state) noexcept {
        return state.status == StrategyQuoteMonitorState::Live
            || state.status == StrategyQuoteMonitorState::AckPending
            || state.status == StrategyQuoteMonitorState::CancelPending;
    }

    /**
     * @brief Is material change.
     * @param state Parameter supplied by the caller.
     * @param intent Parameter supplied by the caller.
     * @param epsilon_px Parameter supplied by the caller.
     * @param min_interval_ns Parameter supplied by the caller.
     * @param now_ns Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static bool is_material_change(const QuoteLifecycleState& state,
                                                 const QuoteLifecycleIntent& intent,
                                                 double epsilon_px,
                                                 int64_t min_interval_ns,
                                                 int64_t now_ns) noexcept {
        if (state.status == StrategyQuoteMonitorState::Idle
            || state.status == StrategyQuoteMonitorState::Suppressed) {
            return true;
        }
        if (now_ns - state.last_quote_ts < min_interval_ns) {
            return false;
        }
        if (std::fabs(state.live_bid - intent.bid) > epsilon_px) return true;
        if (std::fabs(state.live_ask - intent.ask) > epsilon_px) return true;
        if (state.live_bid_vol != intent.bid_vol) return true;
        if (state.live_ask_vol != intent.ask_vol) return true;
        return false;
    }

    /**
     * @brief Note quote submitted.
     * @param state Parameter supplied by the caller.
     * @param quote_id Parameter supplied by the caller.
     * @param intent Parameter supplied by the caller.
     * @param now_ns Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    static void note_quote_submitted(QuoteLifecycleState& state,
                                     QuoteId quote_id,
                                     const QuoteLifecycleIntent& intent,
                                     int64_t now_ns) noexcept {
        state.pending_quote_id = quote_id;
        state.pending_quote = intent;
        state.last_quote_ts = now_ns;
        state.status = StrategyQuoteMonitorState::AckPending;
    }

    /**
     * @brief Cancel target quote id.
     * @param state Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static QuoteId cancel_target_quote_id(
            const QuoteLifecycleState& state) noexcept {
        if (state.status == StrategyQuoteMonitorState::CancelPending) {
            return state.cancel_target_quote_id;
        }
        if (state.live_quote_id != 0) {
            return state.live_quote_id;
        }
        return state.pending_quote_id;
    }

    /**
     * @brief Note cancel submitted.
     * @param state Parameter supplied by the caller.
     * @param target_quote_id Parameter supplied by the caller.
     * @param now_ns Parameter supplied by the caller.
     * @param cfg Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    static void note_cancel_submitted(QuoteLifecycleState& state,
                                      QuoteId target_quote_id,
                                      int64_t now_ns,
                                      const QuoteLifecycleConfig& cfg) noexcept {
        state.cancel_target_quote_id = target_quote_id;
        state.cancel_last_send_ts = now_ns;
        state.cancel_attempts = static_cast<uint8_t>(std::min<int>(
            cfg.max_cancel_attempts,
            static_cast<int>(state.cancel_attempts) + 1));
        state.last_quote_ts = now_ns;
        state.status = StrategyQuoteMonitorState::CancelPending;
    }

    /**
     * @brief Manage.
     * @param state Parameter supplied by the caller.
     * @param cfg Parameter supplied by the caller.
     * @param now_ns Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static QuoteLifecycleWork manage(QuoteLifecycleState& state,
                                                   const QuoteLifecycleConfig& cfg,
                                                   int64_t now_ns) noexcept {
        QuoteLifecycleWork work{};

        if (state.status == StrategyQuoteMonitorState::CancelFailed) {
            if (quote_fully_filled(state)) {
                /**
                 * @brief Reset.
                 * @param state Parameter supplied by the caller.
                 * @param Idle Parameter supplied by the caller.
                 * @return None.
                 */
                reset(state, StrategyQuoteMonitorState::Idle);
            } else {
                work.block_new_quote = true;
            }
            return work;
        }

        if (state.status == StrategyQuoteMonitorState::CancelPending) {
            if (quote_fully_filled(state)) {
                /**
                 * @brief Reset.
                 * @param state Parameter supplied by the caller.
                 * @param Idle Parameter supplied by the caller.
                 * @return None.
                 */
                reset(state, StrategyQuoteMonitorState::Idle);
                return work;
            }
            work.block_new_quote = true;
            if (now_ns - state.cancel_last_send_ts < cfg.cancel_retry_ns) {
                return work;
            }
            if (state.cancel_attempts >= cfg.max_cancel_attempts) {
                state.status = StrategyQuoteMonitorState::CancelFailed;
                work.publish_cancel_failed_alert = true;
                return work;
            }
            work.send_cancel = true;
            work.cancel_target_quote_id = cancel_target_quote_id(state);
            return work;
        }

        const int64_t live_since_ts =
            state.live_since_ts != 0 ? state.live_since_ts : state.last_quote_ts;
        if (state.status == StrategyQuoteMonitorState::Live
            && state.live_quote_id != 0
            && live_since_ts > 0
            && now_ns - live_since_ts >= cfg.quote_max_live_ns) {
            work.block_new_quote = true;
            work.send_cancel = true;
            work.cancel_target_quote_id = state.live_quote_id;
        }

        return work;
    }

    /**
     * @brief Mark requote after update.
     * @param state Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    static void mark_requote_after_update(QuoteLifecycleState& state) noexcept {
        if (waiting_for_quote_update(state)) {
            state.reevaluate_after_quote_update = true;
        }
    }

    /**
     * @brief Discard requote after update.
     * @param state Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    static void discard_requote_after_update(QuoteLifecycleState& state) noexcept {
        state.reevaluate_after_quote_update = false;
    }

    /**
     * @brief Take requote after update.
     * @param state Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static bool take_requote_after_update(QuoteLifecycleState& state) noexcept {
        const bool should_requote = state.reevaluate_after_quote_update;
        state.reevaluate_after_quote_update = false;
        return should_requote;
    }

    /**
     * @brief Note quote fill.
     * @param state Parameter supplied by the caller.
     * @param client_quote_id Parameter supplied by the caller.
     * @param side Parameter supplied by the caller.
     * @param fill_volume Parameter supplied by the caller.
     * @param promote_ts Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static bool note_quote_fill(QuoteLifecycleState& state,
                                              QuoteId client_quote_id,
                                              Side side,
                                              Volume fill_volume,
                                              int64_t promote_ts) noexcept {
        const bool matches_pending =
            state.pending_quote_id != 0 && client_quote_id == state.pending_quote_id;
        const bool matches_live =
            state.live_quote_id != 0 && client_quote_id == state.live_quote_id;
        const bool matches_cancel_target =
            state.cancel_target_quote_id != 0 && client_quote_id == state.cancel_target_quote_id;
        if (!matches_pending && !matches_live && !matches_cancel_target) {
            return false;
        }

        const bool filling_replaced_live =
            matches_live
            && state.status == StrategyQuoteMonitorState::AckPending
            && state.pending_quote_id != 0;

        if (matches_pending && state.status == StrategyQuoteMonitorState::AckPending) {
            /**
             * @brief Promote pending quote to live.
             * @param state Parameter supplied by the caller.
             * @param promote_ts Parameter supplied by the caller.
             * @return None.
             */
            promote_pending_quote_to_live(state, promote_ts);
        }

        if (side == Side::Buy) {
            state.live_bid_vol = std::max<Volume>(0, state.live_bid_vol - fill_volume);
        } else {
            state.live_ask_vol = std::max<Volume>(0, state.live_ask_vol - fill_volume);
        }

        if (filling_replaced_live && state.live_bid_vol <= 0 && state.live_ask_vol <= 0) {
            /**
             * @brief Clear live quote.
             * @param state Parameter supplied by the caller.
             * @return None.
             */
            clear_live_quote(state);
            state.status = state.pending_quote_id != 0
                ? StrategyQuoteMonitorState::AckPending
                : StrategyQuoteMonitorState::Idle;
        } else if (quote_fully_filled(state)) {
            /**
             * @brief Reset.
             * @param state Parameter supplied by the caller.
             * @param Idle Parameter supplied by the caller.
             * @return None.
             */
            reset(state, StrategyQuoteMonitorState::Idle);
        }
        return true;
    }

    /**
     * @brief On quote ack.
     * @param state Parameter supplied by the caller.
     * @param quote Parameter supplied by the caller.
     * @param now_ns Parameter supplied by the caller.
     * @param request_requote Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static bool on_quote_ack(QuoteLifecycleState& state,
                                           const Quote& quote,
                                           int64_t now_ns,
                                           bool* request_requote) noexcept {
        if (request_requote) *request_requote = false;

        const bool ack_matches_pending =
            state.pending_quote_id != 0 && quote.client_quote_id == state.pending_quote_id;
        const bool ack_matches_cancel_target =
            state.status == StrategyQuoteMonitorState::CancelPending
            && state.cancel_target_quote_id != 0
            && quote.client_quote_id == state.cancel_target_quote_id;
        if (state.pending_quote_id != 0 && !ack_matches_pending && !ack_matches_cancel_target) {
            return false;
        }

        if (quote.bid_volume == 0 && quote.ask_volume == 0) {
            if (ack_matches_pending) {
                /**
                 * @brief Clear pending quote.
                 * @param state Parameter supplied by the caller.
                 * @return None.
                 */
                clear_pending_quote(state);
                if (state.live_quote_id != 0) {
                    state.status = StrategyQuoteMonitorState::Live;
                    if (request_requote) *request_requote = take_requote_after_update(state);
                } else {
                    /**
                     * @brief Reset.
                     * @param state Parameter supplied by the caller.
                     * @param Suppressed Parameter supplied by the caller.
                     * @return None.
                     */
                    reset(state, StrategyQuoteMonitorState::Suppressed);
                }
                return true;
            }
            if (ack_matches_cancel_target) {
                state.status = StrategyQuoteMonitorState::CancelPending;
                return true;
            }
            /**
             * @brief Reset.
             * @param state Parameter supplied by the caller.
             * @param Suppressed Parameter supplied by the caller.
             * @return None.
             */
            reset(state, StrategyQuoteMonitorState::Suppressed);
            return true;
        }

        if (ack_matches_pending) {
            const int64_t ack_ts = quote.ack_ts != 0 ? quote.ack_ts : now_ns;
            /**
             * @brief Promote pending quote to live.
             * @param state Parameter supplied by the caller.
             * @param ack_ts Parameter supplied by the caller.
             * @return None.
             */
            promote_pending_quote_to_live(state, ack_ts);
            if (request_requote) *request_requote = take_requote_after_update(state);
            return true;
        }

        if (!ack_matches_cancel_target && quote.client_quote_id != state.live_quote_id) {
            return false;
        }

        state.live_quote_id = quote.client_quote_id;
        state.live_bid = quote.bid_price;
        state.live_ask = quote.ask_price;
        state.live_bid_vol = quote.bid_volume;
        state.live_ask_vol = quote.ask_volume;
        state.live_since_ts = quote.ack_ts != 0 ? quote.ack_ts : now_ns;
        state.status = ack_matches_cancel_target
            ? StrategyQuoteMonitorState::CancelPending
            : StrategyQuoteMonitorState::Live;
        if (!ack_matches_cancel_target) {
            state.cancel_target_quote_id = 0;
            state.cancel_last_send_ts = 0;
            state.cancel_attempts = 0;
            if (request_requote) *request_requote = take_requote_after_update(state);
        }
        return true;
    }

    /**
     * @brief On quote cancel.
     * @param state Parameter supplied by the caller.
     * @param quote Parameter supplied by the caller.
     * @param request_requote Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static bool on_quote_cancel(QuoteLifecycleState& state,
                                              const Quote& quote,
                                              bool* request_requote) noexcept {
        if (request_requote) *request_requote = false;

        const bool matches_pending =
            state.pending_quote_id != 0 && quote.client_quote_id == state.pending_quote_id;
        const bool matches_live =
            state.live_quote_id != 0 && quote.client_quote_id == state.live_quote_id;
        const bool matches_cancel_target =
            state.cancel_target_quote_id != 0 && quote.client_quote_id == state.cancel_target_quote_id;
        if (!matches_pending && !matches_live && !matches_cancel_target) {
            return false;
        }

        if (matches_pending) {
            /**
             * @brief Clear pending quote.
             * @param state Parameter supplied by the caller.
             * @return None.
             */
            clear_pending_quote(state);
            if (state.live_quote_id != 0) {
                state.status = StrategyQuoteMonitorState::Live;
                if (request_requote) *request_requote = take_requote_after_update(state);
            } else {
                /**
                 * @brief Reset.
                 * @param state Parameter supplied by the caller.
                 * @param Suppressed Parameter supplied by the caller.
                 * @return None.
                 */
                reset(state, StrategyQuoteMonitorState::Suppressed);
                if (request_requote) *request_requote = true;
            }
            return true;
        }

        if (matches_cancel_target
            || (matches_live && state.status == StrategyQuoteMonitorState::CancelPending)) {
            /**
             * @brief Reset.
             * @param state Parameter supplied by the caller.
             * @param Suppressed Parameter supplied by the caller.
             * @return None.
             */
            reset(state, StrategyQuoteMonitorState::Suppressed);
            if (request_requote) *request_requote = true;
            return true;
        }

        /**
         * @brief Clear live quote.
         * @param state Parameter supplied by the caller.
         * @return None.
         */
        clear_live_quote(state);
        if (state.pending_quote_id != 0) {
            state.status = StrategyQuoteMonitorState::AckPending;
            if (request_requote) *request_requote = take_requote_after_update(state);
            return true;
        }

        /**
         * @brief Reset.
         * @param state Parameter supplied by the caller.
         * @param Suppressed Parameter supplied by the caller.
         * @return None.
         */
        reset(state, StrategyQuoteMonitorState::Suppressed);
        if (request_requote) *request_requote = true;
        return true;
    }

    /**
     * @brief On quote reject.
     * @param state Parameter supplied by the caller.
     * @param quote Parameter supplied by the caller.
     * @param request_requote Parameter supplied by the caller.
     * @return Return value produced by the operation.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    [[nodiscard]] static bool on_quote_reject(QuoteLifecycleState& state,
                                              const Quote& quote,
                                              bool* request_requote) noexcept {
        if (request_requote) *request_requote = false;

        const bool matches_pending =
            state.pending_quote_id != 0 && quote.client_quote_id == state.pending_quote_id;
        const bool matches_live =
            state.live_quote_id != 0 && quote.client_quote_id == state.live_quote_id;
        if (!matches_pending && !matches_live) {
            return false;
        }

        if (matches_pending) {
            /**
             * @brief Clear pending quote.
             * @param state Parameter supplied by the caller.
             * @return None.
             */
            clear_pending_quote(state);
            if (state.live_quote_id != 0) {
                state.status = StrategyQuoteMonitorState::Live;
                if (request_requote) *request_requote = take_requote_after_update(state);
                return true;
            }
        }

        /**
         * @brief Reset.
         * @param state Parameter supplied by the caller.
         * @param Suppressed Parameter supplied by the caller.
         * @return None.
         */
        reset(state, StrategyQuoteMonitorState::Suppressed);
        return true;
    }

    /**
     * @brief Reset.
     * @param state Parameter supplied by the caller.
     * @param next_state Parameter supplied by the caller.
     * @return None.
     * @note Noexcept API preserves hot-path failure and latency invariants.
     */
    static void reset(QuoteLifecycleState& state,
                      StrategyQuoteMonitorState next_state) noexcept {
        /**
         * @brief Clear live quote.
         * @param state Parameter supplied by the caller.
         * @return None.
         */
        clear_live_quote(state);
        /**
         * @brief Clear pending quote.
         * @param state Parameter supplied by the caller.
         * @return None.
         */
        clear_pending_quote(state);
        state.cancel_target_quote_id = 0;
        state.cancel_last_send_ts = 0;
        state.cancel_attempts = 0;
        state.reevaluate_after_quote_update = false;
        state.status = next_state;
    }

private:
    [[nodiscard]] static bool quote_fully_filled(const QuoteLifecycleState& state) noexcept {
        return has_tracked_quote(state)
            && state.live_bid_vol <= 0
            && state.live_ask_vol <= 0;
    }

    static void clear_live_quote(QuoteLifecycleState& state) noexcept {
        state.live_bid = 0.0;
        state.live_ask = 0.0;
        state.live_bid_vol = 0;
        state.live_ask_vol = 0;
        state.live_quote_id = 0;
        state.live_since_ts = 0;
    }

    static void clear_pending_quote(QuoteLifecycleState& state) noexcept {
        state.pending_quote_id = 0;
        state.pending_quote = QuoteLifecycleIntent{};
    }

    static void promote_pending_quote_to_live(QuoteLifecycleState& state,
                                              int64_t ack_ts) noexcept {
        if (state.pending_quote_id == 0 || !state.pending_quote.valid) {
            return;
        }
        state.live_quote_id = state.pending_quote_id;
        state.live_bid = state.pending_quote.bid;
        state.live_ask = state.pending_quote.ask;
        state.live_bid_vol = state.pending_quote.bid_vol;
        state.live_ask_vol = state.pending_quote.ask_vol;
        state.live_since_ts = ack_ts;
        clear_pending_quote(state);
        state.cancel_target_quote_id = 0;
        state.cancel_last_send_ts = 0;
        state.cancel_attempts = 0;
        state.status = StrategyQuoteMonitorState::Live;
    }
};

} // namespace omm

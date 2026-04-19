#include "sim/sim_instruments.h"
#include "common/thread_utils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace omm {

namespace {

constexpr int64_t kNsPerDay = 86'400'000'000'000LL;

Exchange parse_exchange(const ExchangeId& exchange_id) noexcept {
    if (exchange_id == "SHFE") return Exchange::SHFE;
    if (exchange_id == "DCE") return Exchange::DCE;
    if (exchange_id == "CZCE") return Exchange::CZCE;
    if (exchange_id == "CFFEX") return Exchange::CFFEX;
    if (exchange_id == "GFEX") return Exchange::GFEX;
    return Exchange::Unknown;
}

bool starts_with_ci(std::string_view text, std::string_view prefix) noexcept {
    if (text.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i]))
            != std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

struct ProductTemplate {
    double future_price;
    double strike_spacing;
    double tick_size;
    double multiplier;
};

ProductTemplate infer_template(const ProductConfig& product,
                               const SimConfig& sim) noexcept {
    const std::string_view code = product.underlying_id.view();
    if (starts_with_ci(code, "au")) {
        return {sim.au_reference_price, 2.0, 0.02, 1000.0};
    }
    if (starts_with_ci(code, "ag")) {
        return {sim.ag_reference_price, 50.0, 1.0, 15.0};
    }
    return {100.0 + 25.0 * product.strategy_core, 5.0, 0.2, 10.0};
}

void write_code(InstrumentCode& dst,
                const char* fmt,
                std::string_view a,
                int expiry_month,
                char b,
                int c) noexcept {
    std::snprintf(dst.data, sizeof(dst.data), fmt,
                  static_cast<int>(a.size()), a.data(), expiry_month, b, c);
}

int make_expiry_date(int expiry_index) noexcept {
    return 20260626 + expiry_index * 100;
}

} // namespace

uint16_t build_sim_instruments(const SystemConfig& cfg,
                               Instrument* out,
                               uint16_t max_count) noexcept {
    if (!out || max_count == 0) return 0;

    std::memset(out, 0, sizeof(Instrument) * max_count);

    const int strikes_per_side = std::max(1, cfg.sim.strikes_per_side);
    const int expiry_count = std::max(1, cfg.sim.expiry_count);
    const int64_t now_ns = get_monotonic_ns();

    uint16_t next_id = 0;

    for (int product_idx = 0; product_idx < cfg.product_count; ++product_idx) {
        if (next_id >= max_count) break;

        const ProductConfig& product = cfg.products[product_idx];
        const ProductTemplate tmpl = infer_template(product, cfg.sim);

        Instrument fut{};
        fut.instrument_id = next_id;
        fut.underlying_id = next_id;
        fut.product_index = static_cast<uint8_t>(product_idx);
        fut.kind = InstrumentKind::Future;
        fut.exchange = parse_exchange(product.exchange_id);
        fut.exchange_id = product.exchange_id;
        fut.tick_size = tmpl.tick_size;
        fut.multiplier = tmpl.multiplier;
        fut.expiry_date = make_expiry_date(0);
        fut.expiry_epoch_ns = now_ns + 30 * kNsPerDay;
        fut.code = product.underlying_id;
        fut.underlying_code = product.underlying_id;
        out[next_id] = fut;
        const uint16_t future_id = next_id;
        ++next_id;

        for (int expiry_idx = 0; expiry_idx < expiry_count && next_id < max_count; ++expiry_idx) {
            const double atm_strike = tmpl.future_price + expiry_idx * tmpl.strike_spacing * 2.0;
            const int64_t expiry_ns = now_ns + static_cast<int64_t>(45 + expiry_idx * 30) * kNsPerDay;

            for (int strike_offset = -strikes_per_side;
                 strike_offset <= strikes_per_side && next_id < max_count;
                 ++strike_offset) {
                const double strike = atm_strike + strike_offset * tmpl.strike_spacing;
                const int strike_tag = static_cast<int>(std::lround(strike));

                for (OptionType option_type : {OptionType::Call, OptionType::Put}) {
                    if (next_id >= max_count) break;

                    Instrument opt{};
                    opt.instrument_id = next_id;
                    opt.underlying_id = future_id;
                    opt.product_index = static_cast<uint8_t>(product_idx);
                    opt.kind = InstrumentKind::Option;
                    opt.option_type = option_type;
                    opt.strike = strike;
                    opt.exchange = fut.exchange;
                    opt.exchange_id = fut.exchange_id;
                    opt.tick_size = std::max(0.01, tmpl.tick_size);
                    opt.multiplier = tmpl.multiplier;
                    opt.expiry_date = make_expiry_date(expiry_idx);
                    opt.expiry_epoch_ns = expiry_ns;
                    opt.underlying_code = fut.code;
                    write_code(opt.code, "%.*s-M%d-%c-%d",
                               fut.code.view(),
                               expiry_idx + 1,
                               option_type == OptionType::Call ? 'C' : 'P',
                               strike_tag);
                    out[next_id++] = opt;
                }
            }
        }
    }

    return next_id;
}

} // namespace omm

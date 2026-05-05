#include "engine_workers.h"

#include "engine/trading_engine.h"

namespace omm {

void PricerWorker::run() noexcept { engine_.pricer_loop(); }

void StrategyWorker::run() noexcept { engine_.strategy_loop(product_index_); }

void ArbitrageWorker::run() noexcept { engine_.arb_loop(product_index_); }

void GatewayDispatcherWorker::run() noexcept { engine_.gateway_dispatcher_loop(); }

void MonitorPublisherWorker::run() noexcept { engine_.monitor_publish_loop(); }

void VolFitterWorker::run() noexcept { engine_.vol_fitter_loop(); }

void RiskMonitorWorker::run() noexcept { engine_.risk_monitor_loop(); }

void TimerWorker::run() noexcept { engine_.timer_loop(); }

} // namespace omm

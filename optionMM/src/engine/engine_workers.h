#pragma once

namespace omm {

class TradingEngine;

class PricerWorker {
public:
    explicit PricerWorker(TradingEngine& engine) noexcept : engine_(engine) {}
    void run() noexcept;

private:
    TradingEngine& engine_;
};

class StrategyWorker {
public:
    StrategyWorker(TradingEngine& engine, int product_index) noexcept
        : engine_(engine), product_index_(product_index) {}
    void run() noexcept;

private:
    TradingEngine& engine_;
    int product_index_;
};

class ArbitrageWorker {
public:
    ArbitrageWorker(TradingEngine& engine, int product_index) noexcept
        : engine_(engine), product_index_(product_index) {}
    void run() noexcept;

private:
    TradingEngine& engine_;
    int product_index_;
};

class GatewayDispatcherWorker {
public:
    explicit GatewayDispatcherWorker(TradingEngine& engine) noexcept : engine_(engine) {}
    void run() noexcept;

private:
    TradingEngine& engine_;
};

class MonitorPublisherWorker {
public:
    explicit MonitorPublisherWorker(TradingEngine& engine) noexcept : engine_(engine) {}
    void run() noexcept;

private:
    TradingEngine& engine_;
};

class VolFitterWorker {
public:
    explicit VolFitterWorker(TradingEngine& engine) noexcept : engine_(engine) {}
    void run() noexcept;

private:
    TradingEngine& engine_;
};

class RiskMonitorWorker {
public:
    explicit RiskMonitorWorker(TradingEngine& engine) noexcept : engine_(engine) {}
    void run() noexcept;

private:
    TradingEngine& engine_;
};

class TimerWorker {
public:
    explicit TimerWorker(TradingEngine& engine) noexcept : engine_(engine) {}
    void run() noexcept;

private:
    TradingEngine& engine_;
};

} // namespace omm

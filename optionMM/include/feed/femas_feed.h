#pragma once

#include "feed/feed_handler.h"
#include "common/config.h"
#include "femas/api_wrapper.h"
#include "USTPFtdcMduserApi.h"

#include <atomic>

namespace omm {

class FEMASFeedHandler : public IFeedHandler, private CUstpFtdcMduserSpi {
public:
    FEMASFeedHandler(const FemasMdConfig& cfg,
                     SPSCRingBuffer<MarketTick, 1024>* tick_buf) noexcept
        : IFeedHandler(tick_buf), cfg_(cfg) {}

    ~FEMASFeedHandler() override { stop(); }

    void start() override;
    void stop() override;

    [[nodiscard]] bool is_connected() const noexcept override {
        return connected_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t message_count() const noexcept override {
        return msg_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t error_count() const noexcept override {
        return err_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t dropped_count() const noexcept override {
        return dropped_count_.load(std::memory_order_relaxed);
    }

private:
    FemasMdConfig       cfg_{};
    IFemasMdApi*        api_{nullptr};
    std::atomic<int>    request_id_{1};
    std::atomic<bool>   login_ready_{false};
    std::atomic<int>    current_sequence_no_{0};

    int next_req_id() noexcept {
        return request_id_.fetch_add(1, std::memory_order_relaxed);
    }

    void subscribe_all_instruments() noexcept;
    void push_tick(const CUstpFtdcDepthMarketDataField& md) noexcept;

    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnPackageStart(int nTopicID, int nSequenceNo) override;
    void OnPackageEnd(int nTopicID, int nSequenceNo) override;
    void OnRspError(CUstpFtdcRspInfoField* pRspInfo,
                    int nRequestID, bool bIsLast) override;
    void OnRspUserLogin(CUstpFtdcRspUserLoginField* pRspUserLogin,
                        CUstpFtdcRspInfoField* pRspInfo,
                        int nRequestID, bool bIsLast) override;
    void OnRspUserLogout(CUstpFtdcRspUserLogoutField* pRspUserLogout,
                         CUstpFtdcRspInfoField* pRspInfo,
                         int nRequestID, bool bIsLast) override;
    void OnRtnDepthMarketData(CUstpFtdcDepthMarketDataField* pDepthMarketData) override;
    void OnRspSubMarketData(CUstpFtdcSpecificInstrumentField* pSpecificInstrument,
                            CUstpFtdcRspInfoField* pRspInfo,
                            int nRequestID, bool bIsLast) override;
};

} // namespace omm

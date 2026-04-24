#include "feed/femas_feed.h"
#include "common/thread_utils.h"
#include "logger/logger.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace omm {

void FEMASFeedHandler::start() {
    if (api_ || !tick_buf_) return;
    if (instrument_count() == 0) {
        OMM_LOG_WARN("femas-md", "start skipped: no instruments available to subscribe");
        return;
    }

    api_ = create_femas_md_api(cfg_.front_addr);
    if (!api_) {
        err_count_.fetch_add(1, std::memory_order_relaxed);
        OMM_LOG_ERROR("femas-md", "CreateFtdcMduserApi failed front={}", cfg_.front_addr);
        return;
    }

    stop_flag_.store(false, std::memory_order_relaxed);
    connected_.store(false, std::memory_order_release);
    login_ready_.store(false, std::memory_order_release);
    current_sequence_no_.store(0, std::memory_order_relaxed);

    api_->RegisterSpi(this);
    api_->SetHeartbeatTimeout(static_cast<unsigned int>(cfg_.heartbeat_timeout_sec));
    api_->SubscribeMarketDataTopic(cfg_.topic_id, USTP_TERT_QUICK);
    api_->RegisterFront(const_cast<char*>(cfg_.front_addr));
    api_->Init();

    OMM_LOG_INFO("femas-md", "connecting to front={} topic_id={} instruments={}",
                 cfg_.front_addr, cfg_.topic_id, instrument_count());
}

void FEMASFeedHandler::stop() {
    stop_flag_.store(true, std::memory_order_release);
    login_ready_.store(false, std::memory_order_release);
    connected_.store(false, std::memory_order_release);

    if (!api_) return;
    api_->RegisterSpi(nullptr);
    api_->Release();
    api_ = nullptr;
}

void FEMASFeedHandler::subscribe_all_instruments() noexcept {
    if (!api_ || !instruments_ || n_instruments_ == 0) return;

    std::vector<char*> codes;
    codes.reserve(n_instruments_);
    for (uint16_t i = 0; i < n_instruments_; ++i) {
        if (instruments_[i].instrument_id == INVALID_INSTRUMENT_ID) continue;
        if (instruments_[i].code.empty()) continue;
        if (instruments_[i].product_index >= MAX_PRODUCTS) continue;
        codes.push_back(const_cast<char*>(instruments_[i].code.data));
    }
    if (codes.empty()) {
        OMM_LOG_WARN("femas-md", "no mapped instruments available for subscription");
        return;
    }

    constexpr int kBatchSize = 64;
    for (std::size_t i = 0; i < codes.size(); i += kBatchSize) {
        const int count = static_cast<int>(std::min<std::size_t>(kBatchSize, codes.size() - i));
        const int ret = api_->SubMarketData(codes.data() + i, count);
        if (ret != 0) {
            err_count_.fetch_add(1, std::memory_order_relaxed);
            OMM_LOG_WARN("femas-md", "SubMarketData failed ret={} batch_start={} count={}",
                         ret, i, count);
        }
    }

    OMM_LOG_INFO("femas-md", "subscription request submitted count={}", codes.size());
}

void FEMASFeedHandler::push_tick(const CUstpFtdcDepthMarketDataField& md) noexcept {
    if (!tick_buf_) return;

    const int64_t recv_ts_ns = get_monotonic_ns();
    const uint16_t instrument_id = resolve_instrument(md.InstrumentID);
    if (instrument_id == INVALID_INSTRUMENT_ID) return;

    MarketTick tick{};
    tick.recv_ts_ns     = recv_ts_ns;
    tick.exchange_ts_ns = recv_ts_ns;
    tick.instrument_id  = instrument_id;
    tick.last_price     = md.LastPrice;
    tick.open_interest  = md.OpenInterest;
    tick.volume         = md.Volume;
    tick.open_price     = md.OpenPrice;
    tick.high_price     = md.HighestPrice;
    tick.low_price      = md.LowestPrice;
    tick.pre_settlement = md.PreSettlementPrice;
    tick.pre_close      = md.PreClosePrice;
    tick.sequence_no    = static_cast<uint64_t>(
        current_sequence_no_.load(std::memory_order_relaxed));

    tick.bid_price[0] = md.BidPrice1; tick.bid_volume[0] = md.BidVolume1;
    tick.bid_price[1] = md.BidPrice2; tick.bid_volume[1] = md.BidVolume2;
    tick.bid_price[2] = md.BidPrice3; tick.bid_volume[2] = md.BidVolume3;
    tick.bid_price[3] = md.BidPrice4; tick.bid_volume[3] = md.BidVolume4;
    tick.bid_price[4] = md.BidPrice5; tick.bid_volume[4] = md.BidVolume5;

    tick.ask_price[0] = md.AskPrice1; tick.ask_volume[0] = md.AskVolume1;
    tick.ask_price[1] = md.AskPrice2; tick.ask_volume[1] = md.AskVolume2;
    tick.ask_price[2] = md.AskPrice3; tick.ask_volume[2] = md.AskVolume3;
    tick.ask_price[3] = md.AskPrice4; tick.ask_volume[3] = md.AskVolume4;
    tick.ask_price[4] = md.AskPrice5; tick.ask_volume[4] = md.AskVolume5;

    if (!tick_buf_->try_push(to_top_of_book_tick(tick))) {
        dropped_count_.fetch_add(1, std::memory_order_relaxed);
    } else {
        msg_count_.fetch_add(1, std::memory_order_relaxed);
    }
}

void FEMASFeedHandler::OnFrontConnected() {
    if (stop_flag_.load(std::memory_order_relaxed)) return;

    OMM_LOG_INFO("femas-md", "front connected, sending login");
    CUstpFtdcReqUserLoginField req{};
    std::strncpy(req.BrokerID, cfg_.broker_id, sizeof(req.BrokerID) - 1);
    std::strncpy(req.UserID, cfg_.user_id, sizeof(req.UserID) - 1);
    std::strncpy(req.Password, cfg_.password, sizeof(req.Password) - 1);

    const int ret = api_->ReqUserLogin(&req, next_req_id());
    if (ret != 0) {
        err_count_.fetch_add(1, std::memory_order_relaxed);
        OMM_LOG_ERROR("femas-md", "ReqUserLogin failed ret={}", ret);
    }
}

void FEMASFeedHandler::OnFrontDisconnected(int nReason) {
    connected_.store(false, std::memory_order_release);
    login_ready_.store(false, std::memory_order_release);
    err_count_.fetch_add(1, std::memory_order_relaxed);
    OMM_LOG_WARN("femas-md", "front disconnected reason={}", nReason);
}

void FEMASFeedHandler::OnPackageStart(int, int nSequenceNo) {
    current_sequence_no_.store(nSequenceNo, std::memory_order_relaxed);
}

void FEMASFeedHandler::OnPackageEnd(int, int) {
}

void FEMASFeedHandler::OnRspError(CUstpFtdcRspInfoField* pRspInfo,
                                  int, bool) {
    err_count_.fetch_add(1, std::memory_order_relaxed);
    OMM_LOG_WARN("femas-md", "response error id={} msg={}",
                 pRspInfo ? pRspInfo->ErrorID : -1,
                 pRspInfo ? pRspInfo->ErrorMsg : "");
}

void FEMASFeedHandler::OnRspUserLogin(CUstpFtdcRspUserLoginField* pRspUserLogin,
                                      CUstpFtdcRspInfoField* pRspInfo,
                                      int, bool) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        err_count_.fetch_add(1, std::memory_order_relaxed);
        OMM_LOG_ERROR("femas-md", "login failed ErrorID={} Msg={}",
                      pRspInfo->ErrorID, pRspInfo->ErrorMsg);
        return;
    }

    login_ready_.store(true, std::memory_order_release);
    connected_.store(true, std::memory_order_release);
    OMM_LOG_INFO("femas-md", "login ok TradingDay={}",
                 pRspUserLogin ? pRspUserLogin->TradingDay : "?");
    subscribe_all_instruments();
}

void FEMASFeedHandler::OnRspUserLogout(CUstpFtdcRspUserLogoutField*,
                                       CUstpFtdcRspInfoField* pRspInfo,
                                       int, bool) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        err_count_.fetch_add(1, std::memory_order_relaxed);
        OMM_LOG_WARN("femas-md", "logout failed ErrorID={} Msg={}",
                     pRspInfo->ErrorID, pRspInfo->ErrorMsg);
    }
}

void FEMASFeedHandler::OnRtnDepthMarketData(CUstpFtdcDepthMarketDataField* pDepthMarketData) {
    if (!pDepthMarketData || stop_flag_.load(std::memory_order_relaxed)) return;
    push_tick(*pDepthMarketData);
}

void FEMASFeedHandler::OnRspSubMarketData(CUstpFtdcSpecificInstrumentField* pSpecificInstrument,
                                          CUstpFtdcRspInfoField* pRspInfo,
                                          int, bool bIsLast) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        err_count_.fetch_add(1, std::memory_order_relaxed);
        OMM_LOG_WARN("femas-md", "subscription failed instrument={} ErrorID={} Msg={}",
                     pSpecificInstrument ? pSpecificInstrument->InstrumentID : "",
                     pRspInfo->ErrorID, pRspInfo->ErrorMsg);
    }

    if (bIsLast)
        OMM_LOG_INFO("femas-md", "subscription responses completed");
}

} // namespace omm

#pragma once

#include "USTPFtdcMduserApi.h"
#include "USTPFtdcTraderApi.h"

#include <string_view>

namespace omm {

class IFemasMdApi {
public:
    virtual void Release() = 0;
    virtual void Init() = 0;
    virtual void RegisterSpi(CUstpFtdcMduserSpi* spi) = 0;
    virtual void SetHeartbeatTimeout(unsigned int timeout) = 0;
    virtual void SubscribeMarketDataTopic(int topic_id,
                                          USTP_TE_RESUME_TYPE resume_type) = 0;
    virtual void RegisterFront(char* front_addr) = 0;
    [[nodiscard]] virtual int ReqUserLogin(CUstpFtdcReqUserLoginField* req,
                                           int request_id) = 0;
    [[nodiscard]] virtual int SubMarketData(char* instrument_ids[],
                                            int count) = 0;

protected:
    virtual ~IFemasMdApi() = default;
};

class IFemasTraderApi {
public:
    virtual void Release() = 0;
    virtual void Init() = 0;
    virtual void RegisterSpi(CUstpFtdcTraderSpi* spi) = 0;
    virtual void SubscribePrivateTopic(USTP_TE_RESUME_TYPE resume_type) = 0;
    virtual void SubscribePublicTopic(USTP_TE_RESUME_TYPE resume_type) = 0;
    virtual void RegisterFront(char* front_addr) = 0;
    [[nodiscard]] virtual int ReqUserLogin(CUstpFtdcReqUserLoginField* req,
                                           int request_id) = 0;
    [[nodiscard]] virtual int ReqOrderInsert(CUstpFtdcInputOrderField* req,
                                             int request_id) = 0;
    [[nodiscard]] virtual int ReqOrderAction(CUstpFtdcOrderActionField* req,
                                             int request_id) = 0;
    [[nodiscard]] virtual int ReqQuoteInsert(CUstpFtdcInputQuoteField* req,
                                             int request_id) = 0;
    [[nodiscard]] virtual int ReqQuoteAction(CUstpFtdcQuoteActionField* req,
                                             int request_id) = 0;
    [[nodiscard]] virtual int ReqQryInstrument(CUstpFtdcQryInstrumentField* req,
                                               int request_id) = 0;

protected:
    virtual ~IFemasTraderApi() = default;
};

[[nodiscard]] bool is_femas_sim_front(std::string_view front_addr) noexcept;
[[nodiscard]] IFemasMdApi* create_femas_md_api(std::string_view front_addr);
[[nodiscard]] IFemasTraderApi* create_femas_trader_api(std::string_view front_addr);

} // namespace omm

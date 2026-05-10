#pragma once

#include "USTPFtdcMduserApi.h"
#include "USTPFtdcTraderApi.h"

#include <string_view>

namespace omm {

class IFemasMdApi {
public:
    /**
     * @brief Release.
     * @return None.
     */
    virtual void Release() = 0;
    /**
     * @brief Init.
     * @return None.
     */
    virtual void Init() = 0;
    /**
     * @brief RegisterSpi.
     * @param spi Parameter supplied by the caller.
     * @return None.
     */
    virtual void RegisterSpi(CUstpFtdcMduserSpi* spi) = 0;
    /**
     * @brief SetHeartbeatTimeout.
     * @param timeout Parameter supplied by the caller.
     * @return None.
     */
    virtual void SetHeartbeatTimeout(unsigned int timeout) = 0;
    /**
     * @brief SubscribeMarketDataTopic.
     * @param topic_id Parameter supplied by the caller.
     * @param resume_type Parameter supplied by the caller.
     * @return None.
     */
    virtual void SubscribeMarketDataTopic(int topic_id,
                                          USTP_TE_RESUME_TYPE resume_type) = 0;
    /**
     * @brief RegisterFront.
     * @param front_addr Parameter supplied by the caller.
     * @return None.
     */
    virtual void RegisterFront(char* front_addr) = 0;
    /**
     * @brief ReqUserLogin.
     * @param req Parameter supplied by the caller.
     * @param request_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    [[nodiscard]] virtual int ReqUserLogin(CUstpFtdcReqUserLoginField* req,
                                           int request_id) = 0;
    /**
     * @brief SubMarketData.
     * @param instrument_ids Parameter supplied by the caller.
     * @param count Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    [[nodiscard]] virtual int SubMarketData(char* instrument_ids[],
                                            int count) = 0;

protected:
    virtual ~IFemasMdApi() = default;
};

class IFemasTraderApi {
public:
    /**
     * @brief Release.
     * @return None.
     */
    virtual void Release() = 0;
    /**
     * @brief Init.
     * @return None.
     */
    virtual void Init() = 0;
    /**
     * @brief RegisterSpi.
     * @param spi Parameter supplied by the caller.
     * @return None.
     */
    virtual void RegisterSpi(CUstpFtdcTraderSpi* spi) = 0;
    /**
     * @brief SubscribePrivateTopic.
     * @param resume_type Parameter supplied by the caller.
     * @return None.
     */
    virtual void SubscribePrivateTopic(USTP_TE_RESUME_TYPE resume_type) = 0;
    /**
     * @brief SubscribePublicTopic.
     * @param resume_type Parameter supplied by the caller.
     * @return None.
     */
    virtual void SubscribePublicTopic(USTP_TE_RESUME_TYPE resume_type) = 0;
    /**
     * @brief RegisterFront.
     * @param front_addr Parameter supplied by the caller.
     * @return None.
     */
    virtual void RegisterFront(char* front_addr) = 0;
    /**
     * @brief ReqUserLogin.
     * @param req Parameter supplied by the caller.
     * @param request_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    [[nodiscard]] virtual int ReqUserLogin(CUstpFtdcReqUserLoginField* req,
                                           int request_id) = 0;
    /**
     * @brief ReqOrderInsert.
     * @param req Parameter supplied by the caller.
     * @param request_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    [[nodiscard]] virtual int ReqOrderInsert(CUstpFtdcInputOrderField* req,
                                             int request_id) = 0;
    /**
     * @brief ReqOrderAction.
     * @param req Parameter supplied by the caller.
     * @param request_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    [[nodiscard]] virtual int ReqOrderAction(CUstpFtdcOrderActionField* req,
                                             int request_id) = 0;
    /**
     * @brief ReqQuoteInsert.
     * @param req Parameter supplied by the caller.
     * @param request_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    [[nodiscard]] virtual int ReqQuoteInsert(CUstpFtdcInputQuoteField* req,
                                             int request_id) = 0;
    /**
     * @brief ReqQuoteAction.
     * @param req Parameter supplied by the caller.
     * @param request_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    [[nodiscard]] virtual int ReqQuoteAction(CUstpFtdcQuoteActionField* req,
                                             int request_id) = 0;
    /**
     * @brief ReqQryInstrument.
     * @param req Parameter supplied by the caller.
     * @param request_id Parameter supplied by the caller.
     * @return Return value produced by the operation.
     */
    [[nodiscard]] virtual int ReqQryInstrument(CUstpFtdcQryInstrumentField* req,
                                               int request_id) = 0;

protected:
    virtual ~IFemasTraderApi() = default;
};

[[nodiscard]] bool is_femas_sim_front(std::string_view front_addr) noexcept;
[[nodiscard]] IFemasMdApi* create_femas_md_api(std::string_view front_addr);
[[nodiscard]] IFemasTraderApi* create_femas_trader_api(std::string_view front_addr);

} // namespace omm

#include "femas/api_wrapper.h"

#include "sim/femas_simulator.h"

namespace omm {

namespace {

class RealFemasMdApi final : public IFemasMdApi {
public:
    RealFemasMdApi() = default;

    ~RealFemasMdApi() override = default;

    void Release() override {
        if (api_) {
            api_->Release();
            api_ = nullptr;
        }
        delete this;
    }

    void Init() override { api_->Init(); }

    void RegisterSpi(CUstpFtdcMduserSpi* spi) override {
        api_->RegisterSpi(spi);
    }

    void SetHeartbeatTimeout(unsigned int timeout) override {
        api_->SetHeartbeatTimeout(timeout);
    }

    void SubscribeMarketDataTopic(int topic_id,
                                  USTP_TE_RESUME_TYPE resume_type) override {
        api_->SubscribeMarketDataTopic(topic_id, resume_type);
    }

    void RegisterFront(char* front_addr) override {
        api_->RegisterFront(front_addr);
    }

    int ReqUserLogin(CUstpFtdcReqUserLoginField* req,
                     int request_id) override {
        return api_->ReqUserLogin(req, request_id);
    }

    int SubMarketData(char* instrument_ids[], int count) override {
        return api_->SubMarketData(instrument_ids, count);
    }

    static IFemasMdApi* Create() {
        auto* wrapper = new RealFemasMdApi();
        wrapper->api_ = CUstpFtdcMduserApi::CreateFtdcMduserApi("./femas_md_flow/");
        if (!wrapper->api_) {
            delete wrapper;
            return nullptr;
        }
        return wrapper;
    }

private:
    CUstpFtdcMduserApi* api_{nullptr};
};

class RealFemasTraderApi final : public IFemasTraderApi {
public:
    RealFemasTraderApi() = default;

    ~RealFemasTraderApi() override = default;

    void Release() override {
        if (api_) {
            api_->Release();
            api_ = nullptr;
        }
        delete this;
    }

    void Init() override { api_->Init(); }

    void RegisterSpi(CUstpFtdcTraderSpi* spi) override {
        api_->RegisterSpi(spi);
    }

    void SubscribePrivateTopic(USTP_TE_RESUME_TYPE resume_type) override {
        api_->SubscribePrivateTopic(resume_type);
    }

    void SubscribePublicTopic(USTP_TE_RESUME_TYPE resume_type) override {
        api_->SubscribePublicTopic(resume_type);
    }

    void RegisterFront(char* front_addr) override {
        api_->RegisterFront(front_addr);
    }

    int ReqUserLogin(CUstpFtdcReqUserLoginField* req,
                     int request_id) override {
        return api_->ReqUserLogin(req, request_id);
    }

    int ReqOrderInsert(CUstpFtdcInputOrderField* req,
                       int request_id) override {
        return api_->ReqOrderInsert(req, request_id);
    }

    int ReqOrderAction(CUstpFtdcOrderActionField* req,
                       int request_id) override {
        return api_->ReqOrderAction(req, request_id);
    }

    int ReqQuoteInsert(CUstpFtdcInputQuoteField* req,
                       int request_id) override {
        return api_->ReqQuoteInsert(req, request_id);
    }

    int ReqQuoteAction(CUstpFtdcQuoteActionField* req,
                       int request_id) override {
        return api_->ReqQuoteAction(req, request_id);
    }

    int ReqQryInstrument(CUstpFtdcQryInstrumentField* req,
                         int request_id) override {
        return api_->ReqQryInstrument(req, request_id);
    }

    static IFemasTraderApi* Create() {
        auto* wrapper = new RealFemasTraderApi();
        wrapper->api_ = CUstpFtdcTraderApi::CreateFtdcTraderApi("./femas_flow/");
        if (!wrapper->api_) {
            delete wrapper;
            return nullptr;
        }
        return wrapper;
    }

private:
    CUstpFtdcTraderApi* api_{nullptr};
};

} // namespace

bool is_femas_sim_front(std::string_view front_addr) noexcept {
    return front_addr.rfind("sim://", 0) == 0;
}

IFemasMdApi* create_femas_md_api(std::string_view front_addr) {
    if (is_femas_sim_front(front_addr)) {
        return create_sim_femas_md_api(front_addr);
    }
    return RealFemasMdApi::Create();
}

IFemasTraderApi* create_femas_trader_api(std::string_view front_addr) {
    if (is_femas_sim_front(front_addr)) {
        return create_sim_femas_trader_api(front_addr);
    }
    return RealFemasTraderApi::Create();
}

} // namespace omm

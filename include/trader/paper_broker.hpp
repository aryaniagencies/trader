#pragma once

#include "trader/broker.hpp"

namespace trader {

class PaperBroker final : public Broker {
public:
    bool connect() override;
    std::vector<Bar> fetch_historical_data(const std::string& symbol) override;
    std::vector<MarketTick> poll_market_data() override;
    OrderResult submit_order(const OrderIntent& order) override;
    const char* name() const override;
};

} // namespace trader
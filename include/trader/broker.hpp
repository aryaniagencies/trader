#pragma once

#include "trader/market_data_provider.hpp"
#include "trader/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace trader {

class Broker : public MarketDataProvider {
public:
    virtual ~Broker() = default;

    virtual bool connect() = 0;
    virtual std::vector<MarketTick> poll_market_data() = 0;
    virtual std::vector<OrderUpdate> poll_order_updates()
    {
        return {};
    }
    virtual OrderResult submit_order(const OrderIntent& order) = 0;
    virtual const char* name() const = 0;

    virtual OrderResult cancel_order(const std::string& order_id)
    {
        (void)order_id;
        return {false, "broker adapter does not support order cancellation", {}};
    }

    virtual OrderResult modify_order(const std::string& order_id, const OrderIntent& order)
    {
        (void)order_id;
        (void)order;
        return {false, "broker adapter does not support order modification", {}};
    }

    virtual std::optional<MarginSnapshot> get_margin(const std::vector<OrderIntent>& orders)
    {
        (void)orders;
        return std::nullopt;
    }
};

} // namespace trader
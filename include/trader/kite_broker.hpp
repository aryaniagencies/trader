#pragma once

#include "trader/broker.hpp"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace trader {

class KiteBroker final : public Broker {
public:
    explicit KiteBroker(
        std::vector<std::string> symbols,
        std::string default_exchange = "NSE",
        std::string product = "CNC",
        std::string history_interval = "day",
        int history_days = 30);

    bool connect() override;
    std::vector<Bar> fetch_historical_data(const std::string& symbol) override;
    std::vector<MarketTick> poll_market_data() override;
    std::vector<OrderUpdate> poll_order_updates() override;
    OrderResult submit_order(const OrderIntent& order) override;
    OrderResult cancel_order(const std::string& order_id) override;
    OrderResult modify_order(const std::string& order_id, const OrderIntent& order) override;
    std::optional<MarginSnapshot> get_margin(const std::vector<OrderIntent>& orders) override;
    const char* name() const override;

private:
    struct Instrument {
        std::string exchange;
        std::string symbol;
        std::uint64_t instrument_token{};
    };

    std::string resolve_exchange(const std::string& symbol) const;
    std::string resolve_symbol(const std::string& symbol) const;
    std::string request(
        const std::string& method,
        const std::string& target,
        const std::string& body = {},
        const std::string& content_type = "application/x-www-form-urlencoded") const;
    std::string order_form(const OrderIntent& order) const;

    std::vector<std::string> requested_symbols_;
    std::string default_exchange_;
    std::string product_;
    std::string history_interval_;
    int history_days_;
    std::string api_key_;
    std::string access_token_;
    bool connected_{false};
    std::vector<Instrument> instruments_;
    std::unordered_map<std::string, Instrument> instrument_by_key_;
    std::unordered_map<std::string, OrderUpdate> last_order_updates_;
    mutable std::mutex api_mutex_;
};

} // namespace trader

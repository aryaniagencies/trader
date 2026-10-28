#include "trader/paper_broker.hpp"

#include "trader/log.hpp"

#include <atomic>

namespace trader {
namespace {

std::atomic<std::uint64_t> next_order_id{1};

} // namespace

bool PaperBroker::connect()
{
    log(LogLevel::info, "paper broker ready; no live broker connection");
    return true;
}

std::vector<Bar> PaperBroker::fetch_historical_data(const std::string& symbol)
{
    (void)symbol;
    return {};
}

std::vector<MarketTick> PaperBroker::poll_market_data()
{
    return {};
}

OrderResult PaperBroker::submit_order(const OrderIntent& order)
{
    const auto side = order.side == Side::buy ? "buy" : "sell";
    log(LogLevel::info,
        std::string("paper order ") + side + " " + std::to_string(order.quantity) + " " +
            order.exchange + ":" + order.symbol);
    return {true, "accepted by paper broker", "paper-" + std::to_string(next_order_id++)};
}

const char* PaperBroker::name() const
{
    return "paper";
}

} // namespace trader
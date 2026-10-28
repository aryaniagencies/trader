#include "trader/business_logic.hpp"

namespace trader {

bool BusinessLogic::initialize(
    const StrategyConfig& config,
    const std::unordered_map<std::string, std::vector<Bar>>& historical_data)
{
    (void)config;
    (void)historical_data;
    return true;
}

std::vector<AnalysisCandidate> BusinessLogic::evaluate_signals(const MarketTick& tick)
{
    return on_market_data(tick);
}

std::vector<AnalysisCandidate> BusinessLogic::on_market_data(const MarketTick& tick)
{
    (void)tick;
    return {};
}

std::vector<AnalysisCandidate> BusinessLogic::on_bar(const Bar& bar)
{
    (void)bar;
    return {};
}

std::vector<AnalysisCandidate> BusinessLogic::on_timer(std::chrono::steady_clock::time_point now)
{
    (void)now;
    return {};
}

} // namespace trader
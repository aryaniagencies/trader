#pragma once

#include "trader/types.hpp"

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace trader {

class BusinessLogic {
public:
    virtual ~BusinessLogic() = default;

    virtual bool initialize(
        const StrategyConfig& config,
        const std::unordered_map<std::string, std::vector<Bar>>& historical_data);
    virtual std::vector<AnalysisCandidate> evaluate_signals(const MarketTick& tick);
    virtual std::vector<AnalysisCandidate> on_market_data(const MarketTick& tick);
    virtual std::vector<AnalysisCandidate> on_bar(const Bar& bar);
    virtual std::vector<AnalysisCandidate> on_timer(std::chrono::steady_clock::time_point now);
};

} // namespace trader
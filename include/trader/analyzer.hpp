#pragma once

#include "trader/business_logic.hpp"
#include "trader/market_data_provider.hpp"
#include "trader/types.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace trader {

class Analyzer {
public:
    Analyzer(StrategyConfig config, BusinessLogic& business_logic);

    bool initialize(MarketDataProvider& market_data);
    AnalysisResult on_tick(const MarketTick& tick);
    AnalysisResult on_bar(const Bar& bar);
    AnalysisResult on_timer(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    const std::unordered_map<std::string, std::vector<Bar>>& historical_data() const;

private:
    AnalysisResult make_result(std::vector<AnalysisCandidate> candidates, std::int64_t event_time_ms = 0);
    bool candidate_in_scope(const AnalysisCandidate& candidate) const;

    StrategyConfig config_;
    BusinessLogic& business_logic_;
    std::uint64_t sequence_{};
    bool initialized_{false};
    std::unordered_map<std::string, std::vector<Bar>> historical_data_;
};

} // namespace trader
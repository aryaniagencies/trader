#pragma once

#include "trader/types.hpp"

#include <string>
#include <vector>

namespace trader {

class MarketDataProvider {
public:
    virtual ~MarketDataProvider() = default;
    virtual std::vector<Bar> fetch_historical_data(const std::string& symbol) = 0;
};

} // namespace trader
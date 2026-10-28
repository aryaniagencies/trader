#pragma once

#include "trader/types.hpp"

#include <boost/asio.hpp>

#include <string>
#include <vector>
#include <unordered_map>

namespace trader {

struct TextMessage {
    std::string type;
    std::unordered_map<std::string, std::string> fields;
};

std::string encode_message(const TextMessage& message);
bool read_message(boost::asio::ip::tcp::socket& socket, TextMessage& message);
void write_message(boost::asio::ip::tcp::socket& socket, const TextMessage& message);

TextMessage encode_market_tick(const MarketTick& tick);
bool decode_market_tick(const TextMessage& message, MarketTick& tick);
TextMessage encode_analysis_result(const AnalysisResult& result);
bool decode_analysis_result(const TextMessage& message, AnalysisResult& result);
TextMessage encode_worker_init(
    const StrategyConfig& config,
    const std::unordered_map<std::string, std::vector<Bar>>& historical_data);
bool decode_worker_init(
    const TextMessage& message,
    StrategyConfig& config,
    std::unordered_map<std::string, std::vector<Bar>>& historical_data);

} // namespace trader

#include "trader/analyzer.hpp"

#include "trader/log.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace trader {
namespace {

std::int64_t unix_time_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

Analyzer::Analyzer(StrategyConfig config, BusinessLogic& business_logic)
    : config_(std::move(config)), business_logic_(business_logic)
{
}

bool Analyzer::initialize(MarketDataProvider& market_data)
{
    if (initialized_ || config_.strategy_id.empty() || config_.symbols.empty() ||
        config_.analysis_candidate_ttl_ms <= 0) {
        return false;
    }
    for (const auto& symbol : config_.symbols) {
        historical_data_[symbol] = market_data.fetch_historical_data(symbol);
    }
    if (!business_logic_.initialize(config_, historical_data_)) {
        historical_data_.clear();
        return false;
    }
    initialized_ = true;
    log(LogLevel::info, "initialized analyzer for strategy " + config_.strategy_id);
    return true;
}

AnalysisResult Analyzer::on_tick(const MarketTick& tick)
{
    if (!initialized_) {
        return make_result({});
    }
    if (std::find(config_.symbols.begin(), config_.symbols.end(), tick.symbol) == config_.symbols.end()) {
        return make_result({}, tick.timestamp_ms);
    }
    return make_result(business_logic_.evaluate_signals(tick), tick.timestamp_ms);
}

AnalysisResult Analyzer::on_bar(const Bar& bar)
{
    if (!initialized_) {
        return make_result({});
    }
    if (std::find(config_.symbols.begin(), config_.symbols.end(), bar.symbol) == config_.symbols.end()) {
        return make_result({}, bar.timestamp_ms);
    }
    return make_result(business_logic_.on_bar(bar), bar.timestamp_ms);
}

AnalysisResult Analyzer::on_timer(std::chrono::steady_clock::time_point now)
{
    if (!initialized_) {
        return make_result({});
    }
    return make_result(business_logic_.on_timer(now));
}

const std::unordered_map<std::string, std::vector<Bar>>& Analyzer::historical_data() const
{
    return historical_data_;
}

bool Analyzer::candidate_in_scope(const AnalysisCandidate& candidate) const
{
    if (std::find(config_.symbols.begin(), config_.symbols.end(), candidate.symbol) != config_.symbols.end()) {
        return true;
    }
    if (!candidate.underlying_symbol.empty() &&
        std::find(config_.symbols.begin(), config_.symbols.end(), candidate.underlying_symbol) != config_.symbols.end()) {
        return true;
    }
    return candidate.option_contract &&
           std::find(config_.symbols.begin(), config_.symbols.end(),
                     candidate.option_contract->underlying_symbol) != config_.symbols.end();
}

AnalysisResult Analyzer::make_result(std::vector<AnalysisCandidate> candidates, std::int64_t event_time_ms)
{
    const auto generated_at_ms = event_time_ms > 0 ? event_time_ms : unix_time_ms();
    AnalysisResult result;
    result.strategy_id = config_.strategy_id;
    result.sequence = ++sequence_;
    result.generated_at_ms = generated_at_ms;
    result.candidates.reserve(candidates.size());
    for (auto& candidate : candidates) {
        if (candidate.exchange.empty() || candidate.symbol.empty() || candidate.lot_size <= 0 ||
            candidate.target_quantity % candidate.lot_size != 0 || !std::isfinite(candidate.score) ||
            !candidate_in_scope(candidate) ||
            (candidate.order_type == OrderType::limit &&
             (!std::isfinite(candidate.limit_price) || candidate.limit_price <= 0.0))) {
            log(LogLevel::warning, "Analyzer discarded malformed or out-of-scope candidate");
            continue;
        }
        if (candidate.instrument_kind == InstrumentKind::option) {
            if (!candidate.option_contract || candidate.option_contract->symbol != candidate.symbol ||
                candidate.option_contract->exchange != candidate.exchange ||
                candidate.option_contract->lot_size != candidate.lot_size) {
                log(LogLevel::warning, "Analyzer discarded option candidate with mismatched contract metadata");
                continue;
            }
        }
        if (candidate.expires_at_ms == 0) {
            candidate.expires_at_ms = generated_at_ms + config_.analysis_candidate_ttl_ms;
        }
        if (candidate.expires_at_ms <= generated_at_ms) {
            log(LogLevel::warning, "Analyzer discarded already-expired candidate");
            continue;
        }
        result.candidates.push_back(std::move(candidate));
    }
    return result;
}

} // namespace trader
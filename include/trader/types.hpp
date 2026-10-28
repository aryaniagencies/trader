#pragma once

#include <cstdint>
#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace trader {

struct MarketTick {
    std::string exchange;
    std::string symbol;
    double last_price{};
    std::int64_t timestamp_ms{};
};

struct Bar {
    std::string exchange;
    std::string symbol;
    std::int64_t timestamp_ms{};
    double open{};
    double high{};
    double low{};
    double close{};
    std::uint64_t volume{};
};

enum class Side {
    buy,
    sell,
};

enum class OrderType {
    market,
    limit,
};

enum class OrderStatus {
    pending,
    partially_filled,
    filled,
    cancelled,
    rejected,
};

struct OrderIntent {
    std::string exchange;
    std::string symbol;
    Side side{Side::buy};
    int quantity{};
    double limit_price{};
    OrderType type{OrderType::limit};
    int lot_size{1};
};

struct OrderResult {
    bool accepted{};
    std::string message;
    std::string order_id;
};

struct OrderUpdate {
    std::string order_id;
    OrderStatus status{OrderStatus::pending};
    int cumulative_filled_quantity{};
    double average_fill_price{};
    std::string message;
};

struct Position {
    std::string exchange;
    std::string symbol;
    std::int64_t quantity{}; // Positive is long; negative is short.
    double entry_price{};
    double mark_price{};
    double realized_pnl{};
    double unrealized_pnl{};
};

struct RiskLimits {
    double max_drawdown{}; // Maximum strategy loss in account currency; zero disables.
    double stop_loss_limit{}; // Maximum unrealized loss per symbol; zero disables.
    int max_position_size{}; // Maximum absolute quantity per symbol; zero disables.
};

struct StrategyConfig {
    std::string strategy_id;
    std::vector<std::string> symbols;
    double allocated_capital{};
    RiskLimits risk_limits;
    bool live_orders_enabled{false};
    std::int64_t analysis_candidate_ttl_ms{3000};
};

struct ActiveOrder {
    OrderIntent intent;
    OrderStatus status{OrderStatus::pending};
    int cumulative_filled_quantity{};
    double cumulative_fill_notional{};
};

struct RuntimeState {
    std::unordered_map<std::string, Position> current_positions;
    std::unordered_map<std::string, ActiveOrder> active_orders;
    double unrealized_pnl{};
    double realized_pnl{};
    bool is_running{false};
};

enum class OptionType {
    call,
    put,
};

struct Greeks {
    double delta{};
    double gamma{};
    double theta{}; // Per calendar day.
    double vega{}; // Per one volatility percentage point.
    double implied_volatility{}; // As a decimal, e.g. 0.20 for 20%.
};

struct FoContract {
    std::string symbol;
    std::string exchange;
    std::string expiry_date; // ISO-8601 date, YYYY-MM-DD.
    double strike_price{};
    OptionType option_type{OptionType::call};
    std::string underlying_symbol;
    int lot_size{};
    double underlying_price{};
    double option_market_price{};
    Greeks greeks;
    double initial_margin_required{};
    double m2m_margin{};
};

struct MarginSnapshot {
    double initial_margin_required{};
    double m2m_margin{};
    double available_balance{};
};

enum class InstrumentKind {
    equity,
    future,
    option,
};

struct AnalysisCandidate {
    std::string exchange;
    std::string symbol;
    std::string underlying_symbol;
    InstrumentKind instrument_kind{InstrumentKind::equity};
    std::int64_t target_quantity{}; // Desired signed position; zero requests square-off.
    OrderType order_type{OrderType::limit};
    double limit_price{};
    int lot_size{1};
    double score{};
    std::string rationale;
    std::int64_t expires_at_ms{};
    std::optional<FoContract> option_contract;
};

struct AnalysisResult {
    static constexpr std::uint32_t current_schema_version = 1;

    std::uint32_t schema_version{current_schema_version};
    std::string strategy_id;
    std::uint64_t sequence{};
    std::int64_t generated_at_ms{};
    std::vector<AnalysisCandidate> candidates;
};

} // namespace trader
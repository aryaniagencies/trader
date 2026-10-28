#pragma once

#include "trader/analyzer.hpp"
#include "trader/broker.hpp"
#include "trader/types.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace trader {

struct PnlSnapshot {
    double realized{};
    double unrealized{};
    double total{};
};

class Trader {
public:
    Trader(StrategyConfig config, Broker& broker);

    bool initialize();
    bool start();
    void stop();

    void on_tick(const MarketTick& tick);
    void on_bar(const Bar& bar);
    void on_order_update(const OrderUpdate& update);

    std::vector<OrderResult> consume_analysis_result(const AnalysisResult& result);
    bool risk_check(const OrderIntent& order, std::string* reason = nullptr) const;
    OrderResult place_order(const OrderIntent& order);
    OrderResult cancel_order(const std::string& order_id);
    OrderResult modify_order(const std::string& order_id, const OrderIntent& replacement);
    OrderResult square_off_all();
    OrderResult square_off_symbol(const std::string& symbol);

    PnlSnapshot calculate_pnl();
    const StrategyConfig& config() const;
    const RuntimeState& state() const;
    void register_contract(const FoContract& contract);
    std::optional<Greeks> calculate_greeks(
        const FoContract& contract,
        double risk_free_rate = 0.06,
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) const;
    static double get_atm_strike(double underlying_price, double strike_interval);
    static double select_strike(
        double underlying_price,
        double strike_interval,
        OptionType option_type,
        int moneyness_steps);

    bool check_expiry_cutoff(
        const FoContract& contract,
        const std::string& exchange_local_date,
        std::chrono::minutes exchange_local_time,
        std::chrono::minutes cutoff = std::chrono::hours(15) + std::chrono::minutes(15));
    bool auto_rollover(const FoContract& current_contract, const FoContract& next_contract);
    std::optional<MarginSnapshot> check_span_margin(const std::vector<OrderIntent>& orders);
    OrderResult place_multi_leg_order(const std::vector<OrderIntent>& orders);
    OrderResult delta_neutral_adjustment(
        const std::string& exchange,
        const std::string& underlying_symbol,
        int lot_size,
        double delta_tolerance = 0.5);
    OrderResult gamma_scalping(
        const std::string& exchange,
        const std::string& underlying_symbol,
        int lot_size,
        double minimum_underlying_move);

private:
    static std::string position_key(const std::string& exchange, const std::string& symbol);
    bool validate_config() const;
    bool risk_check(const OrderIntent& order, const std::string& excluded_order_id, std::string* reason) const;
    bool risk_check(
        const OrderIntent& order,
        const std::string& excluded_order_id,
        const std::vector<OrderIntent>& preceding_orders,
        std::string* reason) const;
    OrderResult square_off_instrument(const std::string& exchange, const std::string& symbol);
    void apply_fill(const OrderIntent& order, int quantity, double price);
    double portfolio_delta(const std::string& underlying_symbol) const;
    double portfolio_gamma(const std::string& underlying_symbol) const;
    OrderResult submit_unchecked(const OrderIntent& order);

    StrategyConfig config_;
    Broker& broker_;
    RuntimeState state_;
    bool initialized_{false};
    double peak_pnl_{};
    std::uint64_t last_analysis_sequence_{};
    std::unordered_map<std::string, double> latest_prices_;
    std::unordered_map<std::string, FoContract> contracts_;
    std::unordered_map<std::string, OrderIntent> pending_rollovers_;
    std::unordered_map<std::string, double> last_gamma_hedge_prices_;
};

} // namespace trader
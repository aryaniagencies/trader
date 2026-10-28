#include "trader/trader.hpp"

#include "trader/log.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <sstream>
#include <utility>

namespace trader {
namespace {

constexpr double seconds_per_year = 365.0 * 24.0 * 60.0 * 60.0;
constexpr double normal_distribution_constant = 0.3989422804014327;

std::int64_t current_time_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

double normal_pdf(double value)
{
    return normal_distribution_constant * std::exp(-0.5 * value * value);
}

double normal_cdf(double value)
{
    return 0.5 * std::erfc(-value / std::sqrt(2.0));
}

double option_price(
    OptionType type,
    double underlying,
    double strike,
    double time_years,
    double rate,
    double volatility)
{
    const auto root_time = std::sqrt(time_years);
    const auto d1 = (std::log(underlying / strike) +
                     (rate + 0.5 * volatility * volatility) * time_years) /
                    (volatility * root_time);
    const auto d2 = d1 - volatility * root_time;
    const auto discounted_strike = strike * std::exp(-rate * time_years);
    if (type == OptionType::call) {
        return underlying * normal_cdf(d1) - discounted_strike * normal_cdf(d2);
    }
    return discounted_strike * normal_cdf(-d2) - underlying * normal_cdf(-d1);
}

std::optional<std::chrono::system_clock::time_point> expiry_time_utc(const std::string& date)
{
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') {
        return std::nullopt;
    }
    int year_value{};
    unsigned month_value{};
    unsigned day_value{};
    const auto parse_part = [](const char* begin, const char* end, auto& value) {
        const auto result = std::from_chars(begin, end, value);
        return result.ec == std::errc{} && result.ptr == end;
    };
    if (!parse_part(date.data(), date.data() + 4, year_value) ||
        !parse_part(date.data() + 5, date.data() + 7, month_value) ||
        !parse_part(date.data() + 8, date.data() + 10, day_value)) {
        return std::nullopt;
    }
    const std::chrono::year_month_day expiry_date{
        std::chrono::year{year_value}, std::chrono::month{month_value}, std::chrono::day{day_value}};
    if (!expiry_date.ok()) {
        return std::nullopt;
    }

    // Indian exchange expiry is treated as 15:30 IST (10:00 UTC).
    const auto expiry = std::chrono::sys_days{expiry_date} + std::chrono::hours{10};
    return std::chrono::system_clock::time_point{expiry};
}

bool valid_positive(double value)
{
    return std::isfinite(value) && value > 0.0;
}

} // namespace

Trader::Trader(StrategyConfig config, Broker& broker)
    : config_(std::move(config)), broker_(broker)
{
}

bool Trader::validate_config() const
{
    const auto& limits = config_.risk_limits;
    return !config_.strategy_id.empty() && !config_.symbols.empty() &&
           valid_positive(config_.allocated_capital) && std::isfinite(limits.max_drawdown) &&
           std::isfinite(limits.stop_loss_limit) && limits.max_drawdown >= 0.0 &&
           limits.stop_loss_limit >= 0.0 && limits.max_position_size >= 0 &&
           config_.analysis_candidate_ttl_ms > 0;
}

bool Trader::initialize()
{
    if (initialized_) {
        return false;
    }
    if (!validate_config()) {
        log(LogLevel::error, "invalid strategy configuration");
        return false;
    }
    if (!broker_.connect()) {
        log(LogLevel::error, "broker connection failed during strategy initialization");
        return false;
    }

    initialized_ = true;
    log(LogLevel::info, "initialized strategy " + config_.strategy_id + " with broker " + broker_.name());
    return true;
}

bool Trader::start()
{
    if (!initialized_ || state_.is_running) {
        return false;
    }
    state_.is_running = true;
    log(LogLevel::info, "started strategy " + config_.strategy_id);
    return true;
}

void Trader::stop()
{
    state_.is_running = false;
    std::vector<std::string> order_ids;
    order_ids.reserve(state_.active_orders.size());
    for (const auto& [order_id, order] : state_.active_orders) {
        if (order.status == OrderStatus::pending || order.status == OrderStatus::partially_filled) {
            order_ids.push_back(order_id);
        }
    }
    for (const auto& order_id : order_ids) {
        const auto result = cancel_order(order_id);
        if (!result.accepted) {
            log(LogLevel::warning, "could not cancel order while stopping: " + result.message);
        }
    }
    log(LogLevel::info, "stopped strategy " + config_.strategy_id + "; positions remain open");
}

std::string Trader::position_key(const std::string& exchange, const std::string& symbol)
{
    return exchange + "|" + symbol;
}

void Trader::on_tick(const MarketTick& tick)
{
    if (!valid_positive(tick.last_price)) {
        return;
    }
    const auto key = position_key(tick.exchange, tick.symbol);
    latest_prices_[key] = tick.last_price;
    if (auto position = state_.current_positions.find(key); position != state_.current_positions.end()) {
        position->second.mark_price = tick.last_price;
    }
    calculate_pnl();

}

void Trader::on_bar(const Bar& bar)
{
    if (!valid_positive(bar.close)) {
        return;
    }
    const auto key = position_key(bar.exchange, bar.symbol);
    latest_prices_[key] = bar.close;
    if (auto position = state_.current_positions.find(key); position != state_.current_positions.end()) {
        position->second.mark_price = bar.close;
    }
    calculate_pnl();
}

void Trader::on_order_update(const OrderUpdate& update)
{
    const auto found = state_.active_orders.find(update.order_id);
    if (found == state_.active_orders.end()) {
        log(LogLevel::warning, "received update for unknown order " + update.order_id);
        return;
    }
    auto& active = found->second;
    const int reported_fills = std::clamp(update.cumulative_filled_quantity, 0, active.intent.quantity);
    if (reported_fills > active.cumulative_filled_quantity) {
        if (!valid_positive(update.average_fill_price)) {
            log(LogLevel::warning, "ignored order update with fills but invalid average price: " + update.order_id);
            return;
        }
        const auto delta = reported_fills - active.cumulative_filled_quantity;
        const auto cumulative_notional = reported_fills * update.average_fill_price;
        const auto incremental_price =
            (cumulative_notional - active.cumulative_fill_notional) / static_cast<double>(delta);
        if (!valid_positive(incremental_price)) {
            log(LogLevel::warning, "ignored order update with invalid incremental fill price: " + update.order_id);
            return;
        }
        apply_fill(active.intent, delta, incremental_price);
        active.cumulative_fill_notional = cumulative_notional;
        active.cumulative_filled_quantity = reported_fills;
    }
    active.status = update.status;

    if (update.status == OrderStatus::filled || update.status == OrderStatus::cancelled ||
        update.status == OrderStatus::rejected) {
        state_.active_orders.erase(found);
    }
    calculate_pnl();
}

std::vector<OrderResult> Trader::consume_analysis_result(const AnalysisResult& result)
{
    std::vector<OrderResult> decisions;
    const auto reject_result = [&decisions](std::string message) {
        decisions.push_back({false, std::move(message), {}});
    };
    if (!state_.is_running) {
        reject_result("strategy is not running");
        return decisions;
    }
    if (result.schema_version != AnalysisResult::current_schema_version) {
        reject_result("unsupported analysis result schema version");
        return decisions;
    }
    if (result.strategy_id != config_.strategy_id) {
        reject_result("analysis result strategy_id does not match trader configuration");
        return decisions;
    }
    if (result.sequence <= last_analysis_sequence_) {
        reject_result("duplicate or out-of-order analysis result sequence");
        return decisions;
    }
    const auto now_ms = current_time_ms();
    if (result.generated_at_ms <= 0 || result.generated_at_ms > now_ms + 1000 ||
        now_ms - result.generated_at_ms > config_.analysis_candidate_ttl_ms) {
        reject_result("analysis result is stale or has an invalid timestamp");
        return decisions;
    }
    last_analysis_sequence_ = result.sequence;

    for (const auto& candidate : result.candidates) {
        if (candidate.expires_at_ms <= now_ms ||
            candidate.expires_at_ms > result.generated_at_ms + config_.analysis_candidate_ttl_ms) {
            decisions.push_back({false, "analysis candidate has expired", {}});
            continue;
        }
        if (candidate.exchange.empty() || candidate.symbol.empty() || candidate.lot_size <= 0 ||
            candidate.target_quantity % candidate.lot_size != 0 ||
            candidate.target_quantity > std::numeric_limits<int>::max() ||
            candidate.target_quantity < std::numeric_limits<int>::min()) {
            decisions.push_back({false, "candidate target quantity is invalid", {}});
            continue;
        }
        if (candidate.order_type == OrderType::limit && !valid_positive(candidate.limit_price)) {
            decisions.push_back({false, "limit candidate requires a finite positive price", {}});
            continue;
        }
        if (candidate.instrument_kind == InstrumentKind::option && !candidate.option_contract) {
            decisions.push_back({false, "option candidate is missing contract metadata", {}});
            continue;
        }
        if (candidate.option_contract) {
            const auto& contract = *candidate.option_contract;
            if (contract.symbol != candidate.symbol || contract.exchange != candidate.exchange ||
                contract.lot_size != candidate.lot_size ||
                (!candidate.underlying_symbol.empty() &&
                 contract.underlying_symbol != candidate.underlying_symbol)) {
                decisions.push_back({false, "candidate and option contract metadata do not match", {}});
                continue;
            }
            register_contract(*candidate.option_contract);
            if (!contracts_.contains(candidate.symbol)) {
                decisions.push_back({false, "option contract failed Trader-side contract validation", {}});
                continue;
            }
        }

        const auto key = position_key(candidate.exchange, candidate.symbol);
        long double projected_quantity{};
        if (const auto position = state_.current_positions.find(key); position != state_.current_positions.end()) {
            projected_quantity = static_cast<long double>(position->second.quantity);
        }
        for (const auto& [order_id, active] : state_.active_orders) {
            (void)order_id;
            if (active.intent.exchange == candidate.exchange && active.intent.symbol == candidate.symbol &&
                active.status != OrderStatus::cancelled && active.status != OrderStatus::rejected &&
                active.status != OrderStatus::filled) {
                const auto remaining = active.intent.quantity - active.cumulative_filled_quantity;
                projected_quantity += active.intent.side == Side::buy ?
                                          static_cast<long double>(remaining) :
                                          -static_cast<long double>(remaining);
            }
        }

        const auto delta_wide = static_cast<long double>(candidate.target_quantity) - projected_quantity;
        if (delta_wide == 0) {
            decisions.push_back({true, "target position already reached", {}});
            continue;
        }
        const auto absolute_delta = std::abs(delta_wide);
        if (absolute_delta > std::numeric_limits<int>::max()) {
            decisions.push_back({false, "target position delta exceeds broker quantity range", {}});
            continue;
        }

        OrderIntent order;
        order.exchange = candidate.exchange;
        order.symbol = candidate.symbol;
        order.side = delta_wide > 0.0L ? Side::buy : Side::sell;
        order.quantity = static_cast<int>(absolute_delta);
        order.limit_price = candidate.limit_price;
        order.type = candidate.order_type;
        order.lot_size = candidate.lot_size;
        decisions.push_back(place_order(order));
    }
    return decisions;
}

bool Trader::risk_check(const OrderIntent& order, std::string* reason) const
{
    return risk_check(order, {}, std::vector<OrderIntent>{}, reason);
}

bool Trader::risk_check(
    const OrderIntent& order,
    const std::string& excluded_order_id,
    std::string* reason) const
{
    return risk_check(order, excluded_order_id, std::vector<OrderIntent>{}, reason);
}

bool Trader::risk_check(
    const OrderIntent& order,
    const std::string& excluded_order_id,
    const std::vector<OrderIntent>& preceding_orders,
    std::string* reason) const
{
    const auto reject = [reason](const std::string& message) {
        if (reason != nullptr) {
            *reason = message;
        }
        return false;
    };
    if (order.exchange.empty() || order.symbol.empty() || order.quantity <= 0 || order.lot_size <= 0) {
        return reject("exchange, symbol, positive quantity, and lot size are required");
    }
    if (order.quantity % order.lot_size != 0) {
        return reject("order quantity must be a multiple of lot size");
    }
    if (order.type == OrderType::limit && !valid_positive(order.limit_price)) {
        return reject("limit order requires a finite positive limit price");
    }

    bool allowed_symbol = std::find(config_.symbols.begin(), config_.symbols.end(), order.symbol) != config_.symbols.end();
    const auto contract = contracts_.find(order.symbol);
    if (contract != contracts_.end()) {
        allowed_symbol = allowed_symbol ||
                         std::find(config_.symbols.begin(), config_.symbols.end(),
                                   contract->second.underlying_symbol) != config_.symbols.end();
    }
    if (!allowed_symbol) {
        return reject("symbol is not configured for this strategy");
    }

    const auto key = position_key(order.exchange, order.symbol);
    std::unordered_map<std::string, std::int64_t> projected_quantities;
    std::unordered_map<std::string, double> projected_prices;
    for (const auto& [position_id, position] : state_.current_positions) {
        projected_quantities[position_id] = position.quantity;
        projected_prices[position_id] = position.mark_price > 0.0 ? position.mark_price : position.entry_price;
    }
    const auto add_order = [&](const OrderIntent& intent, int remaining) {
        if (remaining <= 0) {
            return;
        }
        const auto order_key = position_key(intent.exchange, intent.symbol);
        projected_quantities[order_key] += intent.side == Side::buy ? remaining : -remaining;
        if (intent.limit_price > 0.0) {
            projected_prices[order_key] = intent.limit_price;
        }
    };
    for (const auto& [order_id, active] : state_.active_orders) {
        if (order_id == excluded_order_id || active.status == OrderStatus::cancelled ||
            active.status == OrderStatus::rejected || active.status == OrderStatus::filled) {
            continue;
        }
        add_order(active.intent, active.intent.quantity - active.cumulative_filled_quantity);
    }
    for (const auto& preceding_order : preceding_orders) {
        add_order(preceding_order, preceding_order.quantity);
    }
    add_order(order, order.quantity);

    const auto projected_quantity = projected_quantities[key];
    if (config_.risk_limits.max_position_size > 0 &&
        std::llabs(projected_quantity) > config_.risk_limits.max_position_size) {
        return reject("maximum position size exceeded");
    }

    const auto latest = latest_prices_.find(key);
    double gross_exposure{};
    for (const auto& [position_id, quantity] : projected_quantities) {
        if (quantity == 0) {
            continue;
        }
        auto price = projected_prices[position_id];
        if (const auto known = latest_prices_.find(position_id); known != latest_prices_.end()) {
            price = known->second;
        }
        if (position_id == key && order.type == OrderType::market && latest != latest_prices_.end()) {
            price = latest->second;
        }
        if (!valid_positive(price)) {
            return reject("cannot value projected position without a market or limit price");
        }
        gross_exposure += static_cast<double>(std::llabs(quantity)) * price;
    }

    const auto f_and_o = contracts_.find(order.symbol);
    if (f_and_o != contracts_.end()) {
        const auto margin = broker_.get_margin({order});
        if (!margin) {
            return reject("F&O order requires broker-provided SPAN margin data");
        }
        if (!std::isfinite(margin->initial_margin_required) || !std::isfinite(margin->m2m_margin) ||
            !std::isfinite(margin->available_balance) ||
            margin->initial_margin_required < 0.0 || margin->m2m_margin < 0.0 ||
            margin->available_balance < 0.0 ||
            margin->initial_margin_required + margin->m2m_margin > config_.allocated_capital ||
            margin->initial_margin_required + margin->m2m_margin > margin->available_balance) {
            return reject("required initial and M2M margin exceeds allocation or available balance");
        }
    } else if (gross_exposure > config_.allocated_capital) {
        return reject("projected gross exposure exceeds allocated capital");
    }

    const auto pnl_total = state_.realized_pnl + state_.unrealized_pnl;
    if (config_.risk_limits.max_drawdown > 0.0 &&
        peak_pnl_ - pnl_total >= config_.risk_limits.max_drawdown) {
        return reject("maximum drawdown limit reached");
    }
    if (config_.risk_limits.stop_loss_limit > 0.0) {
        if (const auto position = state_.current_positions.find(key);
            position != state_.current_positions.end() &&
            position->second.unrealized_pnl <= -config_.risk_limits.stop_loss_limit) {
            const auto old_quantity = position->second.quantity;
            const auto signed_order = order.side == Side::buy ? order.quantity : -order.quantity;
            if (std::llabs(static_cast<long long>(old_quantity) + signed_order) > std::llabs(old_quantity)) {
                return reject("symbol stop-loss limit reached; position increases are blocked");
            }
        }
    }
    if (reason != nullptr) {
        reason->clear();
    }
    return true;
}

OrderResult Trader::place_order(const OrderIntent& order)
{
    if (!state_.is_running) {
        return {false, "strategy is not running", {}};
    }
    std::string reason;
    if (!risk_check(order, &reason)) {
        return {false, reason, {}};
    }
    if (!config_.live_orders_enabled && std::string_view{broker_.name()} != "paper") {
        return {false, "live orders are disabled in strategy configuration", {}};
    }
    return submit_unchecked(order);
}

OrderResult Trader::submit_unchecked(const OrderIntent& order)
{
    if (!config_.live_orders_enabled && std::string_view{broker_.name()} != "paper") {
        return {false, "live orders are disabled in strategy configuration", {}};
    }
    auto result = broker_.submit_order(order);
    if (!result.accepted) {
        return result;
    }
    if (result.order_id.empty()) {
        static std::atomic<std::uint64_t> local_order_id{1};
        result.order_id = "untracked-" + std::to_string(local_order_id++);
        log(LogLevel::warning, "broker accepted an order without an order ID; tracking is local only");
    }
    state_.active_orders[result.order_id] = {order, OrderStatus::pending, 0};
    return result;
}

OrderResult Trader::cancel_order(const std::string& order_id)
{
    const auto found = state_.active_orders.find(order_id);
    if (found == state_.active_orders.end()) {
        return {false, "order is not active", {}};
    }
    auto result = broker_.cancel_order(order_id);
    if (result.accepted) {
        state_.active_orders.erase(found);
    }
    return result;
}

OrderResult Trader::modify_order(const std::string& order_id, const OrderIntent& replacement)
{
    if (!config_.live_orders_enabled && std::string_view{broker_.name()} != "paper") {
        return {false, "live orders are disabled in strategy configuration", {}};
    }
    const auto found = state_.active_orders.find(order_id);
    if (found == state_.active_orders.end()) {
        return {false, "order is not active", {}};
    }
    if (replacement.quantity < found->second.cumulative_filled_quantity) {
        return {false, "replacement quantity is below cumulative fills", {}};
    }
    std::string reason;
    if (!risk_check(replacement, order_id, &reason)) {
        return {false, reason, {}};
    }
    auto result = broker_.modify_order(order_id, replacement);
    if (!result.accepted) {
        return result;
    }
    auto updated = found->second;
    updated.intent = replacement;
    state_.active_orders.erase(found);
    const auto replacement_id = result.order_id.empty() ? order_id : result.order_id;
    state_.active_orders[replacement_id] = std::move(updated);
    result.order_id = replacement_id;
    return result;
}

void Trader::apply_fill(const OrderIntent& order, int quantity, double price)
{
    if (quantity <= 0 || !valid_positive(price)) {
        return;
    }
    const auto key = position_key(order.exchange, order.symbol);
    auto& position = state_.current_positions[key];
    position.exchange = order.exchange;
    position.symbol = order.symbol;
    const auto signed_fill = static_cast<std::int64_t>(order.side == Side::buy ? quantity : -quantity);
    const auto old_quantity = position.quantity;
    if (old_quantity == 0 || (old_quantity > 0) == (signed_fill > 0)) {
        const auto new_quantity = old_quantity + signed_fill;
        const auto old_notional = std::abs(old_quantity) * position.entry_price;
        const auto fill_notional = std::abs(signed_fill) * price;
        position.entry_price = (old_notional + fill_notional) / std::abs(new_quantity);
        position.quantity = new_quantity;
    } else {
        const auto closed_quantity = std::min(std::abs(old_quantity), std::abs(signed_fill));
        const auto realized = closed_quantity * (price - position.entry_price) * (old_quantity > 0 ? 1.0 : -1.0);
        position.realized_pnl += realized;
        state_.realized_pnl += realized;
        const auto new_quantity = old_quantity + signed_fill;
        position.quantity = new_quantity;
        if (new_quantity == 0) {
            position.entry_price = 0.0;
        } else if ((new_quantity > 0) != (old_quantity > 0)) {
            position.entry_price = price;
        }
    }
    position.mark_price = latest_prices_.contains(key) ? latest_prices_.at(key) : price;
    if (position.quantity == 0) {
        state_.current_positions.erase(key);
    }
}

PnlSnapshot Trader::calculate_pnl()
{
    double unrealized{};
    for (auto& [key, position] : state_.current_positions) {
        if (const auto price = latest_prices_.find(key); price != latest_prices_.end()) {
            position.mark_price = price->second;
        }
        position.unrealized_pnl =
            (position.mark_price - position.entry_price) * static_cast<double>(position.quantity);
        unrealized += position.unrealized_pnl;
    }
    state_.unrealized_pnl = unrealized;
    const auto total = state_.realized_pnl + state_.unrealized_pnl;
    peak_pnl_ = std::max(peak_pnl_, total);
    return {state_.realized_pnl, state_.unrealized_pnl, total};
}

OrderResult Trader::square_off_instrument(const std::string& exchange, const std::string& symbol)
{
    std::vector<std::string> order_ids;
    for (const auto& [order_id, active] : state_.active_orders) {
        if (active.intent.exchange == exchange && active.intent.symbol == symbol &&
            (active.status == OrderStatus::pending || active.status == OrderStatus::partially_filled)) {
            order_ids.push_back(order_id);
        }
    }
    for (const auto& order_id : order_ids) {
        const auto cancelled = cancel_order(order_id);
        if (!cancelled.accepted) {
            return {false, "could not cancel working order before square-off: " + cancelled.message, {}};
        }
    }

    const auto position = state_.current_positions.find(position_key(exchange, symbol));
    if (position == state_.current_positions.end() || position->second.quantity == 0) {
        return {true, "no open position", {}};
    }
    if (std::llabs(position->second.quantity) > std::numeric_limits<int>::max()) {
        return {false, "position quantity exceeds broker order quantity range", {}};
    }
    OrderIntent close;
    close.exchange = position->second.exchange;
    close.symbol = position->second.symbol;
    close.side = position->second.quantity > 0 ? Side::sell : Side::buy;
    close.quantity = static_cast<int>(std::llabs(position->second.quantity));
    close.limit_price = position->second.mark_price;
    close.type = OrderType::market;
    if (const auto contract = contracts_.find(symbol); contract != contracts_.end()) {
        close.lot_size = contract->second.lot_size;
    }
    return submit_unchecked(close);
}

OrderResult Trader::square_off_symbol(const std::string& symbol)
{
    std::vector<std::string> exchanges;
    for (const auto& [key, position] : state_.current_positions) {
        (void)key;
        if (position.symbol == symbol) {
            exchanges.push_back(position.exchange);
        }
    }
    for (const auto& [order_id, active] : state_.active_orders) {
        (void)order_id;
        if (active.intent.symbol == symbol) {
            exchanges.push_back(active.intent.exchange);
        }
    }
    std::sort(exchanges.begin(), exchanges.end());
    exchanges.erase(std::unique(exchanges.begin(), exchanges.end()), exchanges.end());
    for (const auto& exchange : exchanges) {
        const auto result = square_off_instrument(exchange, symbol);
        if (!result.accepted) {
            return result;
        }
    }
    return {true, exchanges.empty() ? "no open position" : "square-off orders submitted", {}};
}

OrderResult Trader::square_off_all()
{
    std::vector<std::pair<std::string, std::string>> open_instruments;
    for (const auto& [key, position] : state_.current_positions) {
        (void)key;
        open_instruments.emplace_back(position.exchange, position.symbol);
    }
    std::vector<std::string> order_ids;
    for (const auto& [order_id, active] : state_.active_orders) {
        (void)active;
        order_ids.push_back(order_id);
    }
    for (const auto& order_id : order_ids) {
        const auto active = state_.active_orders.find(order_id);
        if (active == state_.active_orders.end()) {
            continue;
        }
        if (active->second.status == OrderStatus::pending ||
            active->second.status == OrderStatus::partially_filled) {
            const auto cancelled = cancel_order(order_id);
            if (!cancelled.accepted) {
                return {false, "could not cancel working order before square-off: " + cancelled.message, {}};
            }
        }
    }
    std::sort(open_instruments.begin(), open_instruments.end());
    open_instruments.erase(std::unique(open_instruments.begin(), open_instruments.end()), open_instruments.end());
    for (const auto& [exchange, symbol] : open_instruments) {
        const auto result = square_off_instrument(exchange, symbol);
        if (!result.accepted) {
            return result;
        }
    }
    return {true, "square-off orders submitted", {}};
}

const StrategyConfig& Trader::config() const
{
    return config_;
}

const RuntimeState& Trader::state() const
{
    return state_;
}

void Trader::register_contract(const FoContract& contract)
{
    if (contract.symbol.empty() || contract.underlying_symbol.empty() || contract.exchange.empty() ||
        contract.lot_size <= 0 || !valid_positive(contract.strike_price) ||
        !expiry_time_utc(contract.expiry_date)) {
        log(LogLevel::warning, "ignored invalid F&O contract registration");
        return;
    }
    contracts_[contract.symbol] = contract;
}

std::optional<Greeks> Trader::calculate_greeks(
    const FoContract& contract,
    double risk_free_rate,
    std::chrono::system_clock::time_point now) const
{
    const auto expiry = expiry_time_utc(contract.expiry_date);
    if (!expiry || !valid_positive(contract.underlying_price) || !valid_positive(contract.strike_price) ||
        !valid_positive(contract.option_market_price) || !std::isfinite(risk_free_rate)) {
        return std::nullopt;
    }
    const auto time_years = std::chrono::duration<double>(*expiry - now).count() / seconds_per_year;
    if (time_years <= 0.0) {
        return std::nullopt;
    }

    double low_volatility = 1e-4;
    double high_volatility = 5.0;
    const auto low_price = option_price(contract.option_type, contract.underlying_price,
                                        contract.strike_price, time_years, risk_free_rate, low_volatility);
    const auto high_price = option_price(contract.option_type, contract.underlying_price,
                                         contract.strike_price, time_years, risk_free_rate, high_volatility);
    if (contract.option_market_price < low_price || contract.option_market_price > high_price) {
        return std::nullopt;
    }
    for (int iteration = 0; iteration < 100; ++iteration) {
        const auto middle = (low_volatility + high_volatility) / 2.0;
        const auto model_price = option_price(contract.option_type, contract.underlying_price,
                                              contract.strike_price, time_years, risk_free_rate, middle);
        if (model_price < contract.option_market_price) {
            low_volatility = middle;
        } else {
            high_volatility = middle;
        }
    }
    const auto volatility = (low_volatility + high_volatility) / 2.0;
    const auto root_time = std::sqrt(time_years);
    const auto d1 = (std::log(contract.underlying_price / contract.strike_price) +
                     (risk_free_rate + 0.5 * volatility * volatility) * time_years) /
                    (volatility * root_time);
    const auto d2 = d1 - volatility * root_time;
    const auto pdf = normal_pdf(d1);
    const auto discounted_strike = contract.strike_price * std::exp(-risk_free_rate * time_years);

    Greeks greeks;
    greeks.implied_volatility = volatility;
    greeks.gamma = pdf / (contract.underlying_price * volatility * root_time);
    greeks.vega = contract.underlying_price * pdf * root_time * 0.01;
    if (contract.option_type == OptionType::call) {
        greeks.delta = normal_cdf(d1);
        greeks.theta = (-contract.underlying_price * pdf * volatility / (2.0 * root_time) -
                        risk_free_rate * discounted_strike * normal_cdf(d2)) /
                       365.0;
    } else {
        greeks.delta = normal_cdf(d1) - 1.0;
        greeks.theta = (-contract.underlying_price * pdf * volatility / (2.0 * root_time) +
                        risk_free_rate * discounted_strike * normal_cdf(-d2)) /
                       365.0;
    }
    return greeks;
}

double Trader::get_atm_strike(double underlying_price, double strike_interval)
{
    if (!valid_positive(underlying_price) || !valid_positive(strike_interval)) {
        return 0.0;
    }
    return std::round(underlying_price / strike_interval) * strike_interval;
}

double Trader::select_strike(
    double underlying_price,
    double strike_interval,
    OptionType option_type,
    int moneyness_steps)
{
    const auto atm = get_atm_strike(underlying_price, strike_interval);
    if (atm == 0.0) {
        return 0.0;
    }
    const auto direction = option_type == OptionType::call ? -1.0 : 1.0;
    return atm + direction * static_cast<double>(moneyness_steps) * strike_interval;
}

bool Trader::check_expiry_cutoff(
    const FoContract& contract,
    const std::string& exchange_local_date,
    std::chrono::minutes exchange_local_time,
    std::chrono::minutes cutoff)
{
    if (exchange_local_date < contract.expiry_date ||
        (exchange_local_date == contract.expiry_date && exchange_local_time < cutoff)) {
        return false;
    }
    const auto result = square_off_symbol(contract.symbol);
    if (!result.accepted) {
        log(LogLevel::error, "expiry square-off failed: " + result.message);
    }
    return result.accepted;
}

bool Trader::auto_rollover(const FoContract& current_contract, const FoContract& next_contract)
{
    if (current_contract.underlying_symbol != next_contract.underlying_symbol ||
        next_contract.expiry_date <= current_contract.expiry_date ||
        current_contract.exchange != next_contract.exchange) {
        return false;
    }
    const auto key = position_key(current_contract.exchange, current_contract.symbol);
    const auto position = state_.current_positions.find(key);
    if (position == state_.current_positions.end() || position->second.quantity == 0) {
        return true;
    }
    if (current_contract.lot_size <= 0 || next_contract.lot_size <= 0 ||
        std::abs(position->second.quantity) % current_contract.lot_size != 0 ||
        std::abs(position->second.quantity) % next_contract.lot_size != 0) {
        return false;
    }

    OrderIntent close;
    close.exchange = current_contract.exchange;
    close.symbol = current_contract.symbol;
    close.side = position->second.quantity > 0 ? Side::sell : Side::buy;
    close.quantity = std::abs(position->second.quantity);
    close.type = OrderType::market;
    close.limit_price = position->second.mark_price;
    close.lot_size = current_contract.lot_size;

    OrderIntent open;
    open.exchange = next_contract.exchange;
    open.symbol = next_contract.symbol;
    open.side = position->second.quantity > 0 ? Side::buy : Side::sell;
    open.quantity = std::abs(position->second.quantity);
    open.type = OrderType::market;
    open.limit_price = next_contract.option_market_price;
    open.lot_size = next_contract.lot_size;
    register_contract(current_contract);
    register_contract(next_contract);
    return place_multi_leg_order({close, open}).accepted;
}

std::optional<MarginSnapshot> Trader::check_span_margin(const std::vector<OrderIntent>& orders)
{
    if (orders.empty()) {
        return std::nullopt;
    }
    const auto margin = broker_.get_margin(orders);
    if (!margin || !std::isfinite(margin->initial_margin_required) ||
        !std::isfinite(margin->m2m_margin) || !std::isfinite(margin->available_balance) ||
        margin->initial_margin_required < 0.0 || margin->m2m_margin < 0.0 ||
        margin->available_balance < 0.0) {
        return std::nullopt;
    }
    return margin;
}

OrderResult Trader::place_multi_leg_order(const std::vector<OrderIntent>& orders)
{
    if (!state_.is_running || orders.empty()) {
        return {false, "strategy must be running and multi-leg order must not be empty", {}};
    }
    const auto margin = check_span_margin(orders);
    if (!margin) {
        return {false, "broker did not provide valid SPAN margin data", {}};
    }
    if (margin->initial_margin_required + margin->m2m_margin > config_.allocated_capital ||
        margin->initial_margin_required + margin->m2m_margin > margin->available_balance) {
        return {false, "multi-leg SPAN/M2M margin exceeds strategy allocation or available balance", {}};
    }
    std::vector<OrderIntent> preceding_orders;
    preceding_orders.reserve(orders.size());
    for (const auto& order : orders) {
        std::string reason;
        if (!risk_check(order, {}, preceding_orders, &reason)) {
            return {false, "multi-leg preflight rejected: " + reason, {}};
        }
        preceding_orders.push_back(order);
    }

    std::vector<std::string> submitted_ids;
    for (const auto& order : orders) {
        auto result = submit_unchecked(order);
        if (!result.accepted) {
            for (const auto& order_id : submitted_ids) {
                const auto cancel_result = cancel_order(order_id);
                if (!cancel_result.accepted) {
                    log(LogLevel::error, "multi-leg rollback cancel failed: " + cancel_result.message);
                }
            }
            return {false, "multi-leg submission was partial; inspect broker orders: " + result.message, {}};
        }
        submitted_ids.push_back(result.order_id);
    }
    return {true, "multi-leg orders submitted sequentially; execution is not atomic", submitted_ids.front()};
}

double Trader::portfolio_delta(const std::string& underlying_symbol) const
{
    double delta{};
    for (const auto& [key, position] : state_.current_positions) {
        (void)key;
        if (const auto contract = contracts_.find(position.symbol); contract != contracts_.end() &&
            contract->second.underlying_symbol == underlying_symbol) {
            auto greeks = contract->second.greeks;
            if (greeks.implied_volatility <= 0.0) {
                if (const auto calculated = calculate_greeks(contract->second)) {
                    greeks = *calculated;
                }
            }
            delta += position.quantity * greeks.delta;
        } else if (position.symbol == underlying_symbol) {
            delta += position.quantity;
        }
    }
    return delta;
}

double Trader::portfolio_gamma(const std::string& underlying_symbol) const
{
    double gamma{};
    for (const auto& [key, position] : state_.current_positions) {
        (void)key;
        if (const auto contract = contracts_.find(position.symbol); contract != contracts_.end() &&
            contract->second.underlying_symbol == underlying_symbol) {
            auto greeks = contract->second.greeks;
            if (greeks.implied_volatility <= 0.0) {
                if (const auto calculated = calculate_greeks(contract->second)) {
                    greeks = *calculated;
                }
            }
            gamma += position.quantity * greeks.gamma;
        }
    }
    return gamma;
}

OrderResult Trader::delta_neutral_adjustment(
    const std::string& exchange,
    const std::string& underlying_symbol,
    int lot_size,
    double delta_tolerance)
{
    if (lot_size <= 0 || !std::isfinite(delta_tolerance) || delta_tolerance < 0.0) {
        return {false, "invalid hedge lot size or delta tolerance", {}};
    }
    const auto delta = portfolio_delta(underlying_symbol);
    if (std::abs(delta) <= delta_tolerance) {
        return {true, "portfolio delta is within tolerance", {}};
    }
    const auto key = position_key(exchange, underlying_symbol);
    const auto price = latest_prices_.find(key);
    if (price == latest_prices_.end()) {
        return {false, "no current underlying price for delta hedge", {}};
    }
    const auto quantity = static_cast<int>(std::round(-delta / lot_size)) * lot_size;
    if (quantity == 0) {
        return {true, "delta adjustment rounds to zero lots", {}};
    }
    OrderIntent hedge{exchange, underlying_symbol, quantity > 0 ? Side::buy : Side::sell,
                      std::abs(quantity), price->second, OrderType::market, lot_size};
    return place_order(hedge);
}

OrderResult Trader::gamma_scalping(
    const std::string& exchange,
    const std::string& underlying_symbol,
    int lot_size,
    double minimum_underlying_move)
{
    if (!valid_positive(minimum_underlying_move)) {
        return {false, "minimum underlying move must be positive", {}};
    }
    const auto key = position_key(exchange, underlying_symbol);
    const auto price = latest_prices_.find(key);
    if (price == latest_prices_.end()) {
        return {false, "no current underlying price for gamma scalping", {}};
    }
    const auto gamma = portfolio_gamma(underlying_symbol);
    if (std::abs(gamma) < 1e-12) {
        return {true, "portfolio gamma is zero", {}};
    }
    auto& previous_price = last_gamma_hedge_prices_[key];
    if (previous_price == 0.0) {
        previous_price = price->second;
        return {true, "gamma hedge baseline initialized", {}};
    }
    if (std::abs(price->second - previous_price) < minimum_underlying_move) {
        return {true, "underlying move is below gamma hedge threshold", {}};
    }
    auto result = delta_neutral_adjustment(exchange, underlying_symbol, lot_size);
    if (result.accepted) {
        previous_price = price->second;
    }
    return result;
}

} // namespace trader
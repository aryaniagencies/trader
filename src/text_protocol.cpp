#include "trader/text_protocol.hpp"

#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>

#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>

namespace trader {
namespace {

constexpr std::size_t maximum_message_size = 1024 * 1024;

std::string escape(std::string_view value)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' || byte == '.') {
            result.push_back(static_cast<char>(byte));
        } else {
            result.push_back('%');
            result.push_back(digits[byte >> 4]);
            result.push_back(digits[byte & 0x0f]);
        }
    }
    return result;
}

int hex_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    return -1;
}

bool unescape(std::string_view value, std::string& result)
{
    result.clear();
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '%') {
            result.push_back(value[index]);
            continue;
        }
        if (index + 2 >= value.size()) {
            return false;
        }
        const auto high = hex_value(value[index + 1]);
        const auto low = hex_value(value[index + 2]);
        if (high < 0 || low < 0) {
            return false;
        }
        result.push_back(static_cast<char>((high << 4) | low));
        index += 2;
    }
    return true;
}

template <typename Value>
std::string number(Value value)
{
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<Value>::max_digits10) << value;
    return output.str();
}

template <typename Value>
bool parse_integer(const std::string& text, Value& value)
{
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto [position, error] = std::from_chars(begin, end, value);
    return error == std::errc{} && position == end;
}

bool parse_double(const std::string& text, double& value)
{
    std::istringstream input(text);
    input >> value;
    return input && input.peek() == std::char_traits<char>::eof() && std::isfinite(value);
}

bool get(const TextMessage& message, const std::string& key, std::string& value)
{
    const auto found = message.fields.find(key);
    if (found == message.fields.end()) {
        return false;
    }
    value = found->second;
    return true;
}

bool get_number(const TextMessage& message, const std::string& key, double& value)
{
    std::string text;
    return get(message, key, text) && parse_double(text, value);
}

template <typename Value>
bool get_integer(const TextMessage& message, const std::string& key, Value& value)
{
    std::string text;
    return get(message, key, text) && parse_integer(text, value);
}

void add(TextMessage& message, const std::string& key, const std::string& value)
{
    message.fields.emplace(key, value);
}

void add(TextMessage& message, const std::string& key, double value)
{
    add(message, key, number(value));
}

template <typename Value>
void add_integer(TextMessage& message, const std::string& key, Value value)
{
    add(message, key, std::to_string(value));
}

std::string candidate_key(std::size_t index, std::string_view field)
{
    return "candidate." + std::to_string(index) + "." + std::string(field);
}

bool get_candidate_string(
    const TextMessage& message,
    std::size_t index,
    std::string_view field,
    std::string& value)
{
    return get(message, candidate_key(index, field), value);
}

template <typename Value>
bool get_candidate_integer(
    const TextMessage& message,
    std::size_t index,
    std::string_view field,
    Value& value)
{
    std::string text;
    return get_candidate_string(message, index, field, text) && parse_integer(text, value);
}

bool get_candidate_double(
    const TextMessage& message,
    std::size_t index,
    std::string_view field,
    double& value)
{
    std::string text;
    return get_candidate_string(message, index, field, text) && parse_double(text, value);
}

} // namespace

std::string encode_message(const TextMessage& message)
{
    std::string output = "BEGIN " + message.type + "\n";
    for (const auto& [key, value] : message.fields) {
        output += key + "=" + escape(value) + "\n";
    }
    output += "END\n";
    return output;
}

bool read_message(boost::asio::ip::tcp::socket& socket, TextMessage& message)
{
    boost::asio::streambuf buffer(maximum_message_size);
    std::string block;
    bool started = false;
    for (;;) {
        const auto bytes = boost::asio::read_until(socket, buffer, '\n');
        if (block.size() + bytes > maximum_message_size) {
            return false;
        }
        std::istream input(&buffer);
        std::string line;
        std::getline(input, line);
        if (!started) {
            constexpr std::string_view prefix = "BEGIN ";
            if (!line.starts_with(prefix) || line.size() == prefix.size()) {
                return false;
            }
            message = {};
            message.type = line.substr(prefix.size());
            started = true;
            continue;
        }
        if (line == "END") {
            return true;
        }
        const auto separator = line.find('=');
        if (separator == std::string::npos || separator == 0) {
            return false;
        }
        std::string value;
        if (!unescape(std::string_view(line).substr(separator + 1), value) ||
            !message.fields.emplace(line.substr(0, separator), std::move(value)).second) {
            return false;
        }
        block += line;
        block.push_back('\n');
    }
}

void write_message(boost::asio::ip::tcp::socket& socket, const TextMessage& message)
{
    const auto encoded = encode_message(message);
    boost::asio::write(socket, boost::asio::buffer(encoded));
}

TextMessage encode_market_tick(const MarketTick& tick)
{
    TextMessage message{"TICK", {}};
    add(message, "exchange", tick.exchange);
    add(message, "symbol", tick.symbol);
    add(message, "last_price", tick.last_price);
    add_integer(message, "timestamp_ms", tick.timestamp_ms);
    return message;
}

bool decode_market_tick(const TextMessage& message, MarketTick& tick)
{
    return message.type == "TICK" && get(message, "exchange", tick.exchange) &&
           get(message, "symbol", tick.symbol) && get_number(message, "last_price", tick.last_price) &&
           get_integer(message, "timestamp_ms", tick.timestamp_ms) && tick.last_price > 0.0;
}

TextMessage encode_analysis_result(const AnalysisResult& result)
{
    TextMessage message{"ANALYSIS_RESULT", {}};
    add_integer(message, "schema_version", result.schema_version);
    add(message, "strategy_id", result.strategy_id);
    add_integer(message, "sequence", result.sequence);
    add_integer(message, "generated_at_ms", result.generated_at_ms);
    add_integer(message, "candidate_count", result.candidates.size());
    for (std::size_t index = 0; index < result.candidates.size(); ++index) {
        const auto& candidate = result.candidates[index];
        const auto key = [index](std::string_view name) { return candidate_key(index, name); };
        add(message, key("exchange"), candidate.exchange);
        add(message, key("symbol"), candidate.symbol);
        add(message, key("underlying_symbol"), candidate.underlying_symbol);
        add_integer(message, key("instrument_kind"), static_cast<int>(candidate.instrument_kind));
        add_integer(message, key("target_quantity"), candidate.target_quantity);
        add_integer(message, key("order_type"), static_cast<int>(candidate.order_type));
        add(message, key("limit_price"), candidate.limit_price);
        add_integer(message, key("lot_size"), candidate.lot_size);
        add(message, key("score"), candidate.score);
        add(message, key("rationale"), candidate.rationale);
        add_integer(message, key("expires_at_ms"), candidate.expires_at_ms);
        add_integer(message, key("has_option_contract"), candidate.option_contract.has_value() ? 1 : 0);
        if (candidate.option_contract) {
            const auto& contract = *candidate.option_contract;
            add(message, key("contract.symbol"), contract.symbol);
            add(message, key("contract.exchange"), contract.exchange);
            add(message, key("contract.expiry_date"), contract.expiry_date);
            add(message, key("contract.strike_price"), contract.strike_price);
            add_integer(message, key("contract.option_type"), static_cast<int>(contract.option_type));
            add(message, key("contract.underlying_symbol"), contract.underlying_symbol);
            add_integer(message, key("contract.lot_size"), contract.lot_size);
            add(message, key("contract.underlying_price"), contract.underlying_price);
            add(message, key("contract.option_market_price"), contract.option_market_price);
            add(message, key("contract.greeks.delta"), contract.greeks.delta);
            add(message, key("contract.greeks.gamma"), contract.greeks.gamma);
            add(message, key("contract.greeks.theta"), contract.greeks.theta);
            add(message, key("contract.greeks.vega"), contract.greeks.vega);
            add(message, key("contract.greeks.implied_volatility"), contract.greeks.implied_volatility);
            add(message, key("contract.initial_margin_required"), contract.initial_margin_required);
            add(message, key("contract.m2m_margin"), contract.m2m_margin);
        }
    }
    return message;
}

bool decode_analysis_result(const TextMessage& message, AnalysisResult& result)
{
    std::uint32_t schema_version{};
    std::uint64_t sequence{};
    std::int64_t generated_at_ms{};
    std::size_t candidate_count{};
    if (message.type != "ANALYSIS_RESULT" ||
        !get_integer(message, "schema_version", schema_version) ||
        schema_version != AnalysisResult::current_schema_version ||
        !get(message, "strategy_id", result.strategy_id) ||
        !get_integer(message, "sequence", sequence) ||
        !get_integer(message, "generated_at_ms", generated_at_ms) ||
        !get_integer(message, "candidate_count", candidate_count) || candidate_count > 10000) {
        return false;
    }

    result.schema_version = schema_version;
    result.sequence = sequence;
    result.generated_at_ms = generated_at_ms;
    result.candidates.clear();
    result.candidates.reserve(candidate_count);
    for (std::size_t index = 0; index < candidate_count; ++index) {
        AnalysisCandidate candidate;
        int instrument_kind{};
        int order_type{};
        int has_contract{};
        if (!get_candidate_string(message, index, "exchange", candidate.exchange) ||
            !get_candidate_string(message, index, "symbol", candidate.symbol) ||
            !get_candidate_string(message, index, "underlying_symbol", candidate.underlying_symbol) ||
            !get_candidate_integer(message, index, "instrument_kind", instrument_kind) ||
            instrument_kind < static_cast<int>(InstrumentKind::equity) ||
            instrument_kind > static_cast<int>(InstrumentKind::option) ||
            !get_candidate_integer(message, index, "target_quantity", candidate.target_quantity) ||
            !get_candidate_integer(message, index, "order_type", order_type) ||
            order_type < static_cast<int>(OrderType::market) ||
            order_type > static_cast<int>(OrderType::limit) ||
            !get_candidate_double(message, index, "limit_price", candidate.limit_price) ||
            !get_candidate_integer(message, index, "lot_size", candidate.lot_size) ||
            !get_candidate_double(message, index, "score", candidate.score) ||
            !get_candidate_string(message, index, "rationale", candidate.rationale) ||
            !get_candidate_integer(message, index, "expires_at_ms", candidate.expires_at_ms) ||
            !get_candidate_integer(message, index, "has_option_contract", has_contract) ||
            (has_contract != 0 && has_contract != 1)) {
            return false;
        }
        candidate.instrument_kind = static_cast<InstrumentKind>(instrument_kind);
        candidate.order_type = static_cast<OrderType>(order_type);
        if (has_contract != 0) {
            FoContract contract;
            int option_type{};
            if (!get_candidate_string(message, index, "contract.symbol", contract.symbol) ||
                !get_candidate_string(message, index, "contract.exchange", contract.exchange) ||
                !get_candidate_string(message, index, "contract.expiry_date", contract.expiry_date) ||
                !get_candidate_double(message, index, "contract.strike_price", contract.strike_price) ||
                !get_candidate_integer(message, index, "contract.option_type", option_type) ||
                option_type < static_cast<int>(OptionType::call) ||
                option_type > static_cast<int>(OptionType::put) ||
                !get_candidate_string(message, index, "contract.underlying_symbol", contract.underlying_symbol) ||
                !get_candidate_integer(message, index, "contract.lot_size", contract.lot_size) ||
                !get_candidate_double(message, index, "contract.underlying_price", contract.underlying_price) ||
                !get_candidate_double(message, index, "contract.option_market_price", contract.option_market_price) ||
                !get_candidate_double(message, index, "contract.greeks.delta", contract.greeks.delta) ||
                !get_candidate_double(message, index, "contract.greeks.gamma", contract.greeks.gamma) ||
                !get_candidate_double(message, index, "contract.greeks.theta", contract.greeks.theta) ||
                !get_candidate_double(message, index, "contract.greeks.vega", contract.greeks.vega) ||
                !get_candidate_double(message, index, "contract.greeks.implied_volatility",
                                      contract.greeks.implied_volatility) ||
                !get_candidate_double(message, index, "contract.initial_margin_required",
                                      contract.initial_margin_required) ||
                !get_candidate_double(message, index, "contract.m2m_margin", contract.m2m_margin)) {
                return false;
            }
            contract.option_type = static_cast<OptionType>(option_type);
            candidate.option_contract = std::move(contract);
        }
        result.candidates.push_back(std::move(candidate));
    }
    return true;
}

TextMessage encode_worker_init(
    const StrategyConfig& config,
    const std::unordered_map<std::string, std::vector<Bar>>& historical_data)
{
    TextMessage message{"INIT", {}};
    add(message, "strategy_id", config.strategy_id);
    add(message, "allocated_capital", config.allocated_capital);
    add(message, "max_drawdown", config.risk_limits.max_drawdown);
    add(message, "stop_loss_limit", config.risk_limits.stop_loss_limit);
    add_integer(message, "max_position_size", config.risk_limits.max_position_size);
    add_integer(message, "analysis_candidate_ttl_ms", config.analysis_candidate_ttl_ms);
    add_integer(message, "live_orders_enabled", config.live_orders_enabled ? 1 : 0);
    add_integer(message, "symbol_count", config.symbols.size());
    for (std::size_t index = 0; index < config.symbols.size(); ++index) {
        add(message, "symbol." + std::to_string(index), config.symbols[index]);
    }
    std::size_t bar_count = 0;
    for (const auto& [symbol, bars] : historical_data) {
        (void)symbol;
        bar_count += bars.size();
    }
    add_integer(message, "bar_count", bar_count);
    std::size_t index = 0;
    for (const auto& [symbol, bars] : historical_data) {
        (void)symbol;
        for (const auto& bar : bars) {
            const auto key = [index](std::string_view field) {
                return "bar." + std::to_string(index) + "." + std::string(field);
            };
            add(message, key("exchange"), bar.exchange);
            add(message, key("symbol"), bar.symbol);
            add_integer(message, key("timestamp_ms"), bar.timestamp_ms);
            add(message, key("open"), bar.open);
            add(message, key("high"), bar.high);
            add(message, key("low"), bar.low);
            add(message, key("close"), bar.close);
            add_integer(message, key("volume"), bar.volume);
            ++index;
        }
    }
    return message;
}

bool decode_worker_init(
    const TextMessage& message,
    StrategyConfig& config,
    std::unordered_map<std::string, std::vector<Bar>>& historical_data)
{
    std::size_t symbol_count{};
    std::size_t bar_count{};
    int live_orders{};
    if (message.type != "INIT" || !get(message, "strategy_id", config.strategy_id) ||
        !get_number(message, "allocated_capital", config.allocated_capital) ||
        !get_number(message, "max_drawdown", config.risk_limits.max_drawdown) ||
        !get_number(message, "stop_loss_limit", config.risk_limits.stop_loss_limit) ||
        !get_integer(message, "max_position_size", config.risk_limits.max_position_size) ||
        !get_integer(message, "analysis_candidate_ttl_ms", config.analysis_candidate_ttl_ms) ||
        !get_integer(message, "live_orders_enabled", live_orders) ||
        (live_orders != 0 && live_orders != 1) ||
        !get_integer(message, "symbol_count", symbol_count) || symbol_count == 0 ||
        symbol_count > 10000 || !get_integer(message, "bar_count", bar_count) || bar_count > 100000) {
        return false;
    }
    config.live_orders_enabled = live_orders != 0;
    config.symbols.clear();
    config.symbols.reserve(symbol_count);
    historical_data.clear();
    for (std::size_t index = 0; index < symbol_count; ++index) {
        std::string symbol;
        if (!get(message, "symbol." + std::to_string(index), symbol) || symbol.empty()) {
            return false;
        }
        config.symbols.push_back(symbol);
        historical_data.try_emplace(std::move(symbol));
    }
    for (std::size_t index = 0; index < bar_count; ++index) {
        const auto key = [index](std::string_view field) {
            return "bar." + std::to_string(index) + "." + std::string(field);
        };
        Bar bar;
        if (!get(message, key("exchange"), bar.exchange) ||
            !get(message, key("symbol"), bar.symbol) ||
            !get_integer(message, key("timestamp_ms"), bar.timestamp_ms) ||
            !get_number(message, key("open"), bar.open) ||
            !get_number(message, key("high"), bar.high) ||
            !get_number(message, key("low"), bar.low) ||
            !get_number(message, key("close"), bar.close) ||
            !get_integer(message, key("volume"), bar.volume) || bar.symbol.empty()) {
            return false;
        }
        historical_data[bar.symbol].push_back(std::move(bar));
    }
    return true;
}

} // namespace trader

#include "trader/kite_broker.hpp"

#include "trader/log.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace trader {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using Json = boost::property_tree::ptree;

constexpr char api_host[] = "api.kite.trade";

std::string environment(const char* name, std::string fallback = {})
{
    const auto* value = std::getenv(name);
    return value == nullptr ? std::move(fallback) : std::string{value};
}

std::string percent_encode(std::string_view value)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string encoded;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
            byte == '.' || byte == '~') {
            encoded.push_back(static_cast<char>(byte));
        } else if (byte == ' ') {
            encoded.push_back('+');
        } else {
            encoded.push_back('%');
            encoded.push_back(digits[byte >> 4]);
            encoded.push_back(digits[byte & 0x0f]);
        }
    }
    return encoded;
}

std::string json_escape(std::string_view value)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string escaped;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (byte) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (byte < 0x20) {
                escaped += "\\u00";
                escaped.push_back(digits[byte >> 4]);
                escaped.push_back(digits[byte & 0x0f]);
            } else {
                escaped.push_back(static_cast<char>(byte));
            }
        }
    }
    return escaped;
}

std::vector<std::string> csv_row(const std::string& line)
{
    std::vector<std::string> columns;
    std::string field;
    bool quoted = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const auto character = line[index];
        if (character == '"') {
            if (quoted && index + 1 < line.size() && line[index + 1] == '"') {
                field.push_back('"');
                ++index;
            } else {
                quoted = !quoted;
            }
        } else if (character == ',' && !quoted) {
            columns.push_back(std::move(field));
            field.clear();
        } else {
            field.push_back(character);
        }
    }
    columns.push_back(std::move(field));
    return columns;
}

Json parse_response(const std::string& response)
{
    std::istringstream input(response);
    Json root;
    boost::property_tree::read_json(input, root);
    const auto status = root.get<std::string>("status", "error");
    if (status != "success") {
        const auto message = root.get<std::string>("message", "unknown Kite Connect API error");
        const auto error_type = root.get<std::string>("error_type", "ApiError");
        throw std::runtime_error("Kite Connect " + error_type + ": " + message);
    }
    return root;
}

std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string exchange_time(std::chrono::system_clock::time_point time)
{
    const auto seconds = std::chrono::system_clock::to_time_t(time) + 5 * 60 * 60 + 30 * 60;
    std::tm local{};
    gmtime_r(&seconds, &local);
    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return output.str();
}

std::int64_t parse_candle_timestamp(const std::string& value)
{
    std::tm timestamp{};
    std::istringstream input(value.substr(0, 19));
    input >> std::get_time(&timestamp, "%Y-%m-%dT%H:%M:%S");
    if (input.fail()) {
        throw std::runtime_error("Kite Connect returned an invalid candle timestamp");
    }
    const auto utc_seconds = timegm(&timestamp) - 5 * 60 * 60 - 30 * 60;
    return static_cast<std::int64_t>(utc_seconds) * 1000;
}

std::string instrument_key(const std::string& exchange, const std::string& symbol)
{
    return exchange + ":" + symbol;
}

std::string kite_order_status(const std::string& status, int filled, int quantity)
{
    if (status == "COMPLETE" || (quantity > 0 && filled >= quantity)) {
        return "filled";
    }
    if (status == "CANCELLED") {
        return "cancelled";
    }
    if (status == "REJECTED") {
        return "rejected";
    }
    return filled > 0 ? "partially_filled" : "pending";
}

OrderStatus parse_order_status(const std::string& status, int filled, int quantity)
{
    if (status == "filled") {
        return OrderStatus::filled;
    }
    if (status == "cancelled") {
        return OrderStatus::cancelled;
    }
    if (status == "rejected") {
        return OrderStatus::rejected;
    }
    if (status == "partially_filled") {
        return OrderStatus::partially_filled;
    }
    (void)filled;
    (void)quantity;
    return OrderStatus::pending;
}

std::string order_type_name(OrderType type)
{
    return type == OrderType::market ? "MARKET" : "LIMIT";
}

} // namespace

KiteBroker::KiteBroker(
    std::vector<std::string> symbols,
    std::string default_exchange,
    std::string product,
    std::string history_interval,
    int history_days)
    : requested_symbols_(std::move(symbols)),
      default_exchange_(std::move(default_exchange)),
      product_(std::move(product)),
      history_interval_(std::move(history_interval)),
      history_days_(history_days)
{
    api_key_ = environment("TRADER_KITE_API_KEY");
    access_token_ = environment("TRADER_KITE_ACCESS_TOKEN");
}

std::string KiteBroker::resolve_exchange(const std::string& symbol) const
{
    const auto separator = symbol.find(':');
    return separator == std::string::npos ? default_exchange_ : symbol.substr(0, separator);
}

std::string KiteBroker::resolve_symbol(const std::string& symbol) const
{
    const auto separator = symbol.find(':');
    return separator == std::string::npos ? symbol : symbol.substr(separator + 1);
}

std::string KiteBroker::request(
    const std::string& method,
    const std::string& target,
    const std::string& body,
    const std::string& content_type) const
{
    asio::io_context context;
    ssl::context tls(ssl::context::tls_client);
    tls.set_default_verify_paths();
    tls.set_verify_mode(ssl::verify_peer);

    tcp::resolver resolver(context);
    beast::ssl_stream<beast::tcp_stream> stream(context, tls);
    if (!SSL_set_tlsext_host_name(stream.native_handle(), api_host)) {
        throw std::runtime_error("could not configure TLS server name for Kite Connect");
    }
    stream.set_verify_callback(ssl::host_name_verification(api_host));
    const auto endpoints = resolver.resolve(api_host, "443");
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(15));
    beast::get_lowest_layer(stream).connect(endpoints);
    stream.handshake(ssl::stream_base::client);

    http::request<http::string_body> request{
        method == "GET" ? http::verb::get :
        method == "POST" ? http::verb::post :
        method == "PUT" ? http::verb::put :
        method == "DELETE" ? http::verb::delete_ : http::verb::get,
        target,
        11};
    request.set(http::field::host, api_host);
    request.set(http::field::user_agent, "trader-cpp/1.0");
    request.set(http::field::accept_encoding, "identity");
    request.keep_alive(false);
    request.set("X-Kite-Version", "3");
    request.set(http::field::authorization, "token " + api_key_ + ":" + access_token_);
    if (!body.empty()) {
        request.set(http::field::content_type, content_type);
        request.body() = body;
        request.prepare_payload();
    }

    http::write(stream, request);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(15));
    http::read(stream, buffer, response);

    beast::error_code shutdown_error;
    stream.shutdown(shutdown_error);
    if (response.result() < http::status::ok || response.result() >= http::status::multiple_choices) {
        try {
            const auto root = parse_response(response.body());
            (void)root;
        } catch (const std::exception& error) {
            throw std::runtime_error(
                "Kite Connect HTTP " + std::to_string(response.result_int()) + ": " + error.what());
        }
        throw std::runtime_error("Kite Connect HTTP request failed with status " +
                                 std::to_string(response.result_int()));
    }
    return response.body();
}

bool KiteBroker::connect()
{
    std::lock_guard lock(api_mutex_);
    connected_ = false;
    if (api_key_.empty() || access_token_.empty()) {
        log(LogLevel::error, "TRADER_KITE_API_KEY and TRADER_KITE_ACCESS_TOKEN are required for Zerodha");
        return false;
    }
    const std::set<std::string> intervals{
        "minute", "3minute", "5minute", "10minute", "15minute", "30minute", "60minute", "day"};
    if (requested_symbols_.empty() || history_days_ <= 0 || default_exchange_.empty() ||
        !intervals.contains(history_interval_) ||
        (product_ != "CNC" && product_ != "MIS" && product_ != "NRML")) {
        log(LogLevel::error, "invalid Zerodha symbol, history, exchange, or product configuration");
        return false;
    }

    try {
        std::set<std::string> exchanges;
        for (const auto& requested : requested_symbols_) {
            if (resolve_exchange(requested).empty() || resolve_symbol(requested).empty()) {
                throw std::runtime_error("invalid configured Zerodha instrument: " + requested);
            }
            exchanges.insert(resolve_exchange(requested));
        }

        std::unordered_map<std::string, Instrument> available;
        for (const auto& exchange : exchanges) {
            std::istringstream csv(request("GET", "/instruments/" + percent_encode(exchange)));
            std::string line;
            if (!std::getline(csv, line)) {
                throw std::runtime_error("Kite Connect returned an empty instrument list");
            }
            while (std::getline(csv, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                const auto columns = csv_row(line);
                if (columns.size() < 12) {
                    continue;
                }
                std::uint64_t token{};
                try {
                    token = std::stoull(columns[0]);
                } catch (const std::exception&) {
                    continue;
                }
                const auto key = columns[11] + ":" + columns[2];
                available.emplace(key, Instrument{columns[11], columns[2], token});
            }
        }

        instruments_.clear();
        instrument_by_key_.clear();
        for (const auto& requested : requested_symbols_) {
            const auto key = resolve_exchange(requested) + ":" + resolve_symbol(requested);
            const auto found = available.find(key);
            if (found == available.end()) {
                throw std::runtime_error("instrument not found in Kite instrument master: " + key);
            }
            instruments_.push_back(found->second);
            instrument_by_key_.emplace(key, found->second);
        }
        connected_ = true;
        log(LogLevel::info, "connected to Zerodha Kite Connect for " +
                                std::to_string(instruments_.size()) + " configured instruments");
        return true;
    } catch (const std::exception& error) {
        log(LogLevel::error, std::string("Zerodha connection failed: ") + error.what());
        return false;
    }
}

std::vector<Bar> KiteBroker::fetch_historical_data(const std::string& symbol)
{
    std::lock_guard lock(api_mutex_);
    if (!connected_) {
        throw std::runtime_error("Zerodha broker is not connected");
    }
    const auto instrument = instrument_by_key_.find(
        instrument_key(resolve_exchange(symbol), resolve_symbol(symbol)));
    auto selected = instrument;
    if (selected == instrument_by_key_.end()) {
        for (auto candidate = instrument_by_key_.begin(); candidate != instrument_by_key_.end(); ++candidate) {
            if (candidate->second.symbol == resolve_symbol(symbol)) {
                if (selected != instrument_by_key_.end()) {
                    throw std::runtime_error("historical-data symbol is ambiguous across exchanges: " + symbol);
                }
                selected = candidate;
            }
        }
    }
    if (selected == instrument_by_key_.end()) {
        throw std::runtime_error("historical-data symbol is not configured: " + symbol);
    }
    const auto now = std::chrono::system_clock::now();
    const auto from = exchange_time(now - std::chrono::hours(24 * history_days_));
    const auto to = exchange_time(now);
    const auto target = "/instruments/historical/" + std::to_string(selected->second.instrument_token) +
                        "/" + percent_encode(history_interval_) + "?from=" + percent_encode(from) +
                        "&to=" + percent_encode(to);
    const auto root = parse_response(request("GET", target));
    const auto& candles = root.get_child("data.candles");
    std::vector<Bar> bars;
    for (const auto& item : candles) {
        const auto& candle = item.second;
        if (candle.size() < 6) {
            throw std::runtime_error("Kite Connect returned a malformed historical candle");
        }
        auto value = candle.begin();
        Bar bar;
        bar.exchange = selected->second.exchange;
        bar.symbol = selected->second.symbol;
        bar.timestamp_ms = parse_candle_timestamp(value++->second.get_value<std::string>());
        bar.open = value++->second.get_value<double>();
        bar.high = value++->second.get_value<double>();
        bar.low = value++->second.get_value<double>();
        bar.close = value++->second.get_value<double>();
        bar.volume = value->second.get_value<std::uint64_t>();
        bars.push_back(std::move(bar));
    }
    return bars;
}

std::vector<MarketTick> KiteBroker::poll_market_data()
{
    std::lock_guard lock(api_mutex_);
    if (!connected_) {
        throw std::runtime_error("Zerodha broker is not connected");
    }
    std::vector<MarketTick> ticks;
    for (std::size_t start = 0; start < instruments_.size(); start += 250) {
        const auto end = std::min(start + 250, instruments_.size());
        std::string target = "/quote/ltp?";
        for (auto index = start; index < end; ++index) {
            if (index != start) {
                target.push_back('&');
            }
            target += "i=" + percent_encode(instruments_[index].exchange + ":" +
                                            instruments_[index].symbol);
        }
        const auto root = parse_response(request("GET", target));
        const auto& data = root.get_child("data");
        const auto timestamp = now_ms();
        for (auto index = start; index < end; ++index) {
            const auto key = instrument_key(instruments_[index].exchange, instruments_[index].symbol);
            const auto quote = data.get_child_optional(key);
            if (!quote) {
                log(LogLevel::warning, "Kite Connect returned no quote for " + key);
                continue;
            }
            const auto price = quote->get<double>("last_price", 0.0);
            if (!std::isfinite(price) || price <= 0.0) {
                log(LogLevel::warning, "Kite Connect returned an invalid quote for " + key);
                continue;
            }
            ticks.push_back({instruments_[index].exchange, instruments_[index].symbol, price, timestamp});
        }
    }
    return ticks;
}

std::vector<OrderUpdate> KiteBroker::poll_order_updates()
{
    std::lock_guard lock(api_mutex_);
    if (!connected_) {
        throw std::runtime_error("Zerodha broker is not connected");
    }
    const auto root = parse_response(request("GET", "/orders"));
    const auto& orders = root.get_child("data");
    std::vector<OrderUpdate> updates;
    for (const auto& item : orders) {
        const auto& order = item.second;
        const auto order_id = order.get<std::string>("order_id", "");
        if (order_id.empty()) {
            continue;
        }
        const auto found = last_order_updates_.find(order_id);
        if (found == last_order_updates_.end()) {
            continue;
        }
        const auto filled = order.get<int>("filled_quantity", 0);
        const auto quantity = order.get<int>("quantity", 0);
        const auto state = kite_order_status(order.get<std::string>("status", ""), filled, quantity);
        OrderUpdate update{
            order_id,
            parse_order_status(state, filled, quantity),
            filled,
            order.get<double>("average_price", 0.0),
            order.get<std::string>("status_message", "")};
        const auto& previous = found->second;
        if (update.status != previous.status ||
            update.cumulative_filled_quantity != previous.cumulative_filled_quantity ||
            update.average_fill_price != previous.average_fill_price) {
            updates.push_back(update);
            found->second = update;
        }
    }
    return updates;
}

std::string KiteBroker::order_form(const OrderIntent& order) const
{
    std::ostringstream form;
    form.imbue(std::locale::classic());
    form << "exchange=" << percent_encode(order.exchange)
         << "&tradingsymbol=" << percent_encode(order.symbol)
         << "&transaction_type=" << (order.side == Side::buy ? "BUY" : "SELL")
         << "&quantity=" << order.quantity
         << "&product=" << percent_encode(product_)
         << "&order_type=" << order_type_name(order.type)
         << "&validity=DAY";
    if (order.type == OrderType::limit) {
        form << "&price=" << std::setprecision(12) << order.limit_price;
    }
    return form.str();
}

OrderResult KiteBroker::submit_order(const OrderIntent& order)
{
    std::lock_guard lock(api_mutex_);
    if (!connected_) {
        return {false, "Zerodha broker is not connected", {}};
    }
    try {
        const auto root = parse_response(
            request("POST", "/orders/regular", order_form(order)));
        const auto order_id = root.get<std::string>("data.order_id", "");
        if (order_id.empty()) {
            return {false, "Kite Connect accepted no order ID", {}};
        }
        last_order_updates_[order_id] = {order_id, OrderStatus::pending, 0, 0.0, {}};
        return {true, "accepted by Zerodha Kite Connect", order_id};
    } catch (const std::exception& error) {
        log(LogLevel::error, std::string("Zerodha order placement failed: ") + error.what());
        return {false, error.what(), {}};
    }
}

OrderResult KiteBroker::cancel_order(const std::string& order_id)
{
    std::lock_guard lock(api_mutex_);
    if (!connected_) {
        return {false, "Zerodha broker is not connected", {}};
    }
    try {
        const auto root = parse_response(request("DELETE", "/orders/regular/" + percent_encode(order_id)));
        const auto returned_id = root.get<std::string>("data.order_id", order_id);
        return {true, "cancel request accepted by Zerodha", returned_id};
    } catch (const std::exception& error) {
        log(LogLevel::error, std::string("Zerodha order cancellation failed: ") + error.what());
        return {false, error.what(), {}};
    }
}

OrderResult KiteBroker::modify_order(const std::string& order_id, const OrderIntent& order)
{
    std::lock_guard lock(api_mutex_);
    if (!connected_) {
        return {false, "Zerodha broker is not connected", {}};
    }
    try {
        const auto root = parse_response(
            request("PUT", "/orders/regular/" + percent_encode(order_id), order_form(order)));
        const auto returned_id = root.get<std::string>("data.order_id", order_id);
        return {true, "modify request accepted by Zerodha", returned_id};
    } catch (const std::exception& error) {
        log(LogLevel::error, std::string("Zerodha order modification failed: ") + error.what());
        return {false, error.what(), {}};
    }
}

std::optional<MarginSnapshot> KiteBroker::get_margin(const std::vector<OrderIntent>& orders)
{
    std::lock_guard lock(api_mutex_);
    if (!connected_ || orders.empty()) {
        return std::nullopt;
    }
    try {
        std::ostringstream body;
        body.imbue(std::locale::classic());
        body << std::setprecision(17) << '[';
        for (std::size_t index = 0; index < orders.size(); ++index) {
            const auto& order = orders[index];
            if (order.exchange.empty() || order.symbol.empty() || order.quantity <= 0 ||
                (order.type == OrderType::limit &&
                 (!std::isfinite(order.limit_price) || order.limit_price <= 0.0))) {
                return std::nullopt;
            }
            if (index != 0) {
                body << ',';
            }
            body << "{\"exchange\":\"" << json_escape(order.exchange)
                 << "\",\"tradingsymbol\":\"" << json_escape(order.symbol)
                 << "\",\"transaction_type\":\"" << (order.side == Side::buy ? "BUY" : "SELL")
                 << "\",\"variety\":\"regular\",\"product\":\"" << json_escape(product_)
                 << "\",\"order_type\":\"" << order_type_name(order.type)
                 << "\",\"quantity\":" << order.quantity
                 << ",\"price\":" << (order.type == OrderType::limit ? order.limit_price : 0.0)
                 << ",\"trigger_price\":0}";
        }
        body << ']';
        const auto margin_root = parse_response(
            request("POST", "/margins/orders", body.str(), "application/json"));
        double required{};
        std::size_t margin_count{};
        for (const auto& item : margin_root.get_child("data")) {
            required += item.second.get<double>("total", std::numeric_limits<double>::quiet_NaN());
            ++margin_count;
        }
        if (!std::isfinite(required) || margin_count != orders.size()) {
            return std::nullopt;
        }

            const bool uses_commodity = std::any_of(orders.begin(), orders.end(), [](const auto& order) {
            return order.exchange == "MCX";
            });
            const bool uses_equity = std::any_of(orders.begin(), orders.end(), [](const auto& order) {
                return order.exchange != "MCX";
            });
            double available{};
            for (const auto* segment : {"equity", "commodity"}) {
                if ((std::string_view{segment} == "equity" && !uses_equity) ||
                    (std::string_view{segment} == "commodity" && !uses_commodity)) {
                    continue;
                }
                const auto funds_root = parse_response(
                    request("GET", "/user/margins/" + std::string(segment)));
                const auto segment_available = funds_root.get<double>("data.available.live_balance", -1.0);
                if (!std::isfinite(segment_available) || segment_available < 0.0) {
                    return std::nullopt;
                }
                available += segment_available;
            }
        return MarginSnapshot{required, 0.0, available};
    } catch (const std::exception& error) {
        log(LogLevel::error, std::string("Zerodha margin query failed: ") + error.what());
        return std::nullopt;
    }
}

const char* KiteBroker::name() const
{
    return "zerodha";
}

} // namespace trader

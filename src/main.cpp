#include "trader/log.hpp"
#include "trader/kite_broker.hpp"
#include "trader/paper_broker.hpp"
#include "trader/scheduler.hpp"
#include "trader/trader.hpp"
#include "trader/websocket_server.hpp"

#include <csignal>
#include <charconv>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

volatile std::sig_atomic_t keep_running = 1;

void request_stop(int)
{
    keep_running = 0;
}

trader::StrategyConfig load_config()
{
    trader::StrategyConfig config;
    const auto* strategy_id = std::getenv("TRADER_STRATEGY_ID");
    config.strategy_id = strategy_id == nullptr ? "paper-demo" : strategy_id;

    const auto* symbols = std::getenv("TRADER_SYMBOLS");
    std::string symbol_list = symbols == nullptr ? "RELIANCE,TCS" : symbols;
    for (std::size_t start = 0; start < symbol_list.size();) {
        const auto separator = symbol_list.find(',', start);
        auto symbol = symbol_list.substr(start, separator - start);
        if (!symbol.empty()) {
            const auto exchange_separator = symbol.find(':');
            config.symbols.push_back(exchange_separator == std::string::npos
                                         ? std::move(symbol)
                                         : symbol.substr(exchange_separator + 1));
        }
        if (separator == std::string::npos) {
            break;
        }
        start = separator + 1;
    }

    const auto* capital = std::getenv("TRADER_ALLOCATED_CAPITAL");
    config.allocated_capital = capital == nullptr ? 100000.0 : std::strtod(capital, nullptr);
    const auto* drawdown = std::getenv("TRADER_MAX_DRAWDOWN");
    config.risk_limits.max_drawdown = drawdown == nullptr ? 0.0 : std::strtod(drawdown, nullptr);
    const auto* stop_loss = std::getenv("TRADER_STOP_LOSS_LIMIT");
    config.risk_limits.stop_loss_limit = stop_loss == nullptr ? 0.0 : std::strtod(stop_loss, nullptr);
    const auto* max_position = std::getenv("TRADER_MAX_POSITION_SIZE");
    config.risk_limits.max_position_size = max_position == nullptr ? 0 : std::atoi(max_position);
    const auto* live_orders = std::getenv("TRADER_LIVE_ORDERS");
    config.live_orders_enabled = live_orders != nullptr && std::string{live_orders} == "true";
    return config;
}

std::vector<std::string> split_symbols(const std::string& symbols)
{
    std::vector<std::string> result;
    for (std::size_t start = 0; start < symbols.size();) {
        const auto separator = symbols.find(',', start);
        auto symbol = symbols.substr(start, separator - start);
        if (!symbol.empty()) {
            result.push_back(std::move(symbol));
        }
        if (separator == std::string::npos) {
            break;
        }
        start = separator + 1;
    }
    return result;
}

bool parse_positive_int(const std::string& text, int& value)
{
    int parsed{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size() || parsed <= 0) {
        return false;
    }
    value = parsed;
    return true;
}

bool parse_port(const char* value, std::uint16_t& port)
{
    unsigned int parsed{};
    const std::string text{value};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size() || parsed == 0 ||
        parsed > 65535) {
        return false;
    }
    port = static_cast<std::uint16_t>(parsed);
    return true;
}

bool parse_worker_count(const char* value, std::size_t& count)
{
    unsigned int parsed{};
    const std::string text{value};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size() || parsed == 0) {
        return false;
    }
    count = parsed;
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    std::string role = argc > 1 ? argv[1] : "scheduler";
    if (role == "--role" && argc > 2) {
        role = argv[2];
        argv += 1;
        argc -= 1;
    }

    if (role == "analyzer") {
        std::uint16_t port = 8766;
        const char* host = "127.0.0.1";
        if (argc > 2) {
            host = argv[2];
        }
        if (argc > 3 && !parse_port(argv[3], port)) {
            trader::log(trader::LogLevel::error, "invalid analyzer worker port");
            return 2;
        }
        return trader::run_analyzer_worker(host, port) ? 0 : 1;
    }
    if (role != "scheduler") {
        trader::log(trader::LogLevel::error, "usage: trader [scheduler [port] | analyzer [host [port]]]");
        return 2;
    }

    const auto config = load_config();
    const auto* configured_symbols = std::getenv("TRADER_SYMBOLS");
    const auto instrument_specs = split_symbols(
        configured_symbols == nullptr ? "RELIANCE,TCS" : configured_symbols);
    const auto broker_name = std::getenv("TRADER_BROKER");
    std::unique_ptr<trader::Broker> broker;
    if (broker_name == nullptr || std::string{broker_name} == "paper") {
        broker = std::make_unique<trader::PaperBroker>();
    } else if (std::string{broker_name} == "zerodha") {
        int history_days = 30;
        const auto history_days_text = std::getenv("TRADER_KITE_HISTORY_DAYS");
        if (history_days_text != nullptr &&
            !parse_positive_int(history_days_text, history_days)) {
            trader::log(trader::LogLevel::error, "invalid TRADER_KITE_HISTORY_DAYS");
            return 2;
        }
        broker = std::make_unique<trader::KiteBroker>(
            instrument_specs,
            std::getenv("TRADER_KITE_DEFAULT_EXCHANGE") == nullptr
                ? "NSE"
                : std::getenv("TRADER_KITE_DEFAULT_EXCHANGE"),
            std::getenv("TRADER_KITE_PRODUCT") == nullptr
                ? "CNC"
                : std::getenv("TRADER_KITE_PRODUCT"),
            std::getenv("TRADER_KITE_HISTORY_INTERVAL") == nullptr
                ? "day"
                : std::getenv("TRADER_KITE_HISTORY_INTERVAL"),
            history_days);
    } else {
        trader::log(trader::LogLevel::error, "TRADER_BROKER must be 'paper' or 'zerodha'");
        return 2;
    }
    trader::Trader algo(config, *broker);
    std::uint16_t worker_port = 8766;
    std::size_t worker_count = 1;
    if (argc > 2 && !parse_port(argv[2], worker_port)) {
        trader::log(trader::LogLevel::error, "invalid scheduler worker port");
        return 2;
    }
    if (argc > 3 && !parse_worker_count(argv[3], worker_count)) {
        trader::log(trader::LogLevel::error, "invalid analyzer worker count");
        return 2;
    }
    if (worker_count > config.symbols.size()) {
        trader::log(trader::LogLevel::error, "worker count cannot exceed configured symbol count");
        return 2;
    }

    trader::WebSocketServer websocket_server(8765);
    std::thread([&websocket_server] { websocket_server.run(); }).detach();
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);

    trader::Scheduler scheduler(*broker, algo, websocket_server, worker_port, worker_count);
    try {
        if (!scheduler.run([] { return keep_running != 0; })) {
            return 1;
        }
    } catch (const std::exception& error) {
        trader::log(trader::LogLevel::error, std::string("scheduler initialization failed: ") + error.what());
        algo.stop();
        return 1;
    }
    return 0;
}
#include "trader/analyzer.hpp"
#include "trader/text_protocol.hpp"
#include "trader/trader.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

class TestBroker final : public trader::Broker {
public:
    bool connect() override { return connect_ok; }
    std::vector<trader::Bar> fetch_historical_data(const std::string&) override { return history; }
    std::vector<trader::MarketTick> poll_market_data() override { return {}; }

    trader::OrderResult submit_order(const trader::OrderIntent& order) override
    {
        submitted.push_back(order);
        return {true, "accepted", "test-" + std::to_string(submitted.size())};
    }

    trader::OrderResult cancel_order(const std::string& order_id) override
    {
        cancelled.push_back(order_id);
        return {true, "cancelled", order_id};
    }

    std::optional<trader::MarginSnapshot> get_margin(const std::vector<trader::OrderIntent>&) override
    {
        return margin;
    }

    const char* name() const override { return "test"; }

    bool connect_ok{true};
    std::vector<trader::Bar> history;
    std::optional<trader::MarginSnapshot> margin;
    std::vector<trader::OrderIntent> submitted;
    std::vector<std::string> cancelled;
};

trader::StrategyConfig config()
{
    trader::StrategyConfig value;
    value.strategy_id = "unit-test";
    value.symbols = {"NIFTY", "NIFTY-OPT"};
    value.allocated_capital = 1'000'000.0;
    value.live_orders_enabled = true;
    value.risk_limits.max_drawdown = 5'000.0;
    value.risk_limits.stop_loss_limit = 1'000.0;
    value.risk_limits.max_position_size = 100;
    return value;
}

class FixedCandidateLogic final : public trader::BusinessLogic {
public:
    std::vector<trader::AnalysisCandidate> evaluate_signals(const trader::MarketTick& tick) override
    {
        trader::AnalysisCandidate candidate;
        candidate.exchange = tick.exchange;
        candidate.symbol = target_symbol.empty() ? tick.symbol : target_symbol;
        candidate.underlying_symbol = candidate.symbol;
        candidate.target_quantity = target_quantity;
        candidate.order_type = trader::OrderType::limit;
        candidate.limit_price = tick.last_price;
        candidate.score = 0.8;
        candidate.rationale = "unit test candidate";
        return {candidate};
    }

    std::string target_symbol;
    std::int64_t target_quantity{5};
};

void test_lifecycle_and_order_risk()
{
    TestBroker broker;
    trader::Trader algo(config(), broker);
    assert(algo.initialize());
    assert(algo.start());
    assert(!algo.start());

    trader::OrderIntent invalid{"NSE", "NIFTY", trader::Side::buy, 3, 100.0,
                                trader::OrderType::limit, 25};
    assert(!algo.risk_check(invalid));

    trader::OrderIntent valid{"NSE", "NIFTY", trader::Side::buy, 25, 100.0,
                              trader::OrderType::limit, 25};
    assert(algo.risk_check(valid));
    assert(algo.place_order(valid).accepted);
    assert(algo.state().active_orders.size() == 1);
    algo.stop();
    assert(!algo.state().is_running);
    assert(broker.cancelled.size() == 1);
}

void test_analyzer_to_trader_handoff()
{
    auto settings = config();
    settings.symbols = {"NIFTY"};
    FixedCandidateLogic logic;
    trader::Analyzer analyzer(settings, logic);
    TestBroker broker;
    broker.history.push_back({"NSE", "NIFTY", 1, 100.0, 101.0, 99.0, 100.5, 1000});
    trader::Trader algo(settings, broker);
    assert(algo.initialize() && algo.start());
    assert(analyzer.initialize(broker));
    assert(analyzer.historical_data().at("NIFTY").size() == 1);

    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const trader::MarketTick tick{"NSE", "NIFTY", 100.0, now_ms};
    const auto result = analyzer.on_tick(tick);
    assert(result.schema_version == trader::AnalysisResult::current_schema_version);
    assert(result.strategy_id == settings.strategy_id);
    assert(result.sequence == 1);
    assert(result.candidates.size() == 1);

    const auto decisions = algo.consume_analysis_result(result);
    assert(decisions.size() == 1 && decisions.front().accepted);
    assert(broker.submitted.size() == 1);
    assert(broker.submitted.front().quantity == 5);
    assert(broker.submitted.front().side == trader::Side::buy);

    const auto repeated_target = analyzer.on_tick(tick);
    const auto repeated_decision = algo.consume_analysis_result(repeated_target);
    assert(repeated_decision.size() == 1 && repeated_decision.front().accepted);
    assert(broker.submitted.size() == 1);

    auto wrong_strategy = analyzer.on_tick(tick);
    wrong_strategy.strategy_id = "another-strategy";
    const auto rejected = algo.consume_analysis_result(wrong_strategy);
    assert(rejected.size() == 1 && !rejected.front().accepted);

    auto stale_result = analyzer.on_tick(tick);
    stale_result.generated_at_ms -= settings.analysis_candidate_ttl_ms + 1;
    const auto stale_decision = algo.consume_analysis_result(stale_result);
    assert(stale_decision.size() == 1 && !stale_decision.front().accepted);

    auto malformed_result = analyzer.on_tick(tick);
    malformed_result.candidates.front().lot_size = 0;
    const auto malformed_decision = algo.consume_analysis_result(malformed_result);
    assert(malformed_decision.size() == 1 && !malformed_decision.front().accepted);

    logic.target_symbol = "NOT_CONFIGURED";
    const auto filtered = analyzer.on_tick(tick);
    assert(filtered.candidates.empty());
}

void test_partial_fill_accounting()
{
    TestBroker broker;
    trader::Trader algo(config(), broker);
    assert(algo.initialize() && algo.start());
    algo.on_tick({"NSE", "NIFTY", 100.0, 1});

    trader::OrderIntent buy{"NSE", "NIFTY", trader::Side::buy, 10, 100.0,
                            trader::OrderType::limit, 1};
    const auto buy_result = algo.place_order(buy);
    assert(buy_result.accepted);
    algo.on_order_update({buy_result.order_id, trader::OrderStatus::partially_filled, 5, 100.0, {}});
    algo.on_order_update({buy_result.order_id, trader::OrderStatus::filled, 10, 110.0, {}});
    assert(algo.state().current_positions.at("NSE|NIFTY").quantity == 10);
    assert(std::abs(algo.state().current_positions.at("NSE|NIFTY").entry_price - 110.0) < 1e-9);

    trader::OrderIntent sell{"NSE", "NIFTY", trader::Side::sell, 15, 120.0,
                             trader::OrderType::limit, 1};
    const auto sell_result = algo.place_order(sell);
    assert(sell_result.accepted);
    algo.on_order_update({sell_result.order_id, trader::OrderStatus::filled, 15, 120.0, {}});
    const auto& position = algo.state().current_positions.at("NSE|NIFTY");
    assert(position.quantity == -5);
    assert(std::abs(position.entry_price - 120.0) < 1e-9);
    assert(std::abs(algo.calculate_pnl().realized - 100.0) < 1e-9);
}

void test_capital_and_drawdown_limits()
{
    TestBroker broker;
    auto settings = config();
    settings.risk_limits.max_drawdown = 500.0;
    trader::Trader algo(settings, broker);
    assert(algo.initialize() && algo.start());
    algo.on_tick({"NSE", "NIFTY", 100.0, 1});

    trader::OrderIntent buy{"NSE", "NIFTY", trader::Side::buy, 10, 100.0,
                            trader::OrderType::limit, 1};
    const auto result = algo.place_order(buy);
    assert(result.accepted);
    algo.on_order_update({result.order_id, trader::OrderStatus::filled, 10, 100.0, {}});
    algo.on_tick({"NSE", "NIFTY", 40.0, 2});
    trader::OrderIntent add_risk{"NSE", "NIFTY", trader::Side::buy, 1, 40.0,
                                 trader::OrderType::limit, 1};
    assert(!algo.risk_check(add_risk));

    auto small_allocation = config();
    small_allocation.allocated_capital = 500.0;
    trader::Trader capital_limited(small_allocation, broker);
    trader::OrderIntent too_large{"NSE", "NIFTY", trader::Side::buy, 10, 100.0,
                                  trader::OrderType::limit, 1};
    assert(!capital_limited.risk_check(too_large));
}

void test_square_off_across_exchanges()
{
    TestBroker broker;
    trader::Trader algo(config(), broker);
    assert(algo.initialize() && algo.start());
    algo.on_tick({"NSE", "NIFTY", 100.0, 1});
    algo.on_tick({"BSE", "NIFTY", 101.0, 2});

    for (const auto* exchange : {"NSE", "BSE"}) {
        trader::OrderIntent buy{exchange, "NIFTY", trader::Side::buy, 1, 100.0,
                                trader::OrderType::limit, 1};
        const auto result = algo.place_order(buy);
        assert(result.accepted);
        algo.on_order_update({result.order_id, trader::OrderStatus::filled, 1, 100.0, {}});
    }
    const auto square_off = algo.square_off_all();
    assert(square_off.accepted);
    assert(broker.submitted.size() == 4);
    assert(broker.submitted[2].exchange != broker.submitted[3].exchange);
    assert(broker.submitted[2].side == trader::Side::sell);
    assert(broker.submitted[3].side == trader::Side::sell);
}

void test_futures_options_helpers()
{
    assert(trader::Trader::get_atm_strike(24'536.0, 50.0) == 24'550.0);
    assert(trader::Trader::select_strike(24'536.0, 50.0, trader::OptionType::call, 1) == 24'500.0);

    TestBroker broker;
    trader::Trader algo(config(), broker);
    trader::FoContract contract;
    contract.symbol = "NIFTY-OPT";
    contract.exchange = "NFO";
    contract.expiry_date = "2026-12-31";
    contract.strike_price = 100.0;
    contract.option_type = trader::OptionType::call;
    contract.underlying_symbol = "NIFTY";
    contract.lot_size = 25;
    contract.underlying_price = 100.0;
    contract.option_market_price = 10.0;
    const auto valuation_time = std::chrono::system_clock::time_point{
        std::chrono::sys_days{std::chrono::year{2026} / std::chrono::October / 1}};
    const auto greeks = algo.calculate_greeks(contract, 0.06, valuation_time);
    assert(greeks);
    assert(greeks->delta > 0.0 && greeks->delta < 1.0);
    assert(greeks->gamma > 0.0);
    assert(greeks->implied_volatility > 0.0);
    assert(!algo.calculate_greeks(contract, 0.06,
                                  std::chrono::system_clock::time_point{
                                      std::chrono::sys_days{std::chrono::year{2027} / std::chrono::January / 1}}));

    algo.register_contract(contract);
    const trader::OrderIntent leg{"NFO", "NIFTY-OPT", trader::Side::buy, 25, 10.0,
                                  trader::OrderType::limit, 25};
    assert(!algo.check_span_margin({leg}));
    broker.margin = trader::MarginSnapshot{50'000.0, 1'000.0, 100'000.0};
    assert(algo.check_span_margin({leg}));
    assert(algo.initialize() && algo.start());

    const trader::OrderIntent first_large_leg{"NSE", "NIFTY", trader::Side::buy, 60, 100.0,
                                              trader::OrderType::limit, 1};
    const trader::OrderIntent second_large_leg{"NSE", "NIFTY", trader::Side::buy, 60, 100.0,
                                               trader::OrderType::limit, 1};
    assert(!algo.place_multi_leg_order({first_large_leg, second_large_leg}).accepted);
    assert(algo.place_multi_leg_order({leg}).accepted);
}

void test_text_protocol_round_trip()
{
    boost::asio::io_context context;
    boost::asio::ip::tcp::acceptor acceptor(
        context, {boost::asio::ip::make_address("127.0.0.1"), 0});
    boost::asio::ip::tcp::socket sender(context);
    sender.connect(acceptor.local_endpoint());
    boost::asio::ip::tcp::socket receiver(context);
    acceptor.accept(receiver);

    trader::MarketTick tick{"NSE", "SYMBOL|WITH\nDELIMITER", 123.45, 1'700'000'000'000};
    const auto encoded_tick = trader::encode_market_tick(tick);
    const auto wire_tick = trader::encode_message(encoded_tick);
    assert(wire_tick.find("BEGIN TICK\n") == 0);
    assert(wire_tick.ends_with("END\n"));
    assert(wire_tick.find("%7C") != std::string::npos);
    assert(wire_tick.find("%0A") != std::string::npos);
    trader::MarketTick decoded_tick;
    trader::write_message(sender, encoded_tick);
    trader::TextMessage received_tick;
    assert(trader::read_message(receiver, received_tick));
    assert(trader::decode_market_tick(received_tick, decoded_tick));
    assert(decoded_tick.exchange == tick.exchange);
    assert(decoded_tick.symbol == tick.symbol);
    assert(decoded_tick.last_price == tick.last_price);
    assert(decoded_tick.timestamp_ms == tick.timestamp_ms);

    trader::AnalysisResult result;
    result.strategy_id = "strategy|one";
    result.sequence = 7;
    result.generated_at_ms = 1'700'000'000'000;
    trader::AnalysisCandidate candidate;
    candidate.exchange = "NFO";
    candidate.symbol = "NIFTY-OPT";
    candidate.underlying_symbol = "NIFTY";
    candidate.instrument_kind = trader::InstrumentKind::option;
    candidate.target_quantity = 25;
    candidate.order_type = trader::OrderType::limit;
    candidate.limit_price = 123.45;
    candidate.lot_size = 25;
    candidate.score = 0.75;
    candidate.rationale = "entry\nsignal";
    candidate.expires_at_ms = result.generated_at_ms + 3000;
    trader::FoContract contract;
    contract.symbol = candidate.symbol;
    contract.exchange = candidate.exchange;
    contract.expiry_date = "2026-12-31";
    contract.strike_price = 25000.0;
    contract.option_type = trader::OptionType::call;
    contract.underlying_symbol = "NIFTY";
    contract.lot_size = 25;
    contract.underlying_price = 25010.0;
    contract.option_market_price = 300.0;
    contract.greeks = {0.5, 0.01, -2.0, 1.2, 0.2};
    contract.initial_margin_required = 10000.0;
    contract.m2m_margin = 250.0;
    candidate.option_contract = contract;
    result.candidates.push_back(candidate);

    trader::AnalysisResult decoded_result;
    trader::write_message(sender, trader::encode_analysis_result(result));
    trader::TextMessage received_result;
    assert(trader::read_message(receiver, received_result));
    assert(trader::decode_analysis_result(received_result, decoded_result));
    assert(decoded_result.schema_version == result.schema_version);
    assert(decoded_result.strategy_id == result.strategy_id);
    assert(decoded_result.sequence == result.sequence);
    assert(decoded_result.generated_at_ms == result.generated_at_ms);
    assert(decoded_result.candidates.size() == 1);
    assert(decoded_result.candidates.front().rationale == candidate.rationale);
    assert(decoded_result.candidates.front().option_contract);
    assert(decoded_result.candidates.front().option_contract->greeks.vega == contract.greeks.vega);

    auto settings = config();
    std::unordered_map<std::string, std::vector<trader::Bar>> history;
    history["NIFTY"].push_back({"NSE", "NIFTY", 5, 100, 102, 99, 101, 50});
    trader::StrategyConfig decoded_config;
    std::unordered_map<std::string, std::vector<trader::Bar>> decoded_history;
    assert(trader::decode_worker_init(
        trader::encode_worker_init(settings, history), decoded_config, decoded_history));
    assert(decoded_config.strategy_id == settings.strategy_id);
    assert(decoded_config.symbols == settings.symbols);
    assert(decoded_history.at("NIFTY").size() == 1);
    assert(decoded_history.at("NIFTY").front().close == 101);
}

} // namespace

int main()
{
    test_lifecycle_and_order_risk();
    test_analyzer_to_trader_handoff();
    test_partial_fill_accounting();
    test_capital_and_drawdown_limits();
    test_square_off_across_exchanges();
    test_futures_options_helpers();
    test_text_protocol_round_trip();
    std::cout << "all trader tests passed\n";
}
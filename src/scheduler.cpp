#include "trader/scheduler.hpp"

#include "trader/business_logic.hpp"
#include "trader/log.hpp"
#include "trader/text_protocol.hpp"

#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/time.h>

namespace trader {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using HistoricalData = std::unordered_map<std::string, std::vector<Bar>>;

class CachedMarketData final : public MarketDataProvider {
public:
    explicit CachedMarketData(HistoricalData data)
        : data_(std::move(data))
    {
    }

    std::vector<Bar> fetch_historical_data(const std::string& symbol) override
    {
        const auto found = data_.find(symbol);
        return found == data_.end() ? std::vector<Bar>{} : found->second;
    }

private:
    HistoricalData data_;
};

struct ScheduledEvent {
    TextMessage command;
};

struct WorkerSession {
    tcp::socket socket;
    std::vector<std::string> symbols;
    std::mutex queue_mutex;
    std::condition_variable queue_changed;
    std::queue<ScheduledEvent> events;
    bool stopping{false};

    explicit WorkerSession(tcp::socket connection)
        : socket(std::move(connection))
    {
    }

    bool enqueue(TextMessage message)
    {
        std::lock_guard lock(queue_mutex);
        if (stopping || events.size() >= 256) {
            return false;
        }
        events.push({std::move(message)});
        queue_changed.notify_one();
        return true;
    }

    bool next(TextMessage& message)
    {
        std::unique_lock lock(queue_mutex);
        queue_changed.wait(lock, [this] { return stopping || !events.empty(); });
        if (events.empty()) {
            return false;
        }
        message = std::move(events.front().command);
        events.pop();
        return true;
    }

    void stop()
    {
        std::lock_guard lock(queue_mutex);
        if (stopping) {
            return;
        }
        stopping = true;
        events.push({TextMessage{"STOP", {}}});
        queue_changed.notify_all();
    }
};

struct WorkerJob {
    std::shared_ptr<WorkerSession> session;
    StrategyConfig config;
    HistoricalData history;
};

} // namespace

Scheduler::Scheduler(
    Broker& broker,
    Trader& trader,
    WebSocketServer& web,
    std::uint16_t worker_port,
    std::size_t worker_count)
    : broker_(broker), trader_(trader), web_(web), worker_port_(worker_port), worker_count_(worker_count)
{
}

bool Scheduler::run(const std::function<bool()>& should_continue)
{
    if (worker_count_ == 0 || worker_count_ > trader_.config().symbols.size()) {
        log(LogLevel::error, "analyzer worker count must be between one and the configured symbol count");
        return false;
    }
    if (!trader_.initialize() || !trader_.start()) {
        log(LogLevel::error, "scheduler could not initialize the trading module");
        return false;
    }

    asio::io_context context;
    const auto address = asio::ip::make_address("127.0.0.1");
    tcp::acceptor acceptor(context, {address, worker_port_});
    acceptor.non_blocking(true);

    HistoricalData history;
    for (const auto& symbol : trader_.config().symbols) {
        history.emplace(symbol, broker_.fetch_historical_data(symbol));
    }

    std::mutex jobs_mutex;
    std::condition_variable jobs_changed;
    std::queue<WorkerJob> jobs;
    bool pool_stopping = false;
    std::mutex sessions_mutex;
    std::vector<std::shared_ptr<WorkerSession>> sessions;
    std::mutex trader_mutex;
    std::atomic<bool> runtime_ok{true};
    std::atomic<std::size_t> ready_workers{0};
    std::uint64_t result_sequence = 0;

    std::vector<std::thread> pool;
    pool.reserve(worker_count_);
    for (std::size_t thread_index = 0; thread_index < worker_count_; ++thread_index) {
        pool.emplace_back([&] {
            for (;;) {
                WorkerJob job;
                {
                    std::unique_lock lock(jobs_mutex);
                    jobs_changed.wait(lock, [&] { return pool_stopping || !jobs.empty(); });
                    if (jobs.empty()) {
                        return;
                    }
                    job = std::move(jobs.front());
                    jobs.pop();
                }

                try {
                    write_message(job.session->socket, encode_worker_init(job.config, job.history));
                    TextMessage reply;
                    if (!read_message(job.session->socket, reply) || reply.type != "READY" ||
                        reply.fields.find("status") == reply.fields.end() ||
                        reply.fields.at("status") != "ok") {
                        throw std::runtime_error("analyzer worker failed initialization");
                    }
                    {
                        std::lock_guard lock(sessions_mutex);
                        sessions.push_back(job.session);
                    }
                    ++ready_workers;

                    TextMessage command;
                    while (job.session->next(command)) {
                        if (command.type == "STOP") {
                            write_message(job.session->socket, command);
                            return;
                        }
                        write_message(job.session->socket, command);
                        if (!read_message(job.session->socket, reply)) {
                            throw std::runtime_error("analyzer worker disconnected while processing a command");
                        }
                        AnalysisResult result;
                        if (!decode_analysis_result(reply, result)) {
                            throw std::runtime_error("analyzer worker returned an invalid analysis result");
                        }
                        {
                            std::lock_guard lock(trader_mutex);
                            result.sequence = ++result_sequence;
                            for (const auto& decision : trader_.consume_analysis_result(result)) {
                                if (!decision.accepted) {
                                    log(LogLevel::warning, "analysis result rejected: " + decision.message);
                                }
                            }
                        }
                    }
                } catch (const std::exception& error) {
                    log(LogLevel::error, std::string("analyzer worker session failed: ") + error.what());
                    runtime_ok = false;
                    job.session->stop();
                    return;
                }
            }
        });
    }

    log(LogLevel::info, "scheduler listening on 127.0.0.1:" + std::to_string(worker_port_) +
                            " with " + std::to_string(worker_count_) + " prestarted worker threads");

    std::thread market_loop([&] {
        using namespace std::chrono_literals;
        while (should_continue() && runtime_ok) {
            const auto cycle_started = std::chrono::steady_clock::now();
            try {
                {
                    std::lock_guard lock(trader_mutex);
                    for (const auto& update : broker_.poll_order_updates()) {
                        trader_.on_order_update(update);
                    }
                }
                for (const auto& tick : broker_.poll_market_data()) {
                    web_.publish(tick);
                    {
                        std::lock_guard lock(trader_mutex);
                        trader_.on_tick(tick);
                    }
                    std::vector<std::shared_ptr<WorkerSession>> current_sessions;
                    {
                        std::lock_guard lock(sessions_mutex);
                        current_sessions = sessions;
                    }
                    for (const auto& session : current_sessions) {
                        if (std::find(session->symbols.begin(), session->symbols.end(), tick.symbol) !=
                                session->symbols.end() &&
                            !session->enqueue(encode_market_tick(tick))) {
                            throw std::runtime_error("analyzer worker queue is full or stopping");
                        }
                    }
                }
                std::vector<std::shared_ptr<WorkerSession>> current_sessions;
                {
                    std::lock_guard lock(sessions_mutex);
                    current_sessions = sessions;
                }
                for (const auto& session : current_sessions) {
                    if (!session->enqueue(TextMessage{"TIMER", {}})) {
                        throw std::runtime_error("analyzer worker queue is full or stopping");
                    }
                }
            } catch (const std::exception& error) {
                log(LogLevel::error, std::string("scheduler market loop stopped: ") + error.what());
                runtime_ok = false;
                break;
            }
            std::this_thread::sleep_until(cycle_started + 1s);
        }
    });

    std::size_t accepted_workers = 0;
    bool startup_failed = false;
    while (should_continue() && runtime_ok &&
           (accepted_workers < worker_count_ || ready_workers.load() < worker_count_)) {
        boost::system::error_code error;
        tcp::socket socket(context);
        acceptor.accept(socket, error);
        if (error == asio::error::would_block || error == asio::error::try_again) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (error) {
            log(LogLevel::error, std::string("scheduler accept failed: ") + error.message());
            runtime_ok = false;
            startup_failed = true;
            break;
        }
        if (accepted_workers >= worker_count_) {
            boost::system::error_code ignored;
            socket.close(ignored);
            continue;
        }

        auto session = std::make_shared<WorkerSession>(std::move(socket));
        const timeval receive_timeout{10, 0};
        if (::setsockopt(session->socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                         &receive_timeout, sizeof(receive_timeout)) != 0) {
            log(LogLevel::error, "could not configure analyzer connection receive timeout");
            runtime_ok = false;
            startup_failed = true;
            break;
        }
        WorkerJob job{session, trader_.config(), {}};
        const auto worker_index = accepted_workers++;
        job.config.symbols.clear();
        for (std::size_t symbol_index = worker_index; symbol_index < trader_.config().symbols.size();
             symbol_index += worker_count_) {
            const auto& symbol = trader_.config().symbols[symbol_index];
            job.config.symbols.push_back(symbol);
            job.history.emplace(symbol, history.at(symbol));
        }
        session->symbols = job.config.symbols;
        {
            std::lock_guard lock(jobs_mutex);
            jobs.push(std::move(job));
        }
        jobs_changed.notify_one();
    }

    if (!should_continue() &&
        (accepted_workers < worker_count_ || ready_workers.load() < worker_count_)) {
        startup_failed = true;
    }
    while (should_continue() && runtime_ok) {
        boost::system::error_code error;
        tcp::socket extra_connection(context);
        acceptor.accept(extra_connection, error);
        if (!error) {
            boost::system::error_code ignored;
            extra_connection.close(ignored);
        } else if (error != asio::error::would_block && error != asio::error::try_again) {
            log(LogLevel::error, std::string("scheduler accept failed: ") + error.message());
            runtime_ok = false;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    market_loop.join();
    std::vector<std::shared_ptr<WorkerSession>> current_sessions;
    {
        std::lock_guard lock(sessions_mutex);
        current_sessions = sessions;
    }
    for (const auto& session : current_sessions) {
        session->stop();
    }
    {
        std::lock_guard lock(jobs_mutex);
        pool_stopping = true;
    }
    jobs_changed.notify_all();
    for (auto& worker : pool) {
        worker.join();
    }
    {
        std::lock_guard lock(trader_mutex);
        trader_.stop();
    }
    return !startup_failed && runtime_ok;
}

bool run_analyzer_worker(const char* host, std::uint16_t port)
{
    try {
        asio::io_context context;
        tcp::resolver resolver(context);
        tcp::socket socket(context);
        asio::connect(socket, resolver.resolve(host, std::to_string(port)));

        TextMessage initialization;
        StrategyConfig config;
        HistoricalData history;
        if (!read_message(socket, initialization) ||
            !decode_worker_init(initialization, config, history)) {
            throw std::runtime_error("scheduler sent invalid worker initialization data");
        }

        BusinessLogic business_logic;
        Analyzer analyzer(config, business_logic);
        CachedMarketData market_data(std::move(history));
        if (!analyzer.initialize(market_data)) {
            write_message(socket, TextMessage{"READY", {{"status", "error"}}});
            return false;
        }
        write_message(socket, TextMessage{"READY", {{"status", "ok"}}});
        log(LogLevel::info, "analyzer worker ready for strategy " + config.strategy_id);

        TextMessage command;
        while (read_message(socket, command)) {
            if (command.type == "STOP") {
                return true;
            }
            AnalysisResult result;
            if (command.type == "TICK") {
                MarketTick tick;
                if (!decode_market_tick(command, tick)) {
                    throw std::runtime_error("scheduler sent an invalid market tick");
                }
                result = analyzer.on_tick(tick);
            } else if (command.type == "TIMER") {
                result = analyzer.on_timer();
            } else {
                throw std::runtime_error("scheduler sent an unknown command");
            }
            write_message(socket, encode_analysis_result(result));
        }
        log(LogLevel::error, "connection to scheduler closed unexpectedly");
    } catch (const std::exception& error) {
        log(LogLevel::error, std::string("analyzer worker stopped: ") + error.what());
    }
    return false;
}

} // namespace trader

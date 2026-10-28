#include "trader/websocket_server.hpp"

#include "trader/log.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <atomic>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace trader {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

class WebSocketSession : public std::enable_shared_from_this<WebSocketSession> {
public:
    explicit WebSocketSession(tcp::socket socket)
        : socket_(std::move(socket))
    {
    }

    void run()
    {
        try {
            socket_.accept();
            for (;;) {
                beast::flat_buffer buffer;
                socket_.read(buffer);
                const auto command = beast::buffers_to_string(buffer.data());
                handle_command(command);
            }
        } catch (const beast::system_error&) {
            subscribed_.store(false);
        }
    }

    void send(std::string message)
    {
        std::lock_guard lock(write_mutex_);
        if (!socket_.next_layer().is_open()) {
            return;
        }
        try {
            socket_.text(true);
            socket_.write(asio::buffer(message));
        } catch (const beast::system_error&) {
            subscribed_.store(false);
        }
    }

    bool subscribed() const
    {
        return subscribed_.load();
    }

private:
    void handle_command(const std::string& command)
    {
        if (command == "get_status") {
            send("status|ok|broker=paper|live_orders=disabled");
        } else if (command == "subscribe_market_data") {
            subscribed_.store(true);
            send("subscribed|market_data");
        } else if (command == "unsubscribe_market_data") {
            subscribed_.store(false);
            send("unsubscribed|market_data");
        } else if (command == "place_order") {
            send("error|live_orders_disabled");
        } else {
            send("error|unknown_method");
        }
    }

    websocket::stream<tcp::socket> socket_;
    std::mutex write_mutex_;
    std::atomic<bool> subscribed_{false};
};

} // namespace

class WebSocketServer::State {
public:
    explicit State(std::uint16_t server_port)
        : port(server_port)
    {
    }

    std::uint16_t port;
    std::mutex clients_mutex;
    std::vector<std::weak_ptr<WebSocketSession>> clients;
};

WebSocketServer::WebSocketServer(std::uint16_t port)
    : state_(std::make_unique<State>(port))
{
}

WebSocketServer::~WebSocketServer() = default;

void WebSocketServer::run()
{
    try {
        asio::io_context context;
        const auto address = asio::ip::make_address("127.0.0.1");
        tcp::acceptor acceptor(context, {address, state_->port});
        log(LogLevel::info, "WebSocket listening on 127.0.0.1:" + std::to_string(state_->port));

        for (;;) {
            tcp::socket socket(context);
            acceptor.accept(socket);
            auto session = std::make_shared<WebSocketSession>(std::move(socket));
            {
                std::lock_guard lock(state_->clients_mutex);
                state_->clients.emplace_back(session);
            }
            std::thread([session] { session->run(); }).detach();
        }
    } catch (const std::exception& error) {
        log(LogLevel::error, std::string("WebSocket server stopped: ") + error.what());
    }
}

void WebSocketServer::publish(const MarketTick& tick)
{
    std::vector<std::shared_ptr<WebSocketSession>> clients;
    {
        std::lock_guard lock(state_->clients_mutex);
        auto& entries = state_->clients;
        for (auto iterator = entries.begin(); iterator != entries.end();) {
            if (auto client = iterator->lock()) {
                clients.emplace_back(std::move(client));
                ++iterator;
            } else {
                iterator = entries.erase(iterator);
            }
        }
    }

    std::ostringstream message;
    message << "tick|" << tick.exchange << '|' << tick.symbol << '|'
            << std::setprecision(12) << tick.last_price << '|' << tick.timestamp_ms;
    for (const auto& client : clients) {
        if (client->subscribed()) {
            client->send(message.str());
        }
    }
}

} // namespace trader
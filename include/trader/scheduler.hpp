#pragma once

#include "trader/broker.hpp"
#include "trader/trader.hpp"
#include "trader/websocket_server.hpp"

#include <cstdint>
#include <functional>
#include <cstddef>

namespace trader {

class Scheduler {
public:
    Scheduler(
        Broker& broker,
        Trader& trader,
        WebSocketServer& web,
        std::uint16_t worker_port,
        std::size_t worker_count = 1);

    bool run(const std::function<bool()>& should_continue);

private:
    Broker& broker_;
    Trader& trader_;
    WebSocketServer& web_;
    std::uint16_t worker_port_;
    std::size_t worker_count_;
};

bool run_analyzer_worker(const char* host, std::uint16_t port);

} // namespace trader

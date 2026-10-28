#pragma once

#include "trader/types.hpp"

#include <cstdint>
#include <memory>

namespace trader {

class WebSocketServer {
public:
    explicit WebSocketServer(std::uint16_t port);
    ~WebSocketServer();

    WebSocketServer(const WebSocketServer&) = delete;
    WebSocketServer& operator=(const WebSocketServer&) = delete;

    void run();
    void publish(const MarketTick& tick);

private:
    class State;
    std::unique_ptr<State> state_;
};

} // namespace trader
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "tcp_interview/protocol.hpp"

namespace tcp_interview {

struct ServerConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{9000};
    std::size_t max_line_bytes{4096};
};

class TcpServer {
public:
    explicit TcpServer(ServerConfig config);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void run();
    void stop();

private:
    void accept_loop();
    void handle_client(int client_fd);

    ServerConfig config_;
    ServerStats stats_;
    std::atomic<bool> running_{false};
    int listen_fd_{-1};
};

}  // namespace tcp_interview

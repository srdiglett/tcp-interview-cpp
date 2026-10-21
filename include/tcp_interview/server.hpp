#pragma once

#include <atomic>
#include <cstdint>
#include <csignal>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tcp_interview/protocol.hpp"

namespace tcp_interview {

struct ServerConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{9000};
    std::size_t max_line_bytes{4096};
    std::size_t worker_count{4};
    std::size_t queue_capacity{16};
};

class TcpServer {
public:
    explicit TcpServer(ServerConfig config);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // Blocking, one-shot call. The object must outlive run().
    void run(const std::atomic<bool>* signal_stop = nullptr);
    // Thread-safe request; run() performs the joins before returning.
    void stop();

private:
    void accept_loop(const std::atomic<bool>* signal_stop);
    void worker_loop(std::size_t index);
    void join_workers();
    void handle_client(int client_fd);

    ServerConfig config_;
    ServerStats stats_;
    std::mutex clients_mutex_;
    std::condition_variable clients_ready_;
    std::deque<int> pending_clients_;
    std::vector<int> active_clients_;
    std::vector<std::thread> workers_;
    bool stopping_{false};  // Protected by clients_mutex_.
    bool started_{false};
    int listen_fd_{-1};
};

}  // namespace tcp_interview

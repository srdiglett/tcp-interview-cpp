#include "tcp_interview/server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <fcntl.h>
#include <poll.h>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace tcp_interview {
namespace {

void close_fd(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

using Clock = std::chrono::steady_clock;

// The deadline belongs to the whole message, not to each byte. Otherwise a
// slow client could keep a worker forever by sending one byte before expiry.
void wait_ready(int fd, short events, Clock::time_point deadline) {
    for (;;) {
        const auto remaining = deadline - Clock::now();
        if (remaining <= Clock::duration::zero()) {
            throw std::runtime_error("I/O deadline exceeded");
        }
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
        pollfd descriptor{fd, events, 0};
        const int result = poll(&descriptor, 1, static_cast<int>(milliseconds.count()) + 1);
        if (result < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("client poll failed");
        }
        if (result == 0) continue;
        if (descriptor.revents & POLLNVAL) throw std::runtime_error("invalid client socket");
        // Let recv/send report EOF or the actual error, including after shutdown.
        if (descriptor.revents & (events | POLLHUP | POLLERR)) return;
    }
}

void send_all(int fd, const std::string& message, int timeout_ms) {
    const char* data = message.data();
    std::size_t remaining = message.size();
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);

    while (remaining > 0) {
        wait_ready(fd, POLLOUT, deadline);
#ifdef MSG_NOSIGNAL
        const ssize_t sent = send(fd, data, remaining, MSG_NOSIGNAL);
#else
        const ssize_t sent = send(fd, data, remaining, 0);
#endif
        if (sent < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            throw std::runtime_error(std::string("send failed: ") + std::strerror(errno));
        }
        if (sent == 0) {
            throw std::runtime_error("send returned 0");
        }

        data += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
}

bool read_line(int fd, std::string& pending, std::string& line,
               std::size_t max_line_bytes, int timeout_ms) {
    line.clear();
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const auto newline = pending.find('\n');
        if (newline != std::string::npos) {
            if (newline > max_line_bytes) throw std::runtime_error("line too long");
            line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            return true;
        }
        if (pending.size() > max_line_bytes) throw std::runtime_error("line too long");
        wait_ready(fd, POLLIN, deadline);
        char buffer[4096];
        const ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
        if (received == 0) {
            if (!pending.empty()) throw std::runtime_error("incomplete frame at EOF");
            return false;
        }
        if (received < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            throw std::runtime_error(std::string("recv failed: ") + std::strerror(errno));
        }

        pending.append(buffer, static_cast<std::size_t>(received));
    }
}

}  // namespace

TcpServer::TcpServer(ServerConfig config) : config_(std::move(config)) {
    if (config_.worker_count == 0 || config_.queue_capacity == 0) {
        throw std::invalid_argument("workers and queue capacity must be positive");
    }
    if (config_.io_timeout_ms < 1 || config_.io_timeout_ms > 3600000 ||
        config_.max_line_bytes == 0) {
        throw std::invalid_argument("invalid I/O limits");
    }
    active_clients_.assign(config_.worker_count, -1);
}

TcpServer::~TcpServer() {
    stop();
}

void TcpServer::run(const std::atomic<bool>* signal_stop) {
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        if (started_) throw std::logic_error("run() can only be called once");
        started_ = true;
    }
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        throw std::runtime_error(std::string("socket failed: ") + std::strerror(errno));
    }

    const int enabled = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config_.port);
    if (inet_pton(AF_INET, config_.host.c_str(), &address.sin_addr) != 1) {
        close_fd(listen_fd_);
        listen_fd_ = -1;
        throw std::runtime_error("invalid IPv4 host: " + config_.host);
    }

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        close_fd(listen_fd_);
        listen_fd_ = -1;
        throw std::runtime_error(std::string("bind failed: ") + std::strerror(errno));
    }

    if (listen(listen_fd_, SOMAXCONN) < 0) {
        close_fd(listen_fd_);
        listen_fd_ = -1;
        throw std::runtime_error(std::string("listen failed: ") + std::strerror(errno));
    }

    try {
        // Only the producer owns the listening socket. Nonblocking accept and
        // a short poll timeout let it observe stop requests without fd races.
        const int flags = fcntl(listen_fd_, F_GETFL, 0);
        if (flags < 0 || fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
            throw std::runtime_error("could not configure listening socket");
        }
        for (std::size_t i = 0; i < config_.worker_count; ++i) {
            workers_.emplace_back(&TcpServer::worker_loop, this, i);
        }
        std::cout << "listening on " << config_.host << ':' << config_.port
                  << " workers=" << config_.worker_count
                  << " queue=" << config_.queue_capacity << std::endl;
        accept_loop(signal_stop);
    } catch (...) {
        stop();
        join_workers();
        close_fd(listen_fd_);
        listen_fd_ = -1;
        throw;
    }
    stop();
    join_workers();
    close_fd(listen_fd_);
    listen_fd_ = -1;
}

void TcpServer::stop() {
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        stopping_ = true;
        for (int fd : pending_clients_) close_fd(fd);
        pending_clients_.clear();
        // Wake blocking recv/send. The worker remains responsible for close().
        // Its close and removal use this same mutex, preventing fd reuse races.
        for (int fd : active_clients_) {
            if (fd >= 0) shutdown(fd, SHUT_RDWR);
        }
    }
    clients_ready_.notify_all();
}

void TcpServer::join_workers() {
    // Never join while holding the mutex: workers need it to finish.
    for (auto& worker : workers_) if (worker.joinable()) worker.join();
    workers_.clear();
}

void TcpServer::accept_loop(const std::atomic<bool>* signal_stop) {
    for (;;) {
        if (signal_stop != nullptr && signal_stop->load(std::memory_order_relaxed)) return;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            if (stopping_) return;
        }
        pollfd ready{listen_fd_, POLLIN, 0};
        const int rc = poll(&ready, 1, 100);
        if (rc < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("poll failed");
        }
        if (rc == 0) continue;
        if (ready.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            throw std::runtime_error("listening socket failed");
        }
        sockaddr_in client_address{};
        socklen_t client_size = sizeof(client_address);
        const int client_fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_address), &client_size);

        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            throw std::runtime_error(std::string("accept failed: ") + std::strerror(errno));
        }

        // poll readiness is only a hint: nonblocking I/O enforces our deadline.
        const int flags = fcntl(client_fd, F_GETFL, 0);
        if (flags < 0 || fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close_fd(client_fd);
            continue;
        }
#ifdef SO_NOSIGPIPE
        const int enabled = 1;
        if (setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0) {
            close_fd(client_fd);
            continue;
        }
#endif
        try {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            if (stopping_ || pending_clients_.size() >= config_.queue_capacity) {
                close_fd(client_fd);  // Overload policy: reject, never grow unbounded.
                continue;
            }
            pending_clients_.push_back(client_fd);
        } catch (...) {
            close_fd(client_fd);
            throw;
        }
        clients_ready_.notify_one();
    }
}

void TcpServer::worker_loop(std::size_t index) {
    for (;;) {
        int client_fd;
        {
            std::unique_lock<std::mutex> lock(clients_mutex_);
            // wait releases the mutex while sleeping and rechecks the predicate.
            clients_ready_.wait(lock, [this] {
                return stopping_ || !pending_clients_.empty();
            });
            if (stopping_) return;
            client_fd = pending_clients_.front();
            pending_clients_.pop_front();
            active_clients_[index] = client_fd;
        }
        handle_client(client_fd);  // No queue mutex during network I/O.
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            close_fd(client_fd);
            active_clients_[index] = -1;
        }
    }
}

void TcpServer::handle_client(int client_fd) {
    ++stats_.total_connections;
    ++stats_.active_connections;

    LineProtocol protocol(stats_);

    try {
        send_all(client_fd, "OK tcp-interview ready\n", config_.io_timeout_ms);

        std::string line;
        std::string pending;
        while (read_line(client_fd, pending, line, config_.max_line_bytes, config_.io_timeout_ms)) {
            const CommandResult result = protocol.handle(line);
            send_all(client_fd, result.response + "\n", config_.io_timeout_ms);
            if (result.close_connection) {
                break;
            }
        }
    } catch (const std::exception& error) {
        static std::mutex log_mutex;
        std::lock_guard<std::mutex> lock(log_mutex);
        std::cerr << "client error: " << error.what() << '\n';
    }

    --stats_.active_connections;
}

}  // namespace tcp_interview

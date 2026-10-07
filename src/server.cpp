#include "tcp_interview/server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
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

void send_all(int fd, const std::string& message) {
    const char* data = message.data();
    std::size_t remaining = message.size();

    while (remaining > 0) {
        const ssize_t sent = send(fd, data, remaining, 0);
        if (sent < 0) {
            if (errno == EINTR) {
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

bool read_line(int fd, std::string& line, std::size_t max_line_bytes) {
    line.clear();

    while (line.size() < max_line_bytes) {
        char ch = 0;
        const ssize_t received = recv(fd, &ch, 1, 0);
        if (received == 0) {
            return !line.empty();
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("recv failed: ") + std::strerror(errno));
        }

        if (ch == '\n') {
            return true;
        }
        line.push_back(ch);
    }

    throw std::runtime_error("line too long");
}

}  // namespace

TcpServer::TcpServer(ServerConfig config) : config_(std::move(config)) {}

TcpServer::~TcpServer() {
    stop();
}

void TcpServer::run() {
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

    running_ = true;
    std::cout << "listening on " << config_.host << ':' << config_.port << '\n';
    accept_loop();
}

void TcpServer::stop() {
    running_ = false;
    close_fd(listen_fd_);
    listen_fd_ = -1;
}

void TcpServer::accept_loop() {
    while (running_) {
        sockaddr_in client_address{};
        socklen_t client_size = sizeof(client_address);
        const int client_fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_address), &client_size);

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (running_) {
                std::cerr << "accept failed: " << std::strerror(errno) << '\n';
            }
            break;
        }

        std::thread(&TcpServer::handle_client, this, client_fd).detach();
    }
}

void TcpServer::handle_client(int client_fd) {
    ++stats_.total_connections;
    ++stats_.active_connections;

    LineProtocol protocol(stats_);

    try {
        send_all(client_fd, "OK tcp-interview ready\n");

        std::string line;
        while (read_line(client_fd, line, config_.max_line_bytes)) {
            const CommandResult result = protocol.handle(line);
            send_all(client_fd, result.response + "\n");
            if (result.close_connection) {
                break;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "client error: " << error.what() << '\n';
    }

    --stats_.active_connections;
    close_fd(client_fd);
}

}  // namespace tcp_interview

#include "tcp_interview/server.hpp"

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

namespace {

tcp_interview::TcpServer* active_server = nullptr;

void handle_signal(int) {
    if (active_server != nullptr) {
        active_server->stop();
    }
}

void print_usage(const char* program) {
    std::cerr << "Usage: " << program << " [--host 127.0.0.1] [--port 9000]\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    tcp_interview::ServerConfig config;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            config.host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            const int port = std::atoi(argv[++i]);
            if (port <= 0 || port > 65535) {
                std::cerr << "invalid port\n";
                return 2;
            }
            config.port = static_cast<std::uint16_t>(port);
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }

    try {
        tcp_interview::TcpServer server(config);
        active_server = &server;
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        server.run();
    } catch (const std::exception& error) {
        std::cerr << "server failed: " << error.what() << '\n';
        return 1;
    }

    return 0;
}

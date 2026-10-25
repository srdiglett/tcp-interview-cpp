#include "tcp_interview/server.hpp"

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

namespace {

// A lock-free atomic store is safe in a signal handler, even if another
// thread receives the signal. Never lock the queue mutex here.
static_assert(std::atomic<bool>::is_always_lock_free, "signal flag must be lock-free");
std::atomic<bool> stop_requested{false};

void handle_signal(int) {
    stop_requested.store(true, std::memory_order_relaxed);
}

void print_usage(const char* program) {
    std::cerr << "Usage: " << program
              << " [--host 127.0.0.1] [--port 9000] [--workers 4] [--queue-capacity 16]"
              << " [--io-timeout-ms 10000]\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    tcp_interview::ServerConfig config;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            config.host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value.empty() || value.size() > 5 ||
                value.find_first_not_of("0123456789") != std::string::npos ||
                std::stoi(value) < 1 || std::stoi(value) > 65535) {
                std::cerr << "invalid port\n";
                return 2;
            }
            config.port = static_cast<std::uint16_t>(std::stoi(value));
        } else if (arg == "--io-timeout-ms" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value.empty() || value.size() > 7 ||
                value.find_first_not_of("0123456789") != std::string::npos ||
                std::stoi(value) < 1 || std::stoi(value) > 3600000) {
                std::cerr << "invalid I/O timeout (expected 1..3600000 ms)\n";
                return 2;
            }
            config.io_timeout_ms = std::stoi(value);
        } else if ((arg == "--workers" || arg == "--queue-capacity") && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value.empty() || value.size() > 4 ||
                value.find_first_not_of("0123456789") != std::string::npos ||
                std::stoi(value) < 1 || std::stoi(value) > 1024) {
                std::cerr << "invalid pool size (expected 1..1024)\n";
                return 2;
            }
            if (arg == "--workers") config.worker_count = std::stoul(value);
            else config.queue_capacity = std::stoul(value);
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
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        server.run(&stop_requested);
    } catch (const std::exception& error) {
        std::cerr << "server failed: " << error.what() << '\n';
        return 1;
    }

    return 0;
}

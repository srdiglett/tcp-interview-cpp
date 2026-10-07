#pragma once

#include <atomic>
#include <map>
#include <string>

namespace tcp_interview {

struct ServerStats {
    std::atomic<int> total_connections{0};
    std::atomic<int> active_connections{0};
    std::atomic<int> total_commands{0};
};

struct CommandResult {
    std::string response;
    bool close_connection{false};
};

class LineProtocol {
public:
    explicit LineProtocol(ServerStats& stats);

    CommandResult handle(const std::string& line);

private:
    CommandResult handle_text_command(const std::string& line);
    CommandResult handle_json_command(const std::string& line);
    CommandResult execute_command(const std::string& command, const std::string& argument);
    CommandResult execute_command(
        const std::string& command,
        const std::string& argument,
        const std::map<std::string, std::string>& fields);

    ServerStats& stats_;
};

}  // namespace tcp_interview

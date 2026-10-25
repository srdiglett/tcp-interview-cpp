#include "tcp_interview/protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>

namespace tcp_interview {
namespace {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }

    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string to_upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

std::string utc_now() {
    const auto now = std::chrono::system_clock::now();
    const auto now_time = std::chrono::system_clock::to_time_t(now);

    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now_time);
#else
    gmtime_r(&now_time, &utc);
#endif

    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

bool starts_like_json_object(const std::string& line) {
    return !line.empty() && line.front() == '{';
}

bool consume_literal(const std::string& input, std::size_t& index, char expected) {
    if (index >= input.size() || input[index] != expected) {
        return false;
    }
    ++index;
    return true;
}

void skip_spaces(const std::string& input, std::size_t& index) {
    while (index < input.size() && std::isspace(static_cast<unsigned char>(input[index]))) {
        ++index;
    }
}

bool parse_json_string(const std::string& input, std::size_t& index, std::string& output) {
    if (!consume_literal(input, index, '"')) {
        return false;
    }

    output.clear();
    while (index < input.size()) {
        const char ch = input[index++];
        if (ch == '"') {
            return true;
        }

        // This tiny parser only implements the escapes useful for hand-written
        // JSON in netcat. A production server should use a real JSON library.
        if (ch == '\\') {
            if (index >= input.size()) {
                return false;
            }
            const char escaped = input[index++];
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    output.push_back(escaped);
                    break;
                case 'n':
                    output.push_back('\n');
                    break;
                case 'r':
                    output.push_back('\r');
                    break;
                case 't':
                    output.push_back('\t');
                    break;
                default:
                    return false;
            }
        } else {
            output.push_back(ch);
        }
    }

    return false;
}

bool parse_flat_json_object(const std::string& input, std::map<std::string, std::string>& values) {
    values.clear();
    std::size_t index = 0;
    skip_spaces(input, index);
    if (!consume_literal(input, index, '{')) {
        return false;
    }

    skip_spaces(input, index);
    if (consume_literal(input, index, '}')) {
        skip_spaces(input, index);
        return index == input.size();
    }

    while (index < input.size()) {
        std::string key;
        std::string value;
        skip_spaces(input, index);
        if (!parse_json_string(input, index, key)) {
            return false;
        }

        skip_spaces(input, index);
        if (!consume_literal(input, index, ':')) {
            return false;
        }

        skip_spaces(input, index);
        if (!parse_json_string(input, index, value)) {
            return false;
        }
        values[key] = value;

        skip_spaces(input, index);
        if (consume_literal(input, index, '}')) {
            skip_spaces(input, index);
            return index == input.size();
        }
        if (!consume_literal(input, index, ',')) {
            return false;
        }
    }

    return false;
}

std::string get_field(const std::map<std::string, std::string>& fields, const std::string& name) {
    const auto it = fields.find(name);
    return it == fields.end() ? "" : it->second;
}

std::string format_stats(ServerStats& stats) {
    return "connections=" + std::to_string(stats.total_connections.load()) +
           " active=" + std::to_string(stats.active_connections.load()) +
           " commands=" + std::to_string(stats.total_commands.load());
}

}  // namespace

LineProtocol::LineProtocol(ServerStats& stats) : stats_(stats) {}

CommandResult LineProtocol::handle_text_command(const std::string& line) {
    const auto separator = line.find(' ');
    const std::string raw_command = separator == std::string::npos ? line : line.substr(0, separator);
    const std::string argument = separator == std::string::npos ? "" : line.substr(separator + 1);

    return execute_command(to_upper(raw_command), argument);
}

CommandResult LineProtocol::handle_json_command(const std::string& line) {
    std::map<std::string, std::string> fields;
    if (!parse_flat_json_object(line, fields)) {
        return {"ERR invalid json", false};
    }

    const auto command_it = fields.find("command");
    if (command_it == fields.end() || trim(command_it->second).empty()) {
        return {"ERR json missing command", false};
    }

    // JSON commands use named fields. ECHO reads text, while PING/TIME/STATS
    // ignore it. This mirrors how many line protocols evolve over time.
    const std::string argument = get_field(fields, "text");
    return execute_command(to_upper(command_it->second), argument, fields);
}

CommandResult LineProtocol::execute_command(const std::string& command, const std::string& argument) {
    return execute_command(command, argument, {});
}

CommandResult LineProtocol::execute_command(
    const std::string& command,
    const std::string& argument,
    const std::map<std::string, std::string>&) {
    // Responses are line-framed too: decoded JSON escapes must not inject
    // additional response lines, even through an unknown command's error.
    if (command.find_first_of("\r\n") != std::string::npos ||
        argument.find_first_of("\r\n") != std::string::npos) {
        return {"ERR multiline fields are not allowed", false};
    }
    if (command == "PING") {
        return {"PONG", false};
    }
    if (command == "ECHO") {
        return {argument, false};
    }
    if (command == "TIME") {
        return {utc_now(), false};
    }
    if (command == "STATS") {
        return {format_stats(stats_), false};
    }
    if (command == "HELP" || command == "?") {
        return {R"(commands: PING, ECHO <text>, TIME, STATS, QUIT; json: {"command":"PING"})", false};
    }
    if (command == "QUIT") {
        return {"BYE", true};
    }

    return {"ERR unknown command: " + command, false};
}

CommandResult LineProtocol::handle(const std::string& raw_line) {
    const std::string line = trim(raw_line);
    if (line.empty()) {
        return {"ERR empty command", false};
    }

    ++stats_.total_commands;

    // A TCP socket is just a byte stream; this server frames messages by line.
    // Once a complete line arrives, we dispatch either JSON Lines or plain text.
    if (starts_like_json_object(line)) {
        return handle_json_command(line);
    }
    return handle_text_command(line);
}

}  // namespace tcp_interview

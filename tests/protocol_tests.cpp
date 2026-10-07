#include "tcp_interview/protocol.hpp"

#include <cassert>
#include <iostream>
#include <string>

using tcp_interview::LineProtocol;
using tcp_interview::ServerStats;

int main() {
    ServerStats stats;
    LineProtocol protocol(stats);

    assert(protocol.handle("PING").response == "PONG");
    assert(protocol.handle("ping").response == "PONG");
    assert(protocol.handle("ECHO hello interview").response == "hello interview");
    assert(protocol.handle("HELP").response.find("PING") != std::string::npos);

    assert(protocol.handle(R"({"command":"PING"})").response == "PONG");
    assert(protocol.handle(R"({"command":"ping"})").response == "PONG");
    assert(protocol.handle(R"({"command":"ECHO","text":"hello json"})").response == "hello json");
    assert(protocol.handle(R"({"text":"missing command"})").response == "ERR json missing command");
    assert(protocol.handle(R"({"command":123})").response == "ERR invalid json");

    const auto stats_response = protocol.handle("STATS").response;
    assert(stats_response.find("commands=10") != std::string::npos);

    const auto quit = protocol.handle(R"({"command":"QUIT"})");
    assert(quit.response == "BYE");
    assert(quit.close_connection);

    const auto unknown = protocol.handle("NOPE");
    assert(unknown.response == "ERR unknown command: NOPE");

    std::cout << "protocol tests passed\n";
    return 0;
}

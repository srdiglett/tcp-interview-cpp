// Example: tiny UX/QoE analytics pipeline for telecom-style events.
//
// This is closer to code you could see in a UX analytics team than the toy TCP
// protocol itself: read network events, parse JSON, aggregate metrics by cell,
// and emit alerts when user experience looks degraded.
//
// In a real system these events might arrive from Kafka, files, probes, network
// functions, or collectors near RAN/core elements. The ideas are the same:
// validate input, normalize it, aggregate it, and make it actionable.

#include <boost/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>

namespace json = boost::json;

namespace {

struct NetworkEvent {
    std::string timestamp;
    std::string cell_id;
    std::string subscriber_id;
    std::string app;
    double latency_ms{0.0};
    double throughput_mbps{0.0};
    double packet_loss_pct{0.0};
};

struct CellWindowStats {
    int samples{0};
    double latency_sum_ms{0.0};
    double throughput_sum_mbps{0.0};
    double packet_loss_sum_pct{0.0};
    int bad_latency_samples{0};
    int bad_loss_samples{0};
    int low_throughput_samples{0};

    void add(const NetworkEvent& event) {
        ++samples;
        latency_sum_ms += event.latency_ms;
        throughput_sum_mbps += event.throughput_mbps;
        packet_loss_sum_pct += event.packet_loss_pct;

        // These thresholds are intentionally simple. In production they would
        // depend on technology, service, geography, plan, device, and SLA/SLO.
        if (event.latency_ms > 120.0) {
            ++bad_latency_samples;
        }
        if (event.packet_loss_pct > 1.0) {
            ++bad_loss_samples;
        }
        if (event.throughput_mbps < 5.0) {
            ++low_throughput_samples;
        }
    }

    double avg_latency_ms() const { return latency_sum_ms / samples; }
    double avg_throughput_mbps() const { return throughput_sum_mbps / samples; }
    double avg_packet_loss_pct() const { return packet_loss_sum_pct / samples; }
};

double read_double(const json::object& object, const char* field) {
    const auto* value = object.if_contains(field);
    if (value == nullptr || !value->is_double()) {
        throw std::runtime_error(std::string("missing numeric field: ") + field);
    }
    return value->as_double();
}

std::string read_string(const json::object& object, const char* field) {
    const auto* value = object.if_contains(field);
    if (value == nullptr || !value->is_string()) {
        throw std::runtime_error(std::string("missing string field: ") + field);
    }
    return std::string(value->as_string());
}

std::optional<NetworkEvent> parse_event_line(const std::string& line, int line_number) {
    try {
        const json::value parsed = json::parse(line);
        if (!parsed.is_object()) {
            throw std::runtime_error("event must be a JSON object");
        }

        const json::object& object = parsed.as_object();
        return NetworkEvent{
            read_string(object, "timestamp"),
            read_string(object, "cell_id"),
            read_string(object, "subscriber_id"),
            read_string(object, "app"),
            read_double(object, "latency_ms"),
            read_double(object, "throughput_mbps"),
            read_double(object, "packet_loss_pct"),
        };
    } catch (const std::exception& error) {
        std::cerr << "dropping malformed event at line " << line_number << ": " << error.what() << '\n';
        return std::nullopt;
    }
}

std::string classify_qoe(const CellWindowStats& stats) {
    if (stats.samples == 0) {
        return "NO_DATA";
    }

    const bool high_latency = stats.avg_latency_ms() > 100.0;
    const bool high_loss = stats.avg_packet_loss_pct() > 1.0;
    const bool low_throughput = stats.avg_throughput_mbps() < 8.0;

    if (high_latency && high_loss) {
        return "CRITICAL";
    }
    if (high_latency || high_loss || low_throughput) {
        return "DEGRADED";
    }
    return "GOOD";
}

void print_report(const std::map<std::string, CellWindowStats>& stats_by_cell) {
    std::cout << std::left << std::setw(12) << "cell"
              << std::right << std::setw(8) << "samples"
              << std::setw(14) << "avg_lat_ms"
              << std::setw(16) << "avg_mbps"
              << std::setw(14) << "avg_loss_%"
              << std::setw(13) << "qoe" << '\n';

    for (const auto& [cell_id, stats] : stats_by_cell) {
        std::cout << std::left << std::setw(12) << cell_id
                  << std::right << std::setw(8) << stats.samples
                  << std::setw(14) << std::fixed << std::setprecision(1) << stats.avg_latency_ms()
                  << std::setw(16) << std::fixed << std::setprecision(1) << stats.avg_throughput_mbps()
                  << std::setw(14) << std::fixed << std::setprecision(2) << stats.avg_packet_loss_pct()
                  << std::setw(13) << classify_qoe(stats) << '\n';
    }
}

void print_alerts(const std::map<std::string, CellWindowStats>& stats_by_cell) {
    for (const auto& [cell_id, stats] : stats_by_cell) {
        const std::string qoe = classify_qoe(stats);
        if (qoe == "GOOD") {
            continue;
        }

        // This is the handoff from analytics to operations: turn raw counters
        // into a compact signal someone can investigate.
        std::cout << "ALERT cell=" << cell_id
                  << " qoe=" << qoe
                  << " avg_latency_ms=" << std::fixed << std::setprecision(1) << stats.avg_latency_ms()
                  << " avg_loss_pct=" << std::fixed << std::setprecision(2) << stats.avg_packet_loss_pct()
                  << " avg_throughput_mbps=" << std::fixed << std::setprecision(1) << stats.avg_throughput_mbps()
                  << '\n';
    }
}

}  // namespace

int main() {
    std::map<std::string, CellWindowStats> stats_by_cell;

    std::string line;
    int line_number = 0;
    while (std::getline(std::cin, line)) {
        ++line_number;
        if (line.empty()) {
            continue;
        }

        const auto event = parse_event_line(line, line_number);
        if (!event.has_value()) {
            continue;
        }

        stats_by_cell[event->cell_id].add(*event);
    }

    if (stats_by_cell.empty()) {
        std::cerr << "no valid events received\n";
        return 1;
    }

    print_report(stats_by_cell);
    std::cout << '\n';
    print_alerts(stats_by_cell);

    return 0;
}

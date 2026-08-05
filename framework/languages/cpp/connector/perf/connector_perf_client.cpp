/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/stream_connector.hpp>
#include <zlink/stream_e2e_client.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/Contracts/Sockets/stream_socket.hpp>

#include "runtime/connector_runtime.hpp"
#include "runtime/protocol/framing/frame_codec.hpp"
#include "runtime/protocol/header_codec.hpp"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{

struct perf_options_t
{
    int clients = 5000;
    int workers = 4;
    std::string duration = "60s";
    std::string warmup = "10s";
    int payload_bytes = 256;
    int inflight = 1;
    int request_timeout_ms = 1000;
    int wait_clients_percent = 10;
    std::string transport = "tcp";
    std::string dispatch_mode = "immediate";
    std::string endpoint;
    bool loopback = false;
    std::filesystem::path report = "connector-perf-report.json";
};

struct perf_result_t
{
    int connected = 0;
    int sends_completed = 0;
    int requests_completed = 0;
    int waits_completed = 0;
    int timeouts = 0;
    int errors = 0;
    std::int64_t duration_ms = 0;
    double throughput_rps = 0.0;
    std::int64_t latency_p50_us = 0;
    std::int64_t latency_p95_us = 0;
    std::int64_t latency_p99_us = 0;
    std::vector<std::int64_t> request_latencies_us;
};

struct perf_server_frame_t
{
    zlink::stream_connector::detail::stream_header_t header;
    std::string payload;
};

int to_int (const char *value)
{
    return std::stoi (std::string (value));
}

perf_options_t parse_options (int argc, char **argv)
{
    perf_options_t options;
    for (int index = 1; index < argc; ++index) {
        const std::string name = argv[index];
        auto next = [&] {
            if (index + 1 >= argc) {
                throw std::invalid_argument ("missing value for " + name);
            }
            return argv[++index];
        };
        if (name == "--clients") {
            options.clients = to_int (next ());
        } else if (name == "--workers") {
            options.workers = to_int (next ());
        } else if (name == "--duration") {
            options.duration = next ();
        } else if (name == "--warmup") {
            options.warmup = next ();
        } else if (name == "--payload-bytes") {
            options.payload_bytes = to_int (next ());
        } else if (name == "--inflight") {
            options.inflight = to_int (next ());
        } else if (name == "--request-timeout-ms") {
            options.request_timeout_ms = to_int (next ());
        } else if (name == "--wait-clients-percent") {
            options.wait_clients_percent = to_int (next ());
        } else if (name == "--transport") {
            options.transport = next ();
        } else if (name == "--dispatch-mode") {
            options.dispatch_mode = next ();
        } else if (name == "--endpoint") {
            options.endpoint = next ();
        } else if (name == "--loopback") {
            options.loopback = true;
        } else if (name == "--report") {
            options.report = next ();
        } else {
            throw std::invalid_argument ("unknown option " + name);
        }
    }
    return options;
}

void write_report (const perf_options_t &options, std::string mode, perf_result_t result = {})
{
    if (options.report.has_parent_path ()) {
        std::filesystem::create_directories (options.report.parent_path ());
    }
    std::ofstream report (options.report);
    report << "{\n"
           << "  \"mode\": \"" << mode << "\",\n"
           << "  \"clients\": " << options.clients << ",\n"
           << "  \"workers\": " << options.workers << ",\n"
           << "  \"connected_clients\": " << result.connected << ",\n"
           << "  \"sends_completed\": " << result.sends_completed << ",\n"
           << "  \"duration_ms\": " << result.duration_ms << ",\n"
           << "  \"requests_total\": " << result.requests_completed << ",\n"
           << "  \"throughput_rps\": " << result.throughput_rps << ",\n"
           << "  \"latency_p50_us\": " << result.latency_p50_us << ",\n"
           << "  \"latency_p95_us\": " << result.latency_p95_us << ",\n"
           << "  \"latency_p99_us\": " << result.latency_p99_us << ",\n"
           << "  \"waits_completed\": " << result.waits_completed << ",\n"
           << "  \"timeouts_total\": " << result.timeouts << ",\n"
           << "  \"errors_total\": " << result.errors << ",\n"
           << "  \"rss_bytes\": 0,\n"
           << "  \"cpu_user_ms\": 0,\n"
           << "  \"cpu_system_ms\": 0\n"
           << "}\n";
}

std::int64_t percentile (std::vector<std::int64_t> values, double ratio)
{
    if (values.empty ()) {
        return 0;
    }
    std::sort (values.begin (), values.end ());
    const auto index = static_cast<std::size_t> (
      std::min<double> (static_cast<double> (values.size () - 1),
                        std::max<double> (0.0, ratio * static_cast<double> (values.size () - 1))));
    return values[index];
}

void finalize_perf_result (perf_result_t &result)
{
    result.latency_p50_us = percentile (result.request_latencies_us, 0.50);
    result.latency_p95_us = percentile (result.request_latencies_us, 0.95);
    result.latency_p99_us = percentile (result.request_latencies_us, 0.99);
    if (result.duration_ms > 0) {
        result.throughput_rps =
          static_cast<double> (result.requests_completed) * 1000.0
          / static_cast<double> (result.duration_ms);
    }
}

std::optional<perf_server_frame_t> try_read_server_frame (std::string &buffer)
{
    if (buffer.size () < 6) {
        return std::nullopt;
    }
    const auto header_size =
      (static_cast<std::uint8_t> (buffer[0]) << 8) | static_cast<std::uint8_t> (buffer[1]);
    const auto payload_size =
      (static_cast<std::size_t> (static_cast<std::uint8_t> (buffer[2])) << 24)
      | (static_cast<std::size_t> (static_cast<std::uint8_t> (buffer[3])) << 16)
      | (static_cast<std::size_t> (static_cast<std::uint8_t> (buffer[4])) << 8)
      | static_cast<std::uint8_t> (buffer[5]);
    if (buffer.size () < 6 + header_size + payload_size) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> header_bytes (
      buffer.begin () + 6, buffer.begin () + 6 + static_cast<std::ptrdiff_t> (header_size));
    auto decoded = zlink::stream_connector::detail::header_codec_t{}.decode (header_bytes);
    if (!decoded) {
        return std::nullopt;
    }
    std::string payload (buffer.begin () + 6 + static_cast<std::ptrdiff_t> (header_size),
                         buffer.begin () + 6 + static_cast<std::ptrdiff_t> (header_size)
                           + static_cast<std::ptrdiff_t> (payload_size));
    buffer.erase (0, 6 + header_size + payload_size);
    return perf_server_frame_t{decoded.value (), std::move (payload)};
}

zlink::message_t make_server_frame (zlink::stream_connector::message_kind_t kind,
                                    std::uint64_t seq,
                                    std::string name,
                                    std::string payload)
{
    zlink::stream_connector::detail::stream_header_t header;
    header.kind = kind;
    header.codec = zlink::stream_connector::codec_t::raw;
    header.flags = zlink::stream_connector::header_flags_t::none;
    header.request_seq = kind == zlink::stream_connector::message_kind_t::request
                             || kind == zlink::stream_connector::message_kind_t::response
                             || kind == zlink::stream_connector::message_kind_t::error
                           ? std::optional<std::uint64_t>{seq}
                           : std::optional<std::uint64_t>{};
    header.name = std::move (name);
    auto header_bytes = zlink::stream_connector::detail::header_codec_t{}.encode (header);
    std::vector<std::uint8_t> payload_bytes (payload.begin (), payload.end ());
    zlink::stream_connector::connector_options_t options;
    options.max_send_payload_size = std::max (options.max_send_payload_size, payload_bytes.size ());
    auto frame = zlink::stream_connector::detail::frame_codec_t::encode (header_bytes.value (),
                                                                         payload_bytes, options);
    return zlink::message_t::from (std::string (frame.value ().begin (), frame.value ().end ()));
}

zlink::stream_e2e_client::task_t<zlink::message_t>
run_request (zlink::stream_e2e_client::coroutine_connector_t &client,
             std::size_t payload_bytes,
             std::chrono::milliseconds timeout)
{
    zlink::stream_connector::packet_t packet;
    packet.name = "connector.perf.request";
    packet.codec = zlink::stream_connector::codec_t::raw;
    packet.payload = zlink::message_t::from (std::string (payload_bytes, 'r'));
    auto reply = co_await client.request (std::move (packet))
                   .timeout (timeout)
                   .async<zlink::message_t> ();
    co_return reply;
}

zlink::stream_e2e_client::task_t<zlink::stream_connector::packet_t>
run_wait (zlink::stream_e2e_client::coroutine_connector_t &client,
          std::chrono::milliseconds timeout)
{
    auto packet = co_await client.wait_for<zlink::stream_connector::packet_t> ()
                    .packet_name ("connector.perf.push")
                    .timeout (timeout)
                    .async ();
    co_return packet;
}

std::thread start_loopback_server (zlink::context_t &context,
                                   std::string &endpoint,
                                   int expected_clients)
{
    auto server = std::make_shared<zlink::stream_socket_t> (context);
    server->options ().notify (false);
    server->bind ("tcp://127.0.0.1:0");
    endpoint = server->options ().last_endpoint ();
    return std::thread ([server, expected_clients] {
        try {
            std::map<std::string, std::string> buffers;
            int replied = 0;
            while (replied < expected_clients) {
                zlink::received_t inbound;
                if (server->recv (inbound) != 0) {
                    return;
                }
                if (!inbound.routing_id ()) {
                    inbound.close ();
                    continue;
                }
                auto &buffer = buffers[inbound.routing_id ()->to_hex ()];
                for (const auto &part : inbound.parts ())
                    buffer += part.to_string ();
                std::string outbound;
                while (auto frame = try_read_server_frame (buffer)) {
                    if (frame->header.kind == zlink::stream_connector::message_kind_t::request) {
                        // Response correlation is carried only by request_seq;
                        // response headers do not carry packet names.
                        auto response = make_server_frame (
                          zlink::stream_connector::message_kind_t::response,
                          frame->header.request_seq.value_or (0), {}, "ok");
                        outbound += response.to_string ();
                        auto push = make_server_frame (
                          zlink::stream_connector::message_kind_t::send, 0,
                          "connector.perf.push", "push");
                        outbound += push.to_string ();
                        ++replied;
                    }
                }
                if (!outbound.empty ()) {
                    inbound.send ().message (zlink::message_t::from (outbound)).submit ();
                }
                inbound.close ();
            }
        }
        catch (const std::exception &) {
            return;
        }
    });
}

zlink::stream_connector::transport_t parse_transport (const std::string &transport)
{
    if (transport == "tcp") {
        return zlink::stream_connector::transport_t::tcp;
    }
    if (transport == "tls") {
        return zlink::stream_connector::transport_t::tls;
    }
    if (transport == "websocket") {
        return zlink::stream_connector::transport_t::websocket;
    }
    if (transport == "websocket-secure") {
        return zlink::stream_connector::transport_t::websocket_secure;
    }
    throw std::invalid_argument ("transport must be tcp, tls, websocket, or websocket-secure");
}

zlink::stream_connector::dispatch_mode_t parse_dispatch_mode (const std::string &mode)
{
    if (mode == "manual") {
        return zlink::stream_connector::dispatch_mode_t::manual;
    }
    if (mode == "immediate") {
        return zlink::stream_connector::dispatch_mode_t::immediate;
    }
    throw std::invalid_argument ("dispatch mode must be manual or immediate");
}

perf_result_t run_endpoint_load (const perf_options_t &options)
{
    perf_result_t result;
    const auto started = std::chrono::steady_clock::now ();
    std::vector<zlink::stream_connector::connector_t> connectors;
    connectors.reserve (static_cast<std::size_t> (options.clients));
    for (int index = 0; index < options.clients; ++index) {
        zlink::stream_connector::connector_options_t connector_options;
        connector_options.endpoint = options.endpoint;
        connector_options.transport = parse_transport (options.transport);
        connector_options.dispatch_mode = parse_dispatch_mode (options.dispatch_mode);
        const auto request_timeout = std::chrono::milliseconds (options.request_timeout_ms);
        connector_options.request_timeout = request_timeout;
        auto connector = zlink::stream_connector::connector_factory_t::create (connector_options);
        auto connected = connector.connect ();
        if (!connected) {
            ++result.errors;
            continue;
        }
        ++result.connected;
        auto client = zlink::stream_e2e_client::use (connector);
        const auto wait_client_threshold = (index + 1) * 100 / options.clients;
        const bool should_wait = wait_client_threshold <= options.wait_clients_percent;
        auto wait_task = run_wait (client, request_timeout);
        if (should_wait) {
            wait_task.start ();
        }
        const auto request_started = std::chrono::steady_clock::now ();
        auto reply = run_request (client, static_cast<std::size_t> (options.payload_bytes),
                                  request_timeout)
                       .result ();
        const auto request_finished = std::chrono::steady_clock::now ();
        if (reply) {
            ++result.requests_completed;
            result.request_latencies_us.push_back (
              std::chrono::duration_cast<std::chrono::microseconds> (request_finished
                                                                     - request_started)
                .count ());
        } else {
            if (reply.error_code () == zlink::stream_connector::error_code_t::request_timeout) {
                ++result.timeouts;
            }
            ++result.errors;
        }
        if (should_wait) {
            auto waited = wait_task.result ();
            if (waited) {
                ++result.waits_completed;
            } else {
                if (waited.error_code () == zlink::stream_connector::error_code_t::request_timeout) {
                    ++result.timeouts;
                }
                ++result.errors;
            }
        }
        connectors.push_back (std::move (connector));
    }
    for (auto &connector : connectors) {
        connector.close ();
    }
    result.duration_ms =
      std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now ()
                                                            - started)
        .count ();
    finalize_perf_result (result);
    return result;
}

} // namespace

int main (int argc, char **argv)
{
    try {
        const auto options = parse_options (argc, argv);
        if (options.clients <= 0 || options.workers <= 0 || options.inflight <= 0
            || options.request_timeout_ms <= 0) {
            std::cerr << "clients, workers, inflight, and request timeout must be positive.\n";
            return 2;
        }
        if (!zlink::stream_connector::detail::configure_shared_runtime_worker_count (
              static_cast<std::size_t> (options.workers))) {
            std::cerr << "connector runtime worker count must be configured before use.\n";
            return 5;
        }
        if (options.dispatch_mode != "manual" && options.dispatch_mode != "immediate") {
            std::cerr << "dispatch mode must be manual or immediate.\n";
            return 3;
        }
        zlink::context_t context;
        std::thread loopback_server;
        auto endpoint_options = options;
        if (endpoint_options.loopback) {
            loopback_server = start_loopback_server (context, endpoint_options.endpoint,
                                                     endpoint_options.clients);
            endpoint_options.transport = "tcp";
        }
        if (options.endpoint.empty ()) {
            if (!endpoint_options.loopback) {
                write_report (options, "schema-smoke");
                return 0;
            }
        }
        auto result = run_endpoint_load (endpoint_options);
        if (loopback_server.joinable ()) {
            loopback_server.join ();
        }
        write_report (endpoint_options, endpoint_options.loopback ? "loopback-request-wait"
                                                                  : "endpoint-request-wait",
                      result);
        if (endpoint_options.loopback
            && (result.requests_completed != endpoint_options.clients || result.errors != 0)) {
            return 4;
        }
        return 0;
    }
    catch (const std::exception &ex) {
        std::cerr << ex.what () << "\n";
        return 1;
    }
}

#ifndef PERF_STREAM_CLIENT_OPTIONS_HPP
#define PERF_STREAM_CLIENT_OPTIONS_HPP

// CLI option parsing, per-case metrics collection, and stop-token helpers.
// Provides:
//   client_options_t       - all benchmark parameters with sensible defaults
//   case_metrics_t         - per-size result accumulator (throughput, latency, errors)
//   parse_options()        - populates client_options_t from argv
//   send_stop_token_once() - sends a stop token to the echo server

#include "perf_stream_arg_reader.hpp"
#include "perf_stream_common.hpp"
#include "stream_client.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

// All CLI-configurable benchmark parameters.
struct client_options_t
{
    std::string transport;
    std::string pattern;
    std::string host;
    int port;
    int ccu;
    std::vector<size_t> sizes;
    int runs;
    int duration;
    int completion_wait_ms;
    int size_transition_completion_wait_ms;
    int io_threads;
    std::string stop_token;
    int send_stop_token;

    client_options_t () :
        transport ("tcp"),
        pattern ("STREAM"),
        host ("127.0.0.1"),
        port (38001),
        ccu (10000),
        sizes (),
        runs (1),
        duration (10),
        completion_wait_ms (500),
        size_transition_completion_wait_ms (0),
        io_threads (4),
        stop_token ("__zlink_perf_stop__"),
        send_stop_token (0)
    {
        sizes.push_back (64);
        sizes.push_back (1024);
        sizes.push_back (65536);
    }
};

// Per-size benchmark result: throughput, latency percentiles, and error counts.
struct case_metrics_t
{
    long connect_ok;         // successful connections
    long connect_fail;       // failed connections
    double throughput_bps;   // bytes per second
    double throughput_mib_s; // MiB per second
    double mean_ns;          // mean RTT (nanoseconds)
    double p50_ns;           // 50th percentile RTT (nanoseconds)
    double p95_ns;           // 95th percentile RTT
    double p99_ns;           // 99th percentile RTT
    long send_error;
    long recv_error;
    long timeout_error;
    long size_mismatch;
    bool pass; // true when no errors and throughput > 0

    case_metrics_t () :
        connect_ok (0),
        connect_fail (0),
        throughput_bps (0.0),
        throughput_mib_s (0.0),
        mean_ns (0.0),
        p50_ns (0.0),
        p95_ns (0.0),
        p99_ns (0.0),
        send_error (0),
        recv_error (0),
        timeout_error (0),
        size_mismatch (0),
        pass (false)
    {
    }
};

// Populate client_options_t from "--key value" pairs in argv.
// --endpoint overrides --host/--port/--transport when present.
// Returns false on validation failure (invalid transport, sizes, etc.).
inline bool parse_options (int argc, char **argv, client_options_t &opt)
{
    const arg_reader_t args (argc, argv);

    opt.transport =
      perf_stream_common::lower_copy (args.get_string ("--transport", opt.transport.c_str ()));
    opt.pattern = args.get_string ("--pattern", opt.pattern.c_str ());
    opt.host = args.get_string ("--host", opt.host.c_str ());
    opt.port = args.get_int ("--port", opt.port, 1);
    opt.ccu = args.get_int ("--ccu", opt.ccu, 1);
    opt.runs = args.get_int ("--runs", opt.runs, 1);
    opt.duration = args.get_int ("--duration", opt.duration, 1);
    opt.completion_wait_ms = args.get_int ("--completion-wait-ms", opt.completion_wait_ms, 0);
    opt.size_transition_completion_wait_ms = args.get_int (
      "--size-transition-completion-wait-ms", opt.size_transition_completion_wait_ms, 0);
    opt.io_threads = args.get_int ("--io-threads", opt.io_threads, 1);
    opt.stop_token = args.get_string ("--stop-token", opt.stop_token.c_str ());
    opt.send_stop_token = args.get_int ("--send-stop-token", opt.send_stop_token, 0);

    const std::string endpoint = args.get_string ("--endpoint", "");
    if (!endpoint.empty ()) {
        std::string endpoint_transport;
        std::string endpoint_host;
        int endpoint_port = 0;
        if (!perf_stream_common::perf_stream_parse_endpoint (endpoint, &endpoint_transport,
                                                             &endpoint_host, &endpoint_port)) {
            std::fprintf (stderr, "invalid --endpoint: %s\n", endpoint.c_str ());
            return false;
        }
        if (opt.transport.empty ())
            opt.transport = endpoint_transport;
        else if (opt.transport != endpoint_transport)
            opt.transport = endpoint_transport;
        opt.host = endpoint_host;
        opt.port = endpoint_port;
    }

    if (opt.transport != "tcp" && opt.transport != "tls" && opt.transport != "ws"
        && opt.transport != "wss") {
        std::fprintf (stderr, "invalid --transport: %s\n", opt.transport.c_str ());
        return false;
    }

    const std::string sizes_text = args.get_string ("--sizes", "");
    if (!sizes_text.empty ()) {
        if (!perf_stream_common::perf_stream_parse_size_list (sizes_text, opt.sizes)) {
            std::fprintf (stderr, "invalid --sizes: %s\n", sizes_text.c_str ());
            return false;
        }
    }

    return true;
}

// Open a one-shot connection and send the stop token to the echo server.
// Used after the benchmark completes to signal the server to shut down.
inline bool send_stop_token_once (const client_options_t &opt)
{
    if (opt.send_stop_token <= 0)
        return true;

    stream_client_t client (opt.transport, opt.host, opt.port);
    if (!client.connect ())
        return false;

    const std::vector<unsigned char> token_payload (opt.stop_token.begin (), opt.stop_token.end ());
    return client.send_payload (token_payload);
}

#endif

// PUBSUB multi client benchmark: one-way subscriber receive workload.
// Topology: server PUB(bind, 1) -> client SUB(connect, N)
// Measurement: active-phase receive throughput + header-based latency sample.

#include "../common/perf_common.hpp"
#include "../common/perf_common_multi.hpp"
#include "../common/perf_entry.hpp"
#include "../common/perf_client_helpers.hpp"
#include "../common/perf_metric_header.hpp"

#include <algorithm>
#include <any>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <vector>

namespace
{

static const char *k_pattern_env = "PUBSUB";
static const char *k_pattern_result = "MULTI_PUBSUB";
static const char *k_topic = "bench";
static const uint32_t k_run_id = 1U;
struct phase_config_t
{
    int active_seconds;
};

struct bench_result_t
{
    unsigned long long active_count;
    perf::multi::bench_latency_stats_t latency;

    bench_result_t () : active_count (0), latency () {}
};

enum pubsub_recv_result_t
{
    pubsub_recv_error = -1,
    pubsub_recv_empty = 0,
    pubsub_recv_payload = 1,
    pubsub_recv_stop = 2
};

pubsub_recv_result_t recv_one_pubsub_message (zlink::sub_socket_t &sock,
                                              size_t expected_msg_size,
                                              perf_metric::header_t *header_out)
{
    std::optional<zlink::routing_id_t> source_rid;
    std::string topic;
    zlink::message_t part;
    bool has_more = false;

    const int rc = sock.subscribe_part (source_rid, topic, part, has_more,
                                        static_cast<int> (zlink::recv_flags_t::dontwait));
    if (rc != 0) {
        const int err = errno;
        if (err == EAGAIN || err == EINTR)
            return pubsub_recv_empty;
        return pubsub_recv_error;
    }

    if ((source_rid && source_rid->size () > 0) || topic != k_topic) {
        return pubsub_recv_payload;
    }

    const size_t recv_size = part.size ();
    const void *recv_data = part.data ();
    if (perf::multi::is_stop_token (recv_data, recv_size)) {
        if (has_more)
            return pubsub_recv_error;
        return pubsub_recv_stop;
    }

    if (perf::multi::measurement_part_count () == 1) {
        if (has_more)
            return pubsub_recv_error;
    } else {
        if (!has_more)
            return pubsub_recv_error;

        std::optional<zlink::routing_id_t> tail_source_rid;
        std::string tail_topic;
        zlink::message_t tail;
        bool tail_has_more = false;
        const int tail_rc = sock.subscribe_part (
          tail_source_rid, tail_topic, tail, tail_has_more,
          static_cast<int> (zlink::recv_flags_t::dontwait));
        if (tail_rc != 0 || tail_has_more || tail.size () != 0)
            return pubsub_recv_error;
    }

    perf_metric::header_t header;
    const bool decoded = perf_metric::decode_payload_header (recv_data, recv_size, &header);
    if (!decoded || header.magic != perf_metric::k_magic || header.run_id != k_run_id
        || header.msg_size != static_cast<uint32_t> (expected_msg_size)) {
        return pubsub_recv_payload;
    }

    if (header_out)
        *header_out = header;
    return pubsub_recv_payload;
}

class pubsub_client_bench_t
{
  public:
    pubsub_client_bench_t (const std::string &transport,
                           const std::string &lib_name,
                           size_t msg_size,
                           const std::string &endpoint,
                           const perf::multi::multi_bench_settings_t &settings) :
        _transport (transport),
        _lib_name (lib_name),
        _msg_size (msg_size),
        _endpoint (endpoint),
        _settings (settings),
        _ctx (),
        _holders (),
        _monitors (),
        _sockets (),
        _poller (),
        _poll_events (),
        _phase_cfg (),
        _result (),
        _failure_stage ("init")
    {
        _holders.reserve (_settings.clients);
        _monitors.reserve (_settings.clients);
        _sockets.reserve (_settings.clients);
        _poll_events.reserve (_settings.clients);

        _phase_cfg.active_seconds = std::max (1, _settings.duration_seconds);
    }

    bool run ()
    {
        if (!setup_sockets ())
            return false;

        std::cout << "CLIENT_READY," << _msg_size << std::endl;
        if (!perf::multi::wait_for_start_from_stdin (_msg_size)) {
            _failure_stage = "start_signal";
            return false;
        }

        if (!run_active_until_stop_token (std::chrono::seconds (_phase_cfg.active_seconds),
                                          &_result.active_count, &_result.latency))
            return false;

        if (_result.active_count == 0) {
            _failure_stage = "no_active_data";
            return false;
        }

        print_result ();
        std::cout << "CLIENT_DONE," << _msg_size << std::endl;
        return true;
    }

    const char *failure_stage () const { return _failure_stage; }

    unsigned long long active_count () const { return _result.active_count; }

  private:
    void close_monitors ()
    {
        for (size_t i = 0; i < _monitors.size (); ++i)
            perf::multi::close_connect_monitor (_monitors[i]);
    }

    bool setup_sockets ()
    {
        for (size_t i = 0; i < _settings.clients; ++i) {
            _holders.emplace_back (new zlink::sub_socket_t (_ctx.ctx ()));
            zlink::sub_socket_t &sock = *_holders.back ();

            (void) sock.set_subscription (std::string (k_topic));
            perf::multi::apply_benchmark_socket_options (sock, _settings, _transport);
            if (!perf::multi::setup_tls_client (sock, _transport))
                return false;
            _monitors.push_back (perf::multi::connect_monitor_t ());
            if (!perf::multi::open_connect_monitor (
                  sock, _settings.monitor_hwm, _monitors.back ())) {
                close_monitors ();
                return false;
            }
            try {
                sock.connect (_endpoint);
            }
            catch (const zlink::binding_error_t &) {
                close_monitors ();
                return false;
            }

            _sockets.push_back (&sock);
            _poller.add (sock, zlink::poll_event_flag_t::pollin, _sockets.size () - 1);
        }
        if (!perf::multi::recalculate_auto_hwm (_ctx))
            return false;
        const bool ready =
          perf::multi::wait_connect_ready_all (_monitors, _settings.connect_ready_timeout_ms);
        close_monitors ();
        if (!ready) {
            _failure_stage = "connect_ready";
            return false;
        }
        if (!_holders.empty () && _holders[0].get () && _holders[0]->valid ()) {
            perf::multi::emit_auto_hwm_detail (*_holders[0], "client", "endpoint", _transport,
                                               _msg_size, "sub");
        }
        return !_sockets.empty ();
    }

    // Active aggregation is bounded by the application clock. The server's
    // wire-level stop token still ends the phase early when delivered, but a
    // lost stop token must not leave the client parked in poll forever after
    // the active deadline has already elapsed.
    bool run_active_until_stop_token (std::chrono::milliseconds duration,
                                      unsigned long long *count_out,
                                      perf::multi::bench_latency_stats_t *lat_out)
    {
        if (duration.count () <= 0) {
            if (count_out)
                *count_out = 0;
            if (lat_out)
                *lat_out = perf::multi::bench_latency_stats_t ();
            return true;
        }

        if (_sockets.empty ())
            return false;

        perf::multi::bench_latency_sampler_t latency;
        unsigned long long count = 0;
        const auto deadline = std::chrono::steady_clock::now () + duration;
        bool phase_done = false;

        while (!phase_done) {
            const auto now = std::chrono::steady_clock::now ();
            if (now >= deadline)
                break;
            std::chrono::milliseconds poll_wait =
              std::chrono::duration_cast<std::chrono::milliseconds> (deadline - now);
            if (poll_wait.count () <= 0)
                poll_wait = std::chrono::milliseconds (1);

            if (_poll_events.size () < _sockets.size ())
                _poll_events.resize (_sockets.size ());
            const size_t poll_rc =
              _poller.wait (_poll_events.data (), _poll_events.size (), poll_wait);
            if (poll_rc == 0) {
                if (std::chrono::steady_clock::now () >= deadline)
                    break;
                continue;
            }

            for (size_t i = 0; i < poll_rc; ++i) {
                const size_t slot_index = _poll_events[i].slot;
                if (slot_index >= _sockets.size ())
                    continue;
                zlink::sub_socket_t *sock = _sockets[slot_index];
                if (!sock)
                    continue;

                for (;;) {
                    perf_metric::header_t header = {};
                    const pubsub_recv_result_t recv_rc =
                      recv_one_pubsub_message (*sock, _msg_size, &header);
                    if (recv_rc == pubsub_recv_error)
                        return false;
                    if (recv_rc == pubsub_recv_empty)
                        break;
                    if (recv_rc == pubsub_recv_stop) {
                        phase_done = true;
                        continue;
                    }

                    if (header.phase != static_cast<uint8_t> (perf_metric::phase_active)
                        || std::chrono::steady_clock::now () >= deadline) {
                        continue;
                    }

                    ++count;
                    if (lat_out) {
                        latency.add (perf_metric::elapsed_latency_ns (perf_metric::now_ns (),
                                                                      header.sent_ts_ns));
                    }
                }
            }
        }

        if (count_out)
            *count_out = count;
        if (lat_out)
            *lat_out = latency.snapshot ();
        return true;
    }

    void print_result () const
    {
        perf::multi::print_client_result_lines (_lib_name, k_pattern_result, _transport, _msg_size,
                                                _result.active_count, _phase_cfg.active_seconds,
                                                1.0, _result.latency);
    }

  private:
    const std::string _transport;
    const std::string _lib_name;
    const size_t _msg_size;
    const std::string _endpoint;
    const perf::multi::multi_bench_settings_t _settings;

    perf::multi::ctx_guard_t _ctx;
    std::vector<std::unique_ptr<zlink::sub_socket_t>> _holders;
    std::vector<perf::multi::connect_monitor_t> _monitors;
    std::vector<zlink::sub_socket_t *> _sockets;
    zlink::poller_t _poller;
    std::vector<zlink::poll_event_t> _poll_events;

    phase_config_t _phase_cfg;
    bench_result_t _result;
    const char *_failure_stage;
};

} // namespace

bool perf_pubsub_client (const std::string &lib_name,
                         const std::string &transport,
                         size_t msg_size,
                         const std::string &endpoint)
{
    perf::multi::set_perf_pattern_env (k_pattern_env);

    if (!perf::multi::is_supported_transport (transport)) {
        std::cout << "UNSUPPORTED," << lib_name << "," << k_pattern_result << "," << transport
                  << std::endl;
        return true;
    }

    const perf::multi::multi_bench_settings_t settings =
      perf::multi::resolve_multi_bench_settings ();

    pubsub_client_bench_t bench (transport, lib_name, msg_size, endpoint, settings);
    if (!bench.run ()) {
        std::cerr << "PUBSUB_CLIENT_FAIL,stage=" << bench.failure_stage ()
                  << ",transport=" << transport << ",size=" << msg_size
                  << ",active=" << bench.active_count () << std::endl;
        return false;
    }

    return true;
}

int main (int argc, char **argv)
{
    if (argc < 4) {
        std::cerr << "usage: <lib_name> <transport> <size> --endpoint <endpoint>" << std::endl;
        return 1;
    }

    const std::string lib_name = argv[1];
    const std::string transport = argv[2];
    const size_t size = static_cast<size_t> (std::strtoull (argv[3], NULL, 10));
    if (size == 0)
        return 1;

    const std::string endpoint = perf::multi::parse_endpoint_arg (argc, argv);
    if (endpoint.empty ()) {
        std::cerr << "missing --endpoint" << std::endl;
        return 1;
    }

    return perf_pubsub_client (lib_name, transport, size, endpoint) ? 0 : 1;
}

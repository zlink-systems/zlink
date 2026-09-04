// DEALER-ROUTER multi client benchmark: echo request/reply workload.
// Topology: client DEALER(connect, N) <-> server ROUTER(bind, 1)
// Measurement: active-phase echo throughput + RTT latency from stamped header.

#include "../common/perf_common.hpp"
#include "../common/perf_common_multi.hpp"
#include "../common/perf_entry.hpp"
#include "../common/perf_client_helpers.hpp"
#include "../common/perf_metric_header.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{

static const char *k_pattern_env = "DEALER_ROUTER_SENDSEND";
static const char *k_pattern_result = "MULTI_DEALER_ROUTER_SENDSEND";
static const char k_payload_fill = 'r';

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

struct socket_state_t
{
    zlink::dealer_socket_t *sock;
    std::vector<char> request_buffer;
    size_t payload_size;

    socket_state_t () :
        sock (NULL),
        request_buffer (),
        payload_size (0)
    {
    }
};

class dealer_router_client_bench_t
{
  public:
    dealer_router_client_bench_t (const std::string &transport,
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
        _socket_states (),
        _poller (),
        _run_id (1U),
        _seq (1),
        _phase_cfg (),
        _result ()
    {
        _holders.reserve (_settings.clients);
        _monitors.reserve (_settings.clients);
        _socket_states.reserve (_settings.clients);

        _phase_cfg.active_seconds = std::max (1, _settings.duration_seconds);
    }

    perf::async_task_t<bool> run ()
    {
        if (!setup_sockets ())
            co_return false;

        bool ok = true;
        if (!co_await run_phase (perf_metric::phase_active, _phase_cfg.active_seconds,
                                 &_result.active_count, &_result.latency))
            ok = false;

        if (ok)
            print_result ();
        // No stop token: matches the C reference echo client
        // (bindings/c/perf/multi/src/perf_multi_dealer_router_client.cpp),
        // which runs to its deadline and never signals the relay server.
        // The server is terminated via the run_comparison.py stdin STOP
        // path (and SIGTERM fallback). dotnet removed its equivalent
        // The stop token uses the same direct terminal for the same reason.
        co_return ok;
    }

  private:
    bool setup_sockets ()
    {
        try {
            const size_t payload_size = std::max<size_t> (_msg_size, perf_metric::header_size ());
            for (size_t i = 0; i < _settings.clients; ++i) {
                _holders.emplace_back (new zlink::dealer_socket_t (_ctx.ctx ()));
                zlink::dealer_socket_t &sock = *_holders.back ();

                const std::string routing_id = std::string ("dr_") + std::to_string (i);
                (void) sock.set_routing_id (zlink::routing_id_t::from (
                  reinterpret_cast<const uint8_t *> (routing_id.data ()), routing_id.size ()));

                perf::multi::apply_benchmark_socket_options (sock, _settings, _transport);
                if (!perf::multi::setup_tls_client (sock, _transport))
                    return false;
                _monitors.push_back (perf::multi::connect_monitor_t ());
                if (!perf::multi::open_connect_monitor (
                      sock, _settings.monitor_hwm, _monitors.back ()))
                    return false;
                sock.connect (_endpoint);

                socket_state_t state;
                state.sock = &sock;
                _socket_states.push_back (state);
                socket_state_t &slot = _socket_states.back ();
                slot.request_buffer.assign (payload_size, k_payload_fill);
                slot.payload_size = payload_size;
                _poller.add (
                  sock,
                  zlink::poll_event_flag_t::pollin
                    | zlink::poll_event_flag_t::pollout
                    | zlink::poll_event_flag_t::pollcompletion,
                  _socket_states.size () - 1);
            }

            const bool ready =
              perf::multi::wait_connect_ready_all (_monitors, _settings.connect_ready_timeout_ms);
            for (size_t i = 0; i < _monitors.size (); ++i)
                perf::multi::close_connect_monitor (_monitors[i]);
            if (!ready)
                return false;
            if (!perf::multi::recalculate_auto_hwm (_ctx))
                return false;
            if (!_holders.empty () && _holders[0].get () && _holders[0]->valid ()) {
                perf::multi::emit_auto_hwm_detail (*_holders[0], "client", "endpoint", _transport,
                                                   _msg_size, "dealer");
            }

            return !_socket_states.empty ();
        }
        catch (const zlink::binding_error_t &) {
            return false;
        }
    }

    perf::async_task_t<bool> run_sender (perf::application_ready_queue_t &ready_queue,
                                         socket_state_t &state,
                                         perf_metric::phase_t phase,
                                         std::chrono::steady_clock::time_point deadline)
    {
        std::vector<char> &request_buffer = state.request_buffer;
        if (!state.sock || request_buffer.empty ())
            co_return false;
        while (std::chrono::steady_clock::now () < deadline) {
            co_await ready_queue.schedule ();
            if (std::chrono::steady_clock::now () >= deadline)
                break;
            const uint64_t seq = _seq.fetch_add (1, std::memory_order_relaxed);
            if (!perf_metric::stamp_payload (
                  request_buffer.data (), state.payload_size, _run_id, phase, _msg_size, seq,
                  perf_metric::now_ns ()))
                co_return false;
            zlink::message_t request = zlink::message_t::from (
              std::as_bytes (std::span<const char> (request_buffer.data (), state.payload_size)));
            if (!request.valid ())
                co_return false;
            try {
                if (perf::multi::measurement_part_count () == 2) {
                    zlink::message_t tail = perf::multi::measurement_empty_part ();
                    co_await std::move (state.sock->send ().message (request)).message (tail)
                      .async ();
                } else {
                    co_await std::move (state.sock->send ()).message (request)
                      .async ();
                }
            }
            catch (const zlink::submit_error_t &err) {
                // Connect-monitor readiness can precede the first route
                // snapshot used by Core async admission. Keep retrying that
                // transient condition within the active application deadline.
                if (err.result () == zlink::submit_result_t::not_connected
                    || err.result () == zlink::submit_result_t::not_found)
                    continue;
                if (err.internal_errno () == EINTR)
                    continue;
                co_return false;
            }
        }
        co_return true;
    }

    int recv_reply (socket_state_t &state, perf_metric::header_t *header_out)
    {
        if (!state.sock || !header_out) {
            errno = EINVAL;
            return -1;
        }

        zlink::received_t reply;
        const int rc = state.sock->recv (reply, zlink::recv_flags_t::dontwait);
        if (rc != 0)
            return -1;

        if (!perf::multi::measurement_parts_valid (reply.parts ())) {
            errno = EPROTO;
            return -1;
        }

        const zlink::message_t &payload = reply.parts ().front ();
        const bool decoded =
          perf_metric::decode_payload_header (payload.data (), payload.size (), header_out);
        if (!decoded)
            return 1;
        return 0;
    }

    perf::async_task_t<bool> run_phase (perf_metric::phase_t phase,
                                        int seconds,
                                        unsigned long long *count_out,
                                        perf::multi::bench_latency_stats_t *lat_out)
    {
        if (seconds <= 0) {
            if (count_out)
                *count_out = 0;
            if (lat_out)
                *lat_out = perf::multi::bench_latency_stats_t ();
            co_return true;
        }

        if (_socket_states.empty ())
            co_return false;

        try {
            perf::multi::bench_latency_sampler_t latency;
            unsigned long long count = 0;
            // PERF_MULTI_TEST_POLICY § 1.3.1: the application deadline bounds an
            // otherwise signal-driven wait. POLLIN and public async send
            // progress share this poller, so no periodic wakeup fallback is
            // required.
            const auto deadline =
              std::chrono::steady_clock::now () + std::chrono::seconds (seconds);
            const auto drain_deadline =
              deadline + std::chrono::milliseconds (_settings.send_drain_timeout_ms);

            perf::application_poller_coordinator_t coordinator (
              _poller, _socket_states.size ());
            std::vector<perf::async_task_t<bool>> senders;
            senders.reserve (_socket_states.size ());
            for (size_t i = 0; i < _socket_states.size (); ++i)
                senders.emplace_back (
                  run_sender (coordinator.ready_queue (), _socket_states[i], phase, deadline));

            const auto dispatch_ready =
              [this, phase, deadline, lat_out, &latency, &count] (
                const zlink::poll_event_t *events_, size_t ready_count) {
                for (size_t i = 0; i < ready_count; ++i) {
                    const size_t slot_index = events_[i].slot;
                    if (slot_index >= _socket_states.size ())
                        continue;
                    socket_state_t *state = &_socket_states[slot_index];
                    if (!state->sock)
                        continue;

                    // POLLOUT/POLLCOMPLETION drive public async send and wake
                    // this application runtime. Neither is payload readiness;
                    // only POLLIN enters recv drain.
                    if (perf::poll_event_has (events_[i].revents,
                                              zlink::poll_event_flag_t::pollin)) {
                        for (;;) {
                            perf_metric::header_t header;
                            const int recv_rc = recv_reply (*state, &header);
                            if (recv_rc < 0) {
                                const int err = errno;
                                if (err == EAGAIN)
                                    break;
                                if (err == EINTR)
                                    continue;
                                return false;
                            }
                            if (recv_rc > 0) {
                                continue;
                            }
                            if (std::chrono::steady_clock::now () >= deadline)
                                break;
                            if (!perf_metric::is_expected (header, _run_id, phase,
                                                          _msg_size)) {
                                continue;
                            }

                            ++count;
                            if (lat_out && phase == perf_metric::phase_active) {
                                const int64_t now_ns = perf_metric::now_ns ();
                                if (header.sent_ts_ns > 0
                                    && now_ns >= header.sent_ts_ns) {
                                    const double latency_ns =
                                      static_cast<double> (
                                        now_ns - header.sent_ts_ns)
                                      * 0.5;
                                    latency.add (latency_ns);
                                }
                            }
                        }
                    }
                }
                return true;
            };
            if (!coordinator.run_until_senders_drained (
                  deadline, drain_deadline, senders, dispatch_ready)) {
                co_return false;
            }

            for (size_t i = 0; i < senders.size (); ++i) {
                if (!co_await std::move (senders[i]))
                    co_return false;
            }

            if (count == 0
                || (lat_out && phase == perf_metric::phase_active && latency.count () == 0))
                co_return false;

            if (count_out)
                *count_out = count;
            if (lat_out)
                *lat_out = latency.snapshot ();
            co_return true;
        }
        catch (const zlink::binding_error_t &) {
            co_return false;
        }
    }

    void print_result () const
    {
        perf::multi::print_client_result_lines (_lib_name, k_pattern_result, _transport, _msg_size,
                                                _result.active_count, _phase_cfg.active_seconds,
                                                2.0, _result.latency);
    }

  private:
    const std::string _transport;
    const std::string _lib_name;
    const size_t _msg_size;
    const std::string _endpoint;
    const perf::multi::multi_bench_settings_t _settings;

    perf::multi::ctx_guard_t _ctx;
    std::vector<std::unique_ptr<zlink::dealer_socket_t>> _holders;
    std::vector<perf::multi::connect_monitor_t> _monitors;
    std::vector<socket_state_t> _socket_states;
    zlink::poller_t _poller;

    const uint32_t _run_id;
    std::atomic<uint64_t> _seq;

    phase_config_t _phase_cfg;
    bench_result_t _result;
};

} // namespace

perf::async_task_t<bool> perf_dealer_router_client (const std::string &lib_name,
                                                    const std::string &transport,
                                                    size_t msg_size,
                                                    const std::string &endpoint)
{
    perf::multi::set_perf_pattern_env (k_pattern_env);

    if (!perf::multi::is_supported_transport (transport)) {
        std::cout << "UNSUPPORTED," << lib_name << "," << k_pattern_result << "," << transport
                  << std::endl;
        co_return true;
    }

    const perf::multi::multi_bench_settings_t settings =
      perf::multi::resolve_multi_bench_settings ();

    dealer_router_client_bench_t bench (transport, lib_name, msg_size, endpoint, settings);
    if (!co_await bench.run ()) {
        std::cerr << "DEALER_ROUTER_CLIENT_FAIL,transport=" << transport << ",size=" << msg_size
                  << std::endl;
        co_return false;
    }

    co_return true;
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

    return perf_dealer_router_client (lib_name, transport, size, endpoint).get () ? 0 : 1;
}

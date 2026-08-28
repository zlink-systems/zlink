// DEALER-ROUTER multi client benchmark: echo request/reply workload.
// Topology: client DEALER(connect, N) <-> server ROUTER(bind, 1)
// Measurement: active-phase echo throughput + RTT latency from stamped header.

#include "../common/perf_common.hpp"
#include "../common/perf_common_multi.hpp"
#include "../common/perf_entry.hpp"
#include "../common/perf_client_helpers.hpp"
#include "../common/perf_metric_header.hpp"

#include <algorithm>
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
    zlink::message_t request;
    zlink::message_t reply;
    size_t payload_size;
    bool use_per_socket_buffer;
    bool send_pending;
    bool pollout_enabled;

    socket_state_t () :
        sock (NULL),
        request_buffer (),
        request (),
        reply (),
        payload_size (0),
        use_per_socket_buffer (false),
        send_pending (false),
        pollout_enabled (false)
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
        _shared_request_buffer (),
        _socket_states (),
        _poller (),
        _poll_events (),
        _run_id (1U),
        _seq (1),
        _phase_cfg (),
        _result ()
    {
        _holders.reserve (_settings.clients);
        _monitors.reserve (_settings.clients);
        _socket_states.reserve (_settings.clients);
        _poll_events.reserve (_settings.clients);

        _phase_cfg.active_seconds = std::max (1, _settings.duration_seconds);
    }

    perf::async_task_t<bool> run ()
    {
        if (!setup_sockets ())
            co_return false;

        bool ok = true;
        if (!co_await run_phase (perf_metric::phase_active, _phase_cfg.active_seconds,
                                 &_result.active_count, &_result.latency)) {
            ok = false;
        }

        if (ok)
            print_result ();
        // No stop token: matches the C reference echo client
        // (bindings/c/perf/multi/src/perf_multi_dealer_router_client.cpp),
        // which runs to its deadline and never signals the relay server.
        // The server is terminated via the run_comparison.py stdin STOP
        // path (and SIGTERM fallback). dotnet removed its equivalent
        // TrySendStopToken for the same reason.
        co_return ok;
    }

  private:
    bool setup_sockets ()
    {
        try {
            const size_t payload_size = std::max<size_t> (_msg_size, perf_metric::header_size ());
            if (_transport != "tcp")
                _shared_request_buffer.assign (payload_size, k_payload_fill);

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
                if (!perf::multi::open_connect_monitor (sock, _monitors.back ()))
                    return false;
                sock.connect (_endpoint);

                socket_state_t state;
                state.sock = &sock;
                _socket_states.push_back (state);
                socket_state_t &slot = _socket_states.back ();
                slot.request_buffer.assign (_transport == "tcp" ? payload_size : 0, k_payload_fill);
                slot.payload_size = payload_size;
                // TCP keeps independent mutable stamp storage per socket.
                // Framed transports use the shared source buffer; message_t::from
                // takes the same owning copy in both cases.
                slot.use_per_socket_buffer = (_transport == "tcp");
                _poller.add (sock, zlink::poll_event_flag_t::pollin, _socket_states.size () - 1);
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

    // PERF_MULTI_TEST_POLICY § 1.3.1: pollers wait with timeout=-1
    // (signal-driven). The outer loops keep enforcing the wall-time
    // deadline via steady_clock checks.

    bool set_pollout (socket_state_t &state, bool enabled)
    {
        if (!state.sock)
            return false;
        if (state.pollout_enabled == enabled)
            return true;

        const zlink::poll_event_flag_t events =
          enabled ? (zlink::poll_event_flag_t::pollin | zlink::poll_event_flag_t::pollout)
                  : zlink::poll_event_flag_t::pollin;
        try {
            _poller.modify (*state.sock, events);
            state.pollout_enabled = enabled;
            return true;
        }
        catch (const zlink::binding_error_t &) {
            return false;
        }
    }

    int try_send_request (socket_state_t &state, perf_metric::phase_t phase)
    {
        std::vector<char> &request_buffer =
          state.use_per_socket_buffer ? state.request_buffer : _shared_request_buffer;
        if (!state.sock || request_buffer.empty ())
            return -1;

        const uint64_t sent_ts_ns = perf_metric::now_ns ();
        if (!perf_metric::stamp_payload (&request_buffer[0], state.payload_size, _run_id, phase,
                                         _msg_size, _seq, sent_ts_ns)) {
            return -1;
        }

        state.request = zlink::message_t::from (
          std::as_bytes (std::span<const char> (request_buffer.data (), state.payload_size)));
        if (!state.request.valid ()) {
            return -1;
        }

        try {
            if (perf::multi::measurement_part_count () == 2) {
                zlink::message_t tail = perf::multi::measurement_empty_part ();
                std::move (state.sock->send ()).message (state.request).message (tail)
                  .flags (zlink::send_flags_t::dontwait).submit ();
            } else {
                std::move (state.sock->send ()).message (state.request)
                  .flags (zlink::send_flags_t::dontwait).submit ();
            }
            ++_seq;
            state.send_pending = false;
            return 1;
        }
        catch (const zlink::submit_error_t &err) {
            const int err_no = err.internal_errno ();
            if (err_no == EAGAIN || err_no == EWOULDBLOCK) {
                state.send_pending = true;
                errno = err_no;
                return 0;
            }
            errno = err_no;
            return -1;
        }
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
            // PERF_MULTI_TEST_POLICY § 1.3.1: round-trip echo window is bounded
            // purely by an application clock (steady_clock deadline) plus a -1
            // (signal-driven) poll wait; no poller timer object is used. Matches
            // the C reference run_echo_window_round_robin
            // (bindings/c/perf/multi/common/perf_multi_client_helpers.hpp:901-1075).
            const auto deadline =
              std::chrono::steady_clock::now () + std::chrono::seconds (seconds);

            while (std::chrono::steady_clock::now () < deadline) {
                bool submitted = false;
                for (size_t attempt = 0; attempt < _socket_states.size (); ++attempt) {
                    socket_state_t &state = _socket_states[attempt];
                    if (!state.sock || state.send_pending)
                        continue;
                    const int send_rc = try_send_request (state, phase);
                    if (send_rc < 0)
                        co_return false;
                    if (send_rc == 0) {
                        if (!set_pollout (state, true))
                            co_return false;
                    } else {
                        submitted = true;
                    }
                }

                const size_t capacity = _socket_states.size () + 1;
                if (_poll_events.size () < capacity)
                    _poll_events.resize (capacity);
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
                  deadline - std::chrono::steady_clock::now ());
                const auto wait = submitted
                                    ? std::chrono::milliseconds (0)
                                    : std::chrono::milliseconds (std::max<int64_t> (
                                        1, std::min<int64_t> (50, remaining.count ())));
                const size_t ready_count = _poller.wait (
                  _poll_events.data (), capacity, wait);
                if (ready_count == 0)
                    continue;

                for (size_t i = 0; i < ready_count; ++i) {
                    const size_t slot_index = _poll_events[i].slot;
                    if (slot_index >= _socket_states.size ())
                        continue;
                    socket_state_t *state = &_socket_states[slot_index];
                    if (!state->sock)
                        continue;

                    if ((static_cast<short> (_poll_events[i].revents)
                         & static_cast<short> (zlink::poll_event_flag_t::pollin)) != 0)
                    for (;;) {
                        perf_metric::header_t header;
                        const int recv_rc = recv_reply (*state, &header);
                        if (recv_rc < 0) {
                            const int err = errno;
                            if (err == EAGAIN)
                                break;
                            if (err == EINTR)
                                continue;
                            co_return false;
                        }
                        if (recv_rc > 0) {
                            continue;
                        }
                        if (!perf_metric::is_expected (header, _run_id, phase, _msg_size)) {
                            continue;
                        }

                        ++count;
                        if (lat_out && phase == perf_metric::phase_active) {
                            const double latency_ns = perf_metric::elapsed_latency_ns (
                                                        perf_metric::now_ns (), header.sent_ts_ns)
                                                      * 0.5;
                            latency.add (latency_ns);
                        }

                    }

                    if ((static_cast<short> (_poll_events[i].revents)
                         & static_cast<short> (zlink::poll_event_flag_t::pollout)) != 0) {
                        state->send_pending = false;
                        if (!set_pollout (*state, false))
                            co_return false;
                    }
                }
            }

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
    std::vector<char> _shared_request_buffer;
    std::vector<socket_state_t> _socket_states;
    zlink::poller_t _poller;
    std::vector<zlink::poll_event_t> _poll_events;

    const uint32_t _run_id;
    uint64_t _seq;

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

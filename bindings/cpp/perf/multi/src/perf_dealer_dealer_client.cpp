// DEALER-DEALER multi client benchmark: one-way DEALER send workload.
// Topology: client DEALER(connect, N) -> server DEALER(bind, 1)
// Measurement: active-phase send throughput + sender-side send latency sample.

#include "../common/perf_common.hpp"
#include "../common/perf_common_multi.hpp"
#include "../common/perf_entry.hpp"
#include "../common/perf_client_helpers.hpp"
#include "../common/perf_metric_header.hpp"

#include <algorithm>
#include <any>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace
{

static const char *k_pattern_env = "DEALER_DEALER";
static const char *k_pattern_result = "MULTI_DEALER_DEALER";
static std::atomic<bool> g_stop_requested (false);

void on_signal (int)
{
    g_stop_requested.store (true, std::memory_order_release);
}

void install_signal_handlers ()
{
    std::signal (SIGINT, on_signal);
#if defined(SIGTERM)
    std::signal (SIGTERM, on_signal);
#endif
}

bool perf_debug_enabled ()
{
    return std::getenv ("PERF_DEBUG") != NULL;
}

void debug_log (const std::string &message_)
{
    if (!perf_debug_enabled ())
        return;
    std::cerr << "dealer_dealer client: " << message_ << std::endl;
}

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
    size_t payload_size;
    zlink::message_t message;
    bool pollout_enabled;
    bool pending;

    socket_state_t () :
        sock (NULL), payload_size (0), message (), pollout_enabled (false), pending (false)
    {
    }
};

class dealer_dealer_client_bench_t
{
  public:
    dealer_dealer_client_bench_t (const std::string &transport,
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

    bool run ()
    {
        if (!setup_sockets ())
            return false;
        refresh_auto_hwm_msg_unit ();

        std::cout << "CLIENT_READY," << _msg_size << std::endl;
        if (!perf::multi::wait_for_start_from_stdin (_msg_size))
            return false;

        if (!run_phase (perf_metric::phase_active, std::chrono::seconds (_phase_cfg.active_seconds),
                        &_result.active_count, NULL))
            return false;

        // Signal active-phase end to the measuring server. A large secure
        // transport case can leave some sockets backpressured at the end of
        // the active window; stop token delivery must not block forever on
        // one saturated socket before trying the rest.
        if (!send_stop_tokens ())
            return false;

        std::cout << "CLIENT_DONE," << _msg_size << std::endl;
        return _result.active_count > 0;
    }

  private:
    bool setup_sockets ()
    {
        try {
            for (size_t i = 0; i < _settings.clients; ++i) {
                _holders.emplace_back (new zlink::dealer_socket_t (_ctx.ctx ()));
                zlink::dealer_socket_t &sock = *_holders.back ();
                if (!sock.valid ()) {
                    debug_log ("socket create failed");
                    return false;
                }

                apply_dealer_socket_options (sock);
                if (!perf::multi::setup_tls_client (sock, _transport)) {
                    debug_log ("setup tls failed");
                    return false;
                }
                _monitors.push_back (perf::multi::connect_monitor_t ());
                zlink::socket_monitor_t monitor =
                  sock.monitor_open (zlink::monitor_event::connection_ready);
                if (!monitor.valid ()) {
                    debug_log ("open connect monitor failed");
                    return false;
                }
                _monitors.back ().monitor.reset (new zlink::socket_monitor_t (std::move (monitor)));
                sock.connect (_endpoint);

                socket_state_t state;
                state.sock = &sock;
                state.payload_size = std::max<size_t> (_msg_size, perf_metric::header_size ());
                _socket_states.push_back (state);
                (void) _poller.add (sock, zlink::poll_event_flag_t::pollout,
                                    _socket_states.size () - 1);
                (void) _poller.modify (sock, zlink::poll_event_flag_t::none);
            }

            const bool ready =
              perf::multi::wait_connect_ready_all (_monitors, _settings.connect_ready_timeout_ms);
            for (size_t i = 0; i < _monitors.size (); ++i)
                perf::multi::close_connect_monitor (_monitors[i]);
            if (!ready) {
                debug_log ("wait_connect_ready_all failed");
                return false;
            }

            return !_socket_states.empty ();
        }
        catch (const zlink::binding_error_t &) {
            return false;
        }
    }

    void refresh_auto_hwm_msg_unit ()
    {
        (void) perf::multi::apply_benchmark_auto_hwm_msg_unit (_ctx, _msg_size);
        (void) perf::multi::recalculate_auto_hwm (_ctx);
        if (!_holders.empty () && _holders[0].get () && _holders[0]->valid ()) {
            perf::multi::emit_auto_hwm_detail (*_holders[0], "client", "endpoint", _transport,
                                               _msg_size, "dealer");
        }
    }

    bool set_pollout (socket_state_t &state, bool enabled)
    {
        if (!state.sock)
            return false;
        if (state.pollout_enabled == enabled)
            return true;
        try {
            _poller.modify (*state.sock, enabled ? zlink::poll_event_flag_t::pollout
                                                 : zlink::poll_event_flag_t::none);
            state.pollout_enabled = enabled;
            return true;
        }
        catch (const zlink::binding_error_t &) {
            debug_log ("poller modify failed errno=" + std::to_string (errno));
            return false;
        }
    }

    bool try_send_once (socket_state_t &state,
                        perf_metric::phase_t phase,
                        unsigned long long *count,
                        perf::multi::bench_latency_sampler_t *lat_out)
    {
        if (!state.sock)
            return false;

        const bool collect_latency = lat_out && phase == perf_metric::phase_active;
        const auto t0 = collect_latency ? std::chrono::steady_clock::now ()
                                        : std::chrono::steady_clock::time_point ();
        bool sent = false;
        const size_t payload_size = state.payload_size;
        if (payload_size == 0) {
            debug_log ("payload size empty");
            return false;
        }
        state.message.init (payload_size);
        if (!state.message.valid ()) {
            debug_log ("message allocate failed");
            return false;
        }
        char *const payload = reinterpret_cast<char *> (state.message.data ());
        if (!payload) {
            debug_log ("message data missing");
            return false;
        }
        const uint64_t sent_ts_ns = perf_metric::now_ns ();
        if (!perf_metric::stamp_payload (payload, payload_size, _run_id, phase, _msg_size, _seq,
                                         sent_ts_ns)) {
            debug_log ("stamp payload failed");
            return false;
        }
        sent = state.sock->send ()
                 .message (state.message)
                 .flags (zlink::send_flags_t::dontwait)
                 .submit ();
        if (sent) {
            ++_seq;
            state.pending = false;
            if (state.pollout_enabled && !set_pollout (state, false))
                return false;
            if (count)
                ++(*count);
            if (collect_latency) {
                const auto t1 = std::chrono::steady_clock::now ();
                const double ns = static_cast<double> (
                  std::chrono::duration_cast<std::chrono::nanoseconds> (t1 - t0).count ());
                lat_out->add (ns);
            }
            return true;
        }

        if (!sent) {
            state.pending = true;
            return set_pollout (state, true);
        }

        return false;
    }

    bool try_send_stop_token (socket_state_t &state)
    {
        if (!state.sock)
            return false;
        const size_t token_size = std::strlen (perf::multi::k_stop_token);
        zlink::message_t part = zlink::message_t::from (
          std::as_bytes (std::span<const char> (perf::multi::k_stop_token, token_size)));
        if (!part.valid ())
            return false;

        try {
            const bool sent = std::move (state.sock->send ())
                                .message (part)
                                .flags (zlink::send_flags_t::dontwait)
                                .submit ();
            if (sent)
                return true;
        }
        catch (const zlink::submit_error_t &) {
            const int err = errno;
            if (err != EINTR && err != EAGAIN && err != EWOULDBLOCK && err != ETIMEDOUT)
                return false;
        }

        return false;
    }

    bool send_stop_tokens ()
    {
        const int wait_ms = std::max (1, _settings.sndtimeo_ms);
        const auto deadline =
          std::chrono::steady_clock::now () + std::chrono::milliseconds (wait_ms);
        std::vector<uint8_t> sent (_socket_states.size (), 0);
        size_t sent_count = 0;

        do {
            for (size_t i = 0; i < _socket_states.size (); ++i) {
                if (sent[i])
                    continue;
                if (try_send_stop_token (_socket_states[i])) {
                    sent[i] = 1;
                    ++sent_count;
                }
                const int err = errno;
                if (err != 0 && err != EINTR && err != EAGAIN && err != EWOULDBLOCK
                    && err != ETIMEDOUT)
                    return false;
            }
            if (sent_count == _socket_states.size ()
                || g_stop_requested.load (std::memory_order_acquire))
                return true;
            std::this_thread::yield ();
        } while (std::chrono::steady_clock::now () < deadline);

        debug_log ("stop token send incomplete; active deadline still bounds server");
        return true;
    }

    bool run_phase (perf_metric::phase_t phase,
                    std::chrono::steady_clock::duration duration,
                    unsigned long long *count_out,
                    perf::multi::bench_latency_stats_t *lat_out)
    {
        if (duration <= std::chrono::steady_clock::duration::zero ()) {
            if (count_out)
                *count_out = 0;
            if (lat_out)
                *lat_out = perf::multi::bench_latency_stats_t ();
            return true;
        }

        if (_socket_states.empty ())
            return false;

        try {
            perf::multi::bench_latency_sampler_t latency;
            unsigned long long count = 0;
            size_t index = 0;
            // PERF_MULTI_TEST_POLICY § 1.3.1: the active send window is bounded
            // purely by an application clock (steady_clock deadline); no poller
            // timer object is used. Matches the C reference run_send_window
            // (bindings/c/perf/multi/src/perf_multi_dealer_dealer_client.cpp:154-243).
            const auto deadline = std::chrono::steady_clock::now () + duration;
            while (std::chrono::steady_clock::now () < deadline) {
                bool progress = false;

                for (size_t i = 0; i < _socket_states.size (); ++i) {
                    socket_state_t &state = _socket_states[(index + i) % _socket_states.size ()];
                    if (state.pending || !state.sock)
                        continue;
                    while (!state.pending && std::chrono::steady_clock::now () < deadline) {
                        if (!try_send_once (state, phase, &count, lat_out ? &latency : NULL))
                            return false;
                        progress = true;
                    }
                }
                ++index;

                bool has_pending = false;
                for (size_t i = 0; i < _socket_states.size (); ++i) {
                    if (_socket_states[i].pending) {
                        has_pending = true;
                        break;
                    }
                }

                if (progress || !has_pending)
                    continue;

                // PERF_MULTI_TEST_POLICY § 1.3.1: signal-driven wait, no
                // timer cap. Outer loop bounds total wall-time via the
                // steady_clock deadline check above.
                // PERF_HOT_PATH: retain one event slot per client socket. Limiting
                // this buffer to one ready socket serializes backpressure recovery
                // and changes the C reference's all-ready-sockets scheduling.
                if (_poll_events.size () != _socket_states.size ())
                    _poll_events.resize (_socket_states.size ());
                const size_t ready_count =
                  _poller.wait (_poll_events.data (), _poll_events.size (),
                                std::chrono::milliseconds (-1));
                if (ready_count == 0)
                    continue;

                for (size_t i = 0; i < ready_count; ++i) {
                    const size_t slot_index = _poll_events[i].slot;
                    if (slot_index >= _socket_states.size ())
                        continue;
                    socket_state_t *state = &_socket_states[slot_index];
                    if (!state->pending
                        || !(static_cast<short> (_poll_events[i].revents)
                             & static_cast<short> (zlink::poll_event_flag_t::pollout))) {
                        continue;
                    }
                    do {
                        if (!try_send_once (*state, phase, &count, lat_out ? &latency : NULL))
                            return false;
                    } while (!state->pending && std::chrono::steady_clock::now () < deadline);
                }
            }

            if (count_out)
                *count_out = count;
            if (lat_out)
                *lat_out = latency.snapshot ();
            return true;
        }
        catch (const zlink::binding_error_t &) {
            return false;
        }
    }

  private:
    void apply_dealer_socket_options (zlink::dealer_socket_t &socket)
    {
        zlink::dealer_socket_options_t options = socket.options ();
        if (perf::multi::manual_socket_overrides_enabled ()) {
            options.send_hwm (
              zlink::byte_count_t::bytes (
                static_cast<uint64_t> (_settings.sndhwm > 0 ? _settings.sndhwm : 1)));
            options.recv_hwm (
              zlink::byte_count_t::bytes (
                static_cast<uint64_t> (_settings.rcvhwm > 0 ? _settings.rcvhwm : 1)));
        }
        options.send_timeout (std::chrono::milliseconds (_settings.sndtimeo_ms));
        options.recv_timeout (std::chrono::milliseconds (_settings.rcvtimeo_ms));
        options.linger (std::chrono::milliseconds (0));
    }

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
    std::vector<zlink::poll_event_t> _poll_events;

    const uint32_t _run_id;
    uint64_t _seq;

    phase_config_t _phase_cfg;
    bench_result_t _result;
};

} // namespace

bool perf_dealer_dealer_client (const std::string &lib_name,
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

    g_stop_requested.store (false, std::memory_order_release);
    install_signal_handlers ();

    dealer_dealer_client_bench_t bench (transport, lib_name, msg_size, endpoint, settings);
    if (!bench.run ()) {
        std::cerr << "DEALER_DEALER_CLIENT_FAIL,transport=" << transport << ",size=" << msg_size
                  << ",errno=" << errno << std::endl;
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

    return perf_dealer_dealer_client (lib_name, transport, size, endpoint) ? 0 : 1;
}

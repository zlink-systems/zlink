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

bool wait_for_runner_release_after_done ()
{
    const char *const enabled =
      std::getenv ("PERF_WAIT_SERVER_STOP_AFTER_CLIENT_DONE");
    if (!enabled || std::strcmp (enabled, "1") != 0)
        return true;

    std::string line;
    while (std::getline (std::cin, line)) {
        if (!line.empty () && line[line.size () - 1] == '\r')
            line.erase (line.size () - 1);
        if (line == "STOP" || line == "QUIT")
            return true;
    }
    return false;
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
    std::vector<zlink::message_t> parts;
    bool send_pending;

    socket_state_t () : sock (NULL), payload_size (0), parts (), send_pending (false) {}
};

enum send_status_t
{
    send_status_sent = 0,
    send_status_blocked = 1,
    send_status_fatal = 2
};

enum stop_token_status_t
{
    stop_token_sent = 0,
    stop_token_retry = 1,
    stop_token_fatal = 2
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

    perf::async_task_t<bool> run ()
    {
        if (!setup_sockets ())
            co_return false;
        refresh_auto_hwm ();

        std::cout << "CLIENT_READY," << _msg_size << std::endl;
        if (!perf::multi::wait_for_start_from_stdin (_msg_size))
            co_return false;

        if (!run_phase (perf_metric::phase_active,
                        std::chrono::seconds (_phase_cfg.active_seconds),
                        &_result.active_count))
            co_return false;

        // Signal active-phase end to the measuring server. Each socket uses
        // the C reference's blocking stop-token retry so the receiver's
        // wire-level phase boundary is not lost under backpressure.
        if (!co_await send_stop_tokens ())
            co_return false;

        std::cout << "CLIENT_DONE," << _msg_size << std::endl;
        // Keep the socket/context alive until the measuring server has consumed
        // every queued payload and per-socket stop token. With linger=0, exiting
        // here can discard stop tokens that are still behind the active backlog.
        if (!wait_for_runner_release_after_done ())
            co_return false;
        co_return _result.active_count > 0;
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
                state.parts.resize (
                  static_cast<size_t> (perf::multi::measurement_part_count ()));
                _socket_states.push_back (state);
                _poller.add (sock, zlink::poll_event_flag_t::none,
                             _socket_states.size () - 1);
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

    void refresh_auto_hwm ()
    {
        (void) perf::multi::recalculate_auto_hwm (_ctx);
        if (!_holders.empty () && _holders[0].get () && _holders[0]->valid ()) {
            perf::multi::emit_auto_hwm_detail (*_holders[0], "client", "endpoint", _transport,
                                               _msg_size, "dealer");
        }
    }

    send_status_t try_send_once (socket_state_t &state, perf_metric::phase_t phase)
    {
        if (!state.sock)
            return send_status_fatal;

        const size_t payload_size = state.payload_size;
        if (payload_size == 0) {
            debug_log ("payload size empty");
            return send_status_fatal;
        }
        if (state.parts.size ()
            != static_cast<size_t> (perf::multi::measurement_part_count ())) {
            debug_log ("message part count mismatch");
            return send_status_fatal;
        }
        state.parts[0].init (payload_size);
        if (!state.parts[0].valid ()) {
            debug_log ("message allocate failed");
            return send_status_fatal;
        }
        if (state.parts.size () == 2)
            state.parts[1] = perf::multi::measurement_empty_part ();
        char *const payload = reinterpret_cast<char *> (state.parts[0].data ());
        if (!payload) {
            debug_log ("message data missing");
            return send_status_fatal;
        }
        const uint64_t sent_ts_ns = perf_metric::now_ns ();
        if (!perf_metric::stamp_payload (payload, payload_size, _run_id, phase, _msg_size, _seq,
                                         sent_ts_ns)) {
            debug_log ("stamp payload failed");
            return send_status_fatal;
        }
        ++_seq;
        try {
            if (state.parts.size () == 2) {
                std::move (state.sock->send ().message (state.parts[0]))
                  .message (state.parts[1])
                  .flags (zlink::send_flags_t::dontwait)
                  .submit ();
            } else {
                std::move (state.sock->send ())
                  .message (state.parts[0])
                  .flags (zlink::send_flags_t::dontwait)
                  .submit ();
            }
            return send_status_sent;
        }
        catch (const zlink::submit_error_t &err) {
            if (err.result () == zlink::submit_result_t::backpressured
                || err.internal_errno () == EAGAIN || err.internal_errno () == EWOULDBLOCK)
                return send_status_blocked;
            return send_status_fatal;
        }
    }

    perf::async_task_t<stop_token_status_t> try_send_stop_token (socket_state_t &state)
    {
        if (!state.sock)
            co_return stop_token_fatal;
        const size_t token_size = std::strlen (perf::multi::k_stop_token);
        zlink::message_t part = zlink::message_t::from (
          std::as_bytes (std::span<const char> (perf::multi::k_stop_token, token_size)));
        if (!part.valid ())
            co_return stop_token_fatal;

        try {
            co_await std::move (state.sock->send ()).message (part).async ();
            co_return stop_token_sent;
        }
        catch (const zlink::submit_error_t &error_) {
            const int err = error_.internal_errno ();
            debug_log ("stop token submit failed errno=" + std::to_string (err));
            if (err != EINTR && err != EAGAIN && err != EWOULDBLOCK && err != ETIMEDOUT)
                co_return stop_token_fatal;
            co_return stop_token_retry;
        }

        co_return stop_token_fatal;
    }

    perf::async_task_t<bool> send_stop_tokens ()
    {
        for (size_t i = 0; i < _socket_states.size (); ++i) {
            while (!g_stop_requested.load (std::memory_order_acquire)) {
                const stop_token_status_t status =
                  co_await try_send_stop_token (_socket_states[i]);
                if (status == stop_token_sent)
                    break;
                if (status == stop_token_fatal)
                    co_return false;
                std::this_thread::yield ();
            }
            if (g_stop_requested.load (std::memory_order_acquire))
                co_return true;
        }

        co_return true;
    }

    bool run_phase (perf_metric::phase_t phase,
                    std::chrono::steady_clock::duration duration,
                    unsigned long long *count_out)
    {
        if (duration <= std::chrono::steady_clock::duration::zero ()) {
            if (count_out)
                *count_out = 0;
            return true;
        }

        if (_socket_states.empty ())
            return false;

        try {
            unsigned long long count = 0;
            const auto deadline = std::chrono::steady_clock::now () + duration;
            while (std::chrono::steady_clock::now () < deadline) {
                size_t pending_count = 0;
                for (size_t i = 0; i < _socket_states.size (); ++i) {
                    socket_state_t &state = _socket_states[i];
                    if (!state.sock)
                        continue;
                    if (state.send_pending) {
                        ++pending_count;
                        continue;
                    }

                    while (std::chrono::steady_clock::now () < deadline) {
                        const send_status_t status = try_send_once (state, phase);
                        if (status == send_status_sent) {
                            ++count;
                            continue;
                        }
                        if (status == send_status_blocked) {
                            state.send_pending = true;
                            _poller.modify (*state.sock, zlink::poll_event_flag_t::pollout);
                            ++pending_count;
                            break;
                        }
                        return false;
                    }
                }

                if (std::chrono::steady_clock::now () >= deadline || pending_count == 0)
                    continue;

                if (_poll_events.size () < pending_count)
                    _poll_events.resize (pending_count);
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
                  deadline - std::chrono::steady_clock::now ());
                const size_t poll_rc = _poller.wait (
                  _poll_events.data (), _poll_events.size (),
                  std::chrono::milliseconds (std::max<int64_t> (1, remaining.count ())));
                for (size_t i = 0; i < poll_rc; ++i) {
                    const size_t slot = _poll_events[i].slot;
                    if (slot >= _socket_states.size ()
                        || (static_cast<int> (_poll_events[i].revents)
                            & static_cast<int> (zlink::poll_event_flag_t::pollout)) == 0)
                        continue;
                    socket_state_t &state = _socket_states[slot];
                    state.send_pending = false;
                    _poller.modify (*state.sock, zlink::poll_event_flag_t::none);
                }
            }

            if (count_out)
                *count_out = count;
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

perf::async_task_t<bool> perf_dealer_dealer_client (const std::string &lib_name,
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

    g_stop_requested.store (false, std::memory_order_release);
    install_signal_handlers ();

    dealer_dealer_client_bench_t bench (transport, lib_name, msg_size, endpoint, settings);
    if (!co_await bench.run ()) {
        std::cerr << "DEALER_DEALER_CLIENT_FAIL,transport=" << transport << ",size=" << msg_size
                  << ",errno=" << errno << std::endl;
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

    return perf_dealer_dealer_client (lib_name, transport, size, endpoint).get () ? 0 : 1;
}

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

    socket_state_t () : sock (NULL), payload_size (0) {}
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
        refresh_auto_hwm ();

        std::cout << "CLIENT_READY," << _msg_size << std::endl;
        if (!perf::multi::wait_for_start_from_stdin (_msg_size))
            co_return false;

        if (!co_await run_phase (perf_metric::phase_active,
                                 std::chrono::seconds (_phase_cfg.active_seconds),
                                 &_result.active_count))
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
                zlink::socket_monitor_t monitor = sock.monitor_open (
                  zlink::monitor_event::connection_ready,
                  zlink::byte_count_t::bytes (_settings.monitor_hwm));
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

    perf::async_task_t<bool> run_sender (perf::application_ready_queue_t &ready_queue,
                                         socket_state_t &state,
                                         perf_metric::phase_t phase,
                                         std::chrono::steady_clock::time_point deadline,
                                         unsigned long long &count)
    {
        if (!state.sock)
            co_return false;

        const size_t payload_size = state.payload_size;
        if (payload_size == 0)
            co_return false;

        // async_task_t is eager. Register every sender with the shared
        // application-thread scheduler once; after that, an immediately
        // admitted public async send may submit the next message directly.
        // A genuinely pending send still suspends and resumes through this
        // queue, so Core backpressure remains the fairness boundary.
        co_await ready_queue.schedule ();
        while (std::chrono::steady_clock::now () < deadline) {
            zlink::message_t payload;
            payload.init (payload_size);
            if (!payload.valid ())
                co_return false;
            const uint64_t seq = _seq.fetch_add (1, std::memory_order_relaxed);
            if (!perf_metric::stamp_payload (payload.data (), payload_size, _run_id, phase,
                                             _msg_size, seq, perf_metric::now_ns ()))
                co_return false;
            try {
                if (perf::multi::measurement_part_count () == 2) {
                    zlink::message_t tail = perf::multi::measurement_empty_part ();
                    co_await std::move (state.sock->send ().message (payload)).message (tail)
                      .async ();
                } else {
                    co_await std::move (state.sock->send ()).message (payload)
                      .async ();
                }
                if (std::chrono::steady_clock::now () < deadline)
                    ++count;
            }
            catch (const zlink::submit_error_t &err) {
                if (err.internal_errno () == EINTR)
                    continue;
                co_return false;
            }
        }
        co_return true;
    }

    perf::async_task_t<bool> run_phase (perf_metric::phase_t phase,
                                        std::chrono::steady_clock::duration duration,
                                        unsigned long long *count_out)
    {
        if (duration <= std::chrono::steady_clock::duration::zero ()) {
            if (count_out)
                *count_out = 0;
            co_return true;
        }

        if (_socket_states.empty ())
            co_return false;

        // Every sender continuation runs through ready_queue on this application
        // thread, so this phase counter has a single writer.
        unsigned long long count = 0;
        const auto deadline = std::chrono::steady_clock::now () + duration;
        const auto drain_deadline =
          deadline + std::chrono::milliseconds (_settings.send_drain_timeout_ms);
        perf::application_ready_queue_t ready_queue;
        std::vector<perf::async_task_t<bool>> senders;
        senders.reserve (_socket_states.size ());
        for (size_t i = 0; i < _socket_states.size (); ++i)
            senders.emplace_back (
              run_sender (ready_queue, _socket_states[i], phase, deadline, count));

        const auto all_senders_done = [&senders] {
            return std::all_of (senders.begin (), senders.end (),
                                [] (const perf::async_task_t<bool> &sender_) {
                                    return sender_.done ();
                                });
        };
        while (std::chrono::steady_clock::now () < deadline) {
            (void) ready_queue.wait_and_run_ready_round_until (
              deadline, _socket_states.size ());
        }
        while (!all_senders_done ()) {
            // Run the local deadline-check snapshot before considering abort.
            // That leaves only public-async suspended operations to cancel if
            // the bounded drain expires; no queued raw coroutine handle is
            // destroyed behind application_ready_queue_t's back.
            if (ready_queue.run_ready_round (_socket_states.size ()) != 0)
                continue;
            if (std::chrono::steady_clock::now () >= drain_deadline)
                co_return false;
            (void) ready_queue.wait_and_run_ready_round_until (
              drain_deadline, _socket_states.size ());
        }

        for (size_t i = 0; i < senders.size (); ++i) {
            if (!co_await std::move (senders[i]))
                co_return false;
        }

        // Keep stop-token sends on the same application runtime. A pending
        // public completion otherwise resumes in Core's callback context, where
        // submitting the next socket's token is rejected with EDEADLK.
        std::vector<perf::async_task_t<bool>> stop_senders;
        stop_senders.reserve (_socket_states.size ());
        for (size_t i = 0; i < _socket_states.size (); ++i)
            stop_senders.emplace_back (
              perf::multi::send_stop_token_until_admitted (
                ready_queue, _socket_states[i].sock, g_stop_requested));

        const auto all_stops_done = [&stop_senders] {
            return std::all_of (stop_senders.begin (), stop_senders.end (),
                                [] (const perf::async_task_t<bool> &sender_) {
                                    return sender_.done ();
                                });
        };
        while (!all_stops_done ()) {
            if (ready_queue.run_ready_round (_socket_states.size ()) != 0)
                continue;
            // Stop-token admission intentionally ignores the active/drain
            // deadlines. Block until Core completes at least one pending send;
            // the comparison runner owns the external failure timeout.
            (void) ready_queue.wait_and_run_ready_round (
              _socket_states.size ());
        }
        for (size_t i = 0; i < stop_senders.size (); ++i) {
            if (!co_await std::move (stop_senders[i]))
                co_return false;
        }

        if (count_out)
            *count_out = count;
        co_return true;
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
    const uint32_t _run_id;
    std::atomic<uint64_t> _seq;

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

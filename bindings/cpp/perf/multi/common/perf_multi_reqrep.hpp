#ifndef PERF_CPP_MULTI_REQREP_HPP
#define PERF_CPP_MULTI_REQREP_HPP

#include "perf_common.hpp"
#include "perf_common_multi.hpp"
#include "perf_client_helpers.hpp"
#include "perf_entry.hpp"
#include "perf_metric_header.hpp"

#include <atomic>
#include <csignal>
#include <memory>
#include <mutex>
#include <type_traits>

namespace perf
{
namespace multi
{
namespace reqrep
{

struct config_t
{
    const char *env_pattern;
    const char *result_pattern;
    bool routed_client;
    bool server_has_routing_id;
};

inline bool transient (int err_)
{
    return err_ == EAGAIN || err_ == EWOULDBLOCK || err_ == EINTR || err_ == ETIMEDOUT
           || err_ == EHOSTUNREACH || err_ == ENOTCONN;
}

template <typename SocketT> struct client_slot_t
{
    client_slot_t () : socket (), payload (), next_seq (1), waiting (false), owner (NULL) {}

    std::unique_ptr<SocketT> socket;
    std::vector<char> payload;
    uint64_t next_seq;
    std::atomic<bool> waiting;
    void *owner;
};

template <typename SocketT> class client_bench_t
{
  public:
    client_bench_t (const config_t &config_,
                    const std::string &lib_name_,
                    const std::string &transport_,
                    size_t msg_size_,
                    const std::string &endpoint_,
                    const multi_bench_settings_t &settings_) :
        _config (config_),
        _lib_name (lib_name_),
        _transport (transport_),
        _msg_size (msg_size_),
        _endpoint (endpoint_),
        _target_rid (zlink::routing_id_t::from (std::string ("SERVER"))),
        _settings (settings_),
        _ctx (),
        _slots (),
        _monitors (),
        _poller (),
        _events (),
        _run_id (1U),
        _deadline_ns (0),
        _completed (0),
        _fatal (false),
        _latency ()
    {
    }

    bool run ()
    {
        if (!setup ())
            return false;

        const int duration_s = std::max (1, _settings.duration_seconds);
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (duration_s);
        _deadline_ns.store (perf_metric::now_ns ()
                              + static_cast<uint64_t> (duration_s) * 1000000000ULL,
                            std::memory_order_release);

        while (std::chrono::steady_clock::now () < deadline
               && !_fatal.load (std::memory_order_acquire)) {
            bool submitted = false;
            for (size_t i = 0; i < _slots.size (); ++i) {
                if (_slots[i]->waiting.load (std::memory_order_acquire))
                    continue;
                if (!submit (*_slots[i]))
                    return false;
                submitted = submitted || _slots[i]->waiting.load (std::memory_order_acquire);
            }
            if (!submitted) {
                try {
                    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
                      deadline - std::chrono::steady_clock::now ());
                    const int wait_ms = static_cast<int> (
                      std::max<int64_t> (1, std::min<int64_t> (50, remaining.count ())));
                    (void) _poller.wait (_events.data (), _events.size (),
                                         std::chrono::milliseconds (wait_ms));
                }
                catch (const zlink::binding_error_t &err) {
                    if (err.internal_errno () != EINTR)
                        return false;
                }
            }
        }

        const auto drain_deadline =
          std::chrono::steady_clock::now () + std::chrono::milliseconds (
                                                std::max (1000, _settings.rcvtimeo_ms * 4));
        while (any_waiting () && std::chrono::steady_clock::now () < drain_deadline) {
            try {
                (void) _poller.wait (_events.data (), _events.size (),
                                     std::chrono::milliseconds (50));
            }
            catch (const zlink::binding_error_t &err) {
                if (err.internal_errno () != EINTR)
                    return false;
            }
        }

        const unsigned long long completed = _completed.load (std::memory_order_acquire);
        if (_fatal.load (std::memory_order_acquire) || any_waiting () || completed == 0)
            return false;
        const bench_latency_stats_t latency = _latency.snapshot ();
        print_client_result_lines (_lib_name, _config.result_pattern, _transport, _msg_size,
                                   completed, duration_s, 2.0, latency);
        return true;
    }

  private:
    bool setup ()
    {
        try {
            const size_t payload_size = std::max<size_t> (_msg_size, perf_metric::header_size ());
            _slots.reserve (_settings.clients);
            _monitors.reserve (_settings.clients);
            for (size_t i = 0; i < _settings.clients; ++i) {
                std::unique_ptr<client_slot_t<SocketT>> slot (new client_slot_t<SocketT> ());
                slot->socket.reset (new SocketT (_ctx.ctx ()));
                if (!slot->socket || !slot->socket->valid ())
                    return false;
                const std::string rid = std::string ("client_") + std::to_string (i);
                slot->socket->set_routing_id (zlink::routing_id_t::from (
                  reinterpret_cast<const uint8_t *> (rid.data ()), rid.size ()));
                if constexpr (std::is_same<SocketT, zlink::router_socket_t>::value) {
                    slot->socket->options ().connect_routing_id (_target_rid);
                }
                apply_benchmark_socket_options (*slot->socket, _settings, _transport);
                if (!apply_benchmark_auto_hwm_msg_unit (_ctx, _msg_size)
                    || !setup_tls_client (*slot->socket, _transport))
                    return false;
                _monitors.push_back (connect_monitor_t ());
                if (!open_connect_monitor (*slot->socket, _monitors.back ()))
                    return false;
                slot->socket->connect (_endpoint);
                slot->payload.assign (payload_size, 'q');
                slot->owner = this;
                _poller.add (*slot->socket, zlink::poll_event_flag_t::pollcompletion, i);
                _slots.push_back (std::move (slot));
            }
            const bool ready = wait_connect_ready_all (_monitors, _settings.connect_ready_timeout_ms);
            for (size_t i = 0; i < _monitors.size (); ++i)
                close_connect_monitor (_monitors[i]);
            if (!ready || !recalculate_auto_hwm (_ctx))
                return false;
            _events.resize (_slots.size ());
            return !_slots.empty ();
        }
        catch (const zlink::binding_error_t &) {
            return false;
        }
    }

    bool submit (client_slot_t<SocketT> &slot_)
    {
        const uint64_t sent_ns = perf_metric::now_ns ();
        if (!perf_metric::stamp_payload (slot_.payload.data (), slot_.payload.size (), _run_id,
                                         perf_metric::phase_active, _msg_size, slot_.next_seq,
                                         sent_ns))
            return false;
        zlink::message_t request = zlink::message_t::from (
          std::as_bytes (std::span<const char> (slot_.payload.data (), slot_.payload.size ())));
        if (!request.valid ())
            return false;

        slot_.waiting.store (true, std::memory_order_release);
        try {
            client_slot_t<SocketT> *slot_ptr = &slot_;
            const auto callback = [slot_ptr] (zlink::request_result_t result,
                                              std::vector<zlink::message_t> parts) {
                client_bench_t *owner = static_cast<client_bench_t *> (slot_ptr->owner);
                slot_ptr->waiting.store (false, std::memory_order_release);
                if (!owner)
                    return;
                if (result != zlink::request_result_t::ok || parts.empty ()) {
                    if (result != zlink::request_result_t::timed_out)
                        owner->_fatal.store (true, std::memory_order_release);
                    return;
                }
                perf_metric::header_t header;
                if (!perf_metric::decode_payload_header (parts.front ().data (),
                                                         parts.front ().size (), &header)
                    || !perf_metric::is_expected (header, owner->_run_id,
                                                  perf_metric::phase_active, owner->_msg_size))
                    return;
                const uint64_t now = perf_metric::now_ns ();
                if (now >= owner->_deadline_ns.load (std::memory_order_acquire))
                    return;
                owner->_latency.add (perf_metric::elapsed_latency_ns (now, header.sent_ts_ns) * 0.5);
                owner->_completed.fetch_add (1, std::memory_order_relaxed);
            };
            bool ok = false;
            if constexpr (std::is_same<SocketT, zlink::router_socket_t>::value) {
                // PERF_POLICY / C parity: the C reference builds the constant
                // server routing id once per active client state, not once per
                // request. Keep routing-id construction out of the measured loop.
                ok = std::move (slot_.socket->request (_target_rid))
                       .message (request)
                       .timeout (std::chrono::milliseconds (std::max (1, _settings.rcvtimeo_ms)))
                       .flags (static_cast<int> (zlink::send_flags_t::none))
                       .submit (callback);
            } else {
                ok = std::move (slot_.socket->request ())
                       .message (request)
                       .timeout (std::chrono::milliseconds (std::max (1, _settings.rcvtimeo_ms)))
                       .flags (static_cast<int> (zlink::send_flags_t::none))
                       .submit (callback);
            }
            if (!ok) {
                slot_.waiting.store (false, std::memory_order_release);
                return true;
            }
            ++slot_.next_seq;
            return true;
        }
        catch (const zlink::submit_error_t &err) {
            slot_.waiting.store (false, std::memory_order_release);
            if (err.result () == zlink::submit_result_t::backpressured
                || transient (err.internal_errno ()))
                return true;
            return false;
        }
    }

    bool any_waiting () const
    {
        for (size_t i = 0; i < _slots.size (); ++i) {
            if (_slots[i]->waiting.load (std::memory_order_acquire))
                return true;
        }
        return false;
    }

    const config_t _config;
    const std::string _lib_name;
    const std::string _transport;
    const size_t _msg_size;
    const std::string _endpoint;
    const zlink::routing_id_t _target_rid;
    const multi_bench_settings_t _settings;
    ctx_guard_t _ctx;
    std::vector<std::unique_ptr<client_slot_t<SocketT>>> _slots;
    std::vector<connect_monitor_t> _monitors;
    zlink::poller_t _poller;
    std::vector<zlink::poll_event_t> _events;
    const uint32_t _run_id;
    std::atomic<uint64_t> _deadline_ns;
    std::atomic<unsigned long long> _completed;
    std::atomic<bool> _fatal;
    bench_latency_sampler_t _latency;
};

inline std::atomic<bool> &server_stop_flag ()
{
    static std::atomic<bool> flag (false);
    return flag;
}

inline void server_signal_handler (int)
{
    server_stop_flag ().store (true, std::memory_order_release);
}

inline bool run_server (const config_t &config_,
                        const std::string &lib_name_,
                        const std::string &transport_,
                        size_t msg_size_)
{
    set_perf_pattern_env (config_.env_pattern);
    if (!is_supported_transport (transport_)) {
        std::cout << "UNSUPPORTED," << lib_name_ << "," << config_.result_pattern << ","
                  << transport_ << std::endl;
        return true;
    }
    const multi_bench_settings_t settings = resolve_multi_bench_settings ();
    ctx_guard_t ctx;
    zlink::router_socket_t server (ctx.ctx ());
    if (config_.server_has_routing_id)
        server.set_routing_id (zlink::routing_id_t::from (std::string ("SERVER")));
    apply_benchmark_socket_options (server, settings, transport_);
    if (!apply_benchmark_auto_hwm_msg_unit (ctx, msg_size_)
        || !setup_tls_server (server, transport_))
        return false;
    const std::string endpoint = bind_and_resolve_endpoint (
      server, transport_, std::string ("cpp_") + config_.env_pattern, settings.server_bind_port);
    if (endpoint.empty () || !recalculate_auto_hwm (ctx))
        return false;

    server_stop_flag ().store (false, std::memory_order_release);
    std::signal (SIGINT, server_signal_handler);
#if defined(SIGTERM)
    std::signal (SIGTERM, server_signal_handler);
#endif
    std::thread ([] () {
        std::string line;
        while (std::getline (std::cin, line)) {
            if (line == "STOP" || line == "QUIT")
                break;
        }
        server_stop_flag ().store (true, std::memory_order_release);
    }).detach ();
    print_ready (endpoint);

    zlink::poller_t poller;
    poller.add (server, zlink::poll_event_flag_t::pollin, 0);
    zlink::poll_event_t event;
    while (!server_stop_flag ().load (std::memory_order_acquire)) {
        try {
            if (poller.wait (&event, 1, std::chrono::milliseconds (200)) == 0)
                continue;
        }
        catch (const zlink::binding_error_t &err) {
            if (err.internal_errno () == EINTR)
                continue;
            return false;
        }
        for (;;) {
            zlink::received_t received;
            const int rc = server.recv (received, zlink::recv_flags_t::dontwait);
            if (rc != 0) {
                if (rc == static_cast<int> (zlink::recv_result_t::no_data) || errno == EAGAIN
                    || errno == EWOULDBLOCK || errno == EINTR)
                    break;
                return false;
            }
            // PERF_POLICY / C parity: the reference server receives and echoes
            // one native part. Use the public single-part view so the C++ case
            // does not add vector materialization that is absent from C.
            if (!received.is_single_part () || !received.request_seq ().has_value ())
                continue;
            try {
                zlink::message_t &part = received.first_part ();
                std::move (received.reply ().message (part)).submit ();
            }
            catch (const zlink::submit_error_t &err) {
                if (transient (err.internal_errno ()))
                    continue;
                return false;
            }
        }
    }
    return true;
}

template <typename SocketT>
inline bool run_client (const config_t &config_,
                        const std::string &lib_name_,
                        const std::string &transport_,
                        size_t msg_size_,
                        const std::string &endpoint_)
{
    set_perf_pattern_env (config_.env_pattern);
    if (!is_supported_transport (transport_)) {
        std::cout << "UNSUPPORTED," << lib_name_ << "," << config_.result_pattern << ","
                  << transport_ << std::endl;
        return true;
    }
    client_bench_t<SocketT> bench (config_, lib_name_, transport_, msg_size_, endpoint_,
                                   resolve_multi_bench_settings ());
    return bench.run ();
}

} // namespace reqrep
} // namespace multi
} // namespace perf

#endif

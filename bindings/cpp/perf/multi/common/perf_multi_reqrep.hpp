#ifndef PERF_CPP_MULTI_REQREP_HPP
#define PERF_CPP_MULTI_REQREP_HPP

#include "perf_common.hpp"
#include "perf_common_multi.hpp"
#include "perf_client_helpers.hpp"
#include "perf_entry.hpp"
#include "perf_metric_header.hpp"

#include <atomic>
#include <csignal>
#include <condition_variable>
#include <exception>
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

struct client_completion_state_t
{
    explicit client_completion_state_t (size_t msg_size_) :
        run_id (1U),
        msg_size (msg_size_),
        deadline_ns (0),
        outstanding (0),
        completed (0),
        change_epoch (0),
        fatal (false),
        latency ()
    {
    }

    const uint32_t run_id;
    const size_t msg_size;
    std::atomic<uint64_t> deadline_ns;
    std::atomic<unsigned long long> outstanding;
    std::atomic<unsigned long long> completed;
    std::atomic<unsigned long long> change_epoch;
    std::atomic<bool> fatal;
    std::mutex change_mutex;
    std::condition_variable changed;
    std::mutex latency_mutex;
    bench_latency_sampler_t latency;
};

enum class logical_admission_t
{
    launching,
    admitted,
    retry,
    fatal
};

struct logical_request_t
{
    logical_request_t () :
        payload (),
        admission (logical_admission_t::launching)
    {
    }

    std::vector<char> payload;
    std::atomic<logical_admission_t> admission;
};

struct bind_application_ready_queue_t
{
    perf::application_ready_queue_t &ready_queue;

    bool await_ready () const noexcept { return false; }

    template <typename TPromise>
    bool await_suspend (std::coroutine_handle<TPromise> continuation_) const noexcept
    {
        continuation_.promise ().zlink_bind_continuation_scheduler (ready_queue);
        return false;
    }

    void await_resume () const noexcept {}
};

template <typename SocketT> struct client_slot_t
{
    client_slot_t () :
        socket (),
        logical (),
        pre_admission_pending (false),
        next_seq (1)
    {
    }

    std::unique_ptr<SocketT> socket;
    logical_request_t logical;
    bool pre_admission_pending;
    uint64_t next_seq;
};

// This is an exceptional teardown path only.  A successfully accepted Core
// close resolves every admitted request through its terminal reply callback;
// keep the application queue alive until those callbacks have resumed and
// destroyed the detached request coroutines.  Returning after a bounded wait
// would leave the continuations with no queue owner.
template <typename SocketT>
inline void close_requesters_and_drain (
  std::vector<std::unique_ptr<client_slot_t<SocketT>>> &slots_,
  perf::application_ready_queue_t &ready_queue_,
  const std::shared_ptr<client_completion_state_t> &completion_)
{
    for (const std::unique_ptr<client_slot_t<SocketT>> &slot : slots_) {
        if (!slot || !slot->socket)
            continue;

        while (slot->socket->valid ()) {
            try {
                slot->socket->close ();
            }
            catch (const zlink::close_error_t &err) {
                if (err.result () == zlink::close_result_t::shutdown
                    && !slot->socket->valid ())
                    break;
                if (err.result () != zlink::close_result_t::busy)
                    // Core did not accept close, so it has not promised a
                    // terminal callback for each live detached coroutine.
                    // Destroying the queue in that state is unsafe.
                    std::terminate ();

                // A callback/API that entered before close admission is still
                // active.  Service the application handoff and retry; there
                // is deliberately no global teardown deadline.
                (void) ready_queue_.wait_and_run_ready_round_until (
                  std::chrono::steady_clock::now () + std::chrono::milliseconds (1));
            }
            catch (...) {
                // Same fail-closed policy as a non-busy close result.
                std::terminate ();
            }
        }
    }

    // Close has synchronously handed every accepted request its terminal
    // result.  This loop is quiescence, not a reply-timeout wait: it drains
    // only the finite continuation handoffs already owned by this queue.
    while (completion_->outstanding.load (std::memory_order_acquire) != 0) {
        (void) ready_queue_.run_ready_round ();
    }
}

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
        _ready_queue (),
        _completion (std::make_shared<client_completion_state_t> (msg_size_))
    {
    }

    bool run ()
    {
        if (!setup ())
            return false;

        const int duration_s = std::max (1, _settings.duration_seconds);
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (duration_s);
        _completion->deadline_ns.store (
          static_cast<uint64_t> (
            std::chrono::duration_cast<std::chrono::nanoseconds> (
              deadline.time_since_epoch ())
              .count ()),
          std::memory_order_release);

        while (std::chrono::steady_clock::now () < deadline
               && !_completion->fatal.load (std::memory_order_acquire)) {
            bool admitted = false;
            for (size_t i = 0; i < _slots.size (); ++i) {
                bool slot_admitted = false;
                if (!launch_request (*_slots[i], slot_admitted)) {
                    _completion->fatal.store (true, std::memory_order_release);
                    signal_change (_completion);
                    break;
                }
                admitted = admitted || slot_admitted;
            }
            if (_completion->fatal.load (std::memory_order_acquire))
                break;
            if (admitted) {
                (void) _ready_queue.run_ready_round ();
            } else if (std::chrono::steady_clock::now () < deadline) {
                const auto wake_deadline = std::min (
                  deadline, std::chrono::steady_clock::now ()
                              + std::chrono::milliseconds (50));
                (void) _ready_queue.wait_and_run_ready_round_until (
                  wake_deadline);
            }
        }

        const auto drain_deadline =
          std::chrono::steady_clock::now () + std::chrono::milliseconds (
                                                std::max (1000, _settings.rcvtimeo_ms * 4));
        while (_completion->outstanding.load (std::memory_order_acquire) != 0
               && std::chrono::steady_clock::now () < drain_deadline) {
            (void) _ready_queue.wait_and_run_ready_round_until (
              drain_deadline);
        }

        if (_completion->outstanding.load (std::memory_order_acquire) != 0) {
            // Do not let a bounded benchmark drain tear down the queue below
            // suspended detached request coroutines.  The normal successful
            // measurement path above remains unchanged.
            close_requesters_and_drain (_slots, _ready_queue, _completion);
            return false;
        }

        const unsigned long long completed =
          _completion->completed.load (std::memory_order_acquire);
        if (_completion->fatal.load (std::memory_order_acquire)
            || _completion->outstanding.load (std::memory_order_acquire) != 0
            || completed == 0)
            return false;
        const bench_latency_stats_t latency = _completion->latency.snapshot ();
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
                if (!setup_tls_client (*slot->socket, _transport))
                    return false;
                _monitors.push_back (connect_monitor_t ());
                if (!open_connect_monitor (*slot->socket, _settings.monitor_hwm,
                                           _monitors.back ()))
                    return false;
                slot->socket->connect (_endpoint);
                slot->logical.payload.assign (payload_size, 'q');
                _slots.push_back (std::move (slot));
            }
            const bool ready = wait_connect_ready_all (_monitors, _settings.connect_ready_timeout_ms);
            for (size_t i = 0; i < _monitors.size (); ++i)
                close_connect_monitor (_monitors[i]);
            if (!ready || !recalculate_auto_hwm (_ctx))
                return false;
            return !_slots.empty ();
        }
        catch (const zlink::binding_error_t &) {
            return false;
        }
    }

    bool launch_request (client_slot_t<SocketT> &slot_, bool &admitted_)
    {
        admitted_ = false;
        if (!slot_.pre_admission_pending) {
            const uint64_t sent_ns = perf_metric::now_ns ();
            if (!perf_metric::stamp_payload (
                  slot_.logical.payload.data (), slot_.logical.payload.size (),
                  _completion->run_id, perf_metric::phase_active, _msg_size,
                  slot_.next_seq, sent_ns))
                return false;
            slot_.pre_admission_pending = true;
        }

        slot_.logical.admission.store (logical_admission_t::launching,
                                       std::memory_order_release);
        submit_async_request (_ready_queue, *slot_.socket, _target_rid, &slot_.logical,
                              std::chrono::milliseconds (
                                std::max (1, _settings.rcvtimeo_ms)),
                              _completion);
        switch (slot_.logical.admission.load (std::memory_order_acquire)) {
            case logical_admission_t::admitted:
                ++slot_.next_seq;
                slot_.pre_admission_pending = false;
                admitted_ = true;
                return true;
            case logical_admission_t::retry:
                return true;
            case logical_admission_t::fatal:
                return false;
            case logical_admission_t::launching:
                return false;
        }
        return false;
    }

    static void signal_change (
      const std::shared_ptr<client_completion_state_t> &completion_)
    {
        completion_->change_epoch.fetch_add (1, std::memory_order_release);
        completion_->changed.notify_all ();
    }

    static zlink::async_result_t<std::vector<zlink::message_t>> begin_request (
      SocketT &socket_, const zlink::routing_id_t &target_rid_,
      zlink::message_t request_, std::chrono::milliseconds timeout_)
    {
        if constexpr (std::is_same<SocketT, zlink::router_socket_t>::value) {
            if (measurement_part_count () == 2) {
                zlink::message_t tail = measurement_empty_part ();
                return std::move (socket_.request (target_rid_).message (request_))
                  .message (tail)
                  .timeout (timeout_)
                  .async ();
            }
            return std::move (socket_.request (target_rid_))
              .message (request_)
              .timeout (timeout_)
              .async ();
        } else {
            if (measurement_part_count () == 2) {
                zlink::message_t tail = measurement_empty_part ();
                return std::move (socket_.request ().message (request_))
                  .message (tail)
                  .timeout (timeout_)
                  .async ();
            }
            return std::move (socket_.request ())
              .message (request_)
              .timeout (timeout_)
              .async ();
        }
    }

    static void observe_reply (
      const std::shared_ptr<client_completion_state_t> &completion_,
      const std::vector<zlink::message_t> &parts_)
    {
        if (!measurement_parts_valid (parts_))
            return;
        perf_metric::header_t header;
        if (perf_metric::decode_payload_header (
              parts_.front ().data (), parts_.front ().size (), &header)
            && perf_metric::is_expected (
              header, completion_->run_id, perf_metric::phase_active,
              completion_->msg_size)) {
            const uint64_t completion_ns = static_cast<uint64_t> (
              std::chrono::duration_cast<std::chrono::nanoseconds> (
                std::chrono::steady_clock::now ().time_since_epoch ())
                .count ());
            if (completion_ns
                < completion_->deadline_ns.load (std::memory_order_acquire)) {
                const uint64_t now = perf_metric::now_ns ();
                std::lock_guard<std::mutex> lock (completion_->latency_mutex);
                completion_->latency.add (
                  perf_metric::elapsed_latency_ns (now, header.sent_ts_ns) * 0.5);
                completion_->completed.fetch_add (1, std::memory_order_relaxed);
            }
        }
    }

    static perf::detached_async_task_t submit_async_request (
      perf::application_ready_queue_t &ready_queue_, SocketT &socket_,
      const zlink::routing_id_t &target_rid_,
      logical_request_t *logical_,
      std::chrono::milliseconds timeout_,
      std::shared_ptr<client_completion_state_t> completion_)
    {
        co_await bind_application_ready_queue_t{ready_queue_};
        try {
            zlink::message_t request = zlink::message_t::from (
              std::as_bytes (std::span<const char> (
                logical_->payload.data (), logical_->payload.size ())));
            if (!request.valid ()) {
                logical_->admission.store (logical_admission_t::fatal,
                                            std::memory_order_release);
                completion_->fatal.store (true, std::memory_order_release);
                signal_change (completion_);
                co_return;
            }
            std::optional<zlink::async_result_t<std::vector<zlink::message_t>>> operation;
            operation.emplace (
              begin_request (socket_, target_rid_, std::move (request), timeout_));
            completion_->outstanding.fetch_add (1, std::memory_order_release);
            logical_->admission.store (logical_admission_t::admitted,
                                        std::memory_order_release);
            signal_change (completion_);
            // Post-admission: operation owns the submitted request. This
            // coroutine must not read the reusable slot/logical buffer again.
            try {
                std::vector<zlink::message_t> reply = co_await std::move (*operation);
                observe_reply (completion_, reply);
            }
            catch (const zlink::request_error_t &err) {
                if (err.result () != zlink::request_result_t::timed_out)
                    completion_->fatal.store (true, std::memory_order_release);
            }
            catch (...) {
                completion_->fatal.store (true, std::memory_order_release);
            }
            completion_->outstanding.fetch_sub (1, std::memory_order_release);
            signal_change (completion_);
            co_return;
        }
        catch (const zlink::request_error_t &err) {
            if (err.result () == zlink::request_result_t::timed_out) {
                logical_->admission.store (logical_admission_t::retry,
                                            std::memory_order_release);
            } else {
                logical_->admission.store (logical_admission_t::fatal,
                                            std::memory_order_release);
                completion_->fatal.store (true, std::memory_order_release);
            }
        }
        catch (const zlink::submit_error_t &err) {
            if (err.result () == zlink::submit_result_t::backpressured
                || transient (err.internal_errno ())) {
                logical_->admission.store (logical_admission_t::retry,
                                            std::memory_order_release);
            } else {
                logical_->admission.store (logical_admission_t::fatal,
                                            std::memory_order_release);
                completion_->fatal.store (true, std::memory_order_release);
            }
        }
        catch (const zlink::binding_error_t &err) {
            if (transient (err.internal_errno ())) {
                logical_->admission.store (logical_admission_t::retry,
                                            std::memory_order_release);
            } else {
                logical_->admission.store (logical_admission_t::fatal,
                                            std::memory_order_release);
                completion_->fatal.store (true, std::memory_order_release);
            }
        }
        catch (...) {
            logical_->admission.store (logical_admission_t::fatal,
                                        std::memory_order_release);
            completion_->fatal.store (true, std::memory_order_release);
        }
        signal_change (completion_);
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
    perf::application_ready_queue_t _ready_queue;
    std::shared_ptr<client_completion_state_t> _completion;
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

inline bool submit_router_reply (zlink::received_t &received_,
                                 zlink::message_t &part_)
{
    try {
        if (measurement_part_count () == 2) {
            zlink::message_t tail = measurement_empty_part ();
            std::move (received_.reply ().message (part_)).message (tail).submit ();
        } else {
            std::move (received_.reply ().message (part_)).submit ();
        }
        return true;
    }
    catch (const zlink::submit_error_t &err) {
        errno = err.internal_errno ();
        return false;
    }
}

inline bool run_server (const config_t &config_,
                        const std::string &lib_name_,
                        const std::string &transport_,
                        size_t msg_size_)
{
    static_cast<void> (msg_size_);
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
    if (!setup_tls_server (server, transport_))
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
            if (poller.wait (&event, 1, std::chrono::milliseconds (100)) == 0)
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
            if (!measurement_parts_valid (received.parts ()) || !received.request_seq ().has_value ())
                continue;
            zlink::message_t &part = received.parts ().front ();
            if (!submit_router_reply (received, part))
                return false;
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

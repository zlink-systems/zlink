#ifndef PERF_CPP_SINGLE_REQREP_HPP
#define PERF_CPP_SINGLE_REQREP_HPP

#include "perf_single_common.hpp"

#include <atomic>
#include <coroutine>
#include <mutex>
#include <optional>
#include <type_traits>

namespace perf
{
namespace single
{

struct reqrep_config_t
{
    const char *pattern;
    bool routed_request;
};

// PERF_SINGLE_TEST_POLICY.md 1.1.3: the only runner-side bound is the number
// of un-settled request awaitables. The public request terminal makes one
// DONTWAIT admission attempt and resumes only from its exact WRITABLE token
// (zlink/Contracts/Messaging/operation_contracts.hpp:320-324), so Core paces
// admission exactly as the C reference does. This bound is a memory bound on
// the un-settled operation objects, never a round-trip gate, so it stays far
// above the steady-state depth. Same knob shape and default as the multi
// suite (PERF_MULTI_REQREP_MAX_OUTSTANDING).
inline int reqrep_max_outstanding ()
{
    return std::max (2, parse_positive_env ("PERF_SINGLE_REQREP_MAX_OUTSTANDING", 64));
}

enum class reqrep_launch_t
{
    launching,
    owned_by_operation,
    retry,
    fatal
};

struct reqrep_state_t
{
    reqrep_state_t (uint32_t run_id_, size_t msg_size_) :
        run_id (run_id_),
        msg_size (msg_size_),
        active_deadline_ns (0),
        completed (0),
        in_flight (0),
        launch (reqrep_launch_t::launching),
        fatal (false),
        latency (resolve_single_latency_sample_cap ())
    {
    }

    uint32_t run_id;
    size_t msg_size;
    std::atomic<int64_t> active_deadline_ns;
    std::atomic<unsigned long long> completed;
    std::atomic<unsigned long long> in_flight;
    std::atomic<reqrep_launch_t> launch;
    std::atomic<bool> fatal;
    std::mutex latency_mutex;
    latency_stats_builder_t latency;
};

// Binds the detached request coroutine to the requester thread's own ready
// queue before it touches the socket. PERF_SINGLE_TEST_POLICY.md 1.1.3
// requires the dedicated requester thread - not an executor or event loop - to
// progress the awaitable to completion, and this scheduler makes every
// continuation run inside that thread's run_ready_round().
struct bind_reqrep_ready_queue_t
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

inline void observe_request_completion (zlink::request_result_t result_,
                                         std::vector<zlink::message_t> parts_,
                                         reqrep_state_t *state_)
{
    if (!state_)
        return;
    if (result_ == zlink::request_result_t::ok) {
        const zlink::message_t *payload = measurement_payload_part (parts_);
        if (!payload) {
            state_->fatal.store (true, std::memory_order_release);
            state_->in_flight.fetch_sub (1, std::memory_order_release);
            return;
        }
        perf_single_metric::header_t header;
        if (perf_single_metric::decode_payload_header (
              payload->data (), payload->size (), &header)
            && perf_single_metric::is_expected (
              header, state_->run_id, perf_single_metric::phase_active, state_->msg_size)) {
            const int64_t completion_ns =
              std::chrono::duration_cast<std::chrono::nanoseconds> (
                std::chrono::steady_clock::now ().time_since_epoch ())
                .count ();
            if (completion_ns
                < state_->active_deadline_ns.load (std::memory_order_acquire)) {
                const uint64_t now = perf_single_metric::now_ns ();
                std::lock_guard<std::mutex> lock (state_->latency_mutex);
                state_->latency.add (
                  perf_single_metric::elapsed_latency_ns (now, header.sent_ts_ns));
                state_->completed.fetch_add (1, std::memory_order_relaxed);
            }
        }
    } else if (result_ != zlink::request_result_t::timed_out) {
        state_->fatal.store (true, std::memory_order_release);
    }
    state_->in_flight.fetch_sub (1, std::memory_order_release);
}

inline bool reqrep_transient_errno (int err_)
{
    return err_ == EAGAIN || err_ == EWOULDBLOCK || err_ == EINTR || err_ == ETIMEDOUT
           || err_ == EHOSTUNREACH || err_ == ENOTCONN;
}

inline void emit_reqrep_result (const std::string &lib_name_,
                                const char *pattern_,
                                const std::string &transport_,
                                size_t msg_size_,
                                double throughput_,
                                const latency_stats_t &latency_)
{
    const double bandwidth = throughput_ * static_cast<double> (msg_size_) * 2.0 / 1000000.0;
    std::cout << "RESULT," << lib_name_ << "," << pattern_ << "," << transport_ << ","
              << msg_size_ << ",throughput," << std::fixed << std::setprecision (3)
              << throughput_ << std::endl;
    std::cout << "RESULT," << lib_name_ << "," << pattern_ << "," << transport_ << ","
              << msg_size_ << ",bandwidth," << bandwidth << std::endl;
    std::cout << "RESULT," << lib_name_ << "," << pattern_ << "," << transport_ << ","
              << msg_size_ << ",latency," << latency_.mean_ns / 1000000.0 << std::endl;
    std::cout << "RESULT," << lib_name_ << "," << pattern_ << "," << transport_ << ","
              << msg_size_ << ",latency_p95," << latency_.p95_ns / 1000000.0 << std::endl;
    std::cout << "RESULT," << lib_name_ << "," << pattern_ << "," << transport_ << ","
              << msg_size_ << ",latency_p99," << latency_.p99_ns / 1000000.0 << std::endl;
}

inline bool receive_router (zlink::router_socket_t &socket_,
                            zlink::received_t &received_,
                            zlink::recv_flags_t flags_ = zlink::recv_flags_t::none)
{
    const int rc = socket_.recv (received_, flags_);
    if (rc == 0)
        return true;
    errno = rc == static_cast<int> (zlink::recv_result_t::no_data) ? EAGAIN : EIO;
    return false;
}

inline bool complete_reqrep_router_handshake (zlink::router_socket_t &server_,
                                               zlink::router_socket_t &client_,
                                               const zlink::routing_id_t &server_rid_)
{
    const auto deadline = std::chrono::steady_clock::now ()
                          + std::chrono::milliseconds (parse_positive_env (
                            "PERF_ROUTER_HANDSHAKE_TIMEOUT_MS", 3000));
    std::optional<zlink::routing_id_t> client_rid;
    while (!client_rid.has_value () && std::chrono::steady_clock::now () < deadline) {
        try {
            zlink::message_t ping = zlink::message_t::from ("PING");
            std::move (client_.send (server_rid_)).message (ping).submit ();
        }
        catch (const zlink::binding_error_t &err) {
            if (!is_transient_routed_send_errno (err.internal_errno ()))
                return false;
        }

        zlink::received_t inbound;
        while (receive_router (server_, inbound, zlink::recv_flags_t::dontwait)) {
            if (inbound.routing_id ().has_value () && inbound.parts ().size () == 1
                && inbound.parts ()[0].to_string () == "PING") {
                client_rid = *inbound.routing_id ();
                break;
            }
        }
        if (!client_rid.has_value ())
            std::this_thread::yield ();
    }
    if (!client_rid.has_value ())
        return false;

    try {
        zlink::message_t pong = zlink::message_t::from ("PONG");
        std::move (server_.send (*client_rid)).message (pong).submit ();
    }
    catch (const zlink::binding_error_t &) {
        return false;
    }

    zlink::received_t response;
    if (!receive_router (client_, response))
        return false;
    return response.routing_id ().has_value () && *response.routing_id () == server_rid_
           && response.parts ().size () == 1 && response.parts ()[0].to_string () == "PONG";
}

// Awaitable request terminal (`async()`), the only public C++ terminal that
// returns before the reply. PERF_SINGLE_TEST_POLICY.md 1.1.1 - admission and
// reply are separate events, so the runner must not use the blocking
// `submit()` terminal, which pins in-flight to one per thread.
template <typename ClientSocket>
inline zlink::async_result_t<std::vector<zlink::message_t>> begin_reqrep_request (
  ClientSocket &client_,
  const zlink::routing_id_t &server_rid_,
  zlink::message_t request_,
  std::chrono::milliseconds timeout_)
{
    if constexpr (std::is_same<ClientSocket, zlink::router_socket_t>::value) {
        if (measurement_part_count () == 2) {
            zlink::message_t tail = message_from_payload (NULL, 0);
            return std::move (client_.request (server_rid_).message (request_))
              .message (tail)
              .timeout (timeout_)
              .async ();
        }
        return std::move (client_.request (server_rid_))
          .message (request_)
          .timeout (timeout_)
          .async ();
    } else {
        if (measurement_part_count () == 2) {
            zlink::message_t tail = message_from_payload (NULL, 0);
            return std::move (client_.request ().message (request_))
              .message (tail)
              .timeout (timeout_)
              .async ();
        }
        return std::move (client_.request ())
          .message (request_)
          .timeout (timeout_)
          .async ();
    }
}

// One logical request. The coroutine is created eagerly on the requester
// thread and runs there until the operation suspends; every continuation is
// scheduled back onto that thread's ready queue by
// bind_reqrep_ready_queue_t. PERF_SINGLE_TEST_POLICY.md 1.1.2/1.1.3: submit
// continuously without awaiting the previous reply, and let the same dedicated
// thread drain completions.
template <typename ClientSocket>
inline perf::detached_async_task_t submit_async_request (
  perf::application_ready_queue_t &ready_queue_,
  ClientSocket &client_,
  const zlink::routing_id_t &server_rid_,
  zlink::message_t request_,
  std::chrono::milliseconds timeout_,
  reqrep_state_t *state_)
{
    co_await bind_reqrep_ready_queue_t{ready_queue_};
    try {
        if (!request_.valid ()) {
            state_->launch.store (reqrep_launch_t::fatal, std::memory_order_release);
            state_->fatal.store (true, std::memory_order_release);
            co_return;
        }
        std::optional<zlink::async_result_t<std::vector<zlink::message_t>>> operation;
        operation.emplace (
          begin_reqrep_request (client_, server_rid_, std::move (request_), timeout_));
        state_->in_flight.fetch_add (1, std::memory_order_release);
        state_->launch.store (reqrep_launch_t::owned_by_operation,
                              std::memory_order_release);
        // The async operation owns the request across any WRITABLE wait,
        // resubmission and the final REQUEST completion.
        try {
            std::vector<zlink::message_t> reply = co_await std::move (*operation);
            observe_request_completion (zlink::request_result_t::ok, std::move (reply),
                                        state_);
        }
        catch (const zlink::request_error_t &err) {
            observe_request_completion (err.result (), {}, state_);
        }
        catch (...) {
            state_->fatal.store (true, std::memory_order_release);
            state_->in_flight.fetch_sub (1, std::memory_order_release);
        }
        co_return;
    }
    catch (const zlink::request_error_t &err) {
        state_->launch.store (err.result () == zlink::request_result_t::timed_out
                                ? reqrep_launch_t::retry
                                : reqrep_launch_t::fatal,
                              std::memory_order_release);
        if (err.result () != zlink::request_result_t::timed_out)
            state_->fatal.store (true, std::memory_order_release);
    }
    catch (const zlink::submit_error_t &err) {
        const bool retry = err.result () == zlink::submit_result_t::backpressured
                           || reqrep_transient_errno (err.internal_errno ());
        state_->launch.store (retry ? reqrep_launch_t::retry : reqrep_launch_t::fatal,
                              std::memory_order_release);
        if (!retry)
            state_->fatal.store (true, std::memory_order_release);
    }
    catch (const zlink::binding_error_t &err) {
        const bool retry = reqrep_transient_errno (err.internal_errno ());
        state_->launch.store (retry ? reqrep_launch_t::retry : reqrep_launch_t::fatal,
                              std::memory_order_release);
        if (!retry)
            state_->fatal.store (true, std::memory_order_release);
    }
    catch (...) {
        state_->launch.store (reqrep_launch_t::fatal, std::memory_order_release);
        state_->fatal.store (true, std::memory_order_release);
    }
    co_return;
}

// Exceptional teardown only. A bounded benchmark drain must never destroy the
// ready queue below suspended detached request coroutines: Core resolves every
// admitted request through its terminal callback once close is accepted, so
// keep the queue alive until those continuations have run.
template <typename ClientSocket>
inline void close_requester_and_drain (ClientSocket &client_,
                                        perf::application_ready_queue_t &ready_queue_,
                                        reqrep_state_t &state_)
{
    while (client_.valid ()) {
        try {
            client_.close ();
        }
        catch (const zlink::close_error_t &err) {
            if (err.result () == zlink::close_result_t::shutdown && !client_.valid ())
                break;
            if (err.result () != zlink::close_result_t::busy)
                std::terminate ();
            (void) ready_queue_.wait_and_run_ready_round_until (
              std::chrono::steady_clock::now () + std::chrono::milliseconds (1));
        }
        catch (...) {
            std::terminate ();
        }
    }
    while (state_.in_flight.load (std::memory_order_acquire) != 0)
        (void) ready_queue_.run_ready_round ();
}

template <typename ClientSocket>
inline bool send_reqrep_stop (ClientSocket &client_, const zlink::routing_id_t &server_rid_)
{
    try {
        zlink::message_t token = message_from_payload (k_stop_token, std::strlen (k_stop_token));
        if constexpr (std::is_same<ClientSocket, zlink::router_socket_t>::value)
            std::move (client_.send (server_rid_)).message (token).submit ();
        else
            std::move (client_.send ()).message (token).submit ();
        return true;
    }
    catch (const zlink::binding_error_t &) {
        return false;
    }
}

template <typename ClientSocket>
inline bool run_reqrep_pattern_impl (const reqrep_config_t &config_,
                                     const std::string &transport_,
                                     size_t msg_size_,
                                     const std::string &lib_name_)
{
    ctx_guard_t ctx;
    zlink::router_socket_t server (ctx.ctx ());
    ClientSocket client (ctx.ctx ());
    if (!ctx.valid () || !server.valid () || !client.valid ())
        return false;

    const zlink::routing_id_t server_rid = zlink::routing_id_t::from (std::string ("SERVER"));
    server.set_routing_id (server_rid);
    if constexpr (std::is_same<ClientSocket, zlink::router_socket_t>::value) {
        client.set_routing_id (zlink::routing_id_t::from (std::string ("CLIENT")));
        client.options ().mandatory (true);
        server.options ().mandatory (true);
        client.options ().connect_routing_id (server_rid);
    }
    if (!recalculate_single_auto_hwm (ctx)
        || !setup_connected_pair (server, client, transport_,
                                  lib_name_ + "_" + config_.pattern))
        return false;
    if constexpr (std::is_same<ClientSocket, zlink::router_socket_t>::value) {
        if (!complete_reqrep_router_handshake (server, client, server_rid))
            return false;
    }

    const size_t payload_size = std::max<size_t> (msg_size_, perf_single_metric::header_size ());
    std::vector<char> payload (payload_size, 'r');
    const int duration_s = std::max (1, resolve_single_duration_seconds ());
    const int request_timeout_ms = parse_positive_env ("PERF_SINGLE_REQREP_TIMEOUT_MS", 200);
    const int drain_timeout_ms =
      parse_positive_env ("PERF_SINGLE_REQREP_DRAIN_TIMEOUT_MS", 10000);
    const uint32_t run_id = 1U;
    reqrep_state_t request_state (run_id, msg_size_);
    std::atomic<bool> server_ok (true);

    std::thread server_thread ([&] () {
        zlink::received_t received;
        while (true) {
            if (!receive_router (server, received)) {
                if (errno == EAGAIN || errno == EINTR)
                    continue;
                server_ok.store (false, std::memory_order_release);
                return;
            }
            if (received.parts ().empty ())
                continue;
            if (received.parts ().size () == 1
                && is_stop_token_message (received.parts ().front ()))
                return;
            if (!received.reply_token ().has_value ()
                || !measurement_parts_valid (received.parts ()))
                continue;
            try {
                zlink::message_t &part = received.parts ().front ();
                if (measurement_part_count () == 2) {
                    zlink::message_t tail = message_from_payload (NULL, 0);
                    std::move (received.reply ().message (part)).message (tail).submit ();
                } else {
                    std::move (received.reply ().message (part)).submit ();
                }
            }
            catch (const zlink::binding_error_t &err) {
                if (reqrep_transient_errno (err.internal_errno ()))
                    continue;
                server_ok.store (false, std::memory_order_release);
                return;
            }
        }
    });

    // PERF_SINGLE_TEST_POLICY.md 1.1.2/1.1.4: this calling thread is the
    // dedicated requester OS thread. It alternates continuous submission with
    // completion progress; it owns the socket completion queue through a
    // public POLLCOMPLETION poller and resumes every request continuation from
    // its own ready queue. No coroutine executor, async runtime or event loop
    // drives progress. The replier runs on its own OS thread above.
    perf::application_ready_queue_t ready_queue;
    zlink::poller_t completion_poller;
    try {
        completion_poller.add (client, zlink::poll_event_flag_t::pollcompletion, 0);
    }
    catch (const zlink::binding_error_t &) {
        request_state.fatal.store (true, std::memory_order_release);
    }

    const int max_outstanding = reqrep_max_outstanding ();
    const auto progress_once = [&] (std::chrono::milliseconds wait_) {
        try {
            zlink::poll_event_t event;
            (void) completion_poller.wait (&event, 1, wait_);
        }
        catch (const zlink::binding_error_t &) {
            request_state.fatal.store (true, std::memory_order_release);
        }
        (void) ready_queue.run_ready_round ();
    };

    {
        uint64_t seq = 1;
        const auto deadline =
          std::chrono::steady_clock::now () + std::chrono::seconds (duration_s);
        request_state.active_deadline_ns.store (
          std::chrono::duration_cast<std::chrono::nanoseconds> (
            deadline.time_since_epoch ())
            .count (),
          std::memory_order_release);
        while (std::chrono::steady_clock::now () < deadline
               && !request_state.fatal.load (std::memory_order_acquire)) {
            bool launched = false;
            unsigned submitted_since_progress = 0;
            // 1) Submit continuously. Nothing here waits for a reply; the only
            //    bound is the un-settled awaitable count.
            while (std::chrono::steady_clock::now () < deadline
                   && !request_state.fatal.load (std::memory_order_acquire)
                   && request_state.in_flight.load (std::memory_order_acquire)
                        < static_cast<unsigned long long> (max_outstanding)) {
                if (!perf_single_metric::stamp_payload (
                      payload.data (), payload.size (), run_id,
                      perf_single_metric::phase_active, msg_size_, seq,
                      perf_single_metric::now_ns ())) {
                    request_state.fatal.store (true, std::memory_order_release);
                    break;
                }
                zlink::message_t request =
                  message_from_payload (payload.data (), payload.size ());
                if (!request.valid ()) {
                    request_state.fatal.store (true, std::memory_order_release);
                    break;
                }
                request_state.launch.store (reqrep_launch_t::launching,
                                            std::memory_order_release);
                submit_async_request (ready_queue, client, server_rid,
                                      std::move (request),
                                      std::chrono::milliseconds (request_timeout_ms),
                                      &request_state);
                const reqrep_launch_t launch =
                  request_state.launch.load (std::memory_order_acquire);
                if (launch == reqrep_launch_t::owned_by_operation) {
                    ++seq;
                    launched = true;
                    // Give completions a turn on the same cadence as the C
                    // reference submit cursor
                    // (bindings/c/perf/single/common/perf_single_reqrep.hpp
                    // run_request_phase, 64 submissions per progress round).
                    if (++submitted_since_progress >= 64)
                        break;
                    continue;
                }
                if (launch == reqrep_launch_t::retry)
                    break;
                request_state.fatal.store (true, std::memory_order_release);
                break;
            }
            if (request_state.fatal.load (std::memory_order_acquire))
                break;
            // 2) Progress completions on this same thread.
            progress_once (launched ? std::chrono::milliseconds (0)
                                    : std::chrono::milliseconds (50));
        }

        // Bounded completion drain of requests submitted before the deadline;
        // no new request is submitted here.
        const auto drain_deadline = std::chrono::steady_clock::now ()
                                    + std::chrono::milliseconds (drain_timeout_ms);
        while (request_state.in_flight.load (std::memory_order_acquire) != 0
               && std::chrono::steady_clock::now () < drain_deadline)
            progress_once (std::chrono::milliseconds (50));
    }

    const bool drained = request_state.in_flight.load (std::memory_order_acquire) == 0;
    if (!drained)
        request_state.fatal.store (true, std::memory_order_release);

    const bool stop_ok = send_reqrep_stop (client, server_rid);
    server_thread.join ();
    if (!drained) {
        // Never destroy the ready queue below suspended detached coroutines.
        close_requester_and_drain (client, ready_queue, request_state);
        return false;
    }

    const unsigned long long completed =
      request_state.completed.load (std::memory_order_acquire);
    if (!stop_ok || request_state.fatal.load (std::memory_order_acquire)
        || !server_ok.load (std::memory_order_acquire) || completed == 0
        || request_state.latency.count () == 0)
        return false;

    const latency_stats_t latency_stats = request_state.latency.snapshot ();
    emit_reqrep_result (lib_name_, config_.pattern, transport_, msg_size_,
                        static_cast<double> (completed) / duration_s, latency_stats);
    return true;
}

inline bool run_reqrep_pattern (const reqrep_config_t &config_,
                                const std::string &transport_,
                                size_t msg_size_,
                                const std::string &lib_name_)
{
    if (!transport_available (transport_)) {
        std::cout << "UNSUPPORTED," << lib_name_ << "," << config_.pattern << "," << transport_
                  << std::endl;
        return true;
    }
    return config_.routed_request
             ? run_reqrep_pattern_impl<zlink::router_socket_t> (
                 config_, transport_, msg_size_, lib_name_)
             : run_reqrep_pattern_impl<zlink::dealer_socket_t> (
                 config_, transport_, msg_size_, lib_name_);
}

} // namespace single
} // namespace perf

#endif

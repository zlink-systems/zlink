#ifndef PERF_CPP_SINGLE_REQREP_HPP
#define PERF_CPP_SINGLE_REQREP_HPP

#include "perf_single_common.hpp"

#include <atomic>
#include <mutex>
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

struct reqrep_state_t
{
    reqrep_state_t (uint32_t run_id_, size_t msg_size_) :
        run_id (run_id_),
        msg_size (msg_size_),
        active_deadline_ns (0),
        completed (0),
        in_flight (0),
        submit_done (false),
        fatal (false),
        latency (resolve_single_latency_sample_cap ())
    {
    }

    uint32_t run_id;
    size_t msg_size;
    std::atomic<int64_t> active_deadline_ns;
    std::atomic<unsigned long long> completed;
    std::atomic<unsigned long long> in_flight;
    std::atomic<bool> submit_done;
    std::atomic<bool> fatal;
    std::mutex latency_mutex;
    latency_stats_builder_t latency;
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

template <typename ClientSocket>
inline bool submit_callback_request (ClientSocket &client_,
                                     const zlink::routing_id_t &server_rid_,
                                     zlink::message_t &payload_,
                                     std::chrono::milliseconds timeout_,
                                     zlink::request_callback_t callback_)
{
    if constexpr (std::is_same<ClientSocket, zlink::router_socket_t>::value) {
        if (measurement_part_count () == 2) {
            zlink::message_t tail = message_from_payload (NULL, 0);
            return std::move (client_.request (server_rid_).message (payload_)).message (tail)
              .timeout (timeout_)
              .flags (static_cast<int> (zlink::send_flags_t::dontwait))
              .submit (std::move (callback_));
        }
        return std::move (client_.request (server_rid_)).message (payload_)
          .timeout (timeout_)
          .flags (static_cast<int> (zlink::send_flags_t::dontwait))
          .submit (std::move (callback_));
    } else {
        if (measurement_part_count () == 2) {
            zlink::message_t tail = message_from_payload (NULL, 0);
            return std::move (client_.request ().message (payload_)).message (tail)
              .timeout (timeout_)
              .flags (static_cast<int> (zlink::send_flags_t::dontwait))
              .submit (std::move (callback_));
        }
        return std::move (client_.request ()).message (payload_)
          .timeout (timeout_)
          .flags (static_cast<int> (zlink::send_flags_t::dontwait))
          .submit (std::move (callback_));
    }
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
            if (!received.request_seq ().has_value ()
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

    zlink::poller_t completion_poller;
    try {
        completion_poller.add (client, zlink::poll_event_flag_t::pollcompletion, 0);
    }
    catch (const zlink::binding_error_t &) {
        request_state.fatal.store (true, std::memory_order_release);
    }

    std::thread completion_thread ([&] () {
        const auto drain_deadline = [&] () {
            while (!request_state.submit_done.load (std::memory_order_acquire)
                   && !request_state.fatal.load (std::memory_order_acquire)) {
                try {
                    zlink::poll_event_t event;
                    (void) completion_poller.wait (&event, 1, std::chrono::milliseconds (50));
                }
                catch (const zlink::binding_error_t &) {
                    request_state.fatal.store (true, std::memory_order_release);
                }
            }
            return std::chrono::steady_clock::now ()
                   + std::chrono::milliseconds (drain_timeout_ms);
        } ();

        while (request_state.in_flight.load (std::memory_order_acquire) > 0
               && std::chrono::steady_clock::now () < drain_deadline) {
            try {
                zlink::poll_event_t event;
                (void) completion_poller.wait (&event, 1, std::chrono::milliseconds (50));
            }
            catch (const zlink::binding_error_t &) {
                request_state.fatal.store (true, std::memory_order_release);
            }
        }
        if (request_state.in_flight.load (std::memory_order_acquire) != 0)
            request_state.fatal.store (true, std::memory_order_release);
    });

    std::thread requester_thread ([&] () {
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
            if (!perf_single_metric::stamp_payload (
                  payload.data (), payload.size (), run_id, perf_single_metric::phase_active,
                  msg_size_, seq, perf_single_metric::now_ns ())) {
                request_state.fatal.store (true, std::memory_order_release);
                break;
            }
            zlink::message_t request = message_from_payload (payload.data (), payload.size ());
            if (!request.valid ()) {
                request_state.fatal.store (true, std::memory_order_release);
                break;
            }
            request_state.in_flight.fetch_add (1, std::memory_order_release);
            try {
                const zlink::request_callback_t callback =
                  [&request_state] (zlink::request_result_t result,
                                    std::vector<zlink::message_t> parts) {
                      observe_request_completion (result, std::move (parts), &request_state);
                  };
                if (!submit_callback_request (
                      client, server_rid, request, std::chrono::milliseconds (request_timeout_ms),
                      callback)) {
                    request_state.in_flight.fetch_sub (1, std::memory_order_release);
                    std::this_thread::yield ();
                    continue;
                }
                ++seq;
            }
            catch (const zlink::submit_error_t &err) {
                request_state.in_flight.fetch_sub (1, std::memory_order_release);
                if (err.result () == zlink::submit_result_t::backpressured
                    || reqrep_transient_errno (err.internal_errno ())) {
                    std::this_thread::yield ();
                    continue;
                }
                request_state.fatal.store (true, std::memory_order_release);
            }
            catch (const zlink::binding_error_t &err) {
                request_state.in_flight.fetch_sub (1, std::memory_order_release);
                if (reqrep_transient_errno (err.internal_errno ())) {
                    std::this_thread::yield ();
                    continue;
                }
                request_state.fatal.store (true, std::memory_order_release);
            }
        }
        request_state.submit_done.store (true, std::memory_order_release);
    });

    requester_thread.join ();
    completion_thread.join ();
    const bool stop_ok = send_reqrep_stop (client, server_rid);
    server_thread.join ();

    const unsigned long long completed =
      request_state.completed.load (std::memory_order_acquire);
    if (!stop_ok || request_state.fatal.load (std::memory_order_acquire)
        || request_state.in_flight.load (std::memory_order_acquire) != 0
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

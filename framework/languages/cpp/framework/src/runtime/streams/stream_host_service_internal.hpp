/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/configuration/service_scope.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/eventing/runtime_wake_timer.hpp"
#include "runtime/execution/state_lane.hpp"
#include "runtime/stateful/stream_session_registry.hpp"
#include "runtime/streams/session_liveness.hpp"
#include "runtime/streams/stream_host_service.hpp"
#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Messaging/stream_packet.hpp>
#include <zlink/Contracts/Sockets/stream_socket.hpp>
#include <opentelemetry/metrics/meter.h>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/websocket/stream_fwd.hpp>
#ifdef ZLINK_FRAMEWORK_STREAM_WITH_OPENSSL
#include <boost/asio/ssl/context.hpp>
#endif

#include <condition_variable>
#include <cstdint>
#include <utility>
#include <unordered_map>
#include <variant>

#ifdef ZLINK_FRAMEWORK_STREAM_WITH_OPENSSL
namespace boost::asio::ssl
{
template <typename Stream> class stream;
}
#endif

namespace zlink::framework::runtime
{

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using websocket_stream_t = boost::beast::websocket::stream<tcp::socket>;
#ifdef ZLINK_FRAMEWORK_STREAM_WITH_OPENSSL
namespace ssl = asio::ssl;
#endif
using detail::stream_header_flags_t;
using detail::stream_header_t;
using detail::stream_message_kind_t;

namespace stream_host_detail
{

/* One listener-wide turn scheduler owns the receive cursor for all externally
 * accepted STREAM connections. Workers release the lease before a synchronous
 * Asio read may wait for more bytes, so one partial frame cannot block another
 * connection. The read itself remains the authority for EOF and transport
 * errors; an availability-byte probe cannot represent an orderly close. */
class stream_receive_scheduler_t final
{
    struct slot_t;

  public:
    class lease_t;

    stream_receive_scheduler_t () = default;

    ~stream_receive_scheduler_t ();

    stream_receive_scheduler_t (const stream_receive_scheduler_t &) = delete;
    stream_receive_scheduler_t &operator= (const stream_receive_scheduler_t &) = delete;

    void start ();

    lease_t register_connection ();

    void request_stop () noexcept;

    void stop () noexcept;

  private:
    bool wait_for_turn (const std::shared_ptr<slot_t> &slot,
                        const std::function<bool ()> &stop_requested) const;

    void release_turn (const std::shared_ptr<slot_t> &slot) noexcept;

    void unregister_slot (const std::shared_ptr<slot_t> &slot) noexcept;

    void run () noexcept;

    mutable std::mutex _mutex;
    std::vector<std::shared_ptr<slot_t>> _slots;
    std::uint64_t _next_sequence = 1;
    std::uint64_t _cursor_sequence = 1;
    bool _stopped = false;
    std::thread _thread;
    std::condition_variable _changed;
};

bool stream_uses_websocket (const stream_snapshot_t &stream);
bool stream_uses_tls (const stream_snapshot_t &stream);
std::optional<std::string> socket_endpoint_text (const tcp::endpoint &endpoint);
bool stream_trace_enabled ();

extern const char session_creation_failed_diagnostic[];
} // namespace stream_host_detail

// Build diagnostic arguments only when STREAM tracing is enabled.
#define trace_stream_host(listener, ...)                                                           \
    do {                                                                                           \
        if (stream_host_detail::stream_trace_enabled ())                                           \
            (listener)->trace_stream_host_enabled (__VA_ARGS__);                                   \
    } while (false)

class stream_host_service_t::listener_t : public std::enable_shared_from_this<listener_t>
{
    friend struct stream_host_service_test_access_t;

  public:
    listener_t (detail::stream_runtime_t runtime,
                stream_snapshot_t stream,
                std::optional<std::string> advertise_host,
                detail::stream_session_factory_t session_factory,
                service_provider_t &services,
                detail::delay_owner_t *delay_owner,
                stream_host_service_t &host,
                std::shared_ptr<std::atomic_bool> drain_flag,
                std::stop_token shutdown_cancellation,
                std::shared_ptr<framework::detail::monitoring_runtime_state_t> monitoring,
                std::shared_ptr<runtime_failure_collector_t> runtime_failures,
                std::shared_ptr<detail::mesh_node_runtime_t> mesh_node,
                std::shared_ptr<application_job_queue_t> application_jobs,
                stream_host_ingress_t &ingress,
                std::chrono::milliseconds session_replacement_callback_timeout,
                asio::io_context &liveness_io);

    ~listener_t ();

    using session_liveness_t = runtime::session_liveness_t;

    struct active_session_t
    {
        stream_t stream;
        std::function<void (stream_close_reason_t, std::string)> request_terminate;
    };

    struct replacement_session_state_t
    {
        struct actor_binding_t
        {
            std::uint64_t object_generation = 0;
            std::uint64_t binding_generation = 0;
        };

        std::mutex gate;
        zlink::routing_id_t session_rid;
        stream_t stream;
        std::map<std::string, actor_binding_t> actor_bindings;
        std::atomic_bool closing{false};
        bool accepting_replacements = true;
        std::function<void ()> close_connection;
        detail::stream_runtime_t::actor_binding_replaced_dispatch_t dispatch_replacement;
        detail::actor_gateway_runtime_t actor_gateway;
        std::shared_ptr<runtime_failure_collector_t> failures;
        std::shared_ptr<detail::bound_session_replacement_handler_t> handler;

        replacement_session_state_t (
          zlink::routing_id_t session_rid_,
          stream_t stream_,
          std::function<void ()> close_connection_,
          detail::stream_runtime_t::actor_binding_replaced_dispatch_t dispatch_replacement_,
          detail::actor_gateway_runtime_t actor_gateway_,
          std::shared_ptr<runtime_failure_collector_t> failures_);

        ~replacement_session_state_t ();

        void deactivate () noexcept;

        task_t<void> dispatch_if_active (stream_t &dispatch_stream, std::string actor_id);

        bool exact (const runtime::protocol::bound_session_replaced_t &replacement);
    };

    /* Async packet dispatch may outlive the socket reader. Keep the service
     * scope, session object and Actor manager together until the serial
     * dispatch queue has completed its last callback.
     * The last reference can be released on any thread after MeshNode has
     * stopped, so the destructor does not use the session registry. The
     * reader closes the registry entry through retire_connection. */
    struct stream_connection_state_t
    {
        stream_connection_state_t (
          detail::service_scope_t scope,
          packet_stream_session_t *session,
          stream_t stream,
          session_actor_manager_t *actors,
          std::optional<runtime::stateful::stream_connection_t> transport_connection);

        ~stream_connection_state_t ();

        stream_connection_state_t (const stream_connection_state_t &) = delete;
        stream_connection_state_t &operator= (const stream_connection_state_t &) = delete;

        detail::service_scope_t scope;
        packet_stream_session_t *session = nullptr;
        stream_t stream;
        session_actor_manager_t *actors = nullptr;
        std::optional<runtime::stateful::stream_connection_t> transport_connection;
        std::shared_ptr<replacement_session_state_t> replacement;
    };
    void begin_connection_retirement (const std::shared_ptr<stream_connection_state_t> &state);

    void retire_connection (
      const std::shared_ptr<stream_connection_state_t> &state,
      std::function<void (std::function<void ()>)> dispatch_final =
        [] (std::function<void ()> completed) { completed (); });

    void register_active_stream (
      const stream_t &stream,
      std::function<void (stream_close_reason_t, std::string)> request_terminate);

    void unregister_active_stream (const stream_t &stream);

    std::size_t active_session_count ();

    void begin_drain_sessions ();

    void notify_sessions_closing (stream_close_reason_t reason, std::string_view diagnostic);

    void force_close_sessions (stream_close_reason_t reason, std::string_view diagnostic);

    template <typename Close>
    void terminate_session (stream_t &stream,
                            const std::shared_ptr<session_liveness_t> &liveness,
                            const Close &close,
                            stream_close_reason_t reason,
                            std::string_view diagnostic)
    {
        if (!liveness->try_terminate (reason))
            return;
        _runtime.send_session_closing (stream, reason, diagnostic,
                                       [close, reason] { close (reason); });
    }

    template <typename Close>
    void evaluate_liveness (stream_t &stream,
                            const std::shared_ptr<session_liveness_t> &liveness,
                            const session_liveness_t::reader_t &reader,
                            const Close &close)
    {
        switch (liveness->evaluate (session_liveness_t::clock_t::now (), reader)) {
            case session_liveness_t::decision_t::none:
                break;
            case session_liveness_t::decision_t::send_heartbeat:
                _runtime.send_heartbeat_ping (stream);
                break;
            case session_liveness_t::decision_t::idle_timeout:
                terminate_session (stream, liveness, close, stream_close_reason_t::idle_timeout,
                                   "application idle timeout");
                return;
            case session_liveness_t::decision_t::heartbeat_timeout:
                terminate_session (stream, liveness, close,
                                   stream_close_reason_t::heartbeat_timeout,
                                   "heartbeat inbound timeout");
                return;
        }
    }

    template <typename OnDue>
    static void schedule_liveness (asio::steady_timer &timer,
                                   const session_liveness_t &liveness,
                                   const session_liveness_t::reader_t &reader,
                                   OnDue on_due)
    {
        const auto due = liveness.next_due (reader);
        timer.expires_at (due);
        if (due == session_liveness_t::clock_t::time_point::max ())
            return;
        timer.async_wait ([on_due = std::move (on_due)] (const boost::system::error_code &error) {
            if (!error)
                on_due ();
        });
    }

    static const char *liveness_close_label (stream_close_reason_t reason) noexcept;

    void record_connection_opened () const;

    void record_connection_closed (const char *close_reason) const;

    bool draining () const noexcept;

    template <typename FailureFactory>
    void report_packet_failure (const stream_header_t &header,
                                const FailureFactory &failure_factory) const;

    void report_packet_dispatch_error (const stream_header_t &header,
                                       const result_t<void> &dispatched) const;

    packet_stream_session_t *create_session (service_provider_t &services,
                                             const stream_header_t *first_packet = nullptr);

    template <typename ExceptionFactory>
    void report_control_failure (const ExceptionFactory &exception_factory) const
    {
        detail::dispatch_error_reporter_t (_runtime.dispatch_options_ref ()).report_lazy ([&] {
            message_dispatch_error_event_t error{};
            error.surface = dispatch_error_surface_t::stream_session;
            error.message_kind = dispatch_message_kind_t::control;
            error.reason = dispatch_error_reason_t::handler_exception;
            error.action = dispatch_error_action_t::drop;
            error.exception = exception_factory ();
            return error;
        });
    }

    void run ();

    void run_guarded () noexcept;

    void wait_started ();

    std::string listener_endpoint () const;

    const std::string &name () const noexcept;

    void request_stop () noexcept;

    void stop_connections () noexcept;

  private:
    struct core_session_t;

    struct core_admission_t
    {
        std::shared_ptr<core_session_t> session;
        std::optional<std::uint64_t> rejection_retirement;
    };

    struct core_close_intent_t
    {
        zlink::routing_id_t rid;
        const char *close_reason;
        std::optional<stream_close_reason_t> notify_reason;
        std::string diagnostic;
    };
    using core_work_t = std::variant<core_close_intent_t, std::weak_ptr<core_session_t>>;

    void schedule_core_liveness (const std::shared_ptr<core_session_t> &session);

    void run_core_liveness (const std::weak_ptr<core_session_t> &session);

    task_t<void>
    send_core_frame (zlink::routing_id_t rid,
                     stream_header_t header,
                     zlink::message_t payload,
                     std::shared_ptr<detail::stream_reply_submission_t> admission_reply = {});

    void send_core_error_frame (const zlink::routing_id_t &rid,
                                const stream_t &stream,
                                const stream_header_t &request_header,
                                const result_t<void> &error,
                                std::function<void (bool)> completed = {},
                                detail::stream_metadata_t admission_metadata = {}) noexcept;

    void reject_core_session (const zlink::routing_id_t &rid, const stream_header_t *first_packet);

    void run_core_stream ();

  public:
    void close_core_resources ();
    void flush_stream_trace (std::string_view reason) noexcept;

  private:
    bool begin_actor_binding_replacement (
      const std::shared_ptr<replacement_session_state_t> &state,
      const runtime::protocol::bound_session_replaced_t &replacement);

    std::shared_ptr<replacement_session_state_t> register_replacement_session (
      const zlink::routing_id_t &session_rid,
      const stream_t &stream,
      std::function<void ()> close_connection,
      detail::stream_runtime_t::actor_binding_replaced_dispatch_t dispatch_replacement);

    void configure_disconnect (session_actor_manager_t &actors,
                               runtime::stateful::stream_connection_t connection,
                               zlink::routing_id_t session_rid);

    request_task_t<void>
    bind_actor_session (runtime::stateful::stream_connection_t transport_connection,
                        zlink::routing_id_t session_rid,
                        actor_ref_t actor,
                        std::shared_ptr<replacement_session_state_t> replacement,
                        std::uint64_t binding_generation,
                        std::function<void ()> publish_client_binding);

    task_t<void> retire_actor_session_bindings (runtime::stateful::stream_connection_t connection,
                                                zlink::routing_id_t session_rid,
                                                session_actor_manager_t &actors);

    void retire_actor_session_bindings_async (runtime::stateful::stream_connection_t connection,
                                              zlink::routing_id_t session_rid,
                                              session_actor_manager_t &actors,
                                              std::function<void ()> completed);

    void drain_core_monitor_events ();

    void handle_core_monitor_event (const zlink::monitor_event_t &monitor_event);

    std::shared_ptr<core_session_t> get_or_create_core_session (
      const zlink::routing_id_t &rid,
      session_liveness_t::clock_t::time_point established = session_liveness_t::clock_t::now (),
      const stream_header_t *first_packet = nullptr);

    void retire_core_rejection_locked (core_admission_t &admission);

    void close_core_session (const zlink::routing_id_t &rid,
                             const char *close_reason,
                             disconnect_reason_t disconnect_reason) noexcept;

    void finalize_core_session (const std::string &retirement_id,
                                const std::shared_ptr<core_session_t> &current) noexcept;

    void track_retiring_session (std::string retirement_id,
                                 std::shared_ptr<const void> owner,
                                 stream_t stream);

    void forget_retiring_session (const std::string &retirement_id, const void *owner) noexcept;

    void begin_core_session_close (
      std::string retirement_id,
      std::shared_ptr<core_session_t> current,
      const char *close_reason,
      std::optional<stream_close_reason_t> notify_reason = std::nullopt,
      std::string_view diagnostic = {},
      std::optional<disconnect_reason_t> disconnect_reason = std::nullopt) noexcept;

    void wait_for_retiring_sessions () noexcept;

    void disconnect_core_peer (const zlink::routing_id_t &rid, const char *close_reason) noexcept;

    void
    request_core_peer_disconnect (const zlink::routing_id_t &rid,
                                  const char *close_reason,
                                  std::optional<stream_close_reason_t> notify_reason = std::nullopt,
                                  std::string diagnostic = {}) noexcept;

    void post_core_work (core_work_t work) noexcept;

    void drain_pending_core_work () noexcept;

    bool dispatch_core_packet (const zlink::routing_id_t &rid,
                               zlink::message_t payload,
                               stream_header_t header,
                               application_job_queue_t::permit_t application_permit,
                               session_liveness_t::clock_t::time_point received_at);

    bool process_core_packet (zlink::stream_packet_t &packet,
                              application_job_queue_t::permit_t permit,
                              session_liveness_t::clock_t::time_point received_at);

    void close_core_sessions (const char *close_reason = "server_shutdown",
                              std::optional<stream_close_reason_t> notify_reason = std::nullopt,
                              std::string_view diagnostic = {}) noexcept;

    struct stream_write_operation_t;
    struct stream_write_queue_t;
    struct tcp_connection_t;
    struct frame_t;

    void finish_stream_write (const std::shared_ptr<tcp_connection_t> &owner,
                              const std::shared_ptr<stream_write_operation_t> &operation,
                              result_t<boost::system::error_code> result);

    void start_next_stream_write (const std::shared_ptr<tcp_connection_t> &owner);

    template <typename Start>
    std::shared_ptr<stream_write_operation_t>
    queue_stream_write_operation (const std::shared_ptr<tcp_connection_t> &owner, Start &&start);

    void mark_started ();

    void mark_start_failed (std::string message);

    void configure_tls_context ();

    template <typename T, typename Owner, typename... Args>
    static std::shared_ptr<T> make_transport (std::shared_ptr<Owner> lifetime, Args &&...args);

    bool start_ingress ();

    void start_boost_accept ();

    void handle_boost_accept (const std::shared_ptr<tcp_connection_t> &connection,
                              const boost::system::error_code &error);

    void close_tcp_socket (tcp::socket &socket) noexcept;

    void track_socket (const std::shared_ptr<tcp_connection_t> &owner);

    template <typename TStream>
    void track_stream (const std::shared_ptr<tcp_connection_t> &owner,
                       const std::shared_ptr<TStream> &stream);

    template <typename TStream>
    task_t<void> close_transport (std::shared_ptr<tcp_connection_t> owner,
                                  std::shared_ptr<TStream> connection);

    void request_close (tcp::socket *socket) noexcept;

    void request_close (tcp::socket &socket) noexcept;

    template <typename TStream> void request_close (TStream &stream) noexcept;

    void close_connection (tcp::socket &socket) noexcept;

    template <typename TStream, typename Reserve, typename Progress>
    std::optional<frame_t> read_frame (TStream &socket,
                                       const Reserve &reserve,
                                       asio::io_context &io,
                                       const std::atomic_bool *connection_stop,
                                       const Progress &progress);

    template <typename Reserve, typename Progress>
    std::optional<frame_t> read_frame (websocket_stream_t &socket,
                                       const Reserve &reserve,
                                       asio::io_context &io,
                                       const std::atomic_bool *connection_stop,
                                       const Progress &progress);

    std::shared_ptr<std::vector<std::uint8_t>> encode_frame_bytes (const stream_header_t &header,
                                                                   const zlink::message_t &payload);

    template <typename TStream>
    std::shared_ptr<stream_write_operation_t>
    queue_frame (const std::shared_ptr<tcp_connection_t> &owner,
                 const std::shared_ptr<TStream> &connection,
                 const stream_header_t &header,
                 const zlink::message_t &payload);

    std::shared_ptr<stream_write_operation_t>
    queue_frame (const std::shared_ptr<tcp_connection_t> &owner,
                 const std::shared_ptr<websocket_stream_t> &connection,
                 const stream_header_t &header,
                 const zlink::message_t &payload);

    template <typename TStream>
    task_t<void> submit_frame (std::shared_ptr<tcp_connection_t> owner,
                               std::shared_ptr<TStream> connection,
                               stream_header_t header,
                               zlink::message_t payload,
                               detail::stream_reply_submission_t *admission_reply = nullptr);

    template <typename TStream>
    void write_error_frame (const std::shared_ptr<tcp_connection_t> &owner,
                            const std::shared_ptr<TStream> &connection,
                            const stream_t &stream,
                            const stream_header_t &request_header,
                            const result_t<void> &error,
                            detail::stream_metadata_t admission_metadata = {});

    template <typename TStream>
    void handle_stream_connection (std::shared_ptr<tcp_connection_t> owner,
                                   std::shared_ptr<TStream> connection,
                                   tcp::socket *tracked_socket,
                                   asio::io_context &io);

#ifdef ZLINK_FRAMEWORK_STREAM_WITH_OPENSSL
    void handle_tls_connection (std::shared_ptr<tcp_connection_t> owner,
                                std::shared_ptr<ssl::stream<tcp::socket>> connection);
#endif

    void handle_websocket_connection (std::shared_ptr<tcp_connection_t> owner,
                                      std::shared_ptr<websocket_stream_t> connection);

    void handle_connection (std::shared_ptr<tcp_connection_t> connection);

    void trace_stream_host_enabled (std::string_view stage,
                                    const stream_header_t *header = nullptr,
                                    std::string_view detail = {},
                                    const void *operation = nullptr) noexcept;

    struct trace_buffer_t;
    std::mutex _trace_mutex;
    std::unique_ptr<trace_buffer_t> _trace_buffer;

    detail::stream_runtime_t _runtime;
    stream_snapshot_t _stream;
    const char *_metric_transport;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<double>> _opened_metric,
      _closed_metric;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::UpDownCounter<double>> _active_metric;
    std::optional<std::string> _advertise_host;
    detail::stream_session_factory_t _session_factory;
    service_provider_t *_services;
    detail::delay_owner_t *_delay_owner;
    stream_host_service_t &_host;
    std::atomic_bool *_stop;
    std::shared_ptr<std::atomic_bool> _drain_flag;
    std::stop_token _shutdown_cancellation;
    std::shared_ptr<framework::detail::monitoring_runtime_state_t> _monitoring;
    std::shared_ptr<runtime_failure_collector_t> _runtime_failures;
    std::shared_ptr<detail::mesh_node_runtime_t> _mesh_node;
    std::shared_ptr<application_job_queue_t> _application_jobs;
    stream_host_ingress_t &_ingress;
    std::chrono::milliseconds _session_replacement_callback_timeout;

    runtime::state_lane_t _active_streams_lane;
    std::vector<active_session_t> _active_streams;
    asio::io_context &_liveness_io;
    asio::io_context _io;
    tcp::acceptor _acceptor;
    asio::steady_timer _accept_retry_timer;
#ifdef ZLINK_FRAMEWORK_STREAM_WITH_OPENSSL
    std::optional<ssl::context> _tls_context;
#endif
    std::mutex _sockets_mutex;
    std::unordered_map<tcp::socket *, std::function<void ()>> _sockets;
    std::mutex _core_socket_mutex;
    std::unique_ptr<zlink::stream_socket_t> _core_socket;
    std::unique_ptr<zlink::socket_monitor_t> _core_monitor;
    std::unique_ptr<zlink::poller_t> _core_poller;
    eventing::runtime_wake_timer_t _core_wake_timer;
    std::unique_ptr<application_supply_slot_t> _core_application_supply;
    // Core exposes the peer identity after recv_packet. The permit gates this
    // listener's reader, so its receive clock is shared by all Core sessions.
    session_liveness_t::reader_t _core_reader_liveness;
    std::mutex _core_pending_work_mutex;
    std::vector<core_work_t> _core_pending_work;
    std::mutex _core_sessions_mutex;
    std::map<std::string, core_admission_t> _core_sessions;
    std::uint64_t _core_rejection_retirement = 0;
    /* Sessions whose retirement has not completed, Core and TLS/WebSocket
     * alike. stop () waits until this is empty (wait_for_retiring_sessions). */
    struct retiring_session_t
    {
        std::shared_ptr<const void> owner;
        stream_t stream;
    };
    std::map<std::string, retiring_session_t> _retiring_sessions;
    std::condition_variable _core_sessions_changed;
    std::uint64_t _core_sessions_revision = 0;
#ifdef ZLINK_FRAMEWORK_TEST_CREATION_PROBE
    stream_host_shutdown_test_hooks_t _shutdown_test_hooks;
#endif
    stream_host_detail::stream_receive_scheduler_t _receive_scheduler;
    std::mutex _workers_mutex;
    std::vector<std::thread> _workers;
    mutable std::mutex _ready_mutex;
    std::condition_variable _ready_cv;
    std::string _bound_endpoint;
    bool _started = false;
    bool _start_failed = false;
    std::string _start_error;
};

} // namespace zlink::framework::runtime

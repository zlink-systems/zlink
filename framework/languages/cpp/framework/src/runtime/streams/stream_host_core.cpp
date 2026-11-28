/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/streams/stream_host_service_internal.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/disconnect_rid.hpp"
#include "runtime/dispatch/receive_batch_budget.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/messaging/submit_result_mapper.hpp"
#include "runtime/streams/stream_host_ingress.hpp"
#include <zlink/Contracts/Errors/errors.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <array>
#include <cerrno>
#include <span>
#include <stdexcept>

namespace zlink::framework::runtime
{

using detail::stream_error_payload;
using stream_host_detail::session_creation_failed_diagnostic;

namespace
{

detail::backend::binding_completion_observer_t
observe_core_reply_admission (zlink::async_result_t<void> pending,
                              std::shared_ptr<detail::stream_reply_submission_t> reply,
                              task_completion_source_t<void> completion)
{
    try {
        co_await std::move (pending);
        reply->complete ();
        completion.complete (result_t<void>::success ());
    }
    catch (...) {
        completion.complete (detail::current_exception_result<void> ());
    }
}

} // namespace

stream_host_core_test_faults_t &stream_host_core_test_faults () noexcept
{
    static stream_host_core_test_faults_t faults;
    return faults;
}

struct stream_host_service_t::listener_t::core_session_t
{
    detail::service_scope_t scope;
    packet_stream_session_t *session;
    session_actor_manager_t *actors;
    stream_t stream;
    runtime::stateful::stream_connection_t transport_connection;
    std::shared_ptr<replacement_session_state_t> replacement;
    std::shared_ptr<session_liveness_t> liveness;
    std::optional<asio::steady_timer> liveness_timer;
    std::mutex gate;
    bool connected = false;
    bool closing = false;
    bool finalized = false;

    core_session_t (detail::service_scope_t scope_,
                    packet_stream_session_t &session_,
                    session_actor_manager_t &actors_,
                    stream_t stream_,
                    session_liveness_t::clock_t::time_point established,
                    detail::stream_liveness_options_t liveness_options,
                    asio::io_context &liveness_io) :
        scope (std::move (scope_)),
        session (&session_),
        actors (&actors_),
        stream (std::move (stream_)),
        liveness (std::make_shared<session_liveness_t> (
          established, session_liveness_t::clock_t::now (), liveness_options)),
        liveness_timer (std::in_place, liveness_io)
    {
    }
};

void stream_host_service_t::listener_t::schedule_core_liveness (
  const std::shared_ptr<core_session_t> &session)
{
    schedule_liveness (
      *session->liveness_timer, *session->liveness, _core_reader_liveness,
      [listener = weak_from_this (), session = std::weak_ptr<core_session_t> (session)] {
          if (const auto owner = listener.lock ())
              owner->post_core_work (session);
      });
}

void stream_host_service_t::listener_t::run_core_liveness (
  const std::weak_ptr<core_session_t> &session)
{
    const auto current = session.lock ();
    if (!current || current->closing || _stop->load (std::memory_order_acquire))
        return;
    const auto rid = current->stream.routing_id ().value ();
    evaluate_liveness (current->stream, current->liveness, _core_reader_liveness,
                       [this, rid] (stream_close_reason_t reason) {
                           request_core_peer_disconnect (rid, liveness_close_label (reason));
                       });
    schedule_core_liveness (current);
}

/* Coroutine invariant (session-reconnect-and-coroutine-lifetime doc):
 * parameters are taken by value. Callers pass stack locals and
 * temporaries (send_core_error_frame's error_header/payload) that die
 * while this coroutine may still be suspended at the tail await; the
 * message copy is a cheap handle copy, not a buffer copy. */
task_t<void> stream_host_service_t::listener_t::send_core_frame (
  zlink::routing_id_t rid,
  stream_header_t header,
  zlink::message_t payload,
  std::shared_ptr<detail::stream_reply_submission_t> admission_reply)
{
    if (header.kind () == stream_message_kind_t::error
        && stream_host_core_test_faults ().fail_core_error_frame_send.load (
          std::memory_order_acquire)) {
        /* Test-only: forces the pre-suspension throw shape (same as an
         * encode failure or a stopped Core socket below) so tests can pin
         * the error-observation path without a wire-level race. */
        throw framework_exception_t (framework_error_kind_t::unavailable,
                                     "test fault: Core STREAM error frame send");
    }
    auto encoded = _runtime.encode_frame (header, payload);
    if (!encoded) {
        throw framework_exception_t (encoded.error_kind (), encoded.error ()
                                                              ? encoded.error ()->what ()
                                                              : "STREAM frame encode failed");
    }
    auto frame = zlink::message_t::from (std::move (encoded.value ()));
    const auto frame_bytes = frame.size ();

    std::optional<zlink::async_result_t<void>> pending;
    try {
        {
            std::lock_guard socket_lock (_core_socket_mutex);
            if (!_core_socket) {
                throw framework_exception_t (framework_error_kind_t::shutting_down,
                                             "Core STREAM socket is stopped");
            }
            auto submission = _core_socket->send (rid).message (std::move (frame)).async ();
            pending = detail::backend::take_submission_admission (submission);
        }
        if (!pending) {
            if (admission_reply)
                admission_reply->complete ();
        } else if (admission_reply) {
            // The binding reports acceptance without waiting for the
            // application's execution turn to resume.
            task_completion_source_t<void> completion;
            auto accepted = completion.task ();
            observe_core_reply_admission (std::move (*pending), std::move (admission_reply),
                                          std::move (completion));
            co_await accepted;
        } else {
            co_await std::move (*pending);
        }
    }
    catch (const zlink::submit_error_t &error) {
        throw runtime::messaging::map_submit_result_exception (
          pending ? runtime::messaging::map_submit_completion_result (error.result ())
                  : error.result (),
          error.what ());
    }
    trace_stream_host (this, "core-write", &header,
                       "rid=" + rid.to_hex () + " bytes=" + std::to_string (frame_bytes)
                         + " result=admitted");
    co_return;
}

/* Writes the error reply for a rejected request. The optional
 * `completed` continuation runs at the error-frame task terminal (or
 * immediately when no error frame applies); callers use it to sequence
 * a follow-up close behind the error write so the peer observes
 * error -> close order. The continuation may run inline in the
 * submitting context (pre-suspension throw), so it must only record
 * intent (request_core_peer_disconnect), never enter a close. */
void stream_host_service_t::listener_t::send_core_error_frame (
  const zlink::routing_id_t &rid,
  const stream_t &stream,
  const stream_header_t &request_header,
  const result_t<void> &error,
  std::function<void (bool)> completed,
  detail::stream_metadata_t admission_metadata) noexcept
{
    if (request_header.kind () != stream_message_kind_t::request
        || !request_header.request_seq ()) {
        if (completed) {
            completed (true);
        }
        return;
    }

    auto error_header = detail::stream_runtime_t::make_terminal_header (
      stream_message_kind_t::error, request_header, std::move (admission_metadata));
    auto send = detail::session_actor_manager_access_t::submit_terminal (
      stream, error_header, stream_error_payload (error),
      [&] (const stream_header_t &header, const zlink::message_t &payload) {
          return send_core_frame (rid, header, payload);
      });
    detail::observe_task_completion (
      send, [this, rid, completed = std::move (completed)] (const result_t<void> &result) {
          if (completed) {
              completed (static_cast<bool> (result));
          } else if (!result) {
              /* This observer runs inline in the submitting context when
               * send_core_frame throws before its first suspension (the
               * task completes eagerly). That context may hold a session
               * gate, and cpp-internals forbids completing work under a
               * session mutex, so only the close intent is recorded here.
               * The Core loop drains it with no session lock held. */
              request_core_peer_disconnect (rid, "transport_error");
          }
      });
}

void stream_host_service_t::listener_t::reject_core_session (const zlink::routing_id_t &rid,
                                                             const stream_header_t *first_packet)
{
    stream_t transport;
    _runtime.attach_transport_writer (
      transport, [this, rid] (const stream_header_t &header, const zlink::message_t &payload) {
          return send_core_frame (rid, header, payload);
      });
    auto notify = [self = shared_from_this (), rid, transport] (bool) mutable {
        self->_runtime.send_session_closing (
          transport, stream_close_reason_t::transport_error, session_creation_failed_diagnostic,
          [self, rid] { self->request_core_peer_disconnect (rid, "transport_error"); });
    };
    if (first_packet) {
        send_core_error_frame (rid, transport, *first_packet,
                               result_t<void>::failure (framework_error_kind_t::internal_failure,
                                                        session_creation_failed_diagnostic),
                               std::move (notify));
    } else {
        notify (true);
    }
}

void stream_host_service_t::listener_t::run_core_stream ()
{
    try {
        _core_socket = std::make_unique<zlink::stream_socket_t> (_mesh_node->native_context ());
        _core_socket->options ().max_message_size (
          zlink::byte_size_t::bytes (_stream.max_message_size > 0 ? _stream.max_message_size : -1));
        _core_socket->options ().recv_mode (zlink::stream_recv_mode_t::packet);
        _core_monitor = std::make_unique<zlink::socket_monitor_t> (_core_socket->monitor_open (
          zlink::monitor_event::disconnected | zlink::monitor_event::connection_ready));
        _core_socket->bind (_stream.bind_endpoint);
        _bound_endpoint = _core_socket->options ().last_endpoint ();
        const bool started = start_ingress ();
        mark_started ();
        if (!started)
            return;
        _core_poller = std::make_unique<zlink::poller_t> ();
        auto &poller = *_core_poller;
        poller.add (*_core_socket,
                    zlink::poll_event_flag_t::pollin | zlink::poll_event_flag_t::pollout
                      | zlink::poll_event_flag_t::pollcompletion,
                    1);
        poller.add (*_core_monitor, zlink::poll_event_flag_t::pollin, 2);
        _core_wake_timer.attach (poller);
        _core_application_supply = std::make_unique<application_supply_slot_t> (
          _application_jobs, [this] { _core_wake_timer.signal (); });
        const short pollin = static_cast<short> (zlink::poll_event_flag_t::pollin);
        const short pollerr = static_cast<short> (zlink::poll_event_flag_t::pollerr);
        const auto poll_stream = [&] (std::chrono::milliseconds timeout) {
            std::array<zlink::poll_event_t, 3> events;
            const auto count = poller.wait (events.data (), events.size (), timeout);
            short stream_events = 0;
            for (const auto &event : std::span (events).first (count)) {
                if (_core_wake_timer.is_event (event)) {
                    _core_wake_timer.consume ();
                } else if (event.slot == 2) {
                    drain_core_monitor_events ();
                } else if (event.slot == 1) {
                    stream_events = static_cast<short> (event.revents);
                }
            }
            return stream_events;
        };
        try {
            while (!_stop->load (std::memory_order_acquire)) {
                auto revents = poll_stream (std::chrono::milliseconds{-1});
                if (_stop->load (std::memory_order_acquire))
                    break;
                // A wake and socket readiness can arrive separately. Give the
                // existing poller one nonblocking observation before settling work.
                if ((revents & (pollin | pollerr)) == 0)
                    revents = poll_stream (std::chrono::milliseconds::zero ());

                receive_batch_budget_t batch;
                while ((revents & pollin) != 0 && !_stop->load (std::memory_order_acquire)) {
                    // Leave the packet in Core until Framework capacity is
                    // reserved, preserving Core RCVHWM backpressure.
                    _core_application_supply->ensure_waiter ();
                    auto reserved = _core_application_supply->take ();
                    _core_reader_liveness.observe_permit (reserved.has_value ());
                    if (!reserved)
                        break;

                    zlink::stream_packet_t packet;
                    try {
                        if (!_core_socket->recv_packet (packet, zlink::recv_flags_t::dontwait))
                            break;
                    }
                    catch (const zlink::recv_error_t &error) {
                        if (_stop->load (std::memory_order_acquire)
                            || error.result () == zlink::recv_result_t::terminated) {
                            revents = pollerr;
                            break;
                        }
                        if (error.internal_errno () == EMSGSIZE) {
                            trace_stream_host (this, "core-recv-size-rejected", nullptr,
                                               "errno=EMSGSIZE limit="
                                                 + std::to_string (_stream.max_message_size));
                            break;
                        }
                        throw;
                    }
                    const auto received_at = session_liveness_t::clock_t::now ();
                    drain_core_monitor_events ();
                    const auto routing_id = packet.routing_id ();
                    const auto header_bytes = packet.header ().size ();
                    const auto payload_bytes = packet.body ().size ();
                    batch.account (6u + header_bytes + payload_bytes);
                    process_core_packet (packet, std::move (*reserved), received_at);
                    if (routing_id) {
                        trace_stream_host (this, "core-recv", nullptr,
                                           "rid=" + routing_id->to_hex ()
                                             + " header_bytes=" + std::to_string (header_bytes)
                                             + " payload_bytes=" + std::to_string (payload_bytes));
                    }
                    if (batch.exhausted ())
                        break;
                    revents = poll_stream (std::chrono::milliseconds::zero ());
                }
                if ((revents & pollerr) != 0 && (revents & pollin) == 0)
                    break;

                // Only the reader settles liveness, after this receive round.
                drain_pending_core_work ();
            }
        }
        catch (...) {
            if (_core_application_supply) {
                _core_application_supply->close ();
                _core_application_supply.reset ();
            }
            _core_wake_timer.detach ();
            throw;
        }
        if (_core_application_supply) {
            _core_application_supply->close ();
            _core_application_supply.reset ();
        }
        _core_wake_timer.detach ();
    }
    catch (const std::exception &error) {
        mark_start_failed (error.what ());
        _runtime_failures->report (std::current_exception ());
    }
    catch (...) {
        _runtime_failures->report (std::current_exception ());
    }
}

void stream_host_service_t::listener_t::close_core_resources ()
{
    drain_pending_core_work ();
    close_core_sessions ();
    if (_core_poller) {
        _core_poller->close ();
        _core_poller.reset ();
    }
    if (_core_monitor) {
        _core_monitor->close ();
        _core_monitor.reset ();
    }
    {
        std::lock_guard lock (_core_socket_mutex);
        if (_core_socket) {
            (void) _core_socket->close ();
            _core_socket.reset ();
        }
    }
}

void stream_host_service_t::listener_t::drain_core_monitor_events ()
{
    for (;;) {
        auto monitor_event = _core_monitor->recv (zlink::recv_flags_t::dontwait);
        if (!monitor_event)
            break;
        handle_core_monitor_event (*monitor_event);
    }
}

void stream_host_service_t::listener_t::handle_core_monitor_event (
  const zlink::monitor_event_t &monitor_event)
{
    if (stream_host_connection_ready (monitor_event)) {
        try {
            get_or_create_core_session (*monitor_event.routing_id);
        }
        catch (const std::exception &) {
            report_control_failure ([] { return std::current_exception (); });
            disconnect_core_peer (*monitor_event.routing_id, "transport_error");
        }
    }
    if (monitor_event.event == zlink::monitor_event::disconnected && monitor_event.routing_id) {
        close_core_session (*monitor_event.routing_id, "client_close",
                            static_cast<disconnect_reason_t> (monitor_event.value));
    }
}

std::shared_ptr<stream_host_service_t::listener_t::core_session_t>
stream_host_service_t::listener_t::get_or_create_core_session (
  const zlink::routing_id_t &rid,
  session_liveness_t::clock_t::time_point established,
  const stream_header_t *first_packet)
{
    const auto key = runtime::stateful::stream_session_registry_t::connection_key (rid);
    {
        const std::lock_guard lock (_core_sessions_mutex);
        if (const auto found = _core_sessions.find (key); found != _core_sessions.end ())
            return found->second.session;
    }

    const auto reject_if_draining = [this] {
        if (draining ())
            throw framework_exception_t (framework_error_kind_t::rejected,
                                         "STREAM node is draining and rejects a new session");
    };
    reject_if_draining ();

    // The Core listener processes both monitor events and packets on
    // its ingress loop, so only one factory can run for this RID.
    auto scope =
      detail::service_scope_t::create (*_services, detail::service_scope_kind_t::stream_session);
    auto *session = create_session (scope.provider (), first_packet);
    if (!session) {
        {
            const std::lock_guard lock (_core_sessions_mutex);
            _core_sessions.emplace (key, core_admission_t{});
        }
        reject_core_session (rid, first_packet);
        return {};
    }
    auto &actors = scope.provider ().get_required<session_actor_manager_t> ();
    auto stream = _runtime.open_session (_stream.name);
    _runtime.set_session_identity (stream, rid);
    detail::session_actor_manager_access_t::attach (actors, stream);
    _runtime.attach_transport_writer (stream, [this, rid] (const stream_header_t &header,
                                                           const zlink::message_t &payload,
                                                           const auto &admission_reply) {
        return send_core_frame (rid, header, payload, admission_reply);
    });
    auto created =
      std::make_shared<core_session_t> (std::move (scope), *session, actors, std::move (stream),
                                        established, _stream.liveness, _liveness_io);
    {
        const std::lock_guard lock (_core_sessions_mutex);
        reject_if_draining ();
        // Publish the accepted stream before drain can take its snapshot.
        // Its close intent will run on this ingress after setup finishes.
        _core_sessions.emplace (key, core_admission_t{created, std::nullopt});
        register_active_stream (
          created->stream, [this, rid] (stream_close_reason_t reason, std::string diagnostic) {
              request_core_peer_disconnect (rid, liveness_close_label (reason), reason,
                                            std::move (diagnostic));
          });
    }

    record_connection_opened ();
    // The ingress close path owns cleanup if setup or Connected submission fails.
    created->transport_connection = _mesh_node->native_node ().sessions ().open (
      key, [this, rid] { request_core_peer_disconnect (rid, "transport_error"); },
      _mesh_node->routing_id (), _mesh_node->native_node ().status ().lifecycle_generation (),
      [this, rid] () -> task_t<void> {
          _core_socket->disconnect_rid (rid);
          co_return;
      });
    auto replacement = register_replacement_session (
      rid, created->stream, [this, rid] { request_core_peer_disconnect (rid, "transport_error"); },
      [weak = std::weak_ptr<core_session_t> (created)] (stream_t &dispatch_stream,
                                                        std::string actor_id) {
          const auto owner = weak.lock ();
          if (!owner || owner->session == nullptr) {
              return task_t<void> (result_t<void>::success ());
          }
          return owner->session->on_actor_binding_replaced (dispatch_stream, std::move (actor_id));
      });
    created->replacement = replacement;
    detail::session_actor_manager_access_t::bind_native (
      actors,
      [this, transport_connection = created->transport_connection, rid,
       replacement] (actor_ref_t actor, std::uint64_t binding_generation, std::uint16_t,
                     std::function<void ()> publish) -> request_task_t<void> {
          return bind_actor_session (transport_connection, rid, actor, replacement,
                                     binding_generation, std::move (publish));
      });
    configure_disconnect (actors, created->transport_connection, rid);
    auto connected = _runtime.dispatch_connected_async (
      *created->session, created->stream, [this, created, rid] (const result_t<void> &result) {
          if (!result)
              request_core_peer_disconnect (rid, "transport_error");
      });
    if (!connected) {
        throw framework_exception_t (connected.error_kind (),
                                     connected.error () ? connected.error ()->what ()
                                                        : "STREAM connected dispatch failed");
    }
    created->connected = true;
    schedule_core_liveness (created);
#ifdef ZLINK_FRAMEWORK_TEST_CREATION_PROBE
    if (_shutdown_test_hooks.registered)
        _shutdown_test_hooks.registered (shared_from_this (), created);
#endif
    return created;
}

void stream_host_service_t::listener_t::retire_core_rejection_locked (core_admission_t &admission)
{
    if (admission.rejection_retirement)
        return;
    const auto retired_at = ++_core_rejection_retirement;
    admission.rejection_retirement = retired_at;
    // Core can still yield queued packets after DISCONNECTED. Keep the
    // latest rejected admissions in the session index, including on a
    // continuously busy listener where a no-data boundary may never occur.
    constexpr std::uint64_t retirement_batch = 2048;
    if (retired_at % retirement_batch == 0) {
        std::erase_if (_core_sessions, [retired_at] (const auto &entry) {
            return entry.second.rejection_retirement
                   && retired_at - *entry.second.rejection_retirement >= retirement_batch;
        });
    }
}

void stream_host_service_t::listener_t::close_core_session (
  const zlink::routing_id_t &rid,
  const char *close_reason,
  disconnect_reason_t disconnect_reason) noexcept
{
    const auto key = runtime::stateful::stream_session_registry_t::connection_key (rid);
    std::shared_ptr<core_session_t> current;
    std::string retirement_id;
    {
        std::lock_guard lock (_core_sessions_mutex);
        const auto found = _core_sessions.find (key);
        if (found == _core_sessions.end ())
            return;
        if (!found->second.session) {
            retire_core_rejection_locked (found->second);
            return;
        }
        current = std::move (found->second.session);
        retirement_id = current->stream.session_id ();
        _core_sessions.erase (found);
        ++_core_sessions_revision;
    }
    _core_sessions_changed.notify_all ();
    begin_core_session_close (std::move (retirement_id), std::move (current), close_reason,
                              std::nullopt, {}, disconnect_reason);
}

void stream_host_service_t::listener_t::finalize_core_session (
  const std::string &retirement_id, const std::shared_ptr<core_session_t> &current) noexcept
{
    std::unique_lock lock (current->gate);
    if (current->finalized)
        return;
    current->finalized = true;
    auto scope = std::move (current->scope);
    lock.unlock ();
    scope.close ();
    flush_stream_trace ("connection-end");
    forget_retiring_session (retirement_id, current.get ());
}

void stream_host_service_t::listener_t::begin_core_session_close (
  std::string retirement_id,
  std::shared_ptr<core_session_t> current,
  const char *close_reason,
  std::optional<stream_close_reason_t> notify_reason,
  std::string_view diagnostic,
  std::optional<disconnect_reason_t> disconnect_reason) noexcept
{
    {
        const std::lock_guard session_lock (current->gate);
        if (current->closing) {
            return;
        }
        current->closing = true;
        record_connection_closed (close_reason);
        if (current->replacement) {
            current->replacement->deactivate ();
        }
    }
    current->liveness_timer.reset ();
    track_retiring_session (retirement_id, current, current->stream);
    unregister_active_stream (current->stream);
    auto complete_close = [this, retirement_id, current, disconnect_reason] {
        bool dispatch_disconnected = false;
        {
            const std::lock_guard session_lock (current->gate);
            if (current->connected) {
                current->connected = false;
                dispatch_disconnected = true;
            }
        }
        if (dispatch_disconnected)
            _runtime.mark_disconnected (current->stream);

        const auto session_rid = current->stream.routing_id ().value_or (
          zlink::routing_id_t::from (current->stream.session_id ()));
        retire_actor_session_bindings_async (
          current->transport_connection, session_rid, *current->actors,
          [this, retirement_id, current, dispatch_disconnected, disconnect_reason] {
              auto finalize = [this, retirement_id, current] (const result_t<void> &) {
                  finalize_core_session (retirement_id, current);
              };
              if (dispatch_disconnected) {
                  auto disconnected = [this, current, finalize] (const result_t<void> &) {
                      auto submitted = _runtime.dispatch_disconnected_async (
                        *current->session, current->stream, finalize);
                      if (!submitted)
                          finalize (submitted);
                  };
                  if (disconnect_reason) {
                      stream_error_t error (stream_session_error_t::transport_error,
                                            "STREAM disconnected");
                      error._disconnect_reason = disconnect_reason;
                      auto submitted = _runtime.dispatch_error_async (
                        *current->session, current->stream, error, disconnected);
                      if (!submitted)
                          disconnected (submitted);
                  } else {
                      disconnected (result_t<void>::success ());
                  }
              } else {
                  finalize (result_t<void>::success ());
              }
          });
        {
            const std::lock_guard lock (_core_sessions_mutex);
            ++_core_sessions_revision;
        }
        _core_sessions_changed.notify_all ();
    };
    if (notify_reason && !current->liveness->forced ()) {
        _runtime.send_session_closing (current->stream, *notify_reason, diagnostic,
                                       std::move (complete_close));
    } else {
        complete_close ();
    }
}

void stream_host_service_t::listener_t::disconnect_core_peer (const zlink::routing_id_t &rid,
                                                              const char *close_reason) noexcept
{
    try {
        std::shared_ptr<core_session_t> current;
        bool rejected = false;
        const auto key = runtime::stateful::stream_session_registry_t::connection_key (rid);
        {
            const std::lock_guard lock (_core_sessions_mutex);
            const auto found = _core_sessions.find (key);
            if (found != _core_sessions.end ()) {
                current = found->second.session;
                rejected = !current;
            }
        }
        if (current)
            begin_core_session_close (current->stream.session_id (), current, close_reason);
        if (_core_socket) {
            disconnect_rid_if_present ([this, &rid] { _core_socket->disconnect_rid (rid); });
            if (rejected) {
                const std::lock_guard lock (_core_sessions_mutex);
                if (const auto found = _core_sessions.find (key); found != _core_sessions.end ())
                    retire_core_rejection_locked (found->second);
            }
        }
        if (!current)
            flush_stream_trace ("connection-failed");
    }
    catch (...) {
        detail::dispatch_error_reporter_t (_runtime.dispatch_options_ref ()).report_lazy ([] {
            message_dispatch_error_event_t error{};
            error.surface = dispatch_error_surface_t::stream_session;
            error.message_kind = dispatch_message_kind_t::control;
            error.reason = dispatch_error_reason_t::handler_exception;
            error.action = dispatch_error_action_t::drop;
            error.exception = std::current_exception ();
            return error;
        });
    }
}

// Completion observers may hold a session gate. The Core loop executes
// deferred work without that gate, in the same owner as receive.
void stream_host_service_t::listener_t::request_core_peer_disconnect (
  const zlink::routing_id_t &rid,
  const char *close_reason,
  std::optional<stream_close_reason_t> notify_reason,
  std::string diagnostic) noexcept
{
    post_core_work (core_close_intent_t{rid, close_reason, notify_reason, std::move (diagnostic)});
}

void stream_host_service_t::listener_t::post_core_work (core_work_t work) noexcept
{
    try {
        {
            const std::lock_guard lock (_core_pending_work_mutex);
            _core_pending_work.push_back (std::move (work));
        }
        _core_wake_timer.signal ();
    }
    catch (...) {
        report_control_failure ([] { return std::current_exception (); });
    }
}

void stream_host_service_t::listener_t::drain_pending_core_work () noexcept
{
    std::vector<core_work_t> pending;
    {
        const std::lock_guard lock (_core_pending_work_mutex);
        pending.swap (_core_pending_work);
    }
    for (auto &work : pending) {
        if (const auto *session = std::get_if<std::weak_ptr<core_session_t>> (&work)) {
            try {
                run_core_liveness (*session);
            }
            catch (...) {
                report_control_failure ([] { return std::current_exception (); });
            }
            continue;
        }
        auto &intent = std::get<core_close_intent_t> (work);
        if (intent.notify_reason) {
            std::shared_ptr<core_session_t> current;
            {
                const std::lock_guard lock (_core_sessions_mutex);
                const auto found = _core_sessions.find (
                  runtime::stateful::stream_session_registry_t::connection_key (intent.rid));
                if (found != _core_sessions.end ())
                    current = found->second.session;
            }
            if (current)
                terminate_session (
                  current->stream, current->liveness,
                  [this, rid = intent.rid] (stream_close_reason_t reason) {
                      request_core_peer_disconnect (rid, liveness_close_label (reason));
                  },
                  *intent.notify_reason, intent.diagnostic);
        } else {
            disconnect_core_peer (intent.rid, intent.close_reason);
        }
    }
}

bool stream_host_service_t::listener_t::dispatch_core_packet (
  const zlink::routing_id_t &rid,
  zlink::message_t payload,
  stream_header_t header,
  application_job_queue_t::permit_t application_permit,
  session_liveness_t::clock_t::time_point received_at)
{
    trace_stream_host (this, "core-frame", &header,
                       "rid=" + rid.to_hex ()
                         + " payload_bytes=" + std::to_string (payload.size ()));
    if (header.kind () == stream_message_kind_t::control) {
        std::shared_ptr<core_session_t> current;
        {
            const std::lock_guard lock (_core_sessions_mutex);
            const auto found = _core_sessions.find (
              runtime::stateful::stream_session_registry_t::connection_key (rid));
            if (found != _core_sessions.end ()) {
                current = found->second.session;
                if (!current)
                    return false;
            }
        }
        if (current)
            current->liveness->record_inbound (received_at, false);
        stream_t stream;
        _runtime.set_session_identity (stream, rid);
        _runtime.attach_transport_writer (
          stream, [this, rid] (const stream_header_t &control, const zlink::message_t &body,
                               const auto &) { return send_core_frame (rid, control, body); });
        try {
            _runtime.dispatch_control_frame (stream, header, payload);
            return true;
        }
        catch (...) {
            disconnect_core_peer (rid, "protocol_error");
            return false;
        }
    }
    std::shared_ptr<core_session_t> current;
    try {
        current = get_or_create_core_session (rid, received_at, &header);
    }
    catch (...) {
        report_control_failure ([] { return std::current_exception (); });
        disconnect_core_peer (rid, "protocol_error");
        return false;
    }
    if (!current)
        return false;

    current->liveness->record_inbound (received_at, true);
    std::optional<result_t<void>> rejected;
    std::optional<detail::stream_metadata_t> admission_metadata;
    try {
        if (current->replacement
            && current->replacement->closing.load (std::memory_order_acquire)) {
            return false;
        }

        std::lock_guard session_lock (current->gate);
        if (current->connected && !current->closing) {
            trace_stream_host (this, "core-dispatch-submit", &header, "rid=" + rid.to_hex ());
            auto dispatched = _ingress.publish (
              _runtime, *current->session, current->stream, header, payload,
              std::move (application_permit),
              [this, current, rid, header] (const result_t<void> &result) {
                  trace_stream_host (
                    this, "core-dispatch-complete", &header,
                    std::string ("result=") + (result ? "success" : "failure")
                      + (result
                           ? std::string ()
                           : " kind=" + std::to_string (static_cast<int> (result.error_kind ()))
                               + " error="
                               + (result.error () ? result.error ()->what () : "unknown")));
                  if (!result) {
                      report_packet_dispatch_error (header, result);
                      send_core_error_frame (rid, current->stream, header, result);
                  }
              },
              [this] { return _stop->load (std::memory_order_acquire); },
              [&] (const result_t<void> &error, detail::stream_metadata_t metadata) {
                  rejected.emplace (error);
                  admission_metadata = std::move (metadata);
              });
            if (!dispatched) {
                /* Session-gate invariant (cpp-internals): no task may be
                 * completed and no close may be entered while
                 * current->gate is held. Only the rejection is recorded
                 * here; the report, the error-frame write, and the close
                 * all run below, after the gate is released. */
                rejected.emplace (dispatched);
            }
        }
    }
    catch (...) {
        disconnect_core_peer (rid, "protocol_error");
        return false;
    }
    if (rejected) {
        trace_stream_host (
          this, "core-dispatch-rejected", &header,
          "rid=" + rid.to_hex ()
            + " kind=" + std::to_string (static_cast<int> (rejected->error_kind ()))
            + " error=" + (rejected->error () ? rejected->error ()->what () : "unknown"));
        report_packet_dispatch_error (header, *rejected);
        /* error -> close order: the protocol close is sequenced behind
         * the error-frame task terminal (disconnect_rid tears the
         * connection down immediately, so submission order alone would
         * let the close overtake the pending error write). The
         * continuation only records the close intent; the Core loop
         * executes it outside every session lock, and
         * begin_core_session_close keeps the duplicate a no-op. */
        if (rejected->error_kind () == framework_error_kind_t::protocol_error) {
            send_core_error_frame (
              rid, current->stream, header, *rejected, [this, rid] (bool sent) {
                  request_core_peer_disconnect (rid, sent ? "protocol_error" : "transport_error");
              });
            return false;
        }
        send_core_error_frame (
          rid, current->stream, header, *rejected, {},
          std::move (admission_metadata).value_or (detail::stream_metadata_t{}));
    }
    return true;
}

bool stream_host_service_t::listener_t::process_core_packet (
  zlink::stream_packet_t &packet,
  application_job_queue_t::permit_t permit,
  session_liveness_t::clock_t::time_point received_at)
{
    if (!packet.routing_id ()) {
        return false;
    }
    const auto rid = *packet.routing_id ();
    auto header_bytes = packet.header ().to_bytes ();
    auto decoded = _runtime.decode_header (header_bytes);
    if (!decoded) {
        disconnect_core_peer (rid, "protocol_error");
        return false;
    }

    return dispatch_core_packet (rid, std::move (packet.body ()), std::move (decoded.value ()),
                                 std::move (permit), received_at);
}

void stream_host_service_t::listener_t::close_core_sessions (
  const char *close_reason,
  std::optional<stream_close_reason_t> notify_reason,
  std::string_view diagnostic) noexcept
{
    struct retiring_core_session_t
    {
        std::string retirement_id;
        std::shared_ptr<core_session_t> session;
    };
    std::vector<retiring_core_session_t> sessions;
    {
        std::lock_guard lock (_core_sessions_mutex);
        sessions.reserve (_core_sessions.size ());
        for (auto &[key, admission] : _core_sessions) {
            auto &current = admission.session;
            if (!current) {
                continue;
            }
            auto retirement_id = current->stream.session_id ();
            sessions.push_back (retiring_core_session_t{std::move (retirement_id), current});
        }
        _core_sessions.clear ();
        ++_core_sessions_revision;
    }
    _core_sessions_changed.notify_all ();
    for (auto &retiring : sessions) {
        begin_core_session_close (std::move (retiring.retirement_id), std::move (retiring.session),
                                  close_reason, notify_reason, diagnostic);
    }
    wait_for_retiring_sessions ();
}

#ifdef ZLINK_FRAMEWORK_TEST_CREATION_PROBE
std::function<bool ()>
stream_host_service_test_access_t::probe_core_session_gate (stream_host_service_t &service,
                                                            const zlink::routing_id_t &rid)
{
    const auto listener = service.listener_snapshot ().at (0);
    const std::lock_guard lock (listener->_core_sessions_mutex);
    auto session =
      listener->_core_sessions.at (stateful::stream_session_registry_t::connection_key (rid))
        .session;
    return [session = std::move (session)] {
        const std::unique_lock lock (session->gate, std::try_to_lock);
        return lock.owns_lock ();
    };
}
#endif

void stream_host_service_test_access_t::create_core_sessions (
  stream_host_service_t &service, const std::vector<zlink::routing_id_t> &rids)
{
    for (const auto &listener : service.listener_snapshot ()) {
        if (!listener->_core_socket)
            continue;
        for (const auto &rid : rids)
            (void) listener->get_or_create_core_session (rid);
        return;
    }
    throw std::logic_error ("test access could not find the Core STREAM listener");
}

void stream_host_service_test_access_t::deliver_core_monitor_event (
  stream_host_service_t &service, const zlink::monitor_event_t &event)
{
    for (const auto &listener : service.listener_snapshot ()) {
        if (listener->_core_socket) {
            listener->handle_core_monitor_event (event);
            return;
        }
    }
    throw std::logic_error ("test access could not find the Core STREAM listener");
}

void stream_host_service_test_access_t::expire_core_sessions (
  stream_host_service_t &service, const std::vector<zlink::routing_id_t> &rids)
{
    for (const auto &listener : service.listener_snapshot ()) {
        if (!listener->_core_socket)
            continue;
        for (const auto &rid : rids) {
            // These test-created sessions have no live Core route. Exercise
            // Core's typed NOT_FOUND result before normal queued expiration cleanup.
            disconnect_rid_if_present (
              [&listener, &rid] { listener->_core_socket->disconnect_rid (rid); });
            listener->request_core_peer_disconnect (rid, "server_shutdown");
        }
        return;
    }
    throw std::logic_error ("test access could not find the Core STREAM listener");
}

} // namespace zlink::framework::runtime

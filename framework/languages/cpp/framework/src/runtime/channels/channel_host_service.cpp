/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/channels/channel_host_service.hpp"
#include "runtime/channels/channel_host_service_test_access.hpp"
#include "runtime/configuration/service_scope.hpp"

#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/channels/channel_packet_dispatcher.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/channels/channel_socket_options.hpp"
#include "runtime/channels/socket_monitor_event.hpp"
#include "runtime/client_server/raw_client_server_owner.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/dispatch/receive_batch_budget.hpp"
#include "runtime/fanout/fanout_subscription.hpp"
#include "runtime/handlers/handler_registry_runtime.hpp"
#include "runtime/transport/listener_identity.hpp"

#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Eventing/events.hpp>
#include <zlink/Contracts/Eventing/monitor.hpp>
#include <zlink/Contracts/Eventing/poll_event.hpp>
#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/Contracts/Messaging/received.hpp>
#include <zlink/Contracts/Messaging/topic_message.hpp>
#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/pubsub_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/results.hpp>
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <optional>
#include <set>
#include <utility>

namespace zlink::framework::runtime
{

namespace
{
constexpr std::uint64_t monitor_poll_slot = 2;
constexpr std::size_t channel_handler_worker_limit = 8;
constexpr std::chrono::milliseconds channel_poll_interval{50};
constexpr std::chrono::milliseconds channel_worker_idle_timeout{100};
} // namespace

class channel_host_service_t::server_loop_t
{
  public:
    friend struct manual_channel_server_test_access_t;

    static std::unique_ptr<offload_executor_t> make_handler_executor ()
    {
        const auto workers =
          static_cast<std::size_t> (std::max (1u, std::thread::hardware_concurrency ()));
        return std::make_unique<offload_executor_t> (
          0, std::min<std::size_t> (workers, channel_handler_worker_limit),
          channel_worker_idle_timeout, "zlink-channel-server");
    }

    server_loop_t (message_bus_t bus,
                   std::string channel_name,
                   std::string endpoint,
                   std::optional<std::string> advertise_host,
                   std::optional<zlink::routing_id_t> routing_id,
                   channel_capability_snapshot_t capability,
                   service_provider_t &services,
                   serializer_registry_t &serializers,
                   const handler_registry_t &handlers,
                   std::atomic_bool &stop,
                   std::shared_ptr<zlink::context_t> core_context,
                   std::shared_ptr<application_job_queue_t> application_jobs,
                   std::shared_ptr<listener_status_registry_t> listener_statuses,
                   std::unique_ptr<offload_executor_t> handler_executor) :
        _runtime (detail::channel_runtime_t::from (bus)),
        _dispatcher (_runtime),
        _channel_name (std::move (channel_name)),
        _capability (std::move (capability)),
        _services (&services),
        _serializers (&serializers),
        _handlers (&handlers),
        _stop (&stop),
        _application_jobs (std::move (application_jobs)),
        _listener_statuses (std::move (listener_statuses)),
        _context (std::move (core_context)),
        _router (std::make_unique<zlink::router_socket_t> (*_context)),
        _handler_executor (std::move (handler_executor))
    {
        detail::apply_weighted_channel_socket_options (*_router, _capability);
        if (routing_id) {
            _router->set_routing_id (*routing_id);
        }
        _monitor = _router->monitor_open (
          zlink::monitor_event::connected | zlink::monitor_event::accepted
          | zlink::monitor_event::connection_ready | zlink::monitor_event::disconnected
          | zlink::monitor_event::closed | zlink::monitor_event::handshake_failed_no_detail
          | zlink::monitor_event::handshake_failed_protocol
          | zlink::monitor_event::handshake_failed_auth);
        _router->bind (endpoint);
        _poller.add (_monitor, zlink::poll_event_flag_t::pollin, monitor_poll_slot);
        auto server_routing_id = zlink::routing_id_t::from ("server");
        _router->get_routing_id (server_routing_id);
        protocol::client_server_server_admission_t descriptor;
        descriptor.channel_name = _channel_name;
        descriptor.server_routing_id = server_routing_id.to_bytes ();
        descriptor.advertised_endpoint = transport::advertised_tcp_endpoint (
          _router->options ().last_endpoint (), advertise_host, "ClientServer");
        descriptor.descriptor_revision = 1;
        descriptor.lifecycle_generation = client_server::make_lifecycle_generation ();
        descriptor.state = mesh::service_node_state_t::serving;
        descriptor.security_identity = transport::default_security_identity;
        descriptor.weight = static_cast<std::uint32_t> (_capability.service_weight);
        descriptor.effective_max_message_bytes = static_cast<std::uint32_t> (
          _capability.max_message_size ? _capability.max_message_size->bytes ()
                                       : std::numeric_limits<std::int32_t>::max ());
        client_server::raw_client_server_server_options_t options{descriptor, advertise_host};
        options.transport_poller = &_poller;
        options.transport_poller_slot = 1;
        options.runtime_failures = _runtime.runtime_failures ();
        _source = std::make_unique<client_server::raw_client_server_server_t> (
          std::move (options), _context, &_runtime.state_executor ());
        _source->start (*_router);
        _source->mailbox ().bind_application_dispatch ({}, [this] (const std::string &owner) {
            try {
                if (_handler_executor->try_submit_internal ([this, owner] { drain_owner (owner); }))
                    return;
            }
            catch (...) {
                _runtime.runtime_failures ()->report (std::current_exception ());
            }
            // A rejected work item accepted no invocation. The mailbox remains the
            // sole claim/settlement owner; each queued permit dies with its record.
            auto &mailbox = _source->mailbox ();
            if (!mailbox.begin_application_drain (owner))
                return;
            try {
                while (auto claim =
                         mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application,
                                                  owner, dispatch_limits::receive_batch_messages,
                                                  dispatch_limits::receive_batch_bytes)) {
                    try {
                        for (std::size_t index = 0; index != claim->claimed_messages; ++index) {
                            if (auto record = mailbox.take ({claim, index}))
                                report_dispatch_rejection (
                                  messaging::message_parts_t (std::move (record->parts)));
                        }
                    }
                    catch (...) {
                        mailbox.release (*claim);
                        throw;
                    }
                    mailbox.release (*claim);
                }
            }
            catch (...) {
                _runtime.runtime_failures ()->report (std::current_exception ());
            }
            mailbox.end_application_drain (owner);
        });
        if (_listener_statuses)
            _listener_statuses->update (
              listener_kind_t::client_server, _channel_name,
              transport::advertised_tcp_endpoint (_router->options ().last_endpoint (),
                                                  std::move (advertise_host), "ClientServer"));
    }

    ~server_loop_t () { stop (); }

    void signal_activity () noexcept { _source->signal_activity (); }

    void run ()
    {
        while (!_stop->load (std::memory_order_acquire)) {
            apply_runtime_options ();
            drain_monitor_events ();
            const auto now = mesh::service_liveness_registry_t::clock_t::now ();
            (void) _source->tick_liveness_owned (now);
            auto next = _source->next_liveness_activity_owned ();
            const auto remaining = [&] {
                return next
                         ? std::max (std::chrono::milliseconds::zero (),
                                     std::chrono::ceil<std::chrono::milliseconds> (
                                       *next - mesh::service_liveness_registry_t::clock_t::now ()))
                         : std::chrono::milliseconds (-1);
            };
            try {
                const auto readiness = _source->poll (remaining ());
                if (!readiness || *readiness == zlink::poll_event_flag_t::none)
                    continue;
            }
            catch (...) {
                break;
            }
            if (_stop->load (std::memory_order_acquire))
                break;
            drain_monitor_events ();
            auto permit = _supply.take_batch (
              *_application_jobs, std::chrono::milliseconds::zero (),
              [this] { _source->signal_activity (); }, application_job_queue_t::origin_t::remote,
              static_cast<std::uint32_t> (dispatch_limits::receive_batch_messages));
            if (!permit) {
                (void) _source->poll (remaining (), false);
                continue;
            }
            if (!*permit)
                break;
            auto &mailbox = _source->mailbox ();
            mailbox.begin_application_receive_turn ();
            try {
                receive_batch_budget_t budget;
                std::vector<client_server::received_application_record_t> activity;
                auto next_permit = (**permit).take ();
                while (next_permit && budget.can_receive ()
                       && !_stop->load (std::memory_order_acquire)) {
                    const int rc = _router->recv (_received, zlink::recv_flags_t::dontwait);
                    if (rc != static_cast<int> (zlink::recv_result_t::ok))
                        break;
                    std::size_t bytes = 0;
                    for (const auto &part : _received.parts ())
                        bytes += part.size ();
                    budget.account (bytes);
                    if (auto control = _source->consume_control (_received)) {
                        _received.close ();
                        next = _source->next_liveness_activity_owned ();
                    } else {
                        if (next && _received.routing_id ()) {
                            mesh::service_mailbox_record_t record{};
                            record.source_routing_id = _received.routing_id ()->to_bytes ();
                            activity.push_back ({std::move (record),
                                                 {},
                                                 mesh::service_liveness_registry_t::clock_t::now (),
                                                 _received.route_generation ()});
                        }
                        enqueue_application (_received, std::move (next_permit));
                    }
                    next_permit = (**permit).take ();
                }
                if (!activity.empty ())
                    _source->record_application_activity (activity);
            }
            catch (...) {
                mailbox.end_application_receive_turn ();
                throw;
            }
            mailbox.end_application_receive_turn ();
        }
        _source->mailbox ().close ();
        if (_handler_executor)
            _handler_executor->drain ();
        _source->mailbox ().bind_application_dispatch ({}, {});
        drain_monitor_events ();
    }

    void stop ()
    {
        runtime_failure_collector_t failures;
        if (_source)
            failures.capture ([&] { _source->mailbox ().close (); });
        if (_handler_executor)
            failures.capture ([&] { _handler_executor->drain (); });
        if (_source) {
            _source->mailbox ().bind_application_dispatch ({}, {});
            failures.capture ([&] { _source->close (); });
        }
        failures.capture ([&] { _poller.close (); });
        if (_monitor.valid ())
            failures.capture ([&] { _monitor.close (); });
        if (_router)
            failures.capture ([&] {
                _router->close ();
                _router.reset ();
            });
        if (_listener_statuses)
            failures.capture (
              [&] { _listener_statuses->remove (listener_kind_t::client_server, _channel_name); });
        failures.rethrow_if_failed ();
    }

  private:
    void enqueue_application (zlink::received_t &received, application_job_queue_t::permit_t permit)
    {
        auto application_job =
          std::make_shared<application_job_queue_t::permit_t> (std::move (permit));
        mesh::service_mailbox_record_t record{};
        record.owner =
          mesh::service_mailbox_t::application_owner (host::owner_kind_t::channel, _channel_name);
        record.domain = mesh::service_mailbox_domain_t::application;
        if (received.routing_id ())
            record.source_routing_id = received.routing_id ()->to_bytes ();
        record.reply_token = received.reply_token ();
        record.before_application_handler = [application_job] {
            application_job->release_for_handler_entry ();
        };
        auto parts = messaging::message_parts_t (std::move (received.parts ()));
        detail::message_flow_tracer_t flow (_runtime.dispatch_options_ref ());
        if (flow.enabled_for (message_flow_outcome_t::received)) {
            auto header = messaging::envelope_codec_t{}.decode_header (parts, true);
            if (header && header.value ().kind != messaging::message_kind_t::publish) {
                auto flow_scope =
                  flow_context_t::enter (header.value ().flow_id, header.value ().flow_origin,
                                         flow.mode (), flow_origin_t::inbound, std::nullopt);
                if (!header.value ().flow_id && flow_context_t::current ()) {
                    header.value ().flow_id = flow_context_t::current ()->flow_id;
                    header.value ().flow_origin = flow_context_t::current ()->origin;
                    auto items = std::move (parts).take_items ();
                    items[0] = messaging::envelope_codec_t{}.encode_header (header.value ());
                    parts = messaging::message_parts_t (std::move (items));
                }

                flow.trace (message_flow_outcome_t::received,
                            header.value ().flow_id ? std::string_view (*header.value ().flow_id)
                                                    : std::string_view{},
                            [&] {
                                message_flow_event_t event{message_flow_outcome_t::received,
                                                           dispatch_error_surface_t::channel,
                                                           header.value ().kind
                                                               == messaging::message_kind_t::request
                                                             ? dispatch_message_kind_t::request
                                                             : dispatch_message_kind_t::send,
                                                           header.value ().message_name,
                                                           _channel_name,
                                                           std::nullopt,
                                                           header.value ().correlation_id,
                                                           std::nullopt,
                                                           std::nullopt,
                                                           std::nullopt,
                                                           std::nullopt};
                                event.flow_id = header.value ().flow_id;
                                event.flow_origin = header.value ().flow_origin;

                                return event;
                            });
                flow.trace (message_flow_outcome_t::admitted,
                            header.value ().flow_id ? std::string_view (*header.value ().flow_id)
                                                    : std::string_view{},
                            [&] {
                                message_flow_event_t event{message_flow_outcome_t::admitted,
                                                           dispatch_error_surface_t::channel,
                                                           header.value ().kind
                                                               == messaging::message_kind_t::request
                                                             ? dispatch_message_kind_t::request
                                                             : dispatch_message_kind_t::send,
                                                           header.value ().message_name,
                                                           _channel_name,
                                                           std::nullopt,
                                                           header.value ().correlation_id,
                                                           std::nullopt,
                                                           std::nullopt,
                                                           std::nullopt,
                                                           std::nullopt};
                                event.flow_id = header.value ().flow_id;
                                event.flow_origin = header.value ().flow_origin;

                                return event;
                            });
            }
        }
        received.close ();
        record.parts = std::move (parts).take_items ();
        if (!_source->mailbox ().try_enqueue (
              std::move (record), [application_job] { application_job->mark_queued (); }))
            report_dispatch_rejection (messaging::message_parts_t (std::move (record.parts)));
    }

    void drain_owner (const std::string &owner)
    {
        auto &mailbox = _source->mailbox ();
        if (!mailbox.begin_application_drain (owner))
            return;
        const auto deadline =
          std::chrono::steady_clock::now () + dispatch_limits::owner_time_budget;
        std::shared_ptr<mesh::service_mailbox_claim_t> claim;
        try {
            do {
                claim = mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application, owner,
                                                 dispatch_limits::receive_batch_messages,
                                                 dispatch_limits::receive_batch_bytes);
                if (!claim)
                    break;
                for (std::size_t index = 0; index != claim->claimed_messages; ++index) {
                    if (auto record = mailbox.take ({claim, index})) {
                        // An ordinary Channel claim reserves FIFO entry, not terminal lifetime.
                        // Preserve the drain ambient while a suspended pipeline keeps its own.
                        flow_context_t::scope_t flow_scope (flow_context_t::current ());
                        dispatch_application (std::move (*record));
                    }
                }
                mailbox.release (*claim);
                claim.reset ();
            } while (std::chrono::steady_clock::now () < deadline);
        }
        catch (...) {
            if (claim)
                mailbox.release (*claim);
            _runtime.runtime_failures ()->report (std::current_exception ());
        }
        mailbox.end_application_drain (owner);
    }

    void dispatch_application (mesh::service_mailbox_record_t record)
    {
        auto routing_id = record.source_routing_id.empty ()
                            ? std::optional<zlink::routing_id_t>{}
                            : std::optional<zlink::routing_id_t> (
                                zlink::routing_id_t::from (record.source_routing_id));
        auto reply_token = std::move (record.reply_token);
        auto parts = messaging::message_parts_t (std::move (record.parts));
        try {
            auto scope = detail::service_scope_t::create (
              *_services, detail::service_scope_kind_t::handler_invocation);
            auto finish = [this, routing_id = std::move (routing_id),
                           reply_token] (const result_t<messaging::message_parts_t> &result,
                                         const messaging::message_parts_t &parts) {
                std::exception_ptr failure;
                try {
                    if (result) {
                        if (result.value ().size () != 0 && routing_id && reply_token)
                            reply (*routing_id, *reply_token, result.value ());
                    } else {
                        failure = result.exception ();
                    }
                }
                catch (...) {
                    failure = std::current_exception ();
                }
                if (!result || failure)
                    report_dispatch_failure (parts, failure);
            };
            auto pending = _dispatcher.dispatch_server_message_in_handler (
              _channel_name, std::move (parts), *_services, *_serializers, *_handlers,
              std::move (record.before_application_handler), std::move (scope),
              _handler_executor.get (), std::move (finish));
        }
        catch (...) {
            report_dispatch_failure (parts, std::current_exception ());
        }
    }

    void report_dispatch_rejection (const messaging::message_parts_t &parts)
    {
        detail::dispatch_error_reporter_t (_runtime.dispatch_options_ref ()).report_lazy ([&] {
            auto decoded = messaging::envelope_codec_t{}.decode_header (
              parts,
              detail::message_flow_tracer_t (_runtime.dispatch_options_ref ()).flow_tracing ());
            message_dispatch_error_event_t event{
              dispatch_error_surface_t::channel,
              decoded && decoded.value ().kind == messaging::message_kind_t::request
                ? dispatch_message_kind_t::request
                : dispatch_message_kind_t::send,
              dispatch_error_reason_t::shutdown,
              dispatch_error_action_t::drop,
              decoded ? std::optional<std::string> (decoded.value ().message_name) : std::nullopt,
              _channel_name,
              std::nullopt,
              std::nullopt,
              std::nullopt,
              std::nullopt,
              decoded ? std::optional<std::string> (decoded.value ().correlation_id)
                      : std::nullopt};
            if (decoded) {
                event.flow_id = decoded.value ().flow_id;
                event.flow_origin = decoded.value ().flow_origin;
            }
            return event;
        });
    }

    void report_dispatch_failure (const messaging::message_parts_t &parts,
                                  std::exception_ptr failure)
    {
        detail::dispatch_error_reporter_t (_runtime.dispatch_options_ref ()).report_lazy ([&] {
            auto decoded = messaging::envelope_codec_t{}.decode_header (
              parts,
              detail::message_flow_tracer_t (_runtime.dispatch_options_ref ()).flow_tracing ());
            message_dispatch_error_event_t event{
              dispatch_error_surface_t::channel,
              decoded && decoded.value ().kind == messaging::message_kind_t::request
                ? dispatch_message_kind_t::request
                : dispatch_message_kind_t::send,
              dispatch_error_reason_t::handler_exception,
              dispatch_error_action_t::drop,
              decoded ? std::optional<std::string> (decoded.value ().message_name) : std::nullopt,
              _channel_name,
              std::nullopt,
              std::nullopt,
              std::nullopt,
              std::nullopt,
              decoded ? std::optional<std::string> (decoded.value ().correlation_id) : std::nullopt,
              failure};
            if (decoded) {
                event.flow_id = decoded.value ().flow_id;
                event.flow_origin = decoded.value ().flow_origin;
            }
            return event;
        });
    }

    void reply (const zlink::routing_id_t &routing_id,
                const zlink::reply_token_t &reply_token,
                messaging::message_parts_t parts)
    {
        try {
            auto messages = std::move (parts).take_items ();
            auto operation = _router->reply (routing_id, reply_token).message (messages[0]);
            for (std::size_t index = 1; index < messages.size (); ++index)
                operation = std::move (operation).message (messages[index]);
            std::move (operation).submit ();
        }
        catch (...) {
            const auto error = std::current_exception ();
            detail::dispatch_error_reporter_t (_runtime.dispatch_options_ref ()).report_lazy ([&] {
                return message_dispatch_error_event_t{dispatch_error_surface_t::channel,
                                                      dispatch_message_kind_t::response,
                                                      dispatch_error_reason_t::unexpected_reply,
                                                      dispatch_error_action_t::drop,
                                                      std::nullopt,
                                                      _channel_name,
                                                      std::nullopt,
                                                      std::nullopt,
                                                      std::nullopt,
                                                      std::nullopt,
                                                      std::nullopt,
                                                      error};
            });
        }
    }

    void apply_runtime_options ()
    {
        const auto peer_weight = _runtime.server_peer_weight_override (_channel_name);
        if (!peer_weight || (_applied_peer_weight && *_applied_peer_weight == *peer_weight)) {
            return;
        }
        _router->options ().peer_weight (
          zlink::peer_weight_t::value (static_cast<std::uint32_t> (std::min (*peer_weight, 100))));
        auto descriptor = _source->descriptor_owned ();
        if (descriptor.descriptor_revision == std::numeric_limits<std::uint64_t>::max ())
            throw std::overflow_error ("ClientServer descriptor revision is exhausted");
        ++descriptor.descriptor_revision;
        descriptor.weight = static_cast<std::uint32_t> (*peer_weight);
        _source->update_descriptor_owned (std::move (descriptor));
        _applied_peer_weight = *peer_weight;
    }

    void drain_monitor_events ()
    {
        if (!_monitor.valid ()) {
            return;
        }
        for (;;) {
            std::optional<zlink::monitor_event_t> event;
            try {
                event = _monitor.recv (zlink::recv_flags_t::dontwait);
            }
            catch (...) {
                return;
            }
            if (!event) {
                return;
            }
            _source->observe_monitor_event (*event);
            const auto kind = detail::map_socket_monitor_event (event->event);
            if (!kind) {
                continue;
            }
            if (!event->remote_addr.empty ()) {
                if (*kind == detail::socket_event_kind_t::connected) {
                    _pending_handshake_remotes.insert (event->remote_addr);
                } else if (*kind == detail::socket_event_kind_t::connection_ready) {
                    _pending_handshake_remotes.erase (event->remote_addr);
                } else if (*kind == detail::socket_event_kind_t::disconnected
                           && _pending_handshake_remotes.erase (event->remote_addr) != 0) {
                    _runtime.publish_socket_event (_channel_name,
                                                   detail::socket_event_kind_t::handshake_failed,
                                                   event->local_addr, event->remote_addr);
                }
            }
            _runtime.publish_socket_event (_channel_name, *kind, event->local_addr,
                                           event->remote_addr);
        }
    }

    detail::channel_runtime_t _runtime;
    detail::channel_packet_dispatcher_t _dispatcher;
    std::string _channel_name;
    channel_capability_snapshot_t _capability;
    service_provider_t *_services;
    serializer_registry_t *_serializers;
    const handler_registry_t *_handlers;
    std::atomic_bool *_stop;
    std::shared_ptr<application_job_queue_t> _application_jobs;
    application_job_queue_t::supply_request_t _supply;
    std::shared_ptr<listener_status_registry_t> _listener_statuses;
    std::shared_ptr<zlink::context_t> _context;
    std::unique_ptr<zlink::router_socket_t> _router;
    zlink::received_t _received;
    zlink::socket_monitor_t _monitor;
    zlink::poller_t _poller;
    std::unique_ptr<client_server::raw_client_server_server_t> _source;
    std::set<std::string> _pending_handshake_remotes;
    std::optional<int> _applied_peer_weight;
    std::unique_ptr<offload_executor_t> _handler_executor;
};

manual_channel_server_test_access_t::manual_channel_server_test_access_t (
  message_bus_t bus,
  channel_snapshot_t channel,
  service_provider_t &services,
  serializer_registry_t &serializers,
  const handler_registry_t &handlers,
  std::atomic_bool &stop,
  std::shared_ptr<zlink::context_t> context,
  std::shared_ptr<application_job_queue_t> jobs,
  std::shared_ptr<listener_status_registry_t> listeners,
  std::unique_ptr<offload_executor_t> executor) :
    _loop (std::make_unique<channel_host_service_t::server_loop_t> (
      bus,
      channel.name,
      detail::client_server_bind_endpoint (channel.server),
      std::nullopt,
      channel.server.routing_id,
      channel.server,
      services,
      serializers,
      handlers,
      stop,
      std::move (context),
      std::move (jobs),
      std::move (listeners),
      std::move (executor)))
{
}

manual_channel_server_test_access_t::~manual_channel_server_test_access_t () = default;
void manual_channel_server_test_access_t::run ()
{
    _loop->run ();
}
void manual_channel_server_test_access_t::signal_activity ()
{
    _loop->signal_activity ();
}
std::size_t manual_channel_server_test_access_t::pending_messages () const
{
    return _loop->_source->mailbox ().pending_messages (
      mesh::service_mailbox_domain_t::application);
}

std::size_t manual_channel_server_test_access_t::peer_count () const
{
    return _loop->_router->routes_snapshot ().size ();
}

class channel_host_service_t::subscriber_loop_t
{
  public:
    subscriber_loop_t (message_bus_t bus,
                       std::string channel_name,
                       detail::channel_runtime_bundle_t &bundle,
                       channel_capability_snapshot_t capability,
                       service_provider_t &services,
                       serializer_registry_t &serializers,
                       const handler_registry_t &handlers,
                       std::atomic_bool &stop,
                       std::shared_ptr<zlink::context_t> core_context,
                       std::shared_ptr<application_job_queue_t> application_jobs) :
        _runtime (detail::channel_runtime_t::from (bus)),
        _channel_name (std::move (channel_name)),
        _bundle (&bundle),
        _capability (std::move (capability)),
        _services (&services),
        _serializers (&serializers),
        _handlers (&handlers),
        _stop (&stop),
        _application_jobs (std::move (application_jobs)),
        _context (std::move (core_context)),
        _subscriber (std::make_unique<zlink::sub_socket_t> (*_context))
    {
        detail::apply_common_channel_socket_options (*_subscriber, _capability);
        fanout::apply_fanout_subscriptions (*_subscriber, _capability.subscription_topics);
        apply_runtime_connections ();
        const auto hardware_workers =
          static_cast<std::size_t> (std::max (1u, std::thread::hardware_concurrency ()));
        const auto max_handler_workers = std::max<std::size_t> (
          1, std::min<std::size_t> (hardware_workers, channel_handler_worker_limit));
        _handler_executor = std::make_unique<offload_executor_t> (
          0, max_handler_workers, channel_worker_idle_timeout, "zlink-channel-subscriber");
        _poller.add (*_subscriber, zlink::poll_event_flag_t::pollin, 1);
    }

    ~subscriber_loop_t () { stop (); }

    void run ()
    {
        while (!_stop->load (std::memory_order_acquire)) {
            apply_runtime_connections ();
            zlink::poll_event_t readiness;
            std::size_t ready_count = 0;
            try {
                ready_count = _poller.wait (&readiness, 1, channel_poll_interval);
            }
            catch (...) {
                break;
            }
            if (_stop->load (std::memory_order_acquire) || ready_count != 1 || readiness.slot != 1
                || (static_cast<short> (readiness.revents)
                    & static_cast<short> (zlink::poll_event_flag_t::pollin))
                     == 0) {
                continue;
            }
            // Permit before receive (Application job queue §3); runtime connection
            // changes keep applying while the supply is pending (hot path I1).
            auto permit =
              _supply.take (*_application_jobs, channel_poll_interval, {},
                            zlink::framework::runtime::application_job_queue_t::origin_t::remote);
            if (!permit)
                continue;
            if (!*permit)
                break;
            const int rc =
              _subscriber->subscribe (_received_message, zlink::recv_flags_t::dontwait);
            if (rc == static_cast<int> (zlink::recv_result_t::no_data)) {
                continue;
            }
            if (rc != static_cast<int> (zlink::recv_result_t::ok)) {
                continue;
            }
            dispatch_async (
              std::make_shared<zlink::topic_message_t> (std::move (_received_message)),
              std::move (**permit));
        }
    }

    void stop ()
    {
        runtime_failure_collector_t failures;
        if (_handler_executor)
            failures.capture ([&] { _handler_executor->drain (); });
        failures.capture ([&] { _poller.close (); });
        if (_subscriber)
            failures.capture ([&] {
                _subscriber->close ();
                _subscriber.reset ();
            });
        failures.rethrow_if_failed ();
    }

  private:
    void dispatch_async (std::shared_ptr<zlink::topic_message_t> message,
                         application_job_queue_t::permit_t permit)
    {
        auto application_job =
          std::make_shared<application_job_queue_t::permit_t> (std::move (permit));
        application_job->mark_queued ();
        auto parts = zlink::framework::runtime::messaging::message_parts_t (
          detail::backend::copy_binding_messages (message->parts ()));
        auto shared_parts =
          std::make_shared<zlink::framework::runtime::messaging::message_parts_t> (
            std::move (parts));
        auto work = [this, message = std::move (message), shared_parts,
                     application_job] () mutable {
            try {
                detail::channel_packet_dispatcher_t dispatcher (_runtime);
                auto scope = detail::service_scope_t::create (
                  *_services, detail::service_scope_kind_t::handler_invocation);
                (void) dispatcher.dispatch_server_message (
                  _channel_name, std::move (*shared_parts), scope.provider (), *_serializers,
                  *_handlers, [application_job] { application_job->release_for_handler_entry (); });
            }
            catch (...) {
            }
        };
        if (!_handler_executor || !_handler_executor->try_submit_internal (std::move (work))) {
        }
    }

    void apply_runtime_connections ()
    {
        if (_bundle == nullptr || _subscriber == nullptr) {
            return;
        }
        std::set<std::string> desired;
        for (const auto &endpoint : _bundle->list_manual_connections ()) {
            desired.insert (endpoint);
            if (_connected.insert (endpoint).second) {
                _subscriber->connect (endpoint);
            }
        }
        for (auto it = _connected.begin (); it != _connected.end ();) {
            if (desired.find (*it) != desired.end ()) {
                ++it;
                continue;
            }
            try {
                _subscriber->disconnect (*it);
            }
            catch (...) {
            }
            it = _connected.erase (it);
        }
    }

    detail::channel_runtime_t _runtime;
    std::string _channel_name;
    detail::channel_runtime_bundle_t *_bundle;
    channel_capability_snapshot_t _capability;
    service_provider_t *_services;
    serializer_registry_t *_serializers;
    const handler_registry_t *_handlers;
    std::atomic_bool *_stop;
    std::shared_ptr<application_job_queue_t> _application_jobs;
    application_job_queue_t::supply_request_t _supply;
    std::shared_ptr<zlink::context_t> _context;
    std::unique_ptr<zlink::sub_socket_t> _subscriber;
    zlink::topic_message_t _received_message;
    zlink::poller_t _poller;
    std::set<std::string> _connected;
    std::unique_ptr<offload_executor_t> _handler_executor;
};

channel_host_service_t::channel_host_service_t (
  std::shared_ptr<runtime_failure_collector_t> failures,
  message_bus_t bus,
  std::vector<channel_snapshot_t> channels,
  handler_registry_t &handlers,
  serializer_registry_t &serializers,
  std::map<std::string, std::string> advertise_hosts,
  std::shared_ptr<application_job_queue_t> application_jobs,
  std::shared_ptr<listener_status_registry_t> listener_statuses) :
    hosted_service_lifecycle_t (std::move (failures)),
    _bus (std::move (bus)),
    _channels (std::move (channels)),
    _advertise_hosts (std::move (advertise_hosts)),
    _handlers (&handlers),
    _serializers (&serializers),
    _core_context (detail::channel_runtime_t::from (_bus).core_context ()),
    _application_jobs (
      application_jobs
        ? std::move (application_jobs)
        : std::make_shared<application_job_queue_t> (application_job_queue_configuration_t{
            application_job_queue_profile_t::balanced, std::nullopt, 1,
            static_cast<std::uint32_t> (std::numeric_limits<std::int32_t>::max ())})),
    _listener_statuses (std::move (listener_statuses))
{
}

channel_host_service_t::~channel_host_service_t () = default;

task_t<void> channel_host_service_t::start (service_provider_t &services)
{
    _services = &services;
    auto manager = detail::channel_runtime_manager_t::from (_bus);
    const bool shared_client_server_runtime_active =
      detail::channel_runtime_t::from (_bus).auto_connect_active ();
    _stop.store (false, std::memory_order_release);
    try {
        for (const auto &channel : _channels) {
            if (!channel.server.enabled || shared_client_server_runtime_active
                || channel.server.bind_endpoints.empty ()) {
                continue;
            }
            const auto advertise_host = _advertise_hosts.find (channel.name);
            auto loop = std::make_unique<server_loop_t> (
              _bus, channel.name, detail::client_server_bind_endpoint (channel.server),
              advertise_host == _advertise_hosts.end ()
                ? std::nullopt
                : std::optional<std::string> (advertise_host->second),
              channel.server.routing_id, channel.server, services, *_serializers, *_handlers, _stop,
              _core_context, _application_jobs, _listener_statuses,
              server_loop_t::make_handler_executor ());
            auto *raw = loop.get ();
            _loops.push_back (std::move (loop));
            _threads.emplace_back ([raw] { raw->run (); });
        }
        for (const auto &channel : _channels) {
            if (!channel.subscriber.enabled || channel.subscriber.discovery
                || (!channel.subscriber.discovery
                    && channel.subscriber.connect_endpoints.empty ())) {
                continue;
            }
            auto &bundle = manager.get_or_create_subscriber_bundle (channel.name);
            auto loop = std::make_unique<subscriber_loop_t> (
              _bus, channel.name, bundle, channel.subscriber, services, *_serializers, *_handlers,
              _stop, _core_context, _application_jobs);
            auto *raw = loop.get ();
            _subscriber_loops.push_back (std::move (loop));
            _threads.emplace_back ([raw] { raw->run (); });
        }
    }
    catch (...) {
        // A partial start leaves joinable loop threads; stop joins them before the
        // failure leaves this service.
        stop ();
        throw;
    }
    detail::channel_runtime_t::from (_bus).bind_server_options_changed ([this] {
        for (const auto &loop : _loops)
            loop->signal_activity ();
    });
    return task_t<void> (result_t<void>::success ());
}

void channel_host_service_t::request_stop () noexcept
{
    _stop.store (true, std::memory_order_release);
    for (const auto &loop : _loops)
        loop->signal_activity ();
    if (_application_jobs)
        _application_jobs->stop ();
}

void channel_host_service_t::stop () noexcept
{
    request_stop ();
    _runtime_failures->capture (
      [&] { detail::channel_runtime_t::from (_bus).bind_server_options_changed ({}); });
    for (auto &thread : _threads)
        if (thread.joinable ())
            _runtime_failures->capture ([&] { thread.join (); });
    std::erase_if (_threads, [] (auto &thread) { return !thread.joinable (); });
    std::erase_if (
      _loops, [&] (auto &loop) { return _runtime_failures->capture ([&] { loop->stop (); }); });
    std::erase_if (_subscriber_loops, [&] (auto &loop) {
        return _runtime_failures->capture ([&] { loop->stop (); });
    });
    _services = nullptr;
}

} // namespace zlink::framework::runtime

/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/diagnostics/mesh_trace.hpp"
#include "runtime/diagnostics/diagnostic_event_sink.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/submit_result_mapper.hpp"

#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include <zlink/framework/contracts/configuration/mesh_node.hpp>
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/mesh/service_channel_membership.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"

#include <array>
#include "runtime/mesh/user_spot_terminal_mapping.hpp"
#include "runtime/dispatch/application_job_receive_flow.hpp"
#include "runtime/dispatch/blocking_task.hpp"
#include "runtime/transport/listener_identity.hpp"

#include "runtime/protocol/service_wire_codec.hpp"

#include <service_wire_pilot_codec.hpp>
#include <service_wire_codec.hpp>

#include <zlink/Contracts/Core/byte_count.hpp>
#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Eventing/events.hpp>
#include <zlink/Contracts/Eventing/poll_event.hpp>
#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Errors/errors.hpp>
#include <zlink/Contracts/Sockets/results.hpp>
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>


#include <nlohmann/json.hpp>
#include <opentelemetry/metrics/provider.h>

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace zlink::framework::detail
{
std::uint64_t next_connection_intent_id ()
{
    static std::atomic_uint64_t next{1};
    return next.fetch_add (1, std::memory_order_relaxed);
}
}

namespace zlink::framework::runtime::mesh
{

namespace
{

detail::backend::raw_message_t request_failure_reply_parts (std::uint64_t correlation,
                                                            zlink::request_result_t terminal)
{
    const auto result = static_cast<std::uint32_t> (terminal);
    return {protocol::encode_reply_header (
      correlation, result, messaging::request_failure_mapper_t{}.reply_failure_code (result))};
}

using ::zlink::framework::detail::mesh_trace_enabled;

const char *admission_rejection_reason (peer_admission_result_t result) noexcept
{
    switch (result) {
        case peer_admission_result_t::mesh_mismatch:
            return "mesh_mismatch";
        case peer_admission_result_t::invalid_descriptor:
            return "invalid_descriptor";
        case peer_admission_result_t::stale_descriptor:
            return "stale_descriptor";
        default:
            return "admission_rejected";
    }
}

void log_admission_rejection (const raw_mesh_node_options_t &options,
                              std::string reason,
                              std::string intent_endpoint,
                              std::string advertised_endpoint) noexcept
{
    detail::diagnostic_event_sink_t::log_if_configured (
      detail::dispatch_options_access_t::logger (options.dispatch), log_level_t::warn,
      "RouteMesh peer admission rejected",
      {{"reason", std::move (reason)},
       {"intent_endpoint", std::move (intent_endpoint)},
       {"advertised_endpoint", std::move (advertised_endpoint)}});
}

void trace_mesh_enabled (const std::string &message)
{
    std::cerr << "zlink mesh " << message << '\n';
}

// Keep all string composition behind the opt-in gate. A function call would
// evaluate concatenation arguments before entering the function.
#define trace_mesh(message)                                                                        \
    do {                                                                                           \
        if (mesh_trace_enabled ())                                                                 \
            trace_mesh_enabled (message);                                                          \
    } while (false)

std::string trace_owner_key (const void *owner)
{
    std::ostringstream stream;
    stream << std::hex << reinterpret_cast<std::uintptr_t> (owner);
    return stream.str ();
}

// Admission records keep the Core selected-route generation as their opaque
// connection identity (Core ROUTER §10.1); only equality is meaningful.
using detail::backend::route_connection_id;

std::size_t raw_received_bytes (const detail::backend::raw_received_t &received) noexcept
{
    std::size_t total = 0;
    for (const auto &part : received.parts) {
        if (part.size () > std::numeric_limits<std::size_t>::max () - total)
            return std::numeric_limits<std::size_t>::max ();
        total += part.size ();
    }
    return total;
}

foundation::operation_terminal_t
request_metric_terminal (const protocol::reply_header_t &reply) noexcept
{
    if (reply.terminal_result == 0)
        return foundation::operation_terminal_t::completed;
    // Reuse the canonical reply classification, including workerTimedOut;
    // metrics must not introduce another wire failure-code table.
    switch (user_spot_terminal::map_user_spot_wire_failure (reply, false)) {
        case framework_error_kind_t::deadline_exceeded:
            return foundation::operation_terminal_t::timed_out;
        case framework_error_kind_t::shutting_down:
            return foundation::operation_terminal_t::shutdown;
        default:
            return foundation::operation_terminal_t::transport_failed;
    }
}

foundation::operation_registry_t::clock_t::time_point
capture_request_deadline (std::chrono::milliseconds timeout)
{
    if (timeout <= std::chrono::milliseconds::zero ())
        throw std::invalid_argument ("raw mesh request timeout must be positive");
    return foundation::operation_registry_t::clock_t::now () + timeout;
}

// Binding completion delivers the registered operation's terminal envelope.
void observe_infrastructure_request (
  std::shared_ptr<task_t<detail::backend::raw_request_completion_t>> native,
  std::shared_ptr<foundation::operation_registry_t> operations,
  foundation::call_id_t operation,
  std::optional<std::uint64_t> correlation,
  std::function<std::vector<std::uint8_t> (const detail::backend::raw_message_t &)> decode_reply)
{
    detail::observe_task_terminal (*native, [native, operations = std::move (operations), operation,
                                             correlation, decode_reply = std::move (decode_reply)] (
                                              const auto &settled) {
        if (operations->contains (operation)) {
            if (!settled) {
                (void) operations->fail (operation,
                                         foundation::operation_terminal_t::transport_failed);
            } else {
                const auto &result = settled.value ();
                trace_mesh ("infrastructure-request-result correlation="
                            + std::to_string (correlation.value_or (0))
                            + " result=" + std::to_string (static_cast<int> (result.terminal)));
                {
                    try {
                        if (result.terminal != zlink::request_result_t::ok) {
                            if (!correlation)
                                (void) operations->fail (
                                  operation,
                                  messaging::request_failure_mapper_t{}
                                    .transport_terminal (result.terminal)
                                    .value_or (foundation::operation_terminal_t::transport_failed));
                            else if (const auto terminal =
                                       messaging::request_failure_mapper_t{}.transport_terminal (
                                         result.terminal))
                                (void) operations->fail (operation, *terminal);
                            else
                                (void) operations->complete (
                                  operation,
                                  detail::backend::materialize_binding_part (decode_reply (
                                    request_failure_reply_parts (*correlation, result.terminal))));
                        } else {
                            if (result.parts.empty ())
                                throw protocol::service_wire_error_t (
                                  "infrastructure reply has no header");
                            if (correlation
                                && protocol::decode_reply_header (
                                     protocol::message_bytes (result.parts.front ()))
                                       .correlation
                                     != *correlation)
                                throw protocol::service_wire_error_t (
                                  "infrastructure reply correlation does not match");
                            (void) operations->complete (
                              operation, detail::backend::materialize_binding_part (decode_reply (
                                           detail::backend::copy_binding_parts (result.parts))));
                        }
                    }
                    catch (const protocol::service_wire_error_t &) {
                        (void) operations->fail (operation,
                                                 foundation::operation_terminal_t::protocol_error);
                    }
                }
            }
        }
    });
}

foundation::operation_registry_t::callback_t
request_admission_completion (foundation::operation_registry_t::callback_t callback,
                              std::shared_ptr<task_completion_source_t<void>> admitted)
{
    return [callback = std::move (callback), admitted = std::move (admitted)] (
             foundation::operation_terminal_t terminal, zlink::message_t payload) {
        if (terminal != foundation::operation_terminal_t::completed) {
            if (terminal == foundation::operation_terminal_t::cancelled)
                admitted->complete (detail::result_access_t::failure<void> (
                  detail::make_cancellation_exception ("request was cancelled")));
            else
                admitted->complete (detail::boundary_failure<void> (
                  terminal == foundation::operation_terminal_t::timed_out
                    ? detail::boundary_error_t::timed_out
                  : terminal == foundation::operation_terminal_t::shutdown
                    ? detail::boundary_error_t::shutdown
                    : detail::boundary_error_t::disconnected,
                  "request ended before admission"));
        }
        callback (terminal, std::move (payload));
    };
}

std::shared_ptr<task_t<detail::backend::raw_request_completion_t>>
start_admitted_request (const std::shared_ptr<detail::backend::raw_route_port_t> &port,
                        const admitted_peer_t *peer,
                        const std::vector<std::uint8_t> &target_routing_id,
                        detail::backend::raw_message_t &request_parts,
                        foundation::operation_registry_t::clock_t::time_point deadline,
                        const std::shared_ptr<task_completion_source_t<void>> &admitted)
{
    if (!peer || !peer->ready)
        return {};
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds> (
      deadline - foundation::operation_registry_t::clock_t::now ());
    if (remaining <= std::chrono::milliseconds::zero ())
        return {};
    return std::make_shared<task_t<detail::backend::raw_request_completion_t>> (port->request (
      target_routing_id, std::move (request_parts), remaining, peer->liveness, admitted));
}

// Readiness waits observe the same final completion as the caller. The
// registry owns timeout; a ready peer admits one binding submission.
template <typename TTerminal>
task_t<void> run_request_admission (
  std::shared_ptr<detail::backend::raw_route_port_t> port,
  service_topology_registry_t::peer_observer_t observe_peer,
  std::shared_ptr<foundation::operation_registry_t> operations,
  foundation::call_id_t operation,
  std::optional<std::uint64_t> correlation,
  std::vector<std::uint8_t> target_routing_id,
  detail::backend::raw_message_t request_parts,
  std::function<std::vector<std::uint8_t> (const detail::backend::raw_message_t &)> decode_reply,
  foundation::operation_registry_t::clock_t::time_point deadline,
  task_t<TTerminal> terminal,
  std::shared_ptr<task_completion_source_t<void>> admitted,
  std::function<void (std::shared_ptr<task_t<detail::backend::raw_request_completion_t>>)> observe =
    {})
{
    struct wait_subscriptions_t
    {
        std::stop_source owner;
        ~wait_subscriptions_t () { owner.request_stop (); }
    };
    std::optional<std::pair<std::uint64_t, std::uint64_t>> baseline;
    bool waiting = false;
    while (operations->contains (operation)) {
        auto observation = observe_peer ();
        // A waiting request checks failure history even if admission recovered.
        const auto attempt = waiting || !observation.peer || !observation.peer->ready
                               ? observation.connection_attempt ()
                               : std::nullopt;
        if (waiting
            && ((baseline
                 && (!attempt || baseline->first != attempt->intent_id
                     || baseline->second != attempt->last_failure_id))
                || (!baseline && attempt && attempt->last_failure_id != 0))) {
            (void) operations->fail (operation,
                                     foundation::operation_terminal_t::route_unavailable);
            co_return;
        }
        if (auto native =
              start_admitted_request (port, observation.peer ? &*observation.peer : nullptr,
                                      target_routing_id, request_parts, deadline, admitted)) {
            if (observe)
                observe (std::move (native));
            else
                observe_infrastructure_request (std::move (native), operations, operation,
                                                correlation, std::move (decode_reply));
            co_return;
        }
        if (foundation::operation_registry_t::clock_t::now () >= deadline) {
            (void) co_await terminal;
            co_return;
        }
        if ((!observation.peer || observation.peer->connection_id.empty ()) && attempt
            && attempt->state == zlink::connection_attempt_state_t::attempt_failed) {
            (void) operations->fail (operation,
                                     foundation::operation_terminal_t::route_unavailable);
            co_return;
        }
        if (!baseline && attempt)
            baseline = std::pair{attempt->intent_id, attempt->last_failure_id};
        waiting = true;
        auto wake = std::make_shared<task_completion_source_t<void>> ();
        auto notified = wake->task ();
        const auto notify = [weak = std::weak_ptr{wake}] (const auto &) {
            if (const auto signal = weak.lock ())
                signal->complete (result_t<void>::success ());
        };
        wait_subscriptions_t subscriptions;
        {
            detail::ambient_context_scope_t subscription_scope (
              nullptr, nullptr, subscriptions.owner.get_token (), false, true);
            detail::observe_task_completion (observation.changed, notify);
            detail::observe_task_completion (terminal, notify);
        }
        co_await notified;
    }
}
template <typename TTerminal>
task_t<bool> submit_registered_request (
  std::shared_ptr<detail::backend::raw_route_port_t> port,
  service_topology_registry_t::peer_observer_t observe_peer,
  std::shared_ptr<foundation::operation_registry_t> operations,
  foundation::call_id_t operation,
  std::optional<std::uint64_t> correlation,
  std::vector<std::uint8_t> target_routing_id,
  detail::backend::raw_message_t request_parts,
  std::function<std::vector<std::uint8_t> (const detail::backend::raw_message_t &)> decode_reply,
  foundation::operation_registry_t::clock_t::time_point deadline,
  task_t<TTerminal> terminal,
  std::shared_ptr<task_completion_source_t<void>> admitted = {},
  std::function<void (std::shared_ptr<task_t<detail::backend::raw_request_completion_t>>)> observe =
    {})
{
    if (!port) {
        (void) operations->fail (operation, foundation::operation_terminal_t::route_unavailable);
        co_return false;
    }
    // Infrastructure progress releases the initiating handler's turn.
    detail::serial_turn_scope_t turn_scope (nullptr);
    detail::ambient_context_scope_t infrastructure_scope (nullptr, nullptr, {}, false, true);
    auto running = std::make_shared<task_t<void>> (run_request_admission (
      std::move (port), std::move (observe_peer), operations, operation, correlation,
      std::move (target_routing_id), std::move (request_parts), std::move (decode_reply), deadline,
      std::move (terminal), std::move (admitted), std::move (observe)));
    detail::observe_task_terminal (*running, [operations, operation,
                                              running] (const result_t<void> &settled) {
        if (!settled)
            (void) operations->fail (operation, foundation::operation_terminal_t::transport_failed);
    });
    co_return true;
}

task_t<bool> await_request_admission (task_t<void> admitted)
try {
    auto convert = [] (const result_t<void> &result) {
        if (!result)
            return detail::result_access_t::failure<bool> (result.exception ());
        return result_t<bool>::success (true);
    };
    if (admitted.await_ready ())
        return task_t<bool> (convert (admitted.result ()));
    return detail::project_task_terminal<bool> (admitted, std::move (convert));
}
catch (...) {
    return task_t<bool> (detail::current_exception_result<bool> ());
}

} // namespace

bool raw_mesh_byte_vector_less_t::operator() (const std::vector<std::uint8_t> &left,
                                              const std::vector<std::uint8_t> &right) const noexcept
{
    return std::lexicographical_compare (left.begin (), left.end (), right.begin (), right.end ());
}

raw_mesh_node_owner_t::raw_mesh_node_owner_t (raw_mesh_node_options_t options,
                                              std::shared_ptr<zlink::context_t> context) :
    _options (std::move (options)),
    _context (context ? std::move (context) : std::make_shared<zlink::context_t> ()),
    _liveness (_lane),
    _topology (_lane,
               _options.descriptor,
               _options.metric_channel_names,
               &_liveness,
               _options.metric_provider),
    _mailbox (),
    _request_metrics (std::make_shared<mesh_request_metrics_t> (_options.descriptor.mesh_name,
                                                                _options.metric_provider)),
    _operations (foundation::operation_registry_t::create_scheduled (_lane))
{
    if (!_options.runtime_failures)
        throw std::invalid_argument ("A host runtime failure collector is required");
    auto meter = _options.metric_provider->GetMeter ("zlink.framework");
    if (!meter)
        return;
    constexpr std::array names{"zlink.mesh_node.peers.configured",
                               "zlink.mesh_node.peers.connected", "zlink.mesh_node.peers.ready",
                               "zlink.mesh_node.channels.ready_members"};
    for (std::size_t index = 0; index < names.size (); ++index) {
        auto &registration = _peer_metrics[index];
        registration.owner = this;
        registration.index = index;
        registration.instrument =
          meter->CreateDoubleObservableGauge (names[index], "", index == 3 ? "{member}" : "{peer}");
        if (registration.instrument)
            registration.instrument->AddCallback (&publish_peer_metrics, &registration);
    }
}

raw_mesh_node_owner_t::~raw_mesh_node_owner_t () noexcept
{
    for (auto &registration : _peer_metrics)
        if (registration.instrument)
            registration.instrument->RemoveCallback (&publish_peer_metrics, &registration);
    if (!_options.runtime_failures->capture ([&] { close (); })) {
        _options.runtime_failures->retain (
          [context = std::move (_context), socket_mutex = std::move (_socket_mutex),
           router = std::move (_router), poller = std::move (_ingress_poller),
           port = std::move (_port)] () mutable {
              (void) context;
              (void) socket_mutex;
              runtime_failure_collector_t::close_resources (port);
              runtime_failure_collector_t::close_resources (poller, router);
          });
    }
}

std::exception_ptr raw_mesh_node_owner_t::deliver_topology_notifications (
  std::vector<std::function<void ()>> &notifications, std::exception_ptr failure) noexcept
{
    // Consume before callbacks can re-enter: every committed publication is delivered once.
    for (auto &changed : std::exchange (notifications, {})) {
        try {
            changed ();
        }
        catch (...) {
            if (failure)
                _options.runtime_failures->report (std::current_exception ());
            else
                failure = std::current_exception ();
        }
    }
    return failure;
}

void raw_mesh_node_owner_t::start ()
{
    std::vector<std::function<void ()>> notifications;
    try {
        _lane
          .run ([this, &notifications] {
              auto descriptor = _topology.local_descriptor ();
              {
                  std::lock_guard lifecycle_lock (_lifecycle_mutex);
                  if (_port) {
                      return;
                  }
                  if (_closed) {
                      throw std::logic_error ("raw mesh node owner cannot restart after close");
                  }
                  auto router = std::make_unique<zlink::router_socket_t> (*_context);
                  router->options ().handover (true);
                  router->options ().mandatory (true);
                  router->options ().linger (std::chrono::milliseconds (0));
                  if (_options.receive_timeout)
                      router->options ().recv_timeout (*_options.receive_timeout);
                  router->set_routing_id (
                    zlink::routing_id_t::from (_options.descriptor.node_routing_id));
                  application_job_queue_t::receive_flow_registration_t receive_flow_registration;
                  if (_options.application_jobs) {
                      receive_flow_registration =
                        _options.application_jobs->register_receive_flow_socket (
                          [socket = router.get ()] (application_job_queue_pressure_state_t state) {
                              return apply_application_job_receive_flow_state (*socket, state);
                          });
                  }
                  router->bind (_options.descriptor.advertised_endpoint);

                  if (descriptor.descriptor_revision
                      == std::numeric_limits<std::uint64_t>::max ()) {
                      throw std::overflow_error ("service descriptor revision is exhausted");
                  }
                  descriptor.advertised_endpoint = transport::advertised_tcp_endpoint (
                    router->options ().last_endpoint (), _options.advertise_host, "MeshNode");
                  ++descriptor.descriptor_revision;

                  auto ingress_poller = std::make_unique<zlink::poller_t> ();
                  // The ingress poller is this ROUTER's one route observer: POLLROUTE
                  // wakes the host and pump_one reads the selected-route snapshot.
                  _port = std::make_shared<detail::backend::raw_route_port_t> (
                    *router, _socket_mutex,
                    zlink::poll_event_flag_t::pollin | zlink::poll_event_flag_t::pollroute
                      | zlink::poll_event_flag_t::pollconnect,
                    ingress_poller.get (), 1);
                  _port->set_request_receive_source ([this] (const auto &target) {
                      const auto peer = _topology.peer (target);
                      return peer ? peer->liveness : service_liveness_registry_t::connection_t{};
                  });
                  _ingress_poller = std::move (ingress_poller);
                  _router = std::move (router);
                  _receive_flow_registration = std::move (receive_flow_registration);
                  // Keep the descriptor in preparing until the receive port, completion
                  // control, and route observation can accept the first admitted message.
                  descriptor.state = service_node_state_t::serving;
              }
              auto changed = _topology.publish_local_on_lane (std::move (descriptor));
              if (changed)
                  notifications.push_back (std::move (changed));
          })
          .get ();
    }
    catch (...) {
        std::rethrow_exception (
          deliver_topology_notifications (notifications, std::current_exception ()));
    }
    if (auto failure = deliver_topology_notifications (notifications))
        std::rethrow_exception (failure);
}

// Every read of the local descriptor that is submitted to a route, and every
// revision change, runs in an owner turn together with its submission, so a
// route never receives a lower revision after a higher one (wire-protocol §4
// DescriptorRevision ordering).
task_t<void> raw_mesh_node_owner_t::publish_draining ()
{
    return runtime::run_blocking_step<void> ([this] () -> task_t<void> {
        std::vector<std::function<void ()>> notifications;
        try {
            _lane
              .run ([this, &notifications] {
                  auto publication = _topology.publish_draining_on_lane ();
                  if (std::get<2> (publication))
                      notifications.push_back (std::move (std::get<2> (publication)));
                  send_descriptor_update_on_lane (std::get<0> (publication),
                                                  std::get<1> (publication));
              })
              .get ();
        }
        catch (...) {
            std::rethrow_exception (
              deliver_topology_notifications (notifications, std::current_exception ()));
        }
        if (auto failure = deliver_topology_notifications (notifications))
            std::rethrow_exception (failure);
        co_return;
    });
}

service_node_descriptor_t raw_mesh_node_owner_t::publish_descriptor_update (
  const std::function<void (service_node_descriptor_t &)> &change)
{
    std::vector<std::function<void ()>> notifications;
    try {
        auto descriptor = _lane
                            .run ([this, &change, &notifications] {
                                auto descriptor = _topology.local_descriptor ();
                                change (descriptor);
                                auto changed = _topology.publish_local_on_lane (descriptor);
                                if (changed)
                                    notifications.push_back (std::move (changed));
                                const auto peers = _topology.peers_on_lane ();
                                send_descriptor_update_on_lane (descriptor, peers);
                                return descriptor;
                            })
                            .get ();
        if (auto failure = deliver_topology_notifications (notifications))
            std::rethrow_exception (failure);
        return descriptor;
    }
    catch (...) {
        std::rethrow_exception (
          deliver_topology_notifications (notifications, std::current_exception ()));
    }
}

void raw_mesh_node_owner_t::send_descriptor_update_on_lane (
  const service_node_descriptor_t &descriptor, const std::vector<admitted_peer_t> &peers)
{
    for (const auto &peer : peers) {
        if (!peer.ready)
            continue;
        (void) submit_header_only_on_lane (
          peer.descriptor.node_routing_id,
          protocol::encode_route_mesh_admission (protocol::command::update, descriptor));
    }
}

void raw_mesh_node_owner_t::close ()
{
    auto [resources, operations] =
      _lane
        .run ([this] {
            std::shared_ptr<foundation::operation_registry_t> operations;
            if (!_closed) {
                _receive_flow_registration.close ();
                _mailbox.close ();
                _routes.clear ();
                operations = _operations;
            }
            std::lock_guard lifecycle_lock (_lifecycle_mutex);
            return std::pair{
              std::tuple{std::move (_port), std::move (_ingress_poller), std::move (_router)},
              std::move (operations)};
        })
        .get ();
    // Only an open owner's close borrows the Registry for draining outside
    // the lane. The existing owner state decides whether shutdown is needed.
    if (operations)
        operations->shutdown ();
    std::exception_ptr failure;
    try {
        auto &[port, poller, router] = resources;
        runtime_failure_collector_t::close_resources (port);
        runtime_failure_collector_t::close_resources (poller, router);
    }
    catch (const std::exception &) {
        failure = std::current_exception ();
    }
    _lane
      .run ([this, &resources, &failure] {
          std::lock_guard lifecycle_lock (_lifecycle_mutex);
          std::tie (_port, _ingress_poller, _router) = std::move (resources);
          _closed = !failure;
      })
      .get ();
    if (failure)
        std::rethrow_exception (failure);
}

bool raw_mesh_node_owner_t::started () const noexcept
{
    try {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        return static_cast<bool> (_port);
    }
    catch (...) {
        return false;
    }
}

std::string raw_mesh_node_owner_t::endpoint () const
{
    return _topology.local_descriptor ().advertised_endpoint;
}

zlink::context_t &raw_mesh_node_owner_t::context ()
{
    return _lane
      .run ([this] () -> zlink::context_t & {
          std::lock_guard lifecycle_lock (_lifecycle_mutex);
          if (!_router || !_context) {
              throw std::logic_error ("raw mesh node owner is not started");
          }
          return *_context;
      })
      .get ();
}

service_topology_registry_t &raw_mesh_node_owner_t::topology () noexcept
{
    return _topology;
}

service_liveness_registry_t &raw_mesh_node_owner_t::liveness () noexcept
{
    return _liveness;
}

service_mailbox_t &raw_mesh_node_owner_t::mailbox () noexcept
{
    return _mailbox;
}

bool raw_mesh_node_owner_t::connect_peer (
  const std::string &endpoint, std::optional<service_node_descriptor_t> expected_descriptor)
{
    return _lane
      .run ([this, &endpoint, expected_descriptor = std::move (expected_descriptor)] () mutable {
          return connect_peer_owned (endpoint, std::move (expected_descriptor));
      })
      .get ();
}
bool raw_mesh_node_owner_t::connect_peer_owned (
  const std::string &endpoint, std::optional<service_node_descriptor_t> expected_descriptor)
{
    std::lock_guard lifecycle_lock (_lifecycle_mutex);
    if (!_router || endpoint.empty ())
        return false;
    const auto expectation =
      expected_descriptor
        ? std::make_optional (std::make_tuple (expected_descriptor->node_routing_id,
                                               expected_descriptor->security_identity,
                                               expected_descriptor->lifecycle_generation))
        : std::nullopt;
    const auto existing = _outbound_endpoints.find (endpoint);
    if (existing != _outbound_endpoints.end ()) {
        if (existing->second == expectation) {
            if (expected_descriptor)
                _expected_peers.insert_or_assign (expected_descriptor->node_routing_id,
                                                  *expected_descriptor);
            return true;
        }
    }
    try {
        std::lock_guard socket_lock (*_socket_mutex);
        if (existing != _outbound_endpoints.end ()) {
            _router->disconnect (endpoint);
            _outbound_endpoints.erase (existing);
            for (auto peer = _expected_peers.begin (); peer != _expected_peers.end ();) {
                if (peer->second.advertised_endpoint != endpoint) {
                    ++peer;
                    continue;
                }
                const auto retired_rid = peer->first;
                peer = _expected_peers.erase (peer);
                end_peer_operations_if_disconnected_locked (retired_rid);
            }
        }
        if (expected_descriptor)
            _router->options ().connect_routing_id (
              zlink::routing_id_t::from (expected_descriptor->node_routing_id));
        _router->connect (endpoint);
        _outbound_endpoints.emplace (endpoint, expectation);
        if (expected_descriptor)
            _expected_peers.insert_or_assign (expected_descriptor->node_routing_id,
                                              *expected_descriptor);
        return true;
    }
    catch (const zlink::binding_error_t &rejection) {
        // A rejected submission has no transport registration to reuse.
        trace_mesh ("connect rejected endpoint=" + endpoint + " error=" + rejection.what ());
        return false;
    }
}

void raw_mesh_node_owner_t::disconnect_peer (const std::string &endpoint) noexcept
{
    (void) disconnect_peer ({}, endpoint);
}

bool raw_mesh_node_owner_t::disconnect_peer (const std::vector<std::uint8_t> &expected_routing_id,
                                             const std::string &endpoint) noexcept
{
    if (endpoint.empty ())
        return false;
    std::vector<std::function<void ()>> notifications;
    try {
        const auto endpoint_retained =
          _lane
            .run ([this, &expected_routing_id, &endpoint, &notifications] {
                return disconnect_peer_owned (expected_routing_id, endpoint, notifications);
            })
            .get ();
        if (deliver_topology_notifications (notifications))
            return false;
        return endpoint_retained;
    }
    catch (...) {
        (void) deliver_topology_notifications (notifications, std::current_exception ());
        return false;
    }
}

bool raw_mesh_node_owner_t::disconnect_peer_owned (
  const std::vector<std::uint8_t> &expected_routing_id,
  const std::string &endpoint,
  std::vector<std::function<void ()>> &notifications)
{
    if (endpoint.empty ())
        return false;
    if (!_router)
        return false;
    try {
        trace_mesh ("disconnect endpoint=" + endpoint
                    + " expected=" + owner_key (expected_routing_id));
        std::optional<admitted_peer_t> admitted;
        if (!expected_routing_id.empty ()) {
            if (const auto peer = _topology.peer (expected_routing_id))
                admitted = *peer;
        } else {
            for (const auto &candidate : _topology.peers_on_lane ()) {
                if (candidate.descriptor.advertised_endpoint == endpoint) {
                    admitted = candidate;
                    break;
                }
            }
        }
        if (admitted) {
            trace_mesh ("disconnect admitted=" + owner_key (admitted->descriptor.node_routing_id)
                        + " connection=" + owner_key (admitted->connection_id));
            auto publication = _topology.disconnect_on_lane (admitted->descriptor.node_routing_id,
                                                             admitted->connection_id);
            if (publication.second)
                notifications.push_back (std::move (publication.second));
            (void) _liveness.disconnect_owned (admitted->descriptor.node_routing_id,
                                               admitted->connection_id);
            refresh_admission_due_on_lane ();
        }
        const auto &ended_routing_id = !expected_routing_id.empty () || !admitted
                                         ? expected_routing_id
                                         : admitted->descriptor.node_routing_id;
        if (!ended_routing_id.empty ())
            end_peer_operations_if_disconnected_locked (ended_routing_id);
        // Endpoint ownership is configuration and descriptor intent; Core
        // owns which physical pipe carries the RID (Core ROUTER §10.1).
        bool endpoint_in_use_by_other = false;
        if (!expected_routing_id.empty ()) {
            for (const auto &candidate : _topology.peers_on_lane ()) {
                if (candidate.descriptor.node_routing_id != expected_routing_id
                    && candidate.descriptor.advertised_endpoint == endpoint) {
                    endpoint_in_use_by_other = true;
                    break;
                }
            }
        }
        if (!expected_routing_id.empty ()) {
            std::lock_guard socket_lock (*_socket_mutex);
            try {
                _router->disconnect_rid (zlink::routing_id_t::from (expected_routing_id));
            }
            catch (...) {
                /* The RID can already have no selected route. Endpoint
                           * teardown remains required to disable reconnect for every
                           * pipe configured by this owner. */
            }
        }
        if (!endpoint_in_use_by_other) {
            try {
                std::lock_guard socket_lock (*_socket_mutex);
                // Remove the configured endpoint after terminating the current
                // RID. Otherwise the binding may reconnect the same stale
                // endpoint. This step must still run when the RID index is stale.
                _router->disconnect (endpoint);
            }
            catch (...) {
            }
            _outbound_endpoints.erase (endpoint);
        }
        trace_mesh ("disconnect endpoint-complete=" + endpoint);
        return endpoint_in_use_by_other;
    }
    catch (...) {
        return false;
    }
}

bool raw_mesh_node_owner_t::connect_peer_endpoint (const std::string &endpoint,
                                                   std::optional<zlink::routing_id_t> expected,
                                                   std::uint64_t expected_lifecycle_generation,
                                                   std::string security_identity,
                                                   bool automatic)
{
    return _lane
      .run ([this, endpoint, expected = std::move (expected), expected_lifecycle_generation,
             security_identity = std::move (security_identity), automatic] () mutable {
          return connect_peer_endpoint_owned (endpoint, std::move (expected),
                                              expected_lifecycle_generation,
                                              std::move (security_identity), automatic);
      })
      .get ();
}

bool raw_mesh_node_owner_t::connect_peer_endpoint_owned (
  const std::string &endpoint,
  std::optional<zlink::routing_id_t> expected,
  std::uint64_t expected_lifecycle_generation,
  std::string security_identity,
  bool automatic)
{
    auto expected_key = expected ? expected->to_string () : std::string{};
    if (automatic && expected
        && (peer_route_on_lane (expected->to_bytes (), std::nullopt).first != 0
            || std::any_of (_peer_endpoints.begin (), _peer_endpoints.end (),
                            [&] (const auto &intent) {
                                return intent.first == endpoint || intent.second == expected_key;
                            })))
        return false;
    bool connected = false;
    if (expected) {
        auto descriptor = _options.descriptor;
        descriptor.node_routing_id = expected->to_bytes ();
        descriptor.advertised_endpoint = endpoint;
        descriptor.lifecycle_generation = expected_lifecycle_generation;
        descriptor.security_identity = std::move (security_identity);
        connected = connect_peer_owned (endpoint, std::move (descriptor));
    } else {
        connected = connect_peer_owned (endpoint, std::nullopt);
    }
    if (connected)
        _peer_endpoints.insert_or_assign (endpoint, std::move (expected_key));
    return connected;
}

void raw_mesh_node_owner_t::disconnect_peer_endpoint (const std::string &endpoint) noexcept
{
    std::vector<std::function<void ()>> notifications;
    try {
        _lane
          .run ([this, endpoint, &notifications] {
              disconnect_peer_endpoint_owned (endpoint, notifications);
          })
          .get ();
    }
    catch (...) {
        std::rethrow_exception (
          deliver_topology_notifications (notifications, std::current_exception ()));
    }
    // The old private disconnect contained callback failure even for the void surface.
    (void) deliver_topology_notifications (notifications);
}

bool raw_mesh_node_owner_t::disconnect_peer_endpoint (
  const std::vector<std::uint8_t> &expected_routing_id, const std::string &endpoint) noexcept
{
    std::vector<std::function<void ()>> notifications;
    bool endpoint_retained;
    try {
        endpoint_retained = _lane
                              .run ([this, expected_routing_id, endpoint, &notifications] {
                                  return disconnect_peer_endpoint_owned (expected_routing_id,
                                                                         endpoint, notifications);
                              })
                              .get ();
    }
    catch (...) {
        std::rethrow_exception (
          deliver_topology_notifications (notifications, std::current_exception ()));
    }
    if (deliver_topology_notifications (notifications))
        return false;
    return endpoint_retained;
}

void raw_mesh_node_owner_t::disconnect_peer_endpoint_owned (
  const std::string &endpoint, std::vector<std::function<void ()>> &notifications)
{
    _peer_endpoints.erase (endpoint);
    (void) disconnect_peer_owned ({}, endpoint, notifications);
}

bool raw_mesh_node_owner_t::disconnect_peer_endpoint_owned (
  const std::vector<std::uint8_t> &expected_routing_id,
  const std::string &endpoint,
  std::vector<std::function<void ()>> &notifications)
{
    const auto endpoint_retained =
      disconnect_peer_owned (expected_routing_id, endpoint, notifications);
    if (!endpoint_retained)
        _peer_endpoints.erase (endpoint);
    return endpoint_retained;
}

void raw_mesh_node_owner_t::connect_peer_intent (const std::string &endpoint,
                                                 std::optional<zlink::routing_id_t> expected,
                                                 std::uint64_t expected_lifecycle_generation,
                                                 std::string security_identity,
                                                 bool automatic)
{
    _lane
      .run ([this, endpoint, expected = std::move (expected), expected_lifecycle_generation,
             security_identity = std::move (security_identity), automatic] () mutable {
          if (connect_peer_endpoint_owned (endpoint, std::move (expected),
                                           expected_lifecycle_generation,
                                           std::move (security_identity), automatic)) {
              const auto [intent, inserted] = _peer_connection_intents.try_emplace (endpoint);
              if (inserted)
                  intent->second = zlink::framework::detail::next_connection_intent_id ();
          }
      })
      .get ();
}

void raw_mesh_node_owner_t::connect_peer_intent (const mesh_peer_connection_t &configured)
{
    _lane
      .run ([this, configured] {
          if (connect_peer_endpoint_owned (configured.endpoint, configured.expected_routing_id, 0,
                                           "default", false))
              _peer_connection_intents.emplace (
                configured.endpoint, zlink::framework::detail::next_connection_intent_id ());
      })
      .get ();
}

void raw_mesh_node_owner_t::disconnect_peer_intent (const std::string &endpoint)
{
    std::vector<std::function<void ()>> notifications;
    try {
        _lane
          .run ([this, endpoint, &notifications] {
              const auto found = _peer_connection_intents.find (endpoint);
              disconnect_peer_endpoint_owned (endpoint, notifications);
              if (found != _peer_connection_intents.end ())
                  _peer_connection_intents.erase (found);
          })
          .get ();
    }
    catch (...) {
        std::rethrow_exception (
          deliver_topology_notifications (notifications, std::current_exception ()));
    }
    (void) deliver_topology_notifications (notifications);
}

void raw_mesh_node_owner_t::disconnect_peer_intent (const zlink::routing_id_t &expected_routing_id,
                                                    const std::string &endpoint,
                                                    bool automatic)
{
    std::vector<std::function<void ()>> notifications;
    try {
        _lane
          .run ([this, expected_routing_id, endpoint, automatic, &notifications] {
              if (automatic && !_peer_connection_intents.contains (endpoint))
                  return;
              const auto endpoint_retained = disconnect_peer_endpoint_owned (
                expected_routing_id.to_bytes (), endpoint, notifications);
              // Core may retain the endpoint for a replacement admitted RID.
              if (!endpoint_retained)
                  _peer_connection_intents.erase (endpoint);
          })
          .get ();
    }
    catch (...) {
        std::rethrow_exception (
          deliver_topology_notifications (notifications, std::current_exception ()));
    }
    (void) deliver_topology_notifications (notifications);
}

void raw_mesh_node_owner_t::clear_peer_connection_intents ()
{
    _lane.run ([this] { _peer_connection_intents.clear (); }).get ();
}

void raw_mesh_node_owner_t::expect_peer (service_node_descriptor_t expected_descriptor)
{
    if (expected_descriptor.node_routing_id.empty ()
        || expected_descriptor.advertised_endpoint.empty ())
        return;
    _lane
      .run ([this, &expected_descriptor] {
          std::lock_guard lifecycle_lock (_lifecycle_mutex);
          _expected_peers.insert_or_assign (expected_descriptor.node_routing_id,
                                            expected_descriptor);
      })
      .get ();
}

void raw_mesh_node_owner_t::configure_peer_reconcile (peer_reconcile_t request)
{
    std::lock_guard lifecycle_lock (_lifecycle_mutex);
    _peer_reconcile = std::move (request);
}

raw_mesh_node_owner_t::peer_reconcile_t raw_mesh_node_owner_t::peer_reconcile () const
{
    std::lock_guard lifecycle_lock (_lifecycle_mutex);
    return _peer_reconcile;
}

void raw_mesh_node_owner_t::configure_request_sources (
  live_location_reader_t *sources,
  std::function<std::shared_ptr<const location_owner_token_t> ()> local_lease)
{
    std::lock_guard lifecycle_lock (_lifecycle_mutex);
    if (_router || _request_sources || _local_request_lease)
        throw std::logic_error ("request source reader must be configured once before start");
    _request_sources = sources;
    _local_request_lease = std::move (local_lease);
}

std::optional<request_source_t>
raw_mesh_node_owner_t::local_accepted_source (const std::vector<std::uint8_t> &rid,
                                              std::uint64_t generation,
                                              const local_request_source_t &local)
{
    if (local.node && rid == local.node->node_routing_id) {
        if (generation != local.node->lifecycle_generation || !local.lease
            || local.lease->owner_id.empty () || local.lease->lease_generation <= 0)
            return std::nullopt;
        return request_source_t{local};
    }
    return std::nullopt;
}

void raw_mesh_node_owner_t::forget_peer (const std::vector<std::uint8_t> &node_routing_id,
                                         const std::string &endpoint)
{
    return _lane
      .run ([this, &node_routing_id, &endpoint] {
          std::lock_guard lifecycle_lock (_lifecycle_mutex);
          const auto found = _expected_peers.find (node_routing_id);
          if (found != _expected_peers.end () && found->second.advertised_endpoint != endpoint)
              return;
          _expected_peers.erase (node_routing_id);
          end_peer_operations_if_disconnected_locked (node_routing_id);
      })
      .get ();
}

void raw_mesh_node_owner_t::end_peer_operations_if_disconnected_locked (
  const std::vector<std::uint8_t> &node_routing_id)
{
    // Re-evaluate after either input changes. Transport loss alone retains
    // the expectation and therefore does not end durable operations.
    if (!_expected_peers.contains (node_routing_id) && !_topology.peer (node_routing_id))
        (void) _operations->fail_target (node_routing_id,
                                         foundation::operation_terminal_t::route_unavailable);
}

peer_admission_result_t
raw_mesh_node_owner_t::admit_peer (service_node_descriptor_t descriptor,
                                   std::vector<std::uint8_t> connection_id,
                                   service_liveness_registry_t::clock_t::time_point now)
{
    std::vector<std::function<void ()>> notifications;
    try {
        const auto result =
          _lane
            .run ([this, descriptor = std::move (descriptor),
                   connection_id = std::move (connection_id), now, &notifications] () mutable {
                if (!_router)
                    return peer_admission_result_t::invalid_descriptor;
                auto admission =
                  _topology.admit_on_lane (std::move (descriptor), std::move (connection_id),
                                           nullptr, now, protocol::command::admit);
                if (admission.second)
                    notifications.push_back (std::move (admission.second));
                refresh_admission_due_on_lane ();
                return admission.first;
            })
            .get ();
        if (auto failure = deliver_topology_notifications (notifications))
            std::rethrow_exception (failure);
        return result;
    }
    catch (...) {
        std::rethrow_exception (
          deliver_topology_notifications (notifications, std::current_exception ()));
    }
}

task_t<bool>
raw_mesh_node_owner_t::send_to_node (const std::vector<std::uint8_t> &target_routing_id,
                                     protocol::application_payload_t application_payload)
{
    co_return co_await send_to_node_result (target_routing_id, std::move (application_payload))
      == zlink::submit_result_t::ok;
}

task_t<zlink::submit_result_t>
raw_mesh_node_owner_t::send_to_node_result (const std::vector<std::uint8_t> &target_routing_id,
                                            protocol::application_payload_t application_payload)
{
    try {
        return send_with_header_result (target_routing_id, protocol::encode_node_send_header (),
                                        std::move (application_payload));
    }
    catch (...) {
        return task_t<zlink::submit_result_t> (
          detail::current_exception_result<zlink::submit_result_t> ());
    }
}

task_t<zlink::submit_result_t> raw_mesh_node_owner_t::send_logical_multicast (
  const std::vector<std::uint8_t> &target_routing_id,
  const std::string &channel_name,
  const std::string &topic,
  const std::string &source_spot_id,
  const protocol::application_payload_t &application_payload,
  const std::vector<std::uint8_t> &metadata)
{
    try {
        namespace wire = protocol::generated::detail;
        wire::logicalMulticast_23_t command;
        command.flags =
          metadata.empty () ? 0 : static_cast<std::uint8_t> (protocol::flag::metadata);
        command.channelName.value = channel_name;
        command.topic.value = topic;
        command.sourceSpotId.value = source_spot_id;
        wire::writer_t header;
        if (wire::encode_head_logicalMulticast_23 (header, command) != wire::error_code::ok)
            throw protocol::service_wire_error_t ("invalid logical multicast header");
        detail::backend::raw_message_t parts;
        parts.reserve (metadata.empty () ? 2 : 3);
        parts.push_back (std::move (header.data));
        if (!metadata.empty ())
            parts.push_back (metadata);
        parts.push_back (protocol::encode_application_payload (application_payload));
        return submit_owned_send_result (target_routing_id, std::move (parts), {});
    }
    catch (...) {
        return task_t<zlink::submit_result_t> (
          detail::current_exception_result<zlink::submit_result_t> ());
    }
}

task_t<bool>
raw_mesh_node_owner_t::request_to_node (const std::vector<std::uint8_t> &target_routing_id,
                                        const protocol::application_payload_t &application_payload,
                                        std::chrono::milliseconds timeout,
                                        foundation::operation_registry_t::callback_t callback,
                                        std::optional<std::uint64_t> correlation)
try {
    const auto deadline = capture_request_deadline (timeout);
    return request_with_header (
      target_routing_id,
      [] (std::uint64_t request_correlation) {
          return protocol::encode_node_request_header (request_correlation);
      },
      application_payload, deadline, std::move (callback), correlation, false,
      mesh_request_metric_t (_request_metrics, mesh_request_surface_t::node));
}
catch (...) {
    return task_t<bool> (detail::current_exception_result<bool> ());
}

task_t<bool> raw_mesh_node_owner_t::request_to_channel (
  const std::string &channel_name,
  const protocol::application_payload_t &application_payload,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback,
  std::optional<std::uint64_t> correlation)
{
    const auto deadline = capture_request_deadline (timeout);
    mesh_request_metric_t request_metric (_request_metrics, mesh_request_surface_t::channel);
    auto selected = _topology.select (channel_name);
    if (!selected) {
        throw *selected.error ();
    }
    co_return co_await request_with_header (
      std::move (selected.value ()),
      [&channel_name] (std::uint64_t request_correlation) {
          return protocol::encode_channel_request_header (request_correlation, channel_name);
      },
      application_payload, deadline, std::move (callback), correlation, true,
      std::move (request_metric));
}

bool raw_mesh_node_owner_t::observe_request (
  std::shared_ptr<foundation::operation_registry_t> operations,
  foundation::call_id_t operation,
  std::uint64_t correlation,
  std::shared_ptr<task_t<detail::backend::raw_request_completion_t>> running)
{
    if (!running) {
        (void) operations->fail (operation, foundation::operation_terminal_t::transport_failed);
        return false;
    }
    detail::observe_task_terminal (
      *running, [operations, operation, correlation,
                 running] (const result_t<detail::backend::raw_request_completion_t> &settled) {
          if (!settled) {
              (void) operations->fail (operation,
                                       foundation::operation_terminal_t::transport_failed);
              return;
          }
          const auto &completion = settled.value ();
          trace_mesh ("request-completion correlation=" + std::to_string (correlation)
                      + " result=" + std::to_string (static_cast<int> (completion.terminal))
                      + " parts=" + std::to_string (completion.parts.size ()));
          if (completion.terminal != zlink::request_result_t::ok) {
              const auto terminal =
                messaging::request_failure_mapper_t{}
                  .transport_terminal (completion.terminal)
                  .value_or (foundation::operation_terminal_t::transport_failed);
              auto failure_reply = request_failure_reply_parts (correlation, completion.terminal);
              (void) operations->fail (operation, terminal,
                                       zlink::message_t::from (failure_reply.front ()));
              return;
          }
          try {
              auto &parts = completion.parts;
              if (parts.empty () || parts.size () > 2) {
                  throw protocol::service_wire_error_t ("request reply has an invalid part count");
              }
              const auto reply =
                protocol::decode_reply_header (protocol::message_bytes (parts.front ()));
              //  This is a generic application request reply (requestToNode
              //  / spotRequest): its originalOperationKind is not one of
              //  request-specific-tail's tail-bearing cases
              //  (service-wire-v1.schema.json), so the schema's "otherwise"
              //  branch applies and the tail MUST be empty. decode_reply_header
              //  itself now permissively accepts tail-bearing frames (fix for
              //  the residual-convergence tail rejection bug), so this generic
              //  caller enforces the empty-tail contract explicitly, matching
              //  Node's raw-service-mesh-runtime.ts generic reply guard
              //  ("Generic node/channel reply carries an operation-specific
              //  tail.").
              if (parts.front ().size () != protocol::reply_header_t::encoded_size) {
                  throw protocol::service_wire_error_t (
                    "generic request reply carries an operation-specific "
                    "tail");
              }
              if (reply.correlation != correlation) {
                  throw protocol::service_wire_error_t ("request reply correlation does not match");
              }
              if (reply.terminal_result != 0) {
                  if (parts.size () != 1) {
                      throw protocol::service_wire_error_t (
                        "failed request reply cannot carry a payload");
                  }
                  (void) operations->fail (
                    operation, foundation::operation_terminal_t::transport_failed,
                    parts.front ().copy (), {}, request_metric_terminal (reply));
                  return;
              }
              if (parts.size () != 2) {
                  throw protocol::service_wire_error_t (
                    "successful request reply must carry a payload");
              }
              (void) protocol::application_payload_hwm_bytes (protocol::message_bytes (parts[1]));
              (void) operations->complete (operation, parts[1].copy ());
          }
          catch (const protocol::service_wire_error_t &) {
              //  Spec 32-framework-error-model:91-92 — a malformed reply is
              //  ProtocolError, not a transport failure. Carry a synthesized
              //  protocolError header; complete_operation decodes it into
              //  terminal 104 instead of collapsing to internal_error.
              (void) operations->fail (
                operation, foundation::operation_terminal_t::transport_failed,
                zlink::message_t::from (protocol::encode_reply_header (
                  correlation,
                  static_cast<std::uint32_t> (protocol::request_terminal_result::protocolError),
                  static_cast<std::uint32_t> (
                    protocol::framework_error_code::requestProtocolError))));
          }
      });
    return true;
}

void raw_mesh_node_owner_t::trace_admission_phase (const std::vector<std::uint8_t> &node_routing_id,
                                                   std::uint64_t lifecycle_generation,
                                                   protocol::command command,
                                                   peer_admission_result_t result)
{
    if (!mesh_trace_enabled ()
        || (result != peer_admission_result_t::admitted
            && result != peer_admission_result_t::duplicate_connection))
        return;
    const auto peer = _topology.peer (node_routing_id);
    const auto peer_generation = peer ? peer->descriptor.lifecycle_generation : 0;
    const auto admission_context = " owner=" + trace_owner_key (this)
                                   + " source=" + owner_key (node_routing_id)
                                   + " result=" + std::to_string (static_cast<int> (result))
                                   + " peer=" + (peer ? std::string ("present") : "absent")
                                   + " peerGeneration=" + std::to_string (peer_generation);
    if (command == protocol::command::hello) {
        trace_mesh ("handshake phase=local-admission-awaiting-remote-admit" + admission_context);
        return;
    }
    if (command != protocol::command::admit)
        return;
    if (lifecycle_generation != 0)
        trace_mesh ("handshake phase=bilateral-ready" + admission_context);
}

service_topology_registry_t::peer_observer_t raw_mesh_node_owner_t::observe_request_peer (
  const std::vector<std::uint8_t> &target_routing_id) const
{
    return _lane.run_sync ([&] { return observe_request_peer_on_lane (target_routing_id); });
}

service_topology_registry_t::peer_observer_t raw_mesh_node_owner_t::observe_request_peer_on_lane (
  const std::vector<std::uint8_t> &target_routing_id) const
{
    const auto expected = _expected_peers.find (target_routing_id);
    return _topology.observe_peer (target_routing_id, expected == _expected_peers.end ()
                                                        ? std::string{}
                                                        : expected->second.advertised_endpoint);
}

task_t<bool> raw_mesh_node_owner_t::request_with_header (
  const std::vector<std::uint8_t> &target_routing_id,
  const std::function<std::vector<std::uint8_t> (std::uint64_t)> &header,
  const protocol::application_payload_t &application_payload,
  foundation::operation_registry_t::clock_t::time_point deadline,
  foundation::operation_registry_t::callback_t callback,
  std::optional<std::uint64_t> requested_correlation,
  bool target_claimed,
  mesh_request_metric_t request_metric,
  std::optional<foundation::operation_registry_t::request_identity_t> request)
try {
    const auto wait_owner = detail::current_wait_owner ();
    struct request_start_t
    {
        foundation::call_id_t operation;
        std::uint64_t correlation = 0;
        std::shared_ptr<task_completion_source_t<void>> admitted =
          std::make_shared<task_completion_source_t<void>> ();
        std::shared_ptr<task_t<detail::backend::raw_request_completion_t>> running;
        bool registered = false;
    };
    auto start =
      _lane.run_sync ([this, &header, &request_metric, &target_routing_id, &application_payload,
                       request = std::move (request), requested_correlation, target_claimed,
                       deadline, wait_owner, callback = std::move (callback)] () mutable {
          const auto peer = _topology.peer (target_routing_id);
          if (!target_claimed && !peer && !_expected_peers.contains (target_routing_id))
              return std::optional<request_start_t>{};
          auto encoded_payload = protocol::encode_application_payload (application_payload);
          std::shared_ptr<detail::backend::raw_route_port_t> port;
          {
              std::lock_guard lifecycle_lock (_lifecycle_mutex);
              port = _port;
          }
          if (!port)
              return std::optional<request_start_t>{};
          request_start_t value;
          value.correlation = take_reply_route_id_locked (requested_correlation);
          value.operation =
            operation_id (_options.descriptor.lifecycle_generation, value.correlation);
          detail::backend::raw_message_t wire;
          wire.reserve (2);
          wire.push_back (header (value.correlation));
          wire.push_back (std::move (encoded_payload));
          if (request && request->operation == foundation::call_id_t{})
              request->operation = value.operation;
          value.registered = _operations->register_operation (
            value.operation, deadline,
            request_admission_completion (std::move (callback), value.admitted),
            peer && peer->ready ? std::vector<std::uint8_t>{} : target_routing_id,
            std::move (request_metric), std::move (request), wait_owner);
          if (value.registered) {
              try {
                  value.running = start_admitted_request (port, peer.get (), target_routing_id,
                                                          wire, deadline, value.admitted);
                  if (!value.running) {
                      (void) submit_registered_request (
                        port, observe_request_peer_on_lane (target_routing_id), _operations,
                        value.operation, value.correlation, target_routing_id, std::move (wire), {},
                        deadline, value.admitted->task (), value.admitted,
                        [operations = _operations, operation = value.operation,
                         correlation = value.correlation] (auto native) {
                            (void) observe_request (operations, operation, correlation,
                                                    std::move (native));
                        });
                  }
              }
              catch (...) {
                  (void) _operations->unregister (value.operation);
                  throw;
              }
          }
          return std::optional<request_start_t>{std::move (value)};
      });
    if (!start)
        return task_t<bool> (result_t<bool>::success (false));
    if (!start->registered) {
        throw framework_exception_t (framework_error_kind_t::shutting_down,
                                     "raw mesh request registry is closed");
    }
    auto admitted = start->admitted->task ();
    if (start->running)
        (void) observe_request (_operations, start->operation, start->correlation,
                                std::move (start->running));
    return await_request_admission (std::move (admitted));
}
catch (...) {
    return task_t<bool> (detail::current_exception_result<bool> ());
}

struct raw_mesh_node_owner_t::send_completion_state_t
{
    std::shared_ptr<task_completion_source_t<zlink::submit_result_t>> source;
    std::optional<zlink::submit_result_t> result;
};

raw_mesh_node_owner_t::send_start_result_t
raw_mesh_node_owner_t::start_send (const std::vector<std::uint8_t> &target_routing_id,
                                   detail::backend::raw_message_t parts,
                                   bool needs_public_completion,
                                   detail::backend::raw_send_stage_trace_t trace)
{
    return _lane.run_sync ([this, parts = std::move (parts), trace = std::move (trace),
                            needs_public_completion, &target_routing_id] () mutable {
        return start_send_on_lane (target_routing_id, std::move (parts), needs_public_completion,
                                   std::move (trace));
    });
}

raw_mesh_node_owner_t::send_start_result_t
raw_mesh_node_owner_t::start_send_on_lane (const std::vector<std::uint8_t> &target_routing_id,
                                           detail::backend::raw_message_t parts,
                                           bool needs_public_completion,
                                           detail::backend::raw_send_stage_trace_t trace)
{
    struct send_start_t
    {
        foundation::call_id_t operation;
        std::shared_ptr<task_t<zlink::submit_result_t>> public_completion;
        bool registered = false;
    };
    const auto &port = _port;
    if (!port) {
        if (trace)
            trace ("router_admission_submit", "terminated");
        return send_start_result_t{send_start_state_t::terminated, std::nullopt, {}};
    }
    // The owner turn keeps the port in place until submission returns.
    // raw_route_port_t serializes native submission versus close with its socket gate.
    auto submission = port->submit_send (target_routing_id, std::move (parts), std::move (trace));
    if (submission.state == detail::backend::raw_send_submission_state_t::immediate) {
        return send_start_result_t{
          send_start_state_t::started, std::move (submission.immediate_result), {}};
    }
    foundation::call_id_t operation;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        operation =
          operation_id (_options.descriptor.lifecycle_generation, take_reply_route_id_locked ());
    }
    send_start_t value{.operation = operation};
    auto completion = std::make_shared<send_completion_state_t> ();
    if (needs_public_completion) {
        completion->source = std::make_shared<task_completion_source_t<zlink::submit_result_t>> ();
        value.public_completion =
          std::make_shared<task_t<zlink::submit_result_t>> (completion->source->task ());
    }
    value.registered = _operations->register_operation (
      operation, foundation::operation_registry_t::clock_t::time_point::max (),
      [completion] (foundation::operation_terminal_t terminal, auto) {
          const auto result =
            completion->result.value_or (terminal == foundation::operation_terminal_t::shutdown
                                           ? zlink::submit_result_t::terminated
                                           : zlink::submit_result_t::internal_error);
          if (completion->source) {
              completion->source->complete (result_t<zlink::submit_result_t>::success (result));
          } else if (result != zlink::submit_result_t::ok && mesh_trace_enabled ()) {
              trace_mesh_enabled (
                std::string ("control-send terminal=")
                + (terminal == foundation::operation_terminal_t::shutdown ? "shutdown"
                   : terminal == foundation::operation_terminal_t::transport_failed
                     ? "transport-failed"
                     : "completed")
                + " result=" + std::to_string (static_cast<int> (result)));
          }
      });
    if (value.registered) {
        try {
            auto running = std::move (submission.pending_completion);
            if (!running) {
                (void) _operations->unregister (operation);
                return send_start_result_t{send_start_state_t::terminated, std::nullopt, {}};
            }
            const auto operations = _operations;
            detail::observe_task_terminal (
              *running, [operations, operation, completion,
                         running] (const result_t<zlink::submit_result_t> &settled) {
                  if (!settled) {
                      (void) operations->fail (operation,
                                               foundation::operation_terminal_t::transport_failed);
                      return;
                  }
                  const auto result = settled.value ();
                  (void) operations->complete (
                    operation, {}, [completion, result] { completion->result = result; });
              });
        }
        catch (...) {
            (void) _operations->unregister (operation);
            throw;
        }
    }
    if (!value.registered) {
        return send_start_result_t{send_start_state_t::terminated, std::nullopt, {}};
    }
    return send_start_result_t{send_start_state_t::started, std::nullopt,
                               std::move (value.public_completion)};
}

task_t<zlink::submit_result_t>
raw_mesh_node_owner_t::send_with_header_result (const std::vector<std::uint8_t> &target_routing_id,
                                                std::vector<std::uint8_t> header,
                                                protocol::application_payload_t application_payload,
                                                detail::backend::raw_send_stage_trace_t trace,
                                                bool target_claimed,
                                                std::optional<std::vector<std::uint8_t>> metadata)
{
    try {
        if (!target_claimed) {
            const auto peer = _topology.peer (target_routing_id);
            if (!peer || !peer->ready) {
                if (trace)
                    trace ("router_admission_submit", "not_connected");
                return task_t<zlink::submit_result_t> (result_t<zlink::submit_result_t>::success (
                  zlink::submit_result_t::not_connected));
            }
        }
        detail::backend::raw_message_t parts;
        parts.reserve (metadata ? 3 : 2);
        parts.push_back (std::move (header));
        if (metadata)
            parts.push_back (std::move (*metadata));
        parts.push_back (protocol::encode_application_payload (application_payload));
        return submit_owned_send_result (target_routing_id, std::move (parts), std::move (trace));
    }
    catch (...) {
        return task_t<zlink::submit_result_t> (
          detail::current_exception_result<zlink::submit_result_t> ());
    }
}

task_t<zlink::submit_result_t>
raw_mesh_node_owner_t::submit_owned_send_result (const std::vector<std::uint8_t> &target_routing_id,
                                                 detail::backend::raw_message_t parts,
                                                 detail::backend::raw_send_stage_trace_t trace)
{
    auto started = start_send (target_routing_id, std::move (parts), true, std::move (trace));
    if (started.state == send_start_state_t::terminated)
        return task_t<zlink::submit_result_t> (
          result_t<zlink::submit_result_t>::success (zlink::submit_result_t::terminated));
    if (started.immediate_result)
        return task_t<zlink::submit_result_t> (std::move (*started.immediate_result));
    return std::move (*started.pending_completion);
}

task_t<bool>
raw_mesh_node_owner_t::send_header_only (const std::vector<std::uint8_t> &target_routing_id,
                                         std::vector<std::uint8_t> header)
{
    auto started =
      start_send (target_routing_id, detail::backend::raw_message_t{std::move (header)}, true);
    if (started.state == send_start_state_t::terminated)
        co_return false;
    if (started.immediate_result)
        co_return started.immediate_result->value () == zlink::submit_result_t::ok;
    co_return co_await *started.pending_completion == zlink::submit_result_t::ok;
}

raw_mesh_node_owner_t::send_start_state_t
raw_mesh_node_owner_t::submit_header_only (const std::vector<std::uint8_t> &target_routing_id,
                                           std::vector<std::uint8_t> header)
{
    return _lane.run_sync ([this, &target_routing_id, &header] {
        return submit_header_only_on_lane (target_routing_id, std::move (header));
    });
}

raw_mesh_node_owner_t::send_start_state_t raw_mesh_node_owner_t::submit_header_only_on_lane (
  const std::vector<std::uint8_t> &target_routing_id, std::vector<std::uint8_t> header)
{
    auto started = start_send_on_lane (
      target_routing_id, detail::backend::raw_message_t{std::move (header)}, false, {});
    if (started.state != send_start_state_t::started && mesh_trace_enabled ())
        trace_mesh_enabled ("control-send start=terminated");
    return started.state;
}

task_t<bool> raw_mesh_node_owner_t::send_control (std::vector<std::uint8_t> target_routing_id,
                                                  detail::backend::raw_message_t parts)
{
    const auto local = _topology.local_descriptor ();
    if (target_routing_id == local.node_routing_id) {
        const auto accepted =
          admit_control (
            local, local,
            service_mailbox_record_t{
              owner_key (local.node_routing_id), service_mailbox_domain_t::infrastructure,
              detail::backend::materialize_binding_parts (parts), local.node_routing_id,
              std::nullopt, std::nullopt, local.lifecycle_generation})
          == raw_mesh_pump_result_t::infrastructure;
        if (accepted)
            signal_activity ();
        co_return accepted;
    }
    // The owner borrows the target id during its synchronous submission.
    co_return co_await submit_owned_send_result (target_routing_id, std::move (parts), {})
      == zlink::submit_result_t::ok;
}

task_t<bool> raw_mesh_node_owner_t::send_session_relocation_route (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::session_relocation_route_t &route)
{
    return send_control (target_routing_id, {protocol::encode_session_relocation_route (route)});
}

task_t<bool> raw_mesh_node_owner_t::send_session_relocation_seal (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::session_relocation_seal_t &seal)
{
    co_return co_await send_header_only (target_routing_id,
                                         protocol::encode_session_relocation_seal (seal));
}

task_t<bool> raw_mesh_node_owner_t::request_session_relocation_seal (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::session_relocation_seal_t &seal,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback)
{
    if (timeout <= std::chrono::milliseconds::zero ())
        throw std::invalid_argument ("Session relocation seal timeout must be positive");
    const auto operation = operation_id (seal.relocation.high, seal.relocation.low);
    if (!_operations->register_operation (
          operation, foundation::operation_registry_t::clock_t::now () + timeout,
          std::move (callback), {}, {}, {}, detail::current_wait_owner ()))
        co_return false;
    if (co_await send_session_relocation_seal (target_routing_id, seal))
        co_return true;
    (void) _operations->fail (operation, foundation::operation_terminal_t::transport_failed);
    co_return false;
}

task_t<bool> raw_mesh_node_owner_t::send_session_relocation_sealed (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::session_relocation_sealed_t &sealed)
{
    co_return co_await send_header_only (target_routing_id,
                                         protocol::encode_session_relocation_sealed (sealed));
}

task_t<bool> raw_mesh_node_owner_t::send_reply_relay (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::reply_relay_t &relay,
  std::optional<protocol::application_payload_t> application_reply)
{
    if (relay.terminal_result != 0 && application_reply) {
        throw std::invalid_argument ("failed reply relay cannot carry an application payload");
    }
    std::vector<std::vector<std::uint8_t>> parts;
    parts.emplace_back (protocol::encode_reply_relay (relay));
    if (application_reply) {
        parts.emplace_back (protocol::encode_application_payload (*application_reply));
    }
    co_return co_await send_control (target_routing_id, std::move (parts));
}

task_t<bool>
raw_mesh_node_owner_t::send_reply_relay_ack (const std::vector<std::uint8_t> &target_routing_id,
                                             const protocol::reply_relay_ack_t &ack)
{
    return send_control (target_routing_id, {protocol::encode_reply_relay_ack (ack)});
}

task_t<bool>
raw_mesh_node_owner_t::send_message_follow (const std::vector<std::uint8_t> &target_routing_id,
                                            const protocol::message_follow_notice_t &notice)
{
    co_return co_await send_header_only (target_routing_id,
                                         protocol::encode_message_follow (notice));
}

task_t<bool>
raw_mesh_node_owner_t::send_relocation_control (const std::vector<std::uint8_t> &target_routing_id,
                                                const protocol::relocation_control_t &control)
{
    co_return co_await send_header_only (target_routing_id,
                                         protocol::encode_relocation_control (control));
}

task_t<relocation_prepare_response_t> raw_mesh_node_owner_t::request_relocation_prepare (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::relocation_prepare_t &prepare,
  std::chrono::milliseconds timeout)
{
    const auto deadline = foundation::operation_registry_t::clock_t::now () + timeout;
    using completion_t = std::pair<foundation::operation_terminal_t, zlink::message_t>;
    auto completion = std::make_shared<task_completion_source_t<completion_t>> ();
    auto pending = completion->task ();
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    foundation::call_id_t id{};
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
        if (!port || timeout <= std::chrono::milliseconds::zero ())
            co_return relocation_prepare_response_t{};
        id = operation_id (_topology.local_descriptor ().lifecycle_generation,
                           take_reply_route_id_locked ());
    }
    if (!_operations->register_operation (
          id, deadline,
          [completion] (auto terminal, auto packed) {
              completion->complete (
                result_t<completion_t>::success ({terminal, std::move (packed)}));
          },
          target_routing_id, {}, {}, detail::current_wait_owner ()))
        co_return relocation_prepare_response_t{};
    detail::backend::raw_message_t parts{protocol::encode_relocation_control (prepare)};
    (void) co_await submit_registered_request (
      std::move (port), observe_request_peer (target_routing_id), _operations, id, std::nullopt,
      target_routing_id, std::move (parts), protocol::pack_infrastructure_reply, deadline,
      completion->task ());
    const auto [terminal, packed] = co_await pending;
    if (terminal != foundation::operation_terminal_t::completed)
        co_return relocation_prepare_response_t{};
    // Exact-identity fencing (spec 15 §4.2 / spec 28): a reply whose
    // identity fields do not match the prepare this call sent is a stale
    // or wrong-attempt reply and must not resolve this call either way —
    // neither as ready nor as an explicit failure.
    const auto identity_matches =
      [&prepare] (const auto &relocation, std::uint64_t target_attempt_generation,
                  const auto &coordinator, const auto &target, const auto &object) {
          return relocation == prepare.relocation
                 && target_attempt_generation == prepare.target_attempt_generation
                 && coordinator == prepare.coordinator && target == prepare.target
                 && object == prepare.object;
      };
    relocation_prepare_response_t response;
    try {
        const auto parts = protocol::unpack_infrastructure_reply (protocol::message_bytes (packed));
        if (parts.size () != 1)
            co_return response;
        const auto control = protocol::decode_relocation_control (parts.front ());
        if (const auto *ready = std::get_if<protocol::relocation_ready_t> (&control)) {
            if (identity_matches (ready->relocation, ready->target_attempt_generation,
                                  ready->coordinator, ready->target, ready->object))
                response.ready = *ready;
        } else if (const auto *failed = std::get_if<protocol::relocation_failed_t> (&control)) {
            // The bug this closes: previously only relocation_ready_t was
            // ever extracted here — an explicit, identity-matched
            // relocationFailed(53) reply fell through to the same "no
            // result" the caller also observes on a genuine timeout,
            // discarding both the fast, explicit rejection and its
            // failure_code.
            if (identity_matches (failed->relocation, failed->target_attempt_generation,
                                  failed->coordinator, failed->target, failed->object))
                response.failed = *failed;
        }
    }
    catch (const protocol::service_wire_error_t &) {
    }
    co_return response;
}

task_t<std::function<bool ()>>
raw_mesh_node_owner_t::prepare_relocation_ready_reply (const service_mailbox_record_t &request,
                                                       const protocol::relocation_ready_t &ready)
{
    if (request.source_routing_id.empty () || !request.reply_token)
        co_return std::function<bool ()>{};
    auto port = co_await _lane.run_task ([this] {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        return _port;
    });
    if (!port)
        co_return std::function<bool ()>{};
    co_return std::function<bool ()>{[port = std::move (port), source = request.source_routing_id,
                                      token = request.reply_token,
                                      parts = detail::backend::raw_message_t{
                                        protocol::encode_relocation_control (ready)}] () mutable {
        return port->reply (detail::backend::raw_received_t{source, token, {}}, std::move (parts));
    }};
}

bool raw_mesh_node_owner_t::reply_relocation_failed (const service_mailbox_record_t &request,
                                                     const protocol::relocation_failed_t &failure)
{
    if (request.source_routing_id.empty () || !request.reply_token)
        return false;
    const auto port = _lane
                        .run ([this] {
                            std::lock_guard lifecycle_lock (_lifecycle_mutex);
                            return _port;
                        })
                        .get ();
    if (!port)
        return false;
    detail::backend::raw_message_t parts{protocol::encode_relocation_control (failure)};
    return port->reply (
      detail::backend::raw_received_t{request.source_routing_id, request.reply_token, {}},
      std::move (parts));
}

bool raw_mesh_node_owner_t::reply (const service_mailbox_record_t &request,
                                   const protocol::application_payload_t &application_payload)
{
    if (request.source_routing_id.empty () || !request.reply_token)
        return false;
    if (!request.correlation) {
        throw std::invalid_argument ("raw mesh reply requires a request mailbox record");
    }
    const auto port = _lane.run_sync ([this] {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        return _port;
    });
    if (!port)
        return false;
    detail::backend::raw_message_t parts;
    parts.reserve (2);
    parts.push_back (protocol::encode_reply_header (*request.correlation, 0, 0));
    parts.push_back (protocol::encode_application_payload (application_payload));
    return port->reply (
      detail::backend::raw_received_t{request.source_routing_id, request.reply_token, {}},
      std::move (parts));
}

bool raw_mesh_node_owner_t::reply_failure (const service_mailbox_record_t &request,
                                           std::uint32_t terminal_result,
                                           std::uint32_t failure_code)
{
    if (request.source_routing_id.empty () || !request.correlation || terminal_result == 0) {
        throw std::invalid_argument (
          "raw mesh failed reply requires a request and terminal result");
    }
    const auto local = _topology.local_descriptor ();
    if (request.source_routing_id == local.node_routing_id) {
        return _operations->fail (operation_id (local.lifecycle_generation, *request.correlation),
                                  foundation::operation_terminal_t::transport_failed);
    }
    if (!request.reply_token)
        return false;
    const auto port = _lane.run_sync ([this] {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        return _port;
    });
    if (!port)
        return false;
    detail::backend::raw_message_t parts{
      protocol::encode_reply_header (*request.correlation, terminal_result, failure_code)};
    return port->reply (
      detail::backend::raw_received_t{request.source_routing_id, request.reply_token, {}},
      std::move (parts));
}

task_t<std::size_t>
raw_mesh_node_owner_t::expire_requests (foundation::operation_registry_t::clock_t::time_point now)
{
    return _operations->expire (now);
}

task_t<std::optional<raw_mesh_node_owner_t::local_operation_reservation_t>>
raw_mesh_node_owner_t::prepare_local_operation_async (
  std::optional<foundation::call_id_t> requested)
{
    return _lane.run_task ([this, requested] () -> std::optional<local_operation_reservation_t> {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        if (!_port)
            return std::nullopt;
        foundation::call_id_t operation;
        if (requested) {
            (void) take_reply_route_id_locked (requested->low);
            operation = *requested;
        } else {
            operation = operation_id (_options.descriptor.lifecycle_generation,
                                      take_reply_route_id_locked ());
        }
        return local_operation_reservation_t{operation, _port};
    });
}

bool raw_mesh_node_owner_t::register_prepared_local_operation (
  const local_operation_reservation_t &reservation,
  foundation::operation_registry_t::clock_t::time_point deadline,
  foundation::operation_registry_t::callback_t callback,
  mesh_request_surface_t request_surface)
{
    if (!callback)
        throw std::invalid_argument ("local operation callback is required");
    const auto operation = reservation.first;
    if (!_operations->register_operation (
          operation, deadline, std::move (callback), {},
          mesh_request_metric_t (_request_metrics, request_surface),
          request_surface == mesh_request_surface_t::spot
              || request_surface == mesh_request_surface_t::actor
            ? request_identity (protocol::wire_operation_id_t{operation.high, operation.low})
            : std::nullopt,
          detail::current_wait_owner ()))
        return false;
    return true;
}

task_t<std::optional<foundation::call_id_t>> raw_mesh_node_owner_t::register_local_operation_async (
  foundation::operation_registry_t::clock_t::time_point deadline,
  foundation::operation_registry_t::callback_t callback,
  std::optional<foundation::call_id_t> requested,
  mesh_request_surface_t request_surface)
{
    const auto reservation = co_await prepare_local_operation_async (requested);
    if (!reservation
        || !register_prepared_local_operation (*reservation, deadline, std::move (callback),
                                               request_surface))
        co_return std::nullopt;
    co_return reservation->first;
}


bool raw_mesh_node_owner_t::complete_local_operation (
  const foundation::call_id_t &operation,
  foundation::operation_registry_t::before_dispatch_t before_dispatch)
{
    return _operations->complete (operation, {}, std::move (before_dispatch));
}

bool raw_mesh_node_owner_t::complete_local_operation_async (
  const foundation::call_id_t &operation, std::function<task_t<void> ()> before_dispatch)
{
    return _operations->complete_async (operation, std::move (before_dispatch));
}

bool raw_mesh_node_owner_t::fail_local_operation (
  const foundation::call_id_t &operation,
  foundation::operation_terminal_t terminal,
  foundation::operation_registry_t::before_dispatch_t before_dispatch)
{
    return _operations->fail (operation, terminal, {}, std::move (before_dispatch));
}

bool raw_mesh_node_owner_t::unregister_local_operation (
  const foundation::call_id_t &operation) noexcept
{
    try {
        return _operations->unregister (operation);
    }
    catch (...) {
        return false;
    }
}

std::optional<foundation::operation_registry_t::request_identity_t>
raw_mesh_node_owner_t::request_identity (
  std::optional<protocol::wire_operation_id_t> operation) const
{
    const auto lease = _local_request_lease ? _local_request_lease () : nullptr;
    if (!lease || lease->owner_id.empty () || lease->lease_generation <= 0)
        return std::nullopt;
    return foundation::operation_registry_t::request_identity_t{
      operation ? foundation::call_id_t{operation->high, operation->low} : foundation::call_id_t{},
      lease};
}

std::optional<protocol::request_source_fence_t>
raw_mesh_node_owner_t::relocated_request_source (const protocol::wire_operation_id_t &operation,
                                                 std::uint64_t reply_route_id) const
{
    const auto identity = _operations->request_identity (
      {_options.descriptor.lifecycle_generation, reply_route_id}, {operation.high, operation.low});
    if (!identity)
        return std::nullopt;
    return protocol::request_source_fence_t{
      identity->lease->owner_id, static_cast<std::uint64_t> (identity->lease->lease_generation),
      _options.descriptor.node_routing_id, _options.descriptor.lifecycle_generation};
}

bool raw_mesh_node_owner_t::owns_request_source (const protocol::request_source_fence_t &source,
                                                 const service_node_descriptor_t &local) noexcept
{
    return source.node_routing_id == local.node_routing_id
           && source.node_generation == local.lifecycle_generation;
}

bool raw_mesh_node_owner_t::complete_relocated_request (
  const protocol::reply_relay_t &relay, const std::optional<protocol::application_payload_t> &reply)
{
    if (!relocated_request_source (relay.operation, relay.reply_route_id))
        return false;
    if (relay.terminal_result == 0 && !reply)
        return false;
    const foundation::call_id_t id{_options.descriptor.lifecycle_generation, relay.reply_route_id};
    if (relay.terminal_result == 0 && reply)
        return _operations->complete (id, detail::backend::materialize_binding_part (
                                            protocol::encode_application_payload (*reply)));
    const protocol::reply_header_t failure{relay.reply_route_id, relay.terminal_result,
                                           static_cast<std::uint32_t> (relay.failure_code)};
    return _operations->fail (
      id, foundation::operation_terminal_t::transport_failed,
      detail::backend::materialize_binding_part (protocol::encode_reply_header (
        failure.correlation, failure.terminal_result, failure.failure_code)),
      {}, request_metric_terminal (failure));
}

bool raw_mesh_node_owner_t::operation_pending (const foundation::call_id_t &operation) const
{
    return _operations->contains (operation);
}

std::size_t raw_mesh_node_owner_t::pending_operation_count () const
{
    return _operations->size ();
}

task_t<bool>
raw_mesh_node_owner_t::send_to_channel (const std::string &channel_name,
                                        protocol::application_payload_t application_payload)
{
    co_return co_await send_to_channel_result (channel_name, std::move (application_payload))
      == zlink::submit_result_t::ok;
}

task_t<zlink::submit_result_t>
raw_mesh_node_owner_t::send_to_channel_result (const std::string &channel_name,
                                               protocol::application_payload_t application_payload)
{
    auto selected = _topology.select (channel_name);
    if (!selected) {
        throw *selected.error ();
    }
    co_return co_await send_with_header_result (std::move (selected.value ()),
                                                protocol::encode_channel_send_header (channel_name),
                                                std::move (application_payload), {}, true);
}

task_t<bool>
raw_mesh_node_owner_t::send_to_spot (const std::vector<std::uint8_t> &target_routing_id,
                                     const std::string &source_spot_id,
                                     const protocol::spot_route_fence_t &target,
                                     protocol::application_payload_t application_payload,
                                     std::optional<protocol::wire_operation_id_t> operation,
                                     std::uint8_t message_follow_hop_count)
{
    co_return co_await send_to_spot_result (target_routing_id, source_spot_id, target,
                                            std::move (application_payload), operation,
                                            message_follow_hop_count)
      == zlink::submit_result_t::ok;
}

task_t<zlink::submit_result_t>
raw_mesh_node_owner_t::send_to_spot_result (const std::vector<std::uint8_t> &target_routing_id,
                                            const std::string &source_spot_id,
                                            const protocol::spot_route_fence_t &target,
                                            protocol::application_payload_t application_payload,
                                            std::optional<protocol::wire_operation_id_t> operation,
                                            std::uint8_t message_follow_hop_count)
{
    const auto sequence = co_await next_operation_sequence_async ();
    if (!sequence)
        co_return zlink::submit_result_t::terminated;
    const auto local = _topology.local_descriptor ();
    co_return co_await send_with_header_result (
      target_routing_id,
      protocol::encode_spot_message_header (
        protocol::command::spotSend, source_spot_id, target,
        operation.value_or (protocol::wire_operation_id_t{local.lifecycle_generation, *sequence}),
        std::nullopt, message_follow_hop_count),
      std::move (application_payload));
}

task_t<bool>
raw_mesh_node_owner_t::request_to_spot (const std::vector<std::uint8_t> &target_routing_id,
                                        const std::string &source_spot_id,
                                        const protocol::spot_route_fence_t &target,
                                        const protocol::application_payload_t &application_payload,
                                        std::chrono::milliseconds timeout,
                                        foundation::operation_registry_t::callback_t callback,
                                        std::optional<protocol::wire_operation_id_t> operation,
                                        std::optional<std::uint64_t> correlation,
                                        std::uint8_t message_follow_hop_count)
{
    const auto deadline = capture_request_deadline (timeout);
    const auto local = _topology.local_descriptor ();
    co_return co_await request_with_header (
      target_routing_id,
      [source_spot_id, target, local, operation,
       message_follow_hop_count] (std::uint64_t correlation) {
          const auto exact = operation.value_or (
            protocol::wire_operation_id_t{local.lifecycle_generation, correlation});
          return protocol::encode_spot_message_header (protocol::command::spotRequest,
                                                       source_spot_id, target, exact, correlation,
                                                       message_follow_hop_count);
      },
      application_payload, deadline, std::move (callback), correlation, false,
      mesh_request_metric_t (_request_metrics, mesh_request_surface_t::spot),
      request_identity (operation));
}

task_t<bool> raw_mesh_node_owner_t::send_to_actor (
  const std::vector<std::uint8_t> &target_routing_id,
  const std::optional<std::pair<std::string, std::uint64_t>> &source_actor,
  const protocol::actor_route_fence_t &target,
  protocol::application_payload_t application_payload,
  std::optional<protocol::actor_message_header_t::bound_session_source_t> bound_session_source,
  std::optional<protocol::wire_operation_id_t> operation,
  std::uint8_t message_follow_hop_count)
{
    co_return co_await send_to_actor_result (
      target_routing_id, source_actor, target, std::move (application_payload),
      std::move (bound_session_source), operation, message_follow_hop_count)
      == zlink::submit_result_t::ok;
}

task_t<zlink::submit_result_t> raw_mesh_node_owner_t::send_to_actor_result (
  const std::vector<std::uint8_t> &target_routing_id,
  const std::optional<std::pair<std::string, std::uint64_t>> &source_actor,
  const protocol::actor_route_fence_t &target,
  protocol::application_payload_t application_payload,
  std::optional<protocol::actor_message_header_t::bound_session_source_t> bound_session_source,
  std::optional<protocol::wire_operation_id_t> operation,
  std::uint8_t message_follow_hop_count)
{
    const auto sequence = co_await next_operation_sequence_async ();
    if (!sequence)
        co_return zlink::submit_result_t::terminated;
    const auto local = _topology.local_descriptor ();
    co_return co_await send_with_header_result (
      target_routing_id,
      protocol::encode_actor_message_header (
        protocol::command::actorSend, source_actor, target,
        operation.value_or (protocol::wire_operation_id_t{local.lifecycle_generation, *sequence}),
        std::nullopt, message_follow_hop_count, std::move (bound_session_source)),
      std::move (application_payload));
}

task_t<bool> raw_mesh_node_owner_t::request_to_actor (
  const std::vector<std::uint8_t> &target_routing_id,
  const std::optional<std::pair<std::string, std::uint64_t>> &source_actor,
  const protocol::actor_route_fence_t &target,
  const protocol::application_payload_t &application_payload,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback,
  std::optional<protocol::wire_operation_id_t> operation,
  std::optional<protocol::actor_message_header_t::bound_session_source_t> bound_session_source,
  std::optional<std::uint64_t> correlation,
  std::uint8_t message_follow_hop_count)
{
    const auto deadline = capture_request_deadline (timeout);
    const auto local = _topology.local_descriptor ();
    co_return co_await request_with_header (
      target_routing_id,
      [source_actor, target, local, operation, message_follow_hop_count,
       bound_session_source = std::move (bound_session_source)] (std::uint64_t correlation) {
          const auto exact = operation.value_or (
            protocol::wire_operation_id_t{local.lifecycle_generation, correlation});
          return protocol::encode_actor_message_header (
            protocol::command::actorRequest, source_actor, target, exact, correlation,
            message_follow_hop_count, bound_session_source);
      },
      application_payload, deadline, std::move (callback), correlation, false,
      mesh_request_metric_t (_request_metrics, mesh_request_surface_t::actor),
      request_identity (operation));
}

task_t<bool> raw_mesh_node_owner_t::request_user_spot_create (
  const std::vector<std::uint8_t> &target_routing_id,
  protocol::user_spot_create_header_t request,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback)
{
    const auto deadline = capture_request_deadline (timeout);
    const auto local = _topology.local_descriptor ();
    if (request.source_node_routing_id != local.node_routing_id
        || request.source_node_generation != local.lifecycle_generation
        || request.reservation.target_node_routing_id != target_routing_id) {
        throw std::invalid_argument ("User Spot create source or target fence is inconsistent");
    }
    co_return co_await request_infrastructure (
      target_routing_id,
      [request = std::move (request)] (std::uint64_t correlation) mutable {
          request.correlation = correlation;
          return protocol::encode_user_spot_create_header (request);
      },
      [] (const detail::backend::raw_message_t &parts) {
          if (parts.empty () || parts.size () > 2)
              throw protocol::service_wire_error_t (
                "User Spot create reply has an invalid part count");
          const auto reply = protocol::decode_user_spot_create_reply (parts.front ());
          if (reply.header.terminal_result != 0 && parts.size () != 1)
              throw protocol::service_wire_error_t (
                "failed User Spot create reply carries a payload");
          if (parts.size () == 2)
              (void) protocol::decode_application_payload (parts[1], false);
          return protocol::pack_infrastructure_reply (parts);
      },
      deadline, std::move (callback));
}

task_t<bool> raw_mesh_node_owner_t::send_bound_session (
  const std::vector<std::uint8_t> &session_owner_routing_id,
  const protocol::bound_session_send_t &record,
  protocol::application_payload_t application_payload)
{
    co_return co_await send_bound_session_result (session_owner_routing_id, record,
                                                  std::move (application_payload))
      == zlink::submit_result_t::ok;
}

task_t<zlink::submit_result_t> raw_mesh_node_owner_t::send_bound_session_result (
  const std::vector<std::uint8_t> &session_owner_routing_id,
  const protocol::bound_session_send_t &record,
  protocol::application_payload_t application_payload,
  detail::backend::raw_send_stage_trace_t trace)
{
    const auto local = _topology.local_descriptor ();
    if (record.actor.target_node_routing_id != local.node_routing_id
        || record.actor.target_node_generation != local.lifecycle_generation) {
        throw std::invalid_argument ("bound Session send Actor authority source is inconsistent");
    }
    co_return co_await send_with_header_result (session_owner_routing_id,
                                                protocol::encode_bound_session_send (record),
                                                std::move (application_payload), std::move (trace));
}

task_t<bool> raw_mesh_node_owner_t::request_bound_session_bind (
  const std::vector<std::uint8_t> &actor_owner_routing_id,
  protocol::bound_session_bind_t record,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback,
  std::shared_ptr<task_completion_source_t<void>> admitted)
{
    if (record.actor.target_node_routing_id != actor_owner_routing_id) {
        throw std::invalid_argument ("bound Session bind Actor authority target is inconsistent");
    }
    if (timeout <= std::chrono::milliseconds::zero ()) {
        throw std::invalid_argument ("bound Session bind request timeout must be positive");
    }

    const auto target_routing_id = actor_owner_routing_id;
    const auto target_generation = record.actor.target_node_generation;
    const auto has_descriptor =
      co_await _lane.run_task ([this, target_routing_id, target_generation] {
          const auto expected = _expected_peers.find (target_routing_id);
          if (expected != _expected_peers.end ()
              && expected->second.lifecycle_generation == target_generation)
              return true;
          const auto peer = _topology.peer (target_routing_id);
          return peer && peer->descriptor.lifecycle_generation == target_generation;
      });
    if (!has_descriptor) {
        throw framework_exception_t (
          framework_error_kind_t::unavailable,
          "Remote Actor session binding owner descriptor is unavailable");
    }

    const auto deadline = foundation::operation_registry_t::clock_t::now () + timeout;
    const auto local = _topology.local_descriptor ();
    const auto operations = _operations;
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    std::uint64_t correlation = 0;
    foundation::call_id_t id{};
    detail::backend::raw_message_t request_parts;
    bool registered = false;
    auto terminal_wake = std::make_shared<task_completion_source_t<void>> ();
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
        if (port) {
            correlation = take_reply_route_id_locked ();
            id = operation_id (local.lifecycle_generation, correlation);
            record.correlation = correlation;
            request_parts = {protocol::encode_bound_session_bind (record)};
        }
    }
    if (!port) {
        callback (foundation::operation_terminal_t::shutdown, {});
        co_return true;
    }
    registered = operations->register_operation (
      id, deadline,
      [callback = std::move (callback), terminal_wake] (auto terminal, auto payload) {
          terminal_wake->complete (result_t<void>::success ());
          callback (terminal, std::move (payload));
      },
      actor_owner_routing_id, {}, {}, detail::current_wait_owner ());
    if (!registered)
        co_return false;

    co_return co_await submit_registered_request (
      std::move (port), observe_request_peer (actor_owner_routing_id), operations, id, correlation,
      actor_owner_routing_id, std::move (request_parts),
      [] (const detail::backend::raw_message_t &parts) {
          if (parts.size () != 1)
              throw protocol::service_wire_error_t (
                "bound Session bind reply must contain one header");
          const auto reply = protocol::decode_reply_header (parts.front ());
          if (!protocol::valid_terminal_failure (
                reply.terminal_result,
                static_cast<protocol::framework_error_code> (reply.failure_code))) {
              throw protocol::service_wire_error_t (
                "bound Session bind reply terminal is inconsistent");
          }
          return parts.front ();
      },
      deadline, terminal_wake->task (), std::move (admitted));
}

task_t<bool> raw_mesh_node_owner_t::send_bound_session_replaced (
  const std::vector<std::uint8_t> &retired_session_owner_routing_id,
  const protocol::bound_session_replaced_t &record)
{
    const auto local = _topology.local_descriptor ();
    if (record.actor_authority.target_node_routing_id != local.node_routing_id
        || record.actor_authority.target_node_generation != local.lifecycle_generation
        || record.retired_session.session_owner_node_routing_id
             != retired_session_owner_routing_id) {
        throw std::invalid_argument ("bound Session replacement source or target is inconsistent");
    }
    co_return co_await send_header_only (retired_session_owner_routing_id,
                                         protocol::encode_bound_session_replaced (record));
}

task_t<void>
raw_mesh_node_owner_t::request_bound_session_close (protocol::bound_session_close_t record,
                                                    std::chrono::milliseconds timeout)
{
    const auto deadline = capture_request_deadline (timeout);
    const auto target = record.session.session_owner_node_routing_id;
    auto completion = std::make_shared<task_completion_source_t<void>> ();
    const auto submitted = co_await request_infrastructure (
      target,
      [record = std::move (record)] (std::uint64_t correlation) mutable {
          record.correlation = correlation;
          return protocol::encode_bound_session_close (record);
      },
      [] (const detail::backend::raw_message_t &parts) {
          if (parts.size () != 1
              || parts.front ().size () != protocol::reply_header_t::encoded_size)
              throw protocol::service_wire_error_t ("Session close reply must have one frame");
          (void) protocol::decode_reply_header (parts.front ());
          return protocol::pack_infrastructure_reply (parts);
      },
      deadline,
      [completion] (foundation::operation_terminal_t terminal, zlink::message_t packed) {
          try {
              protocol::reply_header_t reply{};
              if (terminal == foundation::operation_terminal_t::completed) {
                  const auto parts =
                    protocol::unpack_infrastructure_reply (protocol::message_bytes (packed));
                  reply = protocol::decode_reply_header (parts.front ());
                  if (reply.terminal_result
                      == static_cast<std::uint32_t> (protocol::request_terminal_result::ok)) {
                      completion->complete (result_t<void>::success ());
                      return;
                  }
              }
              completion->complete (result_t<void>::failure (
                user_spot_terminal::map_user_spot_operation_failure (terminal, reply, false),
                "Bound Session close failed"));
          }
          catch (const protocol::service_wire_error_t &error) {
              completion->complete (
                result_t<void>::failure (framework_error_kind_t::protocol_error, error.what ()));
          }
      });
    if (!submitted)
        completion->complete (result_t<void>::failure (framework_error_kind_t::unavailable,
                                                       "Session close was not admitted"));
    co_await completion->task ();
}

bool raw_mesh_node_owner_t::reply_bound_session_bind (
  const service_mailbox_record_t &request,
  std::uint32_t terminal_result,
  std::uint32_t failure_code,
  std::optional<protocol::stream_bind_reply_tail_t> binding)
{
    if (!request.correlation)
        throw std::invalid_argument ("bound Session bind reply requires a correlation");
    const auto wire = protocol::decode_header (protocol::message_bytes (request.parts.front ()));
    if (wire.kind == protocol::command::boundSessionBind
        && protocol::decode_bound_session_bind (protocol::message_bytes (request.parts.front ()))
               .binding.state
             == protocol::bound_session_binding_state_t::active)
        return reply_infrastructure (
          request, protocol::encode_stream_bind_reply (*request.correlation, terminal_result,
                                                       failure_code, binding));
    return reply_infrastructure (
      request, protocol::encode_reply_header (*request.correlation, terminal_result, failure_code));
}

task_t<bool>
raw_mesh_node_owner_t::request_actor_create (std::vector<std::uint8_t> target_routing_id,
                                             protocol::actor_create_header_t request,
                                             std::chrono::milliseconds timeout,
                                             foundation::operation_registry_t::callback_t callback)
{
    const auto deadline = capture_request_deadline (timeout);
    const auto local = _topology.local_descriptor ();
    if (request.source_node_routing_id != local.node_routing_id
        || request.source_node_generation != local.lifecycle_generation
        || request.reservation.target_node_routing_id != target_routing_id)
        throw std::invalid_argument ("Actor create source or target fence is inconsistent");

    if (target_routing_id == local.node_routing_id) {
        co_return co_await request_infrastructure (
          target_routing_id,
          [request = std::move (request)] (std::uint64_t correlation) mutable {
              request.correlation = correlation;
              return protocol::encode_actor_create_header (request);
          },
          [] (const detail::backend::raw_message_t &parts) {
              if (parts.empty () || parts.size () > 2)
                  throw protocol::service_wire_error_t (
                    "Actor create reply has an invalid part count");
              const auto reply = protocol::decode_actor_create_reply (parts.front ());
              if (reply.header.terminal_result != 0 && parts.size () != 1)
                  throw protocol::service_wire_error_t (
                    "failed Actor create reply carries a payload");
              if (parts.size () == 2)
                  (void) protocol::decode_application_payload (parts[1], false);
              return protocol::pack_infrastructure_reply (parts);
          },
          deadline, std::move (callback));
    }

    const auto operations = _operations;
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    std::uint64_t correlation = 0;
    foundation::call_id_t id{};
    detail::backend::raw_message_t request_parts;
    bool registered = false;
    auto terminal_wake = std::make_shared<task_completion_source_t<void>> ();
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
        if (port) {
            correlation = take_reply_route_id_locked ();
            id = operation_id (local.lifecycle_generation, correlation);
            request.correlation = correlation;
            request_parts = {protocol::encode_actor_create_header (request)};
        }
    }
    if (!port) {
        callback (foundation::operation_terminal_t::shutdown, {});
        co_return true;
    }
    registered = operations->register_operation (
      id, deadline,
      [callback = std::move (callback), terminal_wake] (auto terminal, auto payload) {
          terminal_wake->complete (result_t<void>::success ());
          callback (terminal, std::move (payload));
      },
      target_routing_id, {}, {}, detail::current_wait_owner ());
    if (!registered)
        co_return false;

    co_return co_await submit_registered_request (
      std::move (port), observe_request_peer (target_routing_id), operations, id, correlation,
      target_routing_id, std::move (request_parts),
      [] (const detail::backend::raw_message_t &parts) {
          if (parts.empty () || parts.size () > 2)
              throw protocol::service_wire_error_t ("Actor create reply has an invalid part count");
          const auto reply = protocol::decode_actor_create_reply (parts.front ());
          if (reply.header.terminal_result != 0 && parts.size () != 1)
              throw protocol::service_wire_error_t ("failed Actor create reply carries a payload");
          if (parts.size () == 2)
              (void) protocol::decode_application_payload (parts[1], false);
          return protocol::pack_infrastructure_reply (parts);
      },
      deadline, terminal_wake->task ());
}

task_t<actor_join_wire_outcome_t> raw_mesh_node_owner_t::request_actor_join (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::actor_join_request_t &request,
  const std::optional<protocol::application_payload_t> &payload,
  std::chrono::milliseconds timeout)
{
    const auto deadline = foundation::operation_registry_t::clock_t::now () + timeout;
    actor_join_wire_outcome_t outcome;
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port) {
        outcome.failure = actor_join_wire_failure_t::unavailable;
        co_return outcome;
    }
    if (timeout <= std::chrono::milliseconds::zero ()) {
        //  The caller's deadline is already spent before the wire is
        //  touched — spec 32 classifies this as DeadlineExceeded, not a
        //  routing failure.
        outcome.failure = actor_join_wire_failure_t::deadline_exceeded;
        co_return outcome;
    }
    if (request.correlation == 0) {
        // The generated actorJoin codec rejects an unfenced request as a
        // wire-contract violation.
        outcome.failure = actor_join_wire_failure_t::protocol_error;
        co_return outcome;
    }
    detail::backend::raw_message_t parts;
    try {
        parts = protocol::encode_actor_join_28 (
          {request.correlation,
           {request.actor.actor_id, request.actor.object_generation,
            request.actor.target_node_routing_id, request.actor.target_node_generation,
            request.actor.authority_owner_generation, request.actor.owner_lease_generation},
           request.entry,
           {request.target_spot.spot_id, request.target_spot.object_generation,
            request.target_spot.target_node_routing_id, request.target_spot.target_node_generation,
            request.target_spot.authority_owner_generation,
            request.target_spot.owner_lease_generation},
           payload
             ? std::optional<
                 protocol::
                   service_wire_pilot_application_payload_envelope_v1>{{payload->packet_name,
                                                                        payload->content_type,
                                                                        payload->payload_bytes ()}}
             : std::nullopt});
    }
    catch (const std::invalid_argument &) {
        throw protocol::service_wire_error_t ("invalid Actor join request");
    }
    using completion_t = std::pair<foundation::operation_terminal_t, zlink::message_t>;
    auto completion = std::make_shared<task_completion_source_t<completion_t>> ();
    auto pending = completion->task ();
    foundation::call_id_t id{};
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        id = operation_id (_topology.local_descriptor ().lifecycle_generation,
                           take_reply_route_id_locked ());
    }
    if (!_operations->register_operation (
          id, deadline,
          [completion] (auto terminal, auto packed) {
              completion->complete (
                result_t<completion_t>::success ({terminal, std::move (packed)}));
          },
          target_routing_id, {}, {}, detail::current_wait_owner ())) {
        outcome.failure = actor_join_wire_failure_t::unavailable;
        co_return outcome;
    }
    (void) co_await submit_registered_request (
      std::move (port), observe_request_peer (target_routing_id), _operations, id,
      request.correlation, target_routing_id, std::move (parts),
      protocol::pack_infrastructure_reply, deadline, completion->task ());
    const auto [terminal, packed] = co_await pending;
    if (terminal != foundation::operation_terminal_t::completed) {
        outcome.failure = terminal == foundation::operation_terminal_t::timed_out
                            ? actor_join_wire_failure_t::deadline_exceeded
                          : terminal == foundation::operation_terminal_t::protocol_error
                            ? actor_join_wire_failure_t::protocol_error
                            : actor_join_wire_failure_t::unavailable;
        co_return outcome;
    }
    try {
        const auto reply_parts =
          protocol::unpack_infrastructure_reply (protocol::message_bytes (packed));
        auto reply = protocol::decode_actor_join_reply (reply_parts.front ());
        if (reply_parts.size () == 2) {
            //  Surface the decoded application reply to the caller instead
            //  of validating and discarding it.
            outcome.application_reply =
              protocol::decode_application_payload (reply_parts[1], false);
        }
        outcome.reply = std::move (reply);
        co_return outcome;
    }
    catch (const protocol::service_wire_error_t &) {
        outcome.failure = actor_join_wire_failure_t::protocol_error;
        outcome.application_reply.reset ();
        co_return outcome;
    }
}

task_t<bool> raw_mesh_node_owner_t::send_instance_spot_activation (
  const std::vector<std::uint8_t> &target_routing_id,
  protocol::instance_spot_activation_header_t request,
  std::optional<std::vector<std::uint8_t>> metadata,
  protocol::application_payload_t application_payload)
{
    const auto local = _topology.local_descriptor ();
    if (request.request || request.reply_route_id != 0
        || request.source_node_routing_id != local.node_routing_id
        || request.source_node_generation != local.lifecycle_generation
        || request.target.target_node_routing_id != target_routing_id
        || request.has_metadata != metadata.has_value ()) {
        throw std::invalid_argument (
          "Instance Spot send source, target, metadata, or operation kind is inconsistent");
    }
    if (target_routing_id != local.node_routing_id)
        co_return co_await send_with_header_result (
          target_routing_id, protocol::encode_instance_spot_activation_header (request),
          std::move (application_payload), {}, false, std::move (metadata))
          == zlink::submit_result_t::ok;
    detail::backend::raw_message_t parts{
      protocol::encode_instance_spot_activation_header (request)};
    if (metadata)
        parts.push_back (std::move (*metadata));
    parts.push_back (protocol::encode_application_payload (application_payload));
    const auto accepted = _mailbox.try_enqueue (service_mailbox_record_t{
      owner_key (local.node_routing_id), service_mailbox_domain_t::infrastructure,
      detail::backend::materialize_binding_parts (std::move (parts)), local.node_routing_id,
      std::nullopt, std::nullopt});
    if (accepted)
        signal_activity ();
    co_return accepted;
}

task_t<bool> raw_mesh_node_owner_t::request_instance_spot_activation (
  const std::vector<std::uint8_t> &target_routing_id,
  protocol::instance_spot_activation_header_t request,
  std::optional<std::vector<std::uint8_t>> metadata,
  protocol::application_payload_t application_payload,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback)
{
    if (timeout <= std::chrono::milliseconds::zero ()) {
        throw std::invalid_argument ("Instance Spot activation timeout must be positive");
    }
    const auto deadline = foundation::operation_registry_t::clock_t::now () + timeout;
    const auto local = _topology.local_descriptor ();
    if (!request.request || request.source_node_routing_id != local.node_routing_id
        || request.source_node_generation != local.lifecycle_generation
        || request.target.target_node_routing_id != target_routing_id
        || request.has_metadata != metadata.has_value ()) {
        throw std::invalid_argument (
          "Instance Spot activation source, target, or metadata fence is inconsistent");
    }
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    std::uint64_t correlation = 0;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
        if (!port) {
            co_return false;
        }
        correlation = take_reply_route_id_locked ();
    }
    request.reply_route_id = correlation;
    detail::backend::raw_message_t parts{
      protocol::encode_instance_spot_activation_header (request)};
    if (metadata)
        parts.push_back (std::move (*metadata));
    parts.push_back (protocol::encode_application_payload (application_payload));
    const auto id = operation_id (local.lifecycle_generation, correlation);
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    if (!_operations->register_operation (
          id, deadline, request_admission_completion (std::move (callback), admitted),
          target_routing_id,
          mesh_request_metric_t (_request_metrics, mesh_request_surface_t::instance_spot), {},
          detail::current_wait_owner ())) {
        co_return false;
    }
    if (target_routing_id == local.node_routing_id) {
        const auto accepted = _mailbox.try_enqueue (service_mailbox_record_t{
          owner_key (local.node_routing_id), service_mailbox_domain_t::infrastructure,
          detail::backend::materialize_binding_parts (std::move (parts)), local.node_routing_id,
          std::nullopt, correlation});
        if (!accepted)
            (void) _operations->fail (id, foundation::operation_terminal_t::transport_failed);
        if (accepted)
            signal_activity ();
        co_return accepted;
    }
    const auto operations = _operations;
    auto observe = [operations, id, correlation] (auto running) {
        detail::observe_task_terminal (
          *running, [operations, id, correlation,
                     running] (const result_t<detail::backend::raw_request_completion_t> &settled) {
              if (!settled) {
                  (void) operations->fail (id, foundation::operation_terminal_t::transport_failed);
                  return;
              }
              const auto &completion = settled.value ();
              if (completion.terminal != zlink::request_result_t::ok) {
                  if (const auto terminal =
                        messaging::request_failure_mapper_t{}.transport_terminal (
                          completion.terminal)) {
                      (void) operations->fail (id, *terminal);
                  } else {
                      (void) operations->complete (
                        id, detail::backend::materialize_binding_part (
                              protocol::pack_infrastructure_reply (
                                request_failure_reply_parts (correlation, completion.terminal))));
                  }
                  return;
              }
              try {
                  constexpr std::size_t header_part_count = 1;
                  constexpr std::size_t reply_with_payload_part_count = 2;
                  const auto reply_parts = detail::backend::copy_binding_parts (completion.parts);
                  if (reply_parts.empty () || reply_parts.size () > reply_with_payload_part_count)
                      throw protocol::service_wire_error_t (
                        "Instance Spot activation reply has an invalid part count");
                  const auto reply = protocol::decode_reply_header (reply_parts.front ());
                  if (reply.correlation != correlation)
                      throw protocol::service_wire_error_t (
                        "Instance Spot activation reply correlation does not match");
                  if (reply.terminal_result
                        != static_cast<std::uint32_t> (protocol::request_terminal_result::ok)
                      && reply_parts.size () != header_part_count)
                      throw protocol::service_wire_error_t (
                        "failed Instance Spot activation reply carries a payload");
                  if (reply_parts.size () == reply_with_payload_part_count)
                      (void) protocol::decode_application_payload (reply_parts[header_part_count],
                                                                   false);
                  (void) operations->complete (id,
                                               detail::backend::materialize_binding_part (
                                                 protocol::pack_infrastructure_reply (reply_parts)),
                                               {}, request_metric_terminal (reply));
              }
              catch (const protocol::service_wire_error_t &) {
                  //  Spec 32-framework-error-model:91-92 — a reply that can't be
                  //  processed is ProtocolError, not a transport failure. Complete
                  //  with a synthesized protocolError header so the upper adapter
                  //  and sink classify it via reply_header_exception.
                  (void) operations->complete (
                    id,
                    detail::backend::materialize_binding_part (protocol::pack_infrastructure_reply (
                      detail::backend::raw_message_t{protocol::encode_reply_header (
                        correlation,
                        static_cast<std::uint32_t> (
                          protocol::request_terminal_result::protocolError),
                        static_cast<std::uint32_t> (
                          protocol::framework_error_code::requestProtocolError))})),
                    {}, foundation::operation_terminal_t::protocol_error);
              }
          });
    };
    (void) co_await submit_registered_request (
      std::move (port), observe_request_peer (target_routing_id), operations, id, correlation,
      target_routing_id, std::move (parts), {}, deadline, admitted->task (), admitted,
      std::move (observe));
    co_return co_await await_request_admission (admitted->task ());
}

task_t<bool> raw_mesh_node_owner_t::request_user_spot_close (
  std::vector<std::uint8_t> target_routing_id,
  protocol::user_spot_close_header_t request,
  std::chrono::steady_clock::time_point deadline,
  foundation::operation_registry_t::callback_t callback)
{
    const auto local = _topology.local_descriptor ();
    if (request.source_node_routing_id != local.node_routing_id
        || request.source_node_generation != local.lifecycle_generation
        || request.target.target_node_routing_id != target_routing_id) {
        throw std::invalid_argument ("User Spot close source or target fence is inconsistent");
    }
    co_return co_await request_infrastructure (
      target_routing_id,
      [request = std::move (request)] (std::uint64_t correlation) mutable {
          request.correlation = correlation;
          return protocol::encode_user_spot_close_header (request);
      },
      [] (const detail::backend::raw_message_t &parts) {
          if (parts.size () != 1)
              throw protocol::service_wire_error_t (
                "User Spot close reply must contain one header");
          (void) protocol::decode_user_spot_close_reply (parts.front ());
          return protocol::pack_infrastructure_reply (parts);
      },
      deadline, std::move (callback));
}

task_t<bool> raw_mesh_node_owner_t::request_infrastructure (
  const std::vector<std::uint8_t> &target_routing_id,
  const std::function<std::vector<std::uint8_t> (std::uint64_t)> &header,
  const std::function<std::vector<std::uint8_t> (const detail::backend::raw_message_t &)>
    &decode_reply,
  foundation::operation_registry_t::clock_t::time_point deadline,
  foundation::operation_registry_t::callback_t callback)
{
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    std::uint64_t correlation = 0;
    const auto local = _topology.local_descriptor ();
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
        if (!port) {
            co_return false;
        }
        correlation = take_reply_route_id_locked ();
    }
    const auto id = operation_id (local.lifecycle_generation, correlation);
    auto ended = std::make_shared<task_completion_source_t<void>> ();
    if (!_operations->register_operation (
          id, deadline,
          [ended, callback = std::move (callback)] (auto terminal, auto payload) {
              ended->complete (result_t<void>::success ());
              callback (terminal, std::move (payload));
          },
          target_routing_id, {}, {}, detail::current_wait_owner ())) {
        co_return false;
    }
    if (target_routing_id == local.node_routing_id) {
        const auto accepted = _mailbox.try_enqueue (service_mailbox_record_t{
          owner_key (local.node_routing_id), service_mailbox_domain_t::infrastructure,
          detail::backend::materialize_binding_parts ({header (correlation)}),
          local.node_routing_id, std::nullopt, correlation});
        if (!accepted)
            (void) _operations->fail (id, foundation::operation_terminal_t::transport_failed);
        if (accepted)
            signal_activity ();
        co_return accepted;
    }
    const auto operations = _operations;
    detail::backend::raw_message_t request_parts{header (correlation)};
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds> (
      deadline - foundation::operation_registry_t::clock_t::now ());
    if (remaining <= std::chrono::milliseconds::zero ())
        co_return true;
    co_return co_await submit_registered_request (
      std::move (port), observe_request_peer (target_routing_id), std::move (operations), id,
      correlation, target_routing_id, std::move (request_parts), decode_reply, deadline,
      ended->task ());
}

bool raw_mesh_node_owner_t::reply_infrastructure (const service_mailbox_record_t &request,
                                                  std::vector<std::uint8_t> header)
{
    if (request.source_routing_id.empty () || !request.correlation) {
        throw std::invalid_argument ("raw mesh infrastructure reply requires a request record");
    }
    const auto local = _topology.local_descriptor ();
    if (request.source_routing_id == local.node_routing_id) {
        return _operations->complete (
          operation_id (local.lifecycle_generation, *request.correlation),
          detail::backend::materialize_binding_part (
            protocol::pack_infrastructure_reply ({std::move (header)})));
    }
    if (!request.reply_token)
        return false;
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port)
        return false;
    detail::backend::raw_message_t parts{std::move (header)};
    return port->reply (
      detail::backend::raw_received_t{request.source_routing_id, request.reply_token, {}},
      std::move (parts));
}

bool raw_mesh_node_owner_t::reply_user_spot_create (
  const service_mailbox_record_t &request,
  const protocol::user_spot_create_reply_t &reply,
  std::optional<protocol::application_payload_t> application_reply)
{
    if (!request.correlation || reply.header.correlation != *request.correlation) {
        throw std::invalid_argument ("User Spot create reply correlation does not match");
    }
    if (reply.header.terminal_result != 0 && application_reply)
        throw std::invalid_argument ("failed User Spot create reply cannot carry a payload");
    detail::backend::raw_message_t parts{protocol::encode_user_spot_create_reply (
      reply.header.correlation, reply.header.terminal_result, reply.header.failure_code,
      reply.result, reply.spot_id, reply.object_generation)};
    if (application_reply) {
        parts.push_back (protocol::encode_application_payload (*application_reply));
    }
    const auto local = _topology.local_descriptor ();
    if (request.source_routing_id == local.node_routing_id) {
        return _operations->complete (
          operation_id (local.lifecycle_generation, *request.correlation),
          detail::backend::materialize_binding_part (protocol::pack_infrastructure_reply (parts)));
    }
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port)
        return false;
    return port->reply (
      detail::backend::raw_received_t{request.source_routing_id, request.reply_token, {}},
      std::move (parts));
}

bool raw_mesh_node_owner_t::reply_actor_create (
  const service_mailbox_record_t &request,
  const protocol::actor_create_reply_t &reply,
  std::optional<protocol::application_payload_t> application_reply)
{
    if (!request.correlation || reply.header.correlation != *request.correlation)
        throw std::invalid_argument ("Actor create reply correlation does not match");
    if (reply.header.terminal_result != 0 && application_reply)
        throw std::invalid_argument ("failed Actor create reply cannot carry a payload");
    detail::backend::raw_message_t parts{protocol::encode_actor_create_reply (
      reply.header.correlation, reply.header.terminal_result, reply.header.failure_code,
      reply.result, reply.node_routing_id, reply.actor_id, reply.object_generation)};
    if (application_reply)
        parts.push_back (protocol::encode_application_payload (*application_reply));
    const auto local = _topology.local_descriptor ();
    if (request.source_routing_id == local.node_routing_id)
        return _operations->complete (
          operation_id (local.lifecycle_generation, *request.correlation),
          detail::backend::materialize_binding_part (protocol::pack_infrastructure_reply (parts)));
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port)
        return false;
    return port->reply (
      detail::backend::raw_received_t{request.source_routing_id, request.reply_token, {}},
      std::move (parts));
}

bool raw_mesh_node_owner_t::reply_actor_join (
  const service_mailbox_record_t &request,
  const protocol::actor_join_result_t &join_result,
  const std::optional<protocol::actor_join_reply_spot_ref_t> &spot,
  std::uint64_t membership_epoch,
  std::uint32_t receive_chunk_limit_bytes,
  std::uint32_t terminal_result,
  std::uint32_t failure_code,
  std::optional<protocol::application_payload_t> application_reply)
{
    if (!request.correlation || request.source_routing_id.empty () || !request.reply_token)
        return false;
    if (terminal_result != 0 && application_reply) {
        throw std::invalid_argument ("failed Actor join reply cannot carry a payload");
    }
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port)
        return false;
    detail::backend::raw_message_t parts{protocol::encode_actor_join_reply (
      *request.correlation, terminal_result, failure_code, join_result, spot, membership_epoch,
      receive_chunk_limit_bytes)};
    if (application_reply) {
        parts.push_back (protocol::encode_application_payload (*application_reply));
    }
    return port->reply (
      detail::backend::raw_received_t{request.source_routing_id, request.reply_token, {}},
      std::move (parts));
}

bool raw_mesh_node_owner_t::reply_instance_spot_activation (
  const service_mailbox_record_t &request,
  std::uint32_t terminal_result,
  std::uint32_t failure_code,
  std::optional<protocol::application_payload_t> application_reply)
{
    if (!request.correlation) {
        return true;
    }
    if (terminal_result != 0 && application_reply) {
        throw std::invalid_argument (
          "failed Instance Spot activation reply cannot carry a payload");
    }
    const auto local = _topology.local_descriptor ();
    // Recovery has no native token. The original Core request owns its terminal;
    // replay cannot create a new reply leg after that connection was lost.
    if (request.source_routing_id != local.node_routing_id && !request.reply_token)
        return false;
    detail::backend::raw_message_t parts{
      protocol::encode_reply_header (*request.correlation, terminal_result, failure_code)};
    if (application_reply) {
        parts.push_back (protocol::encode_application_payload (*application_reply));
    }
    if (request.source_routing_id == local.node_routing_id) {
        return _operations->complete (
          operation_id (local.lifecycle_generation, *request.correlation),
          detail::backend::materialize_binding_part (protocol::pack_infrastructure_reply (parts)),
          {}, request_metric_terminal ({*request.correlation, terminal_result, failure_code}));
    }
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port)
        return false;
    return port->reply (
      detail::backend::raw_received_t{request.source_routing_id, request.reply_token, {}},
      std::move (parts));
}

bool raw_mesh_node_owner_t::reply_user_spot_close (const service_mailbox_record_t &request,
                                                   const protocol::user_spot_close_reply_t &reply)
{
    if (!request.correlation || reply.header.correlation != *request.correlation) {
        throw std::invalid_argument ("User Spot close reply correlation does not match");
    }
    return reply_infrastructure (request, protocol::encode_user_spot_close_reply (
                                            reply.header.correlation, reply.header.terminal_result,
                                            reply.header.failure_code, reply.closed));
}

void raw_mesh_node_owner_t::publish_peer_metrics (opentelemetry::metrics::ObserverResult result,
                                                  void *state)
{
    const auto &registration = *static_cast<peer_metric_registration_t *> (state);
    auto &owner = *registration.owner;
    if (registration.index == 3) {
        const auto closed = owner._lane.run ([&] { return owner._closed; }).get ();
        owner._topology.observe_channel_metrics (result, closed);
        return;
    }
    const auto count =
      owner._lane
        .run ([&] () -> std::size_t {
            if (registration.index == 1)
                return owner._closed ? 0 : owner._routes.size ();
            const auto peers = owner._topology.peers_on_lane ();
            if (registration.index == 2)
                return owner._closed
                         ? 0
                         : std::count_if (peers.begin (), peers.end (), [] (const auto &peer) {
                               return peer.ready
                                      && peer.descriptor.state == service_node_state_t::serving;
                           });
            std::size_t configured =
              owner._expected_peers.size ()
              - owner._expected_peers.count (owner._options.descriptor.node_routing_id);
            for (const auto &peer : peers)
                configured += !owner._expected_peers.contains (peer.descriptor.node_routing_id);
            for (const auto &peer : owner._topology.not_required_peers_on_lane ())
                configured += !owner._expected_peers.contains (peer.node_routing_id);
            return configured;
        })
        .get ();
    auto observer = opentelemetry::nostd::get<
      opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObserverResultT<double>>> (result);
    observer->Observe (static_cast<double> (count),
                       {{"mesh_name", owner._options.descriptor.mesh_name},
                        {"source", owner._options.metric_source}});
}

raw_mesh_pump_result_t
raw_mesh_node_owner_t::enqueue_received (service_mailbox_record_t record,
                                         raw_mesh_pump_result_t accepted_result)
{
    detail::message_flow_tracer_t flow (_options.dispatch);
    std::function<void ()> admitted;
    std::optional<message_flow_event_t> event;
    if (flow.enabled_for (message_flow_outcome_t::received)
        && record.domain == service_mailbox_domain_t::application && record.parts.size () >= 2) {
        const auto wire = protocol::decode_header (protocol::message_bytes (record.parts.front ()));
        if (wire.kind == protocol::command::channelSend
            || wire.kind == protocol::command::channelRequest) {
            const auto payload = protocol::decode_application_payload (
              protocol::message_bytes (record.parts[1]), true);
            if (payload.packet_name == protocol::framework_multipart_packet_name
                && payload.content_type == protocol::framework_multipart_content_type) {
                auto parts = protocol::decode_application_parts (payload);
                messaging::message_parts_t encoded (std::move (parts));
                auto decoded = messaging::envelope_codec_t{}.decode_header (encoded, true);
                if (decoded) {
                    auto &header = decoded.value ();
                    auto scope =
                      flow_context_t::enter (header.flow_id, header.flow_origin, flow.mode (),
                                             flow_origin_t::inbound, std::nullopt);
                    if (!header.flow_id && flow_context_t::current ()) {
                        header.flow_id = flow_context_t::current ()->flow_id;
                        header.flow_origin = flow_context_t::current ()->origin;
                        auto updated = std::move (encoded).take_items ();
                        updated[0] = messaging::envelope_codec_t{}.encode_header (header);
                        record.parts[1] =
                          zlink::message_t::from (protocol::encode_application_payload (
                            protocol::application_payload_t::from_parts (std::move (updated))));
                    }
                    event.emplace (message_flow_event_t{
                      message_flow_outcome_t::received,
                      dispatch_error_surface_t::route_mesh_channel,
                      wire.kind == protocol::command::channelRequest
                        ? dispatch_message_kind_t::request
                        : dispatch_message_kind_t::send,
                      header.message_name, header.channel_name, header.topic, header.correlation_id,
                      std::string (record.source_routing_id.begin (),
                                   record.source_routing_id.end ()),
                      std::nullopt, std::nullopt, std::nullopt});
                    event->flow_id = header.flow_id;
                    event->flow_origin = header.flow_origin;
                    event->mesh_name = _options.descriptor.mesh_name;
                    event->channel_route_kind = "route_mesh";
                    flow.trace (message_flow_outcome_t::received,
                                event->flow_id ? std::string_view (*event->flow_id)
                                               : std::string_view{},
                                [&] { return *event; });
                    admitted = [&] {
                        event->outcome = message_flow_outcome_t::admitted;
                        flow.trace (message_flow_outcome_t::admitted,
                                    event->flow_id ? std::string_view (*event->flow_id)
                                                   : std::string_view{},
                                    [&] { return *event; });
                    };
                }
            }
        }
    }
    if (_mailbox.try_enqueue (std::move (record), admitted))
        return accepted_result;
    return raw_mesh_pump_result_t::backpressured;
}

task_t<raw_mesh_pump_result_t>
raw_mesh_node_owner_t::pump_one (service_liveness_registry_t::clock_t::time_point now,
                                 bool accept_application_receive)
{
    _last_pump_bytes.store (0, std::memory_order_relaxed);
    const auto readiness = begin_receive_turn ();
    if (!readiness)
        co_return raw_mesh_pump_result_t::protocol_error;
    // The ROUTER carries every ordinary record, including control and
    // malformed input.  Without the host-wide supply permit none of those
    // records may be dequeued and classified after receive; terminal
    // reply/error completion progresses on its separate path.
    if (!accept_application_receive
        || (static_cast<short> (*readiness) & static_cast<short> (zlink::poll_event_flag_t::pollin))
             == 0)
        co_return raw_mesh_pump_result_t::no_data;
    co_return receive_next (now);
}

std::optional<zlink::poll_event_flag_t> raw_mesh_node_owner_t::begin_receive_turn ()
{
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port)
        return zlink::poll_event_flag_t::none;
    zlink::poll_event_flag_t readiness = zlink::poll_event_flag_t::none;
    try {
        readiness =
          port->poll (std::chrono::milliseconds::zero ()).value_or (zlink::poll_event_flag_t::none);
    }
    catch (...) {
        return std::nullopt;
    }
    // POLLROUTE stays ready until a snapshot sees every change; the host
    // wakes on it and this observer reads the snapshot (Core ROUTER §10.1).
    (void) apply_route_snapshot (readiness);
    return readiness;
}

raw_mesh_pump_result_t raw_mesh_node_owner_t::admit_descriptor_record (
  const std::vector<std::uint8_t> &source_routing_id,
  std::uint64_t route_generation,
  protocol::command kind,
  std::span<const std::uint8_t> bytes,
  service_liveness_registry_t::clock_t::time_point now)
{
    if (kind == protocol::command::hello && _options.shutdown_admission_seal
        && _options.shutdown_admission_seal->load (std::memory_order_acquire)) {
        return raw_mesh_pump_result_t::infrastructure;
    }
    const auto descriptor = protocol::decode_route_mesh_admission (bytes, kind, source_routing_id);
    const auto connection_id = route_connection_id (route_generation);
    struct admission_expectation_t
    {
        std::string intent_endpoint;
        std::optional<service_node_descriptor_t> expected_descriptor;
        bool expected_descriptor_mismatch = false;
    };
    const auto expectation =
      _lane
        .run ([this, &source_routing_id, &descriptor] {
            admission_expectation_t value;
            std::lock_guard lifecycle_lock (_lifecycle_mutex);
            const auto expected = _expected_peers.find (source_routing_id);
            if (expected == _expected_peers.end ())
                return value;
            value.intent_endpoint = expected->second.advertised_endpoint;
            value.expected_descriptor_mismatch =
              expected->second.mesh_name != descriptor.mesh_name
              || expected->second.node_routing_id != descriptor.node_routing_id
              || expected->second.advertised_endpoint != descriptor.advertised_endpoint
              || expected->second.security_identity != descriptor.security_identity
              || (expected->second.lifecycle_generation != 0
                  && expected->second.lifecycle_generation != descriptor.lifecycle_generation);
            if (expected->second.lifecycle_generation != 0)
                value.expected_descriptor = expected->second;
            return value;
        })
        .get ();
    if (expectation.expected_descriptor_mismatch) {
        log_admission_rejection (_options, "expected_route_mismatch", expectation.intent_endpoint,
                                 descriptor.advertised_endpoint);
        const auto submitted = submit_header_only (source_routing_id, protocol::encode_reject (3));
        return raw_mesh_pump_result_t::infrastructure;
    }
    std::vector<std::function<void ()>> notifications;
    peer_admission_result_t admission;
    try {
        admission =
          _lane
            .run ([this, &source_routing_id, &descriptor, &expectation, &connection_id, now, kind,
                   &notifications] {
                auto publication = _topology.admit_on_lane (
                  descriptor, connection_id,
                  expectation.expected_descriptor ? &*expectation.expected_descriptor : nullptr,
                  now, kind);
                if (publication.second)
                    notifications.push_back (std::move (publication.second));
                const auto result = publication.first;
                trace_admission_phase (source_routing_id, descriptor.lifecycle_generation, kind,
                                       result);
                refresh_admission_due_on_lane ();
                if (result == peer_admission_result_t::not_required
                    || result == peer_admission_result_t::admitted
                    || result == peer_admission_result_t::duplicate_connection) {
                    if (kind == protocol::command::hello) {
                        auto local = _topology.local_descriptor ();
                        const auto route = _routes.find (source_routing_id);
                        // The exchange owns the descriptor until the remote ADMIT;
                        // its catch-up publication sends later revisions as UPDATE.
                        if (route != _routes.end () && !route->second.hello.empty ())
                            local = protocol::decode_route_mesh_admission (
                              route->second.hello, protocol::command::hello, local.node_routing_id);
                        (void) submit_header_only_on_lane (
                          source_routing_id,
                          protocol::encode_route_mesh_admission (protocol::command::admit, local));
                    } else if (kind == protocol::command::admit) {
                        const auto route = _routes.find (source_routing_id);
                        if (route != _routes.end () && !route->second.hello.empty ()) {
                            auto hello = std::exchange (route->second.hello, {});
                            const auto local = _topology.local_descriptor ();
                            if (hello
                                != protocol::encode_route_mesh_admission (protocol::command::hello,
                                                                          local)) {
                                const auto peer = _topology.peer (source_routing_id);
                                if (peer)
                                    send_descriptor_update_on_lane (local, {*peer});
                            }
                        }
                    }
                }
                return result;
            })
            .get ();
    }
    catch (...) {
        std::rethrow_exception (
          deliver_topology_notifications (notifications, std::current_exception ()));
    }
    if (auto failure = deliver_topology_notifications (notifications))
        std::rethrow_exception (failure);
    if (admission == peer_admission_result_t::not_required) {
        if (kind != protocol::command::hello) {
            // NotRequired ends this configured connection intent; the
            // same manual configuration generation does not reconnect it.
            _lane
              .run ([this, &descriptor] {
                  std::lock_guard lifecycle_lock (_lifecycle_mutex);
                  try {
                      std::lock_guard socket_lock (*_socket_mutex);
                      if (_router)
                          _router->disconnect (descriptor.advertised_endpoint);
                  }
                  catch (...) {
                  }
              })
              .get ();
        }
        return raw_mesh_pump_result_t::infrastructure;
    }
    if (admission != peer_admission_result_t::admitted
        && admission != peer_admission_result_t::duplicate_connection) {
        log_admission_rejection (_options, admission_rejection_reason (admission),
                                 expectation.intent_endpoint, descriptor.advertised_endpoint);
        const auto reason = admission == peer_admission_result_t::mesh_mismatch      ? 2u
                            : admission == peer_admission_result_t::stale_descriptor ? 7u
                                                                                     : 11u;
        const auto submitted =
          submit_header_only (source_routing_id, protocol::encode_reject (reason));
        return raw_mesh_pump_result_t::infrastructure;
    }
    return raw_mesh_pump_result_t::infrastructure;
}

raw_mesh_pump_result_t
raw_mesh_node_owner_t::admit_control (const service_node_descriptor_t &local,
                                      const service_node_descriptor_t &actual_source,
                                      service_mailbox_record_t record)
{
    try {
        if (record.parts.empty () || record.reply_token
            || record.source_routing_id != actual_source.node_routing_id
            || record.source_node_generation != actual_source.lifecycle_generation)
            return raw_mesh_pump_result_t::protocol_error;
        const auto header =
          protocol::decode_header (protocol::message_bytes (record.parts.front ()));
        if (header.flags != 0)
            return raw_mesh_pump_result_t::protocol_error;
        switch (header.kind) {
            case protocol::command::replyRelay: {
                if (record.parts.size () > 2)
                    return raw_mesh_pump_result_t::protocol_error;
                const auto relay =
                  protocol::decode_reply_relay (protocol::message_bytes (record.parts.front ()));
                if (relay.terminal_result != 0 && record.parts.size () == 2)
                    return raw_mesh_pump_result_t::protocol_error;
                if (record.parts.size () == 2)
                    (void) protocol::decode_application_payload (
                      protocol::message_bytes (record.parts.back ()), false);
                break;
            }
            case protocol::command::replyRelayAck: {
                if (record.parts.size () != 1)
                    return raw_mesh_pump_result_t::protocol_error;
                const auto ack = protocol::decode_reply_relay_ack (
                  protocol::message_bytes (record.parts.front ()));
                if (ack.request_source.node_routing_id != actual_source.node_routing_id
                    || ack.request_source.node_generation != actual_source.lifecycle_generation)
                    return raw_mesh_pump_result_t::protocol_error;
                break;
            }
            case protocol::command::sessionRelocationRoute: {
                if (record.parts.size () != 1)
                    return raw_mesh_pump_result_t::protocol_error;
                const auto route = protocol::decode_session_relocation_route (
                  protocol::message_bytes (record.parts.front ()));
                const auto &expected_rid = route.sender_role == protocol::relocation_role_t::target
                                             ? route.route.target_node_routing_id
                                             : route.coordinator.node_routing_id;
                const auto expected_generation =
                  route.sender_role == protocol::relocation_role_t::target
                    ? route.route.target_node_generation
                    : route.coordinator.node_generation;
                if (route.session_owner_node_routing_id != local.node_routing_id
                    || route.session_owner_node_generation != local.lifecycle_generation
                    || expected_rid != actual_source.node_routing_id
                    || expected_generation != actual_source.lifecycle_generation)
                    return raw_mesh_pump_result_t::protocol_error;
                break;
            }
            default:
                return raw_mesh_pump_result_t::protocol_error;
        }
        return enqueue_received (std::move (record), raw_mesh_pump_result_t::infrastructure);
    }
    catch (const protocol::service_wire_error_t &) {
        return raw_mesh_pump_result_t::protocol_error;
    }
}

raw_mesh_pump_result_t
raw_mesh_node_owner_t::receive_next (service_liveness_registry_t::clock_t::time_point now)
{
    _last_pump_bytes.store (0, std::memory_order_relaxed);
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port)
        return raw_mesh_pump_result_t::no_data;
    // Readiness belongs to the receive turn; within it, the ROUTER is read
    // until it reports no record (08-messaging-hot-path I2).
    auto received = port->receive_if_ready (zlink::poll_event_flag_t::pollin);
    if (!received) {
        return raw_mesh_pump_result_t::no_data;
    }
    _last_pump_bytes.store (raw_received_bytes (*received), std::memory_order_relaxed);
    if (received->parts.empty ()) {
        return raw_mesh_pump_result_t::protocol_error;
    }
    try {
        const auto header =
          protocol::decode_header (protocol::message_bytes (received->parts.front ()));
        trace_mesh ("receive kind=" + std::to_string (static_cast<int> (header.kind))
                    + " parts=" + std::to_string (received->parts.size ()) + " replyToken="
                    + (received->reply_token ? std::string ("present") : std::string ("-")));
        const bool descriptor_record = header.kind == protocol::command::hello
                                       || header.kind == protocol::command::admit
                                       || header.kind == protocol::command::update;
        const bool handshake = (descriptor_record && header.kind != protocol::command::update)
                               || header.kind == protocol::command::reject;
        const auto admitted = _topology.peer (received->source_routing_id);
        if (!handshake
            && (!admitted
                || !detail::backend::is_route_connection (admitted->connection_id,
                                                          received->route_generation))) {
            if (mesh_trace_enabled ())
                trace_mesh ("record-drop reason=route-not-admitted kind="
                            + std::to_string (static_cast<int> (header.kind)));
            return raw_mesh_pump_result_t::infrastructure;
        }
        if (descriptor_record) {
            if (received->parts.size () != 1) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            return admit_descriptor_record (
              received->source_routing_id, received->route_generation, header.kind,
              protocol::message_bytes (received->parts.front ()), now);
        }
        if (header.kind == protocol::command::reject) {
            if (received->parts.size () != 1) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            (void) protocol::decode_reject (protocol::message_bytes (received->parts.front ()));
            std::vector<std::function<void ()>> notifications;
            try {
                _lane.run_sync ([this, &received, &notifications] {
                    const auto peer = _topology.peer (received->source_routing_id);
                    if (!peer)
                        return;
                    auto publication = _topology.disconnect_on_lane (received->source_routing_id,
                                                                     peer->connection_id);
                    if (publication.second)
                        notifications.push_back (std::move (publication.second));
                    (void) _liveness.disconnect_owned (received->source_routing_id,
                                                       peer->connection_id);
                    refresh_admission_due_on_lane ();
                });
            }
            catch (...) {
                std::rethrow_exception (
                  deliver_topology_notifications (notifications, std::current_exception ()));
            }
            if (auto failure = deliver_topology_notifications (notifications))
                std::rethrow_exception (failure);
            return raw_mesh_pump_result_t::infrastructure;
        }
        if (admitted->liveness)
            admitted->liveness->record_received (now);
        if (header.kind == protocol::command::livenessProbe
            || header.kind == protocol::command::livenessAck) {
            if (received->parts.size () != 1) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto record =
              protocol::decode_liveness (protocol::message_bytes (received->parts.front ()));
            std::array<std::uint8_t, sizeof (received->route_generation)> connection_id;
            detail::backend::encode_route_connection_id (received->route_generation, connection_id);
            if (record.kind == protocol::command::livenessProbe) {
                const auto ack = _lane.run_sync ([this, &received, &connection_id, &record] {
                    return _liveness.acknowledge_probe_owned (received->source_routing_id,
                                                              connection_id, record.probe_id);
                });
                if (!ack)
                    return raw_mesh_pump_result_t::protocol_error;
                const auto submitted = submit_header_only (
                  received->source_routing_id,
                  protocol::encode_liveness (protocol::command::livenessAck, record.probe_id));
                if (submitted == send_start_state_t::terminated)
                    return raw_mesh_pump_result_t::protocol_error;
            } else {
                (void) _lane.run_sync ([this, &received, &connection_id, &record, now] {
                    return _liveness.acknowledge_owned (received->source_routing_id, connection_id,
                                                        record.probe_id, now);
                });
            }
            return raw_mesh_pump_result_t::infrastructure;
        }
        if (header.kind == protocol::command::messageFollow) {
            if (header.flags != 0 || received->parts.size () != 1 || received->reply_token) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto notice =
              protocol::decode_message_follow (protocol::message_bytes (received->parts.front ()));
            const auto source_matches_peer = std::visit (
              [&received, &admitted] (const auto &source) {
                  return source.target_node_routing_id == received->source_routing_id
                         && source.target_node_generation
                              == admitted->descriptor.lifecycle_generation;
              },
              notice.source);
            const auto same_object = [&notice] {
                if (const auto *source =
                      std::get_if<protocol::actor_route_fence_t> (&notice.source)) {
                    const auto *target =
                      std::get_if<protocol::actor_route_fence_t> (&notice.target);
                    return target != nullptr && source->actor_id == target->actor_id
                           && source->object_generation == target->object_generation;
                }
                const auto *source = std::get_if<protocol::spot_route_fence_t> (&notice.source);
                const auto *target = std::get_if<protocol::spot_route_fence_t> (&notice.target);
                return target != nullptr && source != nullptr && source->spot_id == target->spot_id
                       && source->object_generation == target->object_generation;
            }();
            const auto authority_generation_increases = std::visit (
              [] (const auto &source, const auto &target) {
                  using source_type = std::decay_t<decltype (source)>;
                  using target_type = std::decay_t<decltype (target)>;
                  if constexpr (!std::is_same_v<source_type, target_type>) {
                      return false;
                  } else {
                      return target.authority_owner_generation > source.authority_owner_generation;
                  }
              },
              notice.source, notice.target);
            const auto local = _topology.local_descriptor ();
            const auto target_matches_topology = std::visit (
              [&local, this] (const auto &target) {
                  if (target.target_node_routing_id == local.node_routing_id) {
                      return target.target_node_generation == local.lifecycle_generation;
                  }
                  const auto admitted_target = _topology.peer (target.target_node_routing_id);
                  return admitted_target
                         && target.target_node_generation
                              == admitted_target->descriptor.lifecycle_generation;
              },
              notice.target);
            if (!source_matches_peer || !same_object || !authority_generation_increases
                || !target_matches_topology) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            return enqueue_received (
              service_mailbox_record_t{
                owner_key (local.node_routing_id), service_mailbox_domain_t::infrastructure,
                std::move (received->parts), std::move (received->source_routing_id), std::nullopt,
                std::nullopt, admitted->descriptor.lifecycle_generation,
                std::pair{notice.original_operation.high, notice.original_operation.low}},
              raw_mesh_pump_result_t::infrastructure);
        }
        if (header.kind == protocol::command::boundSessionSend) {
            if (header.flags != 0 || received->parts.size () != 2 || received->reply_token) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto send = protocol::decode_bound_session_send (
              protocol::message_bytes (received->parts.front ()));
            if (send.actor.target_node_routing_id != received->source_routing_id
                || send.actor.target_node_routing_id != admitted->descriptor.node_routing_id
                || send.actor.target_node_generation != admitted->descriptor.lifecycle_generation) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            (void) protocol::decode_application_payload (
              protocol::message_bytes (received->parts.back ()), false);
            return enqueue_received (
              service_mailbox_record_t{
                "bound-session:" + send.actor.actor_id, service_mailbox_domain_t::application,
                std::move (received->parts), std::move (received->source_routing_id), std::nullopt,
                std::nullopt, admitted->descriptor.lifecycle_generation},
              raw_mesh_pump_result_t::application);
        }
        if (header.kind == protocol::command::boundSessionBind) {
            if (header.flags != 0 || received->parts.size () != 1 || !received->reply_token) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto bind = protocol::decode_bound_session_bind (
              protocol::message_bytes (received->parts.front ()));
            const auto local = _topology.local_descriptor ();
            if (bind.actor.target_node_routing_id != local.node_routing_id
                || bind.actor.target_node_generation != local.lifecycle_generation) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            return enqueue_received (
              service_mailbox_record_t{
                owner_key (local.node_routing_id), service_mailbox_domain_t::infrastructure,
                std::move (received->parts), std::move (received->source_routing_id),
                received->reply_token, bind.correlation, admitted->descriptor.lifecycle_generation},
              raw_mesh_pump_result_t::infrastructure);
        }
        if (header.kind == protocol::command::boundSessionClose) {
            if (header.flags != 0 || received->parts.size () != 1 || !received->reply_token)
                return raw_mesh_pump_result_t::protocol_error;
            const auto close = protocol::decode_bound_session_close (
              protocol::message_bytes (received->parts.front ()));
            const auto local = _topology.local_descriptor ();
            return enqueue_received (
              service_mailbox_record_t{
                owner_key (local.node_routing_id), service_mailbox_domain_t::infrastructure,
                std::move (received->parts), std::move (received->source_routing_id),
                received->reply_token, close.correlation,
                admitted->descriptor.lifecycle_generation},
              raw_mesh_pump_result_t::infrastructure);
        }
        if (header.kind == protocol::command::boundSessionReplaced) {
            if (header.flags != 0 || received->parts.size () != 1 || received->reply_token) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto replacement = protocol::decode_bound_session_replaced (
              protocol::message_bytes (received->parts.front ()));
            const auto local = _topology.local_descriptor ();
            if (replacement.actor_authority.target_node_routing_id != received->source_routing_id
                || replacement.actor_authority.target_node_routing_id
                     != admitted->descriptor.node_routing_id
                || replacement.actor_authority.target_node_generation
                     != admitted->descriptor.lifecycle_generation
                || replacement.retired_session.session_owner_node_routing_id
                     != local.node_routing_id
                || replacement.retired_session.session_owner_node_generation
                     != local.lifecycle_generation) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            return enqueue_received (
              service_mailbox_record_t{
                owner_key (local.node_routing_id), service_mailbox_domain_t::infrastructure,
                std::move (received->parts), std::move (received->source_routing_id), std::nullopt,
                std::nullopt, admitted->descriptor.lifecycle_generation},
              raw_mesh_pump_result_t::infrastructure);
        }
        if (header.kind == protocol::command::instanceSpot) {
            const auto activation = protocol::decode_instance_spot_activation_header (
              protocol::message_bytes (received->parts.front ()));
            const auto expected_parts = activation.has_metadata ? 3u : 2u;
            if (received->parts.size () != expected_parts
                || activation.source_node_routing_id != received->source_routing_id
                || activation.source_node_routing_id != admitted->descriptor.node_routing_id
                || activation.source_node_generation != admitted->descriptor.lifecycle_generation) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto local = _topology.local_descriptor ();
            if (activation.target.target_node_routing_id != local.node_routing_id
                || activation.target.target_node_generation != local.lifecycle_generation
                || (activation.request != received->reply_token.has_value ())
                || (!activation.request && activation.reply_route_id != 0)) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            (void) protocol::decode_application_payload (
              protocol::message_bytes (received->parts.back ()), false);
            return enqueue_received (
              service_mailbox_record_t{
                owner_key (local.node_routing_id), service_mailbox_domain_t::infrastructure,
                std::move (received->parts), std::move (received->source_routing_id),
                received->reply_token,
                activation.request ? std::make_optional (activation.reply_route_id) : std::nullopt},
              raw_mesh_pump_result_t::infrastructure);
        }
        if (header.kind == protocol::command::actorCreate
            || header.kind == protocol::command::userSpotCreate
            || header.kind == protocol::command::userSpotClose) {
            if (header.flags != 0 || received->parts.size () != 1 || !received->reply_token) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto local = _topology.local_descriptor ();
            std::vector<std::uint8_t> source_node_routing_id;
            std::uint64_t source_node_generation = 0;
            std::vector<std::uint8_t> target_node_routing_id;
            std::uint64_t target_node_generation = 0;
            std::uint64_t correlation = 0;
            if (header.kind == protocol::command::actorCreate) {
                const auto create = protocol::decode_actor_create_header (
                  protocol::message_bytes (received->parts.front ()));
                source_node_routing_id = create.source_node_routing_id;
                source_node_generation = create.source_node_generation;
                target_node_routing_id = create.reservation.target_node_routing_id;
                target_node_generation = create.reservation.target_node_generation;
                correlation = create.correlation;
            } else if (header.kind == protocol::command::userSpotCreate) {
                const auto create = protocol::decode_user_spot_create_header (
                  protocol::message_bytes (received->parts.front ()));
                source_node_routing_id = create.source_node_routing_id;
                source_node_generation = create.source_node_generation;
                target_node_routing_id = create.reservation.target_node_routing_id;
                target_node_generation = create.reservation.target_node_generation;
                correlation = create.correlation;
            } else {
                const auto close = protocol::decode_user_spot_close_header (
                  protocol::message_bytes (received->parts.front ()));
                source_node_routing_id = close.source_node_routing_id;
                source_node_generation = close.source_node_generation;
                target_node_routing_id = close.target.target_node_routing_id;
                target_node_generation = close.target.target_node_generation;
                correlation = close.correlation;
            }
            if (source_node_routing_id != received->source_routing_id
                || source_node_routing_id != admitted->descriptor.node_routing_id
                || source_node_generation != admitted->descriptor.lifecycle_generation
                || target_node_routing_id != local.node_routing_id
                || target_node_generation != local.lifecycle_generation) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            return enqueue_received (
              service_mailbox_record_t{
                owner_key (local.node_routing_id), service_mailbox_domain_t::infrastructure,
                std::move (received->parts), std::move (received->source_routing_id),
                received->reply_token, correlation},
              raw_mesh_pump_result_t::infrastructure);
        }
        if (header.kind == protocol::command::actorJoin) {
            if (header.flags != 0 || received->parts.empty () || received->parts.size () > 2
                || !received->reply_token) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto decoded_canonical = [&] {
                try {
                    return protocol::decode_actor_join_28 (
                      detail::backend::copy_binding_parts (received->parts));
                }
                catch (const std::invalid_argument &) {
                    throw protocol::service_wire_error_t ("invalid Actor join request");
                }
            }();
            const protocol::actor_join_request_t request{
              decoded_canonical.correlation,
              {decoded_canonical.actor.id, decoded_canonical.actor.generation,
               decoded_canonical.actor.target_node_rid,
               decoded_canonical.actor.target_node_generation,
               decoded_canonical.actor.expected_authority_owner_generation,
               decoded_canonical.actor.expected_owner_lease_generation},
              decoded_canonical.entry,
              {decoded_canonical.target_spot.id, decoded_canonical.target_spot.generation,
               decoded_canonical.target_spot.target_node_rid,
               decoded_canonical.target_spot.target_node_generation,
               decoded_canonical.target_spot.expected_authority_owner_generation,
               decoded_canonical.target_spot.expected_owner_lease_generation}};
            const auto local = _topology.local_descriptor ();
            // Transport owns only the authenticated peer and its exact node
            // execution generation. The canonical target Spot/Authority
            // fences deliberately travel untouched to the target admission;
            // that Store-backed boundary is their single owner.
            if (request.actor.target_node_routing_id != received->source_routing_id
                || request.actor.target_node_routing_id != admitted->descriptor.node_routing_id
                || request.actor.target_node_generation
                     != admitted->descriptor.lifecycle_generation) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            return enqueue_received (
              service_mailbox_record_t{
                owner_key (local.node_routing_id), service_mailbox_domain_t::infrastructure,
                std::move (received->parts), std::move (received->source_routing_id),
                received->reply_token, request.correlation},
              raw_mesh_pump_result_t::infrastructure);
        }
        if (header.kind == protocol::command::relocationPrepare
            || header.kind == protocol::command::relocationData
            || header.kind == protocol::command::relocationCutover
            || header.kind == protocol::command::relocationState) {
            if (received->parts.empty ()
                || (received->parts.size () != 1
                    && header.kind != protocol::command::relocationPrepare)
                || (received->reply_token && header.kind != protocol::command::relocationPrepare)) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto control = protocol::decode_relocation_control (
              protocol::message_bytes (received->parts.front ()));
            const auto source_fence = std::visit (
              [] (const auto &value)
                -> std::optional<std::pair<std::vector<std::uint8_t>, std::uint64_t>> {
                  using record_t = std::decay_t<decltype (value)>;
                  if constexpr (std::is_same_v<record_t, protocol::relocation_prepare_t>) {
                      if (value.initiator_role == protocol::relocation_role_t::source)
                          return std::pair{value.source_node_routing_id,
                                           value.source_node_generation};
                      if (value.initiator_role == protocol::relocation_role_t::target)
                          return std::pair{value.target.target_node_routing_id,
                                           value.target.target_node_generation};
                      return std::pair{value.coordinator.node_routing_id,
                                       value.coordinator.node_generation};
                  } else if constexpr (std::is_same_v<record_t, protocol::relocation_data_t>) {
                      // The record source is the original request source, not
                      // the relocation transport peer. The registered target
                      // validates the exact relocation source peer.
                      return std::nullopt;
                  } else {
                      if (value.sender_role == protocol::relocation_role_t::coordinator)
                          return std::pair{value.coordinator.node_routing_id,
                                           value.coordinator.node_generation};
                      return std::nullopt;
                  }
              },
              control);
            if (source_fence
                && (source_fence->first != received->source_routing_id
                    || source_fence->first != admitted->descriptor.node_routing_id
                    || source_fence->second != admitted->descriptor.lifecycle_generation)) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto local = _topology.local_descriptor ();
            return enqueue_received (
              service_mailbox_record_t{
                owner_key (local.node_routing_id), service_mailbox_domain_t::infrastructure,
                std::move (received->parts), std::move (received->source_routing_id),
                received->reply_token, std::nullopt, admitted->descriptor.lifecycle_generation},
              raw_mesh_pump_result_t::infrastructure);
        }
        if (header.kind == protocol::command::replyRelay
            || header.kind == protocol::command::replyRelayAck
            || header.kind == protocol::command::sessionRelocationRoute) {
            const auto local = _topology.local_descriptor ();
            return admit_control (
              local, admitted->descriptor,
              service_mailbox_record_t{
                owner_key (local.node_routing_id), service_mailbox_domain_t::infrastructure,
                std::move (received->parts), std::move (received->source_routing_id),
                received->reply_token, std::nullopt, admitted->descriptor.lifecycle_generation});
        }
        if (header.kind == protocol::command::sessionRelocationSeal
            || header.kind == protocol::command::sessionRelocationSealed) {
            if (header.flags != 0 || received->parts.size () != 1 || received->reply_token) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto local = _topology.local_descriptor ();
            if (header.kind == protocol::command::sessionRelocationSeal) {
                const auto seal = protocol::decode_session_relocation_seal (
                  protocol::message_bytes (received->parts.front ()));
                const auto expected_source_rid =
                  seal.sender_role == protocol::relocation_role_t::source
                    ? seal.actor.target_node_routing_id
                    : seal.coordinator.node_routing_id;
                const auto expected_source_generation =
                  seal.sender_role == protocol::relocation_role_t::source
                    ? seal.actor.target_node_generation
                    : seal.coordinator.node_generation;
                if (seal.session_owner_node_routing_id != local.node_routing_id
                    || seal.session_owner_node_generation != local.lifecycle_generation
                    || expected_source_rid != received->source_routing_id
                    || expected_source_rid != admitted->descriptor.node_routing_id
                    || expected_source_generation != admitted->descriptor.lifecycle_generation) {
                    return raw_mesh_pump_result_t::protocol_error;
                }
                return enqueue_received (
                  service_mailbox_record_t{
                    owner_key (local.node_routing_id), service_mailbox_domain_t::infrastructure,
                    std::move (received->parts), std::move (received->source_routing_id),
                    std::nullopt, std::nullopt, admitted->descriptor.lifecycle_generation},
                  raw_mesh_pump_result_t::infrastructure);
            }
            if (header.kind == protocol::command::sessionRelocationSealed) {
                const auto sealed = protocol::decode_session_relocation_sealed (
                  protocol::message_bytes (received->parts.front ()));
                if (sealed.session_owner_node_routing_id != received->source_routing_id
                    || sealed.session_owner_node_routing_id != admitted->descriptor.node_routing_id
                    || sealed.session_owner_node_generation
                         != admitted->descriptor.lifecycle_generation) {
                    return raw_mesh_pump_result_t::protocol_error;
                }
                const auto operation = operation_id (sealed.relocation.high, sealed.relocation.low);
                if (!_operations->complete (operation, received->parts.front ().copy ())) {
                    return raw_mesh_pump_result_t::protocol_error;
                }
                return raw_mesh_pump_result_t::infrastructure;
            }
        }
        if (header.kind == protocol::command::logicalMulticast) {
            namespace wire = protocol::generated::detail;
            wire::logicalMulticast_23_t multicast;
            wire::reader_t reader (protocol::message_bytes (received->parts.front ()));
            if (wire::decode_head_logicalMulticast_23 (reader, multicast) != wire::error_code::ok
                || received->parts.size ()
                     != ((multicast.flags & static_cast<std::uint8_t> (protocol::flag::metadata))
                             == 0
                           ? 2u
                           : 3u))
                return raw_mesh_pump_result_t::protocol_error;
            if ((multicast.flags & static_cast<std::uint8_t> (protocol::flag::metadata)) != 0) {
                wire::metadata_frame_t metadata;
                wire::reader_t metadata_reader (protocol::message_bytes (received->parts[1]));
                if (wire::decode_value_metadata_frame (metadata_reader, metadata)
                      != wire::error_code::ok
                    || !metadata_reader.empty ())
                    return raw_mesh_pump_result_t::protocol_error;
            }
            (void) protocol::decode_application_payload (
              protocol::message_bytes (received->parts.back ()), false);
            return enqueue_received (
              service_mailbox_record_t{
                service_mailbox_t::application_owner (host::owner_kind_t::node),
                service_mailbox_domain_t::application, std::move (received->parts),
                std::move (received->source_routing_id), received->reply_token, std::nullopt,
                admitted->descriptor.lifecycle_generation},
              raw_mesh_pump_result_t::application);
        }
        if ((header.kind != protocol::command::nodeSend
             && header.kind != protocol::command::nodeRequest
             && header.kind != protocol::command::channelSend
             && header.kind != protocol::command::channelRequest
             && header.kind != protocol::command::spotSend
             && header.kind != protocol::command::spotRequest
             && header.kind != protocol::command::actorSend
             && header.kind != protocol::command::actorRequest)
            || received->parts.size () != 2) {
            trace_mesh ("application-drop reason=invalid-application-shape kind="
                        + std::to_string (static_cast<int> (header.kind))
                        + " parts=" + std::to_string (received->parts.size ()));
            return raw_mesh_pump_result_t::protocol_error;
        }
        std::string mailbox_owner;
        std::optional<std::uint64_t> correlation;
        std::optional<std::pair<std::uint64_t, std::uint64_t>> operation;
        std::optional<service_bound_session_source_t> bound_session_source;
        if (header.kind == protocol::command::nodeSend
            || header.kind == protocol::command::nodeRequest) {
            if (header.kind == protocol::command::nodeSend
                && received->parts.front ().size () != 5) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            if (header.kind == protocol::command::nodeRequest) {
                if (!received->reply_token) {
                    return raw_mesh_pump_result_t::protocol_error;
                }
                correlation = protocol::decode_node_request_header (
                  protocol::message_bytes (received->parts.front ()));
            }
            mailbox_owner = service_mailbox_t::application_owner (host::owner_kind_t::node);
        } else if (header.kind == protocol::command::channelSend
                   || header.kind == protocol::command::channelRequest) {
            std::string channel_name;
            if (header.kind == protocol::command::channelSend) {
                channel_name = protocol::decode_channel_send_header (
                  protocol::message_bytes (received->parts.front ()));
            } else {
                if (!received->reply_token) {
                    return raw_mesh_pump_result_t::protocol_error;
                }
                auto channel_request = protocol::decode_channel_request_header (
                  protocol::message_bytes (received->parts.front ()));
                correlation = channel_request.correlation;
                channel_name = std::move (channel_request.channel_name);
            }
            const auto local = _topology.local_descriptor ();
            if (!find_service_channel (local, channel_name)) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            mailbox_owner =
              service_mailbox_t::application_owner (host::owner_kind_t::channel, channel_name);
        } else if (header.kind == protocol::command::spotSend
                   || header.kind == protocol::command::spotRequest) {
            if (header.kind == protocol::command::spotRequest && !received->reply_token) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto spot = protocol::decode_spot_message_header (
              protocol::message_bytes (received->parts.front ()), header.kind);
            const auto local = _topology.local_descriptor ();
            if (spot.target.target_node_routing_id != local.node_routing_id
                || spot.target.target_node_generation != local.lifecycle_generation) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            correlation = spot.correlation;
            operation = std::pair{spot.operation.high, spot.operation.low};
            mailbox_owner =
              service_mailbox_t::application_owner (host::owner_kind_t::spot, spot.target.spot_id);
        } else {
            if (header.kind == protocol::command::actorRequest && !received->reply_token) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            auto actor = protocol::decode_actor_message_header (
              protocol::message_bytes (received->parts.front ()), header.kind);
            const auto local = _topology.local_descriptor ();
            if (actor.target.target_node_routing_id != local.node_routing_id
                || actor.target.target_node_generation != local.lifecycle_generation) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            correlation = actor.correlation;
            operation = std::pair{actor.operation.high, actor.operation.low};
            if (actor.bound_session_source) {
                bound_session_source = service_bound_session_source_t{
                  std::move (actor.bound_session_source->session_routing_id),
                  actor.bound_session_source->binding_generation,
                  actor.bound_session_source->session_sequence};
            }
            mailbox_owner = service_mailbox_t::application_owner (host::owner_kind_t::actor,
                                                                  actor.target.actor_id);
        }
        service_mailbox_record_t record{std::move (mailbox_owner),
                                        service_mailbox_domain_t::application,
                                        std::move (received->parts),
                                        std::move (received->source_routing_id),
                                        received->reply_token,
                                        correlation,
                                        admitted->descriptor.lifecycle_generation,
                                        operation,
                                        std::move (bound_session_source)};
        // The receive turn only observes the polling list; a missing source is read by
        // the authority owner after the canonical claim has recorded its shared lookup.
        if (operation && _request_sources)
            if (auto source = _request_sources->cached_request_source (
                  _options.descriptor.mesh_name, record.source_routing_id,
                  record.source_node_generation))
                record.source_original = request_source_t{std::move (*source)};
        auto result = enqueue_received (std::move (record), raw_mesh_pump_result_t::application);
        trace_mesh (
          "application-enqueue result=" + std::to_string (static_cast<int> (result)) + " pending="
          + std::to_string (_mailbox.pending_messages (service_mailbox_domain_t::application)));
        return result;
    }
    catch (const protocol::service_wire_error_t &) {
        return raw_mesh_pump_result_t::protocol_error;
    }
}

bool raw_mesh_node_owner_t::wait_for_activity (std::chrono::milliseconds timeout,
                                               bool accept_application_receive) noexcept
{
    if (_mailbox.pending_messages (service_mailbox_domain_t::infrastructure) != 0
        || (accept_application_receive && !_mailbox.has_application_dispatch ()
            && _mailbox.pending_messages (service_mailbox_domain_t::application) != 0))
        return true;

    std::shared_ptr<detail::backend::raw_route_port_t> port;
    try {
        auto next_activity = _liveness.next_activity ();
        if (next_activity) {
            const auto now = foundation::operation_registry_t::clock_t::now ();
            const auto remaining =
              *next_activity <= now
                ? std::chrono::milliseconds::zero ()
                : std::chrono::ceil<std::chrono::milliseconds> (*next_activity - now);
            if (timeout < std::chrono::milliseconds::zero () || remaining < timeout)
                timeout = remaining;
        }
        {
            std::lock_guard lock (_lifecycle_mutex);
            port = _port;
        }
    }
    catch (...) {
        return false;
    }
    if (!port)
        return false;
    try {
        return port->poll (timeout, accept_application_receive).has_value ();
    }
    catch (...) {
        return false;
    }
}

void raw_mesh_node_owner_t::signal_activity () noexcept
{
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    try {
        std::lock_guard lock (_lifecycle_mutex);
        port = _port;
    }
    catch (...) {
        return;
    }
    if (port)
        port->signal_activity ();
}

task_t<std::optional<std::uint64_t>> raw_mesh_node_owner_t::next_operation_sequence_async ()
{
    return _lane.run_task ([this] { return next_operation_sequence_on_lane (); });
}

std::optional<std::uint64_t> raw_mesh_node_owner_t::next_operation_sequence_on_lane ()
{
    std::lock_guard lifecycle_lock (_lifecycle_mutex);
    if (!_port)
        return std::nullopt;
    return take_operation_sequence_locked ();
}


std::uint64_t raw_mesh_node_owner_t::take_operation_sequence_locked ()
{
    if (_next_operation_sequence == 0)
        throw std::overflow_error ("raw mesh operation sequence is exhausted");
    const auto sequence = _next_operation_sequence;
    _next_operation_sequence =
      sequence == std::numeric_limits<std::uint64_t>::max () ? 0 : sequence + 1;
    return sequence;
}

std::size_t raw_mesh_node_owner_t::last_pump_bytes () const
{
    return _last_pump_bytes.load (std::memory_order_relaxed);
}

std::uint64_t
raw_mesh_node_owner_t::take_reply_route_id_locked (std::optional<std::uint64_t> requested)
{
    if (_next_reply_route_id == 0) {
        throw std::overflow_error ("raw mesh reply route id is exhausted");
    }
    if (requested && (*requested == 0 || *requested < _next_reply_route_id)) {
        throw std::invalid_argument (
          "raw mesh reply route id must be unique in the current lifecycle");
    }
    const auto reply_route_id = requested.value_or (_next_reply_route_id);
    _next_reply_route_id =
      reply_route_id == std::numeric_limits<std::uint64_t>::max () ? 0 : reply_route_id + 1;
    return reply_route_id;
}

bool raw_mesh_node_owner_t::route_current_locked (const std::vector<std::uint8_t> &node_routing_id,
                                                  std::uint64_t route_generation) const
{
    const auto route = _routes.find (node_routing_id);
    return route != _routes.end () && route->second.generation == route_generation;
}

raw_mesh_node_owner_t::node_target_t raw_mesh_node_owner_t::classify_node_target (
  const std::vector<std::uint8_t> &target_routing_id) const
{
    // An Object Client is never a Node direct application target, even when
    // a channel connection admitted it (02-channel-messaging.ko.md section 8).
    if (const auto peer = _topology.peer (target_routing_id))
        return peer->descriptor.object_role == service_object_role_t::client
                 ? node_target_t::object_client
                 : node_target_t::application;
    for (const auto &descriptor : _topology.not_required_peers ()) {
        if (descriptor.node_routing_id == target_routing_id)
            return node_target_t::object_client;
    }
    return node_target_t::unknown;
}

std::pair<std::uint64_t, bool>
raw_mesh_node_owner_t::peer_route (const std::vector<std::uint8_t> &rid,
                                   std::optional<std::uint64_t> lifecycle_generation) const
{
    return _lane
      .run ([this, &rid, lifecycle_generation] {
          return peer_route_on_lane (rid, lifecycle_generation);
      })
      .get ();
}

std::pair<std::uint64_t, bool>
raw_mesh_node_owner_t::peer_route_on_lane (const std::vector<std::uint8_t> &rid,
                                           std::optional<std::uint64_t> lifecycle_generation) const
{
    const auto selected = _routes.find (rid);
    if (selected == _routes.end ())
        return std::pair<std::uint64_t, bool>{0, false};
    const auto peer = _topology.peer (rid);
    return std::pair{
      selected->second.generation,
      peer && peer->ready
        && (!lifecycle_generation || peer->descriptor.lifecycle_generation == *lifecycle_generation)
        && detail::backend::is_route_connection (peer->connection_id, selected->second.generation)};
}

std::size_t raw_mesh_node_owner_t::observe_routes ()
{
    // Core applies route changes while the socket is polled; poll through the
    // ingress poller (zero timeout) before reading the snapshot.
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port)
        return 0;
    const auto readiness =
      port->poll (std::chrono::milliseconds::zero ()).value_or (zlink::poll_event_flag_t::none);
    return apply_route_snapshot (readiness);
}

std::size_t raw_mesh_node_owner_t::apply_route_snapshot (zlink::poll_event_flag_t readiness)
{
    const auto ready = [&] (zlink::poll_event_flag_t flag) {
        return (static_cast<short> (readiness) & static_cast<short> (flag)) != 0;
    };
    if (!ready (zlink::poll_event_flag_t::pollroute)
        && !ready (zlink::poll_event_flag_t::pollconnect))
        return 0;
    std::vector<std::function<void ()>> notifications;
    try {
        const auto result =
          _lane
            .run ([this, &notifications, &ready] {
                std::size_t changes = 0;
                {
                    if (!_router)
                        return changes;
                    if (ready (zlink::poll_event_flag_t::pollconnect)) {
                        std::vector<zlink::connection_attempt_t> attempts;
                        {
                            std::lock_guard socket_lock (*_socket_mutex);
                            attempts = _router->connection_attempts_snapshot ();
                        }
                        notifications.push_back (
                          _topology.publish_connection_attempts_on_lane (std::move (attempts)));
                        ++changes;
                    }
                    if (!ready (zlink::poll_event_flag_t::pollroute))
                        return changes;
                    std::vector<zlink::router_route_t> snapshot;
                    {
                        std::lock_guard socket_lock (*_socket_mutex);
                        snapshot = _router->routes_snapshot ();
                    }
                    decltype (_routes) observed;
                    for (const auto &route : snapshot) {
                        const auto rid = route.routing_id.to_bytes ();
                        auto previous = _routes.find (rid);
                        observed.emplace (rid, previous != _routes.end ()
                                                   && previous->second.generation
                                                        == route.route_generation
                                                 ? std::move (previous->second)
                                                 : selected_route_t{route.route_generation, {}});
                    }
                    // A route that ended or was replaced loses its admission and
                    // liveness; a new route admits only through a new handshake.
                    for (const auto &[node_routing_id, route] : _routes) {
                        const auto still = observed.find (node_routing_id);
                        if (still != observed.end ()
                            && still->second.generation == route.generation)
                            continue;
                        ++changes;
                        const auto connection_id = route_connection_id (route.generation);
                        trace_mesh ("route-ended routing=" + owner_key (node_routing_id)
                                    + " connection=" + owner_key (connection_id));
                        auto publication =
                          _topology.disconnect_on_lane (node_routing_id, connection_id);
                        if (publication.second)
                            notifications.push_back (std::move (publication.second));
                        const auto removed = publication.first;
                        (void) _liveness.disconnect_owned (node_routing_id, connection_id);
                        if (removed)
                            end_peer_operations_if_disconnected_locked (node_routing_id);
                    }
                    for (const auto &[node_routing_id, route] : observed) {
                        if (route_current_locked (node_routing_id, route.generation))
                            continue;
                        if (!_routes.contains (node_routing_id))
                            ++changes;
                        trace_mesh ("route-selected routing=" + owner_key (node_routing_id)
                                    + " connection="
                                    + owner_key (route_connection_id (route.generation)));
                    }
                    _routes = std::move (observed);
                }
                if (changes != 0)
                    submit_pending_hellos_on_lane ();
                return changes;
            })
            .get ();
        if (auto failure = deliver_topology_notifications (notifications))
            std::rethrow_exception (failure);
        return result;
    }
    catch (...) {
        std::rethrow_exception (
          deliver_topology_notifications (notifications, std::current_exception ()));
    }
}

bool raw_mesh_node_owner_t::route_needs_hello_on_lane (const std::vector<std::uint8_t> &rid,
                                                       std::uint64_t generation) const
{
    if (_options.shutdown_admission_seal
        && _options.shutdown_admission_seal->load (std::memory_order_acquire))
        return false;
    const auto peer = _topology.peer (rid);
    return !peer || !peer->ready
           || !detail::backend::is_route_connection (peer->connection_id, generation);
}

void raw_mesh_node_owner_t::refresh_admission_due_on_lane ()
{
    _liveness.set_admission_pending_owned (
      std::any_of (_routes.begin (), _routes.end (), [this] (const auto &route) {
          return route_needs_hello_on_lane (route.first, route.second.generation);
      }));
}

void raw_mesh_node_owner_t::submit_pending_hellos_on_lane ()
{
    bool pending = false;
    for (auto &[rid, route] : _routes) {
        if (!route_needs_hello_on_lane (rid, route.generation))
            continue;
        pending = true;
        if (route.hello.empty ())
            route.hello = protocol::encode_route_mesh_admission (protocol::command::hello,
                                                                 _topology.local_descriptor ());
        (void) submit_header_only_on_lane (rid, route.hello);
    }
    _liveness.set_admission_pending_owned (pending);
}

task_t<service_liveness_tick_t>
raw_mesh_node_owner_t::tick_liveness (service_liveness_registry_t::clock_t::time_point now)
{
    if (!_liveness.due (now))
        return task_t<service_liveness_tick_t> (
          result_t<service_liveness_tick_t>::success (service_liveness_tick_t{}));
    return tick_due_liveness (now);
}

task_t<service_liveness_tick_t>
raw_mesh_node_owner_t::tick_due_liveness (service_liveness_registry_t::clock_t::time_point now)
{
    std::vector<std::function<void ()>> notifications;
    std::vector<std::exception_ptr> failures;
    try {
        const auto [result, port] =
          _lane
            .run ([this, now, &notifications, &failures] {
                auto tick = _liveness.tick_owned (now);
                std::shared_ptr<detail::backend::raw_route_port_t> port;
                {
                    std::lock_guard lifecycle_lock (_lifecycle_mutex);
                    port = _port;
                }
                if (!_router)
                    return std::pair{std::move (tick), port};
                for (const auto &timed_out : tick.timed_out_nodes) {
                    const auto peer = _topology.peer (timed_out);
                    if (!peer)
                        continue;
                    auto publication =
                      _topology.disconnect_on_lane (timed_out, peer->connection_id);
                    if (publication.second)
                        notifications.push_back (std::move (publication.second));
                    const auto removed = publication.first;
                    if (removed) {
                        {
                            std::lock_guard socket_lock (*_socket_mutex);
                            try {
                                _router->disconnect_rid (zlink::routing_id_t::from (timed_out));
                            }
                            catch (const zlink::connect_error_t &error) {
                                // Core may already have removed the selected route.
                                if (error.result () != zlink::connect_result_t::not_found)
                                    failures.push_back (std::current_exception ());
                            }
                            catch (...) {
                                failures.push_back (std::current_exception ());
                            }
                        }
                        end_peer_operations_if_disconnected_locked (timed_out);
                    }
                }
                submit_pending_hellos_on_lane ();
                return std::pair{std::move (tick), port};
            })
            .get ();
        auto failure = deliver_topology_notifications (
          notifications, failures.empty () ? std::exception_ptr{} : failures.front ());
        for (std::size_t index = 1; index < failures.size (); ++index)
            _options.runtime_failures->report (failures[index]);
        if (failure)
            std::rethrow_exception (failure);
        if (!port)
            co_return result;
        for (const auto &probe : result.probes) {
            (void) submit_header_only (
              probe.node_routing_id,
              protocol::encode_liveness (protocol::command::livenessProbe, probe.probe_id));
        }
        co_return result;
    }
    catch (...) {
        std::rethrow_exception (
          deliver_topology_notifications (notifications, std::current_exception ()));
    }
}

std::string raw_mesh_node_owner_t::owner_key (const std::vector<std::uint8_t> &routing_id)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill ('0');
    for (const auto byte : routing_id) {
        stream << std::setw (2) << static_cast<unsigned int> (byte);
    }
    return stream.str ();
}

foundation::call_id_t raw_mesh_node_owner_t::operation_id (std::uint64_t lifecycle_generation,
                                                           std::uint64_t correlation)
{
    return foundation::call_id_t{lifecycle_generation, correlation};
}

} // namespace zlink::framework::runtime::mesh

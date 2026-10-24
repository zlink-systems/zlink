/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/diagnostics/mesh_trace.hpp"
#include "runtime/diagnostics/diagnostic_event_sink.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/submit_result_mapper.hpp"

#include "runtime/mesh/raw_mesh_node_owner.hpp"
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

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/system_executor.hpp>

#include <nlohmann/json.hpp>
#include <opentelemetry/metrics/provider.h>

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <type_traits>
#include <utility>

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

// The same bounded surface axis serves accumulation and metric/flow projection.
struct inbound_surface_t
{
    protocol::command send;
    protocol::command request;
    const char *metric_surface;
    dispatch_error_surface_t flow_surface;
};
constexpr std::array inbound_surfaces{
  inbound_surface_t{protocol::command::nodeSend, protocol::command::nodeRequest, "node",
                    dispatch_error_surface_t::node},
  inbound_surface_t{protocol::command::channelSend, protocol::command::channelRequest, "channel",
                    dispatch_error_surface_t::route_mesh_channel},
  inbound_surface_t{protocol::command::spotSend, protocol::command::spotRequest, "spot",
                    dispatch_error_surface_t::spot_route},
  inbound_surface_t{protocol::command::actorSend, protocol::command::actorRequest, "actor",
                    dispatch_error_surface_t::spot_actor}};
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

// Every attempt can deliver the operation's terminal envelope, including an
// attempt superseded by a selected-route change. Publish its completion only
// after classifying the result, so a ready task cannot race terminal delivery.
task_t<void> observe_durable_infrastructure_attempt (
  std::shared_ptr<task_t<detail::backend::raw_request_completion_t>> native,
  std::shared_ptr<foundation::operation_registry_t> operations,
  foundation::call_id_t operation,
  std::optional<std::uint64_t> correlation,
  std::function<std::vector<std::uint8_t> (const detail::backend::raw_message_t &)> decode_reply)
{
    auto completion = std::make_shared<task_completion_source_t<void>> ();
    auto completed = completion->task ();
    detail::observe_task_terminal (*native, [native, operations = std::move (operations), operation,
                                             correlation, decode_reply = std::move (decode_reply),
                                             completion] (const auto &settled) {
        if (operations->contains (operation)) {
            if (!settled) {
                (void) operations->fail (operation,
                                         foundation::operation_terminal_t::transport_failed);
            } else {
                const auto &result = settled.value ();
                trace_mesh ("infrastructure-request-result correlation="
                            + std::to_string (correlation.value_or (0))
                            + " result=" + std::to_string (static_cast<int> (result.terminal)));
                if (result.terminal != zlink::request_result_t::not_connected
                    && result.terminal != zlink::request_result_t::timed_out) {
                    try {
                        if (result.terminal != zlink::request_result_t::ok) {
                            if (!correlation)
                                (void) operations->fail (
                                  operation, foundation::operation_terminal_t::transport_failed);
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
        completion->complete (result_t<void>::success ());
    });
    return completed;
}

// Byte control records retain their replay/storage representation. Only the
// conversion boundary precedes the port's single native request implementation.
task_t<detail::backend::raw_request_completion_t>
request_control_parts (detail::backend::raw_route_port_t &port,
                       const std::vector<std::uint8_t> &target_routing_id,
                       detail::backend::raw_message_t parts,
                       std::chrono::milliseconds timeout,
                       const std::shared_ptr<task_completion_source_t<void>> &admitted)
{
    std::vector<zlink::message_t> messages;
    try {
        messages = detail::backend::materialize_binding_parts (std::move (parts));
    }
    catch (...) {
        auto failed = task_t<detail::backend::raw_request_completion_t> (
          detail::backend::raw_request_submission_failure (std::current_exception ()));
        detail::backend::observe_raw_request_admission_failure (failed, admitted);
        return failed;
    }
    return port.request (target_routing_id, messages, timeout, {}, admitted);
}

// The coroutine owns one operation's replay state. Topology publications and
// binding completions only wake it; they never make an independent retry decision.
task_t<void> run_durable_infrastructure_request (
  std::shared_ptr<detail::backend::raw_route_port_t> port,
  service_topology_registry_t::peer_observer_t observe_peer,
  std::shared_ptr<foundation::operation_registry_t> operations,
  foundation::call_id_t operation,
  std::optional<std::uint64_t> correlation,
  std::vector<std::uint8_t> target_routing_id,
  detail::backend::raw_message_t request_parts,
  std::function<std::vector<std::uint8_t> (const detail::backend::raw_message_t &)> decode_reply,
  foundation::operation_registry_t::clock_t::time_point deadline,
  std::shared_ptr<task_completion_source_t<void>> admission)
{
    using clock_t = foundation::operation_registry_t::clock_t;
    auto first_admission =
      admission ? std::move (admission) : std::make_shared<task_completion_source_t<void>> ();
    auto admitted = first_admission->task ();
    auto deadline_signal = std::make_shared<task_completion_source_t<void>> ();
    auto expired = deadline_signal->task ();
    // Bound admission waits by the operation deadline. A submitted attempt
    // retains the binding's full remaining-deadline completion owner.
    boost::asio::steady_timer deadline_timer{boost::asio::system_executor ()};
    deadline_timer.expires_at (deadline);
    deadline_timer.async_wait ([deadline_signal] (const boost::system::error_code &error) {
        if (!error)
            deadline_signal->complete (result_t<void>::success ());
    });
    std::shared_ptr<task_t<void>> running;
    std::vector<std::uint8_t> submitted_connection;
    struct wait_subscriptions_t
    {
        std::stop_source owner;
        ~wait_subscriptions_t () { owner.request_stop (); }
    };
    while (operations->contains (operation)) {
        auto observation = observe_peer ();
        if (running && running->await_ready ()) {
            running.reset ();
            if (!operations->contains (operation))
                co_return;
        }
        const auto remaining =
          std::chrono::ceil<std::chrono::milliseconds> (deadline - clock_t::now ());
        if (!running && remaining <= std::chrono::milliseconds::zero ()) {
            (void) operations->fail (operation,
                                     admitted.await_ready () && admitted.result ()
                                       ? foundation::operation_terminal_t::timed_out
                                       : foundation::operation_terminal_t::route_unavailable);
            co_return;
        }
        // NotConnected may arrive before the route observer invalidates the
        // previous ready publication. Only a different observed selected route
        // can admit another attempt (Actor model §8.1); if already ready, submit
        // immediately, including while the previous native attempt is pending.
        if (observation.peer && observation.peer->ready
            && observation.peer->connection_id != submitted_connection
            && remaining > std::chrono::milliseconds::zero ()) {
            submitted_connection = observation.peer->connection_id;
            auto accepted = std::make_shared<task_completion_source_t<void>> ();
            auto native = std::make_shared<task_t<detail::backend::raw_request_completion_t>> (
              request_control_parts (*port, target_routing_id, request_parts, remaining, accepted));
            auto accepted_task = accepted->task ();
            detail::observe_task_terminal (accepted_task,
                                           [first_admission] (const result_t<void> &result) {
                                               if (result)
                                                   first_admission->complete (result);
                                           });
            running = std::make_shared<task_t<void>> (observe_durable_infrastructure_attempt (
              std::move (native), operations, operation, correlation, decode_reply));
        }
        auto wake = std::make_shared<task_completion_source_t<void>> ();
        auto notified = wake->task ();
        const auto notify = [weak = std::weak_ptr{wake}] (const auto &) {
            if (const auto signal = weak.lock ())
                signal->complete (result_t<void>::success ());
        };
        wait_subscriptions_t subscriptions;
        {
            // End every losing registration when this wait completes. In
            // particular, a quiet topology must not retain expired operations.
            detail::ambient_context_scope_t subscription_scope (
              nullptr, nullptr, subscriptions.owner.get_token (), false, true);
            detail::observe_task_completion (observation.changed, notify);
            if (!running)
                detail::observe_task_completion (expired, notify);
            if (!admitted.await_ready ())
                detail::observe_task_completion (admitted, notify);
            if (running)
                detail::observe_task_completion (*running, notify);
        }
        co_await notified;
    }
}
task_t<bool> submit_registered_infrastructure_request_with_retry (
  std::shared_ptr<detail::backend::raw_route_port_t> port,
  service_topology_registry_t::peer_observer_t observe_peer,
  std::shared_ptr<foundation::operation_registry_t> operations,
  foundation::call_id_t operation,
  std::optional<std::uint64_t> correlation,
  std::vector<std::uint8_t> target_routing_id,
  detail::backend::raw_message_t request_parts,
  std::function<std::vector<std::uint8_t> (const detail::backend::raw_message_t &)> decode_reply,
  foundation::operation_registry_t::clock_t::time_point deadline,
  std::shared_ptr<task_completion_source_t<void>> admitted = {})
{
    if (!port || deadline <= foundation::operation_registry_t::clock_t::now ()) {
        (void) operations->fail (operation, foundation::operation_terminal_t::route_unavailable);
        co_return false;
    }
    // Admission and replay progress independently of the initiating handler's
    // turn and cancellation owner (execution gate §13).
    detail::serial_turn_scope_t turn_scope (nullptr);
    detail::ambient_context_scope_t infrastructure_scope (nullptr, nullptr, {}, false, true);
    auto running = std::make_shared<task_t<void>> (run_durable_infrastructure_request (
      std::move (port), std::move (observe_peer), operations, operation, correlation,
      std::move (target_routing_id), std::move (request_parts), std::move (decode_reply), deadline,
      std::move (admitted)));
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
        try {
            result.value ();
            return result_t<bool>::success (true);
        }
        catch (const framework_exception_t &) {
            // The native reply observer completes the operation failure.
            return result_t<bool>::success (false);
        }
    };
    if (admitted.await_ready ())
        return task_t<bool> (convert (admitted.result ()));
    return detail::project_task_terminal<bool> (admitted, std::move (convert));
}
catch (...) {
    return task_t<bool> (detail::current_exception_result<bool> ());
}

task_t<bool> submit_registered_infrastructure_request (
  std::shared_ptr<detail::backend::raw_route_port_t> port,
  std::shared_ptr<foundation::operation_registry_t> operations,
  foundation::call_id_t operation,
  std::uint64_t correlation,
  std::vector<std::uint8_t> target_routing_id,
  detail::backend::raw_message_t request_parts,
  std::function<std::vector<std::uint8_t> (const detail::backend::raw_message_t &)> decode_reply,
  std::chrono::milliseconds timeout,
  std::shared_ptr<task_completion_source_t<void>> admitted = {})
{
    if (!port || timeout <= std::chrono::milliseconds::zero ()) {
        (void) operations->fail (operation, foundation::operation_terminal_t::transport_failed);
        co_return false;
    }
    auto accepted =
      admitted ? std::move (admitted) : std::make_shared<task_completion_source_t<void>> ();
    auto running =
      std::make_shared<task_t<detail::backend::raw_request_completion_t>> (request_control_parts (
        *port, target_routing_id, std::move (request_parts), timeout, accepted));
    detail::observe_task_terminal (
      *running, [operations = std::move (operations), operation, correlation,
                 decode_reply = std::move (decode_reply),
                 running] (const result_t<detail::backend::raw_request_completion_t> &settled) {
          if (!settled) {
              (void) operations->fail (operation,
                                       foundation::operation_terminal_t::transport_failed);
              return;
          }
          const auto &completion = settled.value ();
          trace_mesh ("infrastructure-request-result correlation=" + std::to_string (correlation)
                      + " result=" + std::to_string (static_cast<int> (completion.terminal)));
          if (completion.terminal != zlink::request_result_t::ok) {
              if (const auto terminal = messaging::request_failure_mapper_t{}.transport_terminal (
                    completion.terminal)) {
                  (void) operations->fail (operation, *terminal);
              } else {
                  auto payload =
                    decode_reply (request_failure_reply_parts (correlation, completion.terminal));
                  (void) operations->complete (
                    operation, detail::backend::materialize_binding_part (std::move (payload)));
              }
              return;
          }
          try {
              const auto parts = detail::backend::copy_binding_parts (completion.parts);
              if (parts.empty ()) {
                  throw protocol::service_wire_error_t ("infrastructure reply has no header");
              }
              const auto prefix = protocol::decode_reply_header (parts.front ());
              if (prefix.correlation != correlation) {
                  throw protocol::service_wire_error_t (
                    "infrastructure reply correlation does not match");
              }
              auto payload = decode_reply (parts);
              (void) operations->complete (
                operation, detail::backend::materialize_binding_part (std::move (payload)));
          }
          catch (const protocol::service_wire_error_t &) {
              (void) operations->fail (operation, foundation::operation_terminal_t::protocol_error);
          }
      });
    co_return co_await await_request_admission (accepted->task ());
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
    _topology (_options.descriptor, _options.metric_channel_names, &_liveness),
    _mailbox (),
    _request_metrics (std::make_shared<mesh_request_metrics_t> (_options.descriptor.mesh_name)),
    _operations (std::make_shared<foundation::operation_registry_t> ())
{
    if (!_options.runtime_failures)
        throw std::invalid_argument ("A host runtime failure collector is required");
    auto meter =
      opentelemetry::metrics::Provider::GetMeterProvider ()->GetMeter ("zlink.framework");
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
    _drop_metric =
      meter->CreateDoubleObservableCounter ("zlink.mesh_node.messages.dropped", "", "{message}");
    if (_drop_metric)
        _drop_metric->AddCallback (&publish_drop_metrics, this);
}

raw_mesh_node_owner_t::~raw_mesh_node_owner_t () noexcept
{
    for (auto &registration : _peer_metrics)
        if (registration.instrument)
            registration.instrument->RemoveCallback (&publish_peer_metrics, &registration);
    if (_drop_metric)
        _drop_metric->RemoveCallback (&publish_drop_metrics, this);
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

void raw_mesh_node_owner_t::start ()
{
    return _lane
      .run ([this] {
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

              if (descriptor.descriptor_revision == std::numeric_limits<std::uint64_t>::max ()) {
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
                zlink::poll_event_flag_t::pollin | zlink::poll_event_flag_t::pollroute,
                ingress_poller.get (), 1);
              _port->set_request_receive_source (
                [this] (const auto &target) { return _liveness.connection (target); });
              _ingress_poller = std::move (ingress_poller);
              _router = std::move (router);
              _receive_flow_registration = std::move (receive_flow_registration);
              // Keep the descriptor in preparing until the receive port, completion
              // control, and route observation can accept the first admitted message.
              descriptor.state = service_node_state_t::serving;
          }
          _topology.publish_local (std::move (descriptor));
      })
      .get ();
}

// Every read of the local descriptor that is submitted to a route, and every
// revision change, runs in an owner turn together with its submission, so a
// route never receives a lower revision after a higher one (wire-protocol §4
// DescriptorRevision ordering).
task_t<void> raw_mesh_node_owner_t::publish_draining ()
{
    return runtime::run_blocking_step<void> ([this] () -> task_t<void> {
        _lane
          .run ([this] {
              const auto publication = _topology.publish_draining_snapshot ();
              send_descriptor_update_on_lane (publication.first, publication.second);
          })
          .get ();
        co_return;
    });
}

service_node_descriptor_t raw_mesh_node_owner_t::publish_descriptor_update (
  const std::function<void (service_node_descriptor_t &)> &change)
{
    return _lane
      .run ([this, &change] {
          auto descriptor = _topology.local_descriptor ();
          change (descriptor);
          const auto peers = _topology.publish_local_snapshot (descriptor);
          send_descriptor_update_on_lane (descriptor, peers);
          return descriptor;
      })
      .get ();
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
    auto resources =
      _lane
        .run ([this] {
            if (!_closed) {
                _receive_flow_registration.close ();
                _mailbox.close ();
                _operations->shutdown ();
                _routes.clear ();
            }
            std::lock_guard lifecycle_lock (_lifecycle_mutex);
            return std::tuple{std::move (_port), std::move (_ingress_poller), std::move (_router)};
        })
        .get ();
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
        return _lane
          .run ([this] {
              std::lock_guard lifecycle_lock (_lifecycle_mutex);
              return static_cast<bool> (_port);
          })
          .get ();
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

bool raw_mesh_node_owner_t::connect_peer (const std::string &endpoint)
{
    return _lane
      .run ([this, &endpoint] {
          std::lock_guard lifecycle_lock (_lifecycle_mutex);
          if (!_router || endpoint.empty ()) {
              return false;
          }
          try {
              std::lock_guard socket_lock (*_socket_mutex);
              trace_mesh ("connect endpoint=" + endpoint);
              _router->connect (endpoint);
              _outbound_endpoints.insert (endpoint);
              return true;
          }
          catch (const zlink::binding_error_t &rejection) {
              // Mesh node: a rejection fails only this attempt and keeps no intent.
              trace_mesh ("connect rejected endpoint=" + endpoint + " error=" + rejection.what ());
              return false;
          }
      })
      .get ();
}

bool raw_mesh_node_owner_t::connect_peer (const std::string &endpoint,
                                          service_node_descriptor_t expected_descriptor)
{
    return _lane
      .run ([this, &endpoint, expected_descriptor = std::move (expected_descriptor)] () mutable {
          std::lock_guard lifecycle_lock (_lifecycle_mutex);
          if (!_router || endpoint.empty ()) {
              return false;
          }
          const auto is_stale_same_endpoint = [&endpoint,
                                               &expected_descriptor] (const auto &entry) {
              return entry.first != expected_descriptor.node_routing_id
                     && entry.second.advertised_endpoint == endpoint;
          };
          const auto endpoint_retargeted =
            std::any_of (_expected_peers.begin (), _expected_peers.end (), is_stale_same_endpoint);
          try {
              std::lock_guard socket_lock (*_socket_mutex);
              trace_mesh ("connect endpoint=" + endpoint
                          + " expected=" + owner_key (expected_descriptor.node_routing_id));
              if (endpoint_retargeted && _outbound_endpoints.contains (endpoint)) {
                  // connect_routing_id is captured only when a new connect intent is
                  // created. Retire the stale same-endpoint intent before assigning
                  // the replacement RID, otherwise reconnect keeps publishing the
                  // previous alias and RouteMesh can never admit the replacement.
                  _router->disconnect (endpoint);
              }
              _router->options ().connect_routing_id (
                zlink::routing_id_t::from (expected_descriptor.node_routing_id));
              _router->connect (endpoint);
              _outbound_endpoints.insert (endpoint);
              if (endpoint_retargeted) {
                  for (auto peer = _expected_peers.begin (); peer != _expected_peers.end ();) {
                      if (!is_stale_same_endpoint (*peer)) {
                          ++peer;
                          continue;
                      }
                      const auto retired_rid = peer->first;
                      peer = _expected_peers.erase (peer);
                      end_peer_operations_if_disconnected_locked (retired_rid);
                  }
              }
              _expected_peers.insert_or_assign (expected_descriptor.node_routing_id,
                                                expected_descriptor);
              return true;
          }
          catch (const zlink::binding_error_t &rejection) {
              // Mesh node: a rejection fails only this attempt and keeps no intent.
              trace_mesh ("connect rejected endpoint=" + endpoint + " error=" + rejection.what ());
              return false;
          }
      })
      .get ();
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
    try {
        return _lane
          .run ([this, &expected_routing_id, &endpoint] {
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
                      for (const auto &candidate : _topology.peers ()) {
                          if (candidate.descriptor.advertised_endpoint == endpoint) {
                              admitted = candidate;
                              break;
                          }
                      }
                  }
                  if (admitted) {
                      trace_mesh ("disconnect admitted="
                                  + owner_key (admitted->descriptor.node_routing_id)
                                  + " connection=" + owner_key (admitted->connection_id));
                      (void) _topology.disconnect (admitted->descriptor.node_routing_id,
                                                   admitted->connection_id);
                      (void) _liveness.disconnect (admitted->descriptor.node_routing_id,
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
                      for (const auto &candidate : _topology.peers ()) {
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
          })
          .get ();
    }
    catch (...) {
        return false;
    }
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
    return _lane
      .run ([this, descriptor = std::move (descriptor), connection_id = std::move (connection_id),
             now] () mutable {
          if (!_router) {
              return peer_admission_result_t::invalid_descriptor;
          }
          const auto result =
            _topology.admit (std::move (descriptor), std::move (connection_id), now);
          refresh_admission_due_on_lane ();
          return result;
      })
      .get ();
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
        return send_with_header_result (target_routing_id,
                                        protocol::encode_node_send_header_message (),
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
        std::array<zlink::message_t, 3> parts;
        auto next = parts.begin ();
        *next++ = detail::backend::materialize_binding_part (std::move (header.data));
        if (!metadata.empty ())
            *next++ = detail::backend::materialize_binding_part (metadata);
        *next++ = protocol::encode_application_payload_message (application_payload);
        return submit_owned_send_result (target_routing_id,
                                         std::span<zlink::message_t> (parts.begin (), next), {});
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
    return request_with_header (
      target_routing_id,
      [] (std::uint64_t request_correlation) {
          return protocol::encode_node_request_header_message (request_correlation);
      },
      application_payload, timeout, std::move (callback), correlation, false,
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
    mesh_request_metric_t request_metric (_request_metrics, mesh_request_surface_t::channel);
    service_liveness_registry_t::connection_t admission;
    auto selected = _topology.select (channel_name, &admission);
    if (!selected) {
        throw *selected.error ();
    }
    co_return co_await request_with_header (
      std::move (selected.value ()),
      [&channel_name] (std::uint64_t request_correlation) {
          return detail::backend::materialize_binding_part (
            protocol::encode_channel_request_header (request_correlation, channel_name));
      },
      application_payload, timeout, std::move (callback), correlation, true,
      std::move (request_metric), std::move (admission));
}

bool raw_mesh_node_owner_t::observe_request (
  foundation::call_id_t operation,
  std::uint64_t correlation,
  std::shared_ptr<task_t<detail::backend::raw_request_completion_t>> running)
{
    if (!running) {
        (void) _operations->fail (operation, foundation::operation_terminal_t::transport_failed);
        return false;
    }
    const auto operations = _operations;
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

task_t<bool> raw_mesh_node_owner_t::request_with_header (
  const std::vector<std::uint8_t> &target_routing_id,
  const std::function<zlink::message_t (std::uint64_t)> &header,
  const protocol::application_payload_t &application_payload,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback,
  std::optional<std::uint64_t> requested_correlation,
  bool target_claimed,
  mesh_request_metric_t request_metric,
  service_liveness_registry_t::connection_t admission,
  std::optional<foundation::operation_registry_t::request_identity_t> request)
try {
    if (timeout <= std::chrono::milliseconds::zero ()) {
        throw std::invalid_argument ("raw mesh request timeout must be positive");
    }
    if (!target_claimed) {
        const auto peer = _topology.peer (target_routing_id);
        if (!peer || !peer->ready)
            return task_t<bool> (result_t<bool>::success (false));
        admission = peer->liveness;
    }
    auto encoded_payload = protocol::encode_application_payload_message (application_payload);
    struct request_start_t
    {
        foundation::call_id_t operation;
        std::uint64_t correlation = 0;
        std::shared_ptr<task_t<detail::backend::raw_request_completion_t>> running;
        std::shared_ptr<task_completion_source_t<void>> admitted =
          std::make_shared<task_completion_source_t<void>> ();
        bool registered = false;
    };
    auto start = _lane.run_sync ([this, &header, &request_metric, &target_routing_id,
                                  request = std::move (request), requested_correlation, timeout,
                                  callback = std::move (callback),
                                  encoded_payload = std::move (encoded_payload),
                                  admission = std::move (admission)] () mutable {
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
        std::array<zlink::message_t, 2> wire{header (value.correlation),
                                             std::move (encoded_payload)};
        if (request && request->operation == foundation::call_id_t{})
            request->operation = value.operation;
        value.registered = _operations->register_operation (
          value.operation, foundation::operation_registry_t::clock_t::now () + timeout,
          std::move (callback), {}, std::move (request_metric), std::move (request));
        if (value.registered) {
            try {
                trace_mesh ("request-submit correlation=" + std::to_string (value.correlation)
                            + " targetBytes=" + std::to_string (target_routing_id.size ()));
                value.running =
                  std::make_shared<task_t<detail::backend::raw_request_completion_t>> (
                    port->request (target_routing_id, wire, timeout, std::move (admission),
                                   value.admitted));
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
    (void) observe_request (start->operation, start->correlation, std::move (start->running));
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
                                   std::span<zlink::message_t> parts,
                                   bool needs_public_completion,
                                   detail::backend::raw_send_stage_trace_t trace)
{
    // run_sync finishes this owner turn before the borrowed frame list returns
    // to its caller; the binding takes each part before this prefix completes.
    return _lane.run_sync ([this, parts, trace = std::move (trace), needs_public_completion,
                            &target_routing_id] () mutable {
        return start_send_on_lane (target_routing_id, std::move (parts), needs_public_completion,
                                   std::move (trace));
    });
}

raw_mesh_node_owner_t::send_start_result_t
raw_mesh_node_owner_t::start_send_on_lane (const std::vector<std::uint8_t> &target_routing_id,
                                           std::span<zlink::message_t> parts,
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
                                                zlink::message_t header,
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
        std::array<zlink::message_t, 3> parts;
        auto next = parts.begin ();
        *next++ = std::move (header);
        if (metadata)
            *next++ = detail::backend::materialize_binding_part (std::move (*metadata));
        *next++ = protocol::encode_application_payload_message (application_payload);
        return submit_owned_send_result (
          target_routing_id, std::span<zlink::message_t> (parts.begin (), next), std::move (trace));
    }
    catch (...) {
        return task_t<zlink::submit_result_t> (
          detail::current_exception_result<zlink::submit_result_t> ());
    }
}

task_t<zlink::submit_result_t>
raw_mesh_node_owner_t::submit_owned_send_result (const std::vector<std::uint8_t> &target_routing_id,
                                                 std::span<zlink::message_t> parts,
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
    detail::backend::raw_message_t parts{std::move (header)};
    std::vector<zlink::message_t> messages;
    try {
        messages = detail::backend::materialize_binding_parts (std::move (parts));
    }
    catch (...) {
        auto failure = detail::backend::raw_send_submission_failure (std::current_exception (), {});
        co_return failure.immediate_result->value () == zlink::submit_result_t::ok;
    }
    auto started = start_send (target_routing_id, messages, true);
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
    detail::backend::raw_message_t parts{std::move (header)};
    std::vector<zlink::message_t> messages;
    try {
        messages = detail::backend::materialize_binding_parts (std::move (parts));
    }
    catch (...) {
        (void) detail::backend::raw_send_submission_failure (std::current_exception (), {});
        return send_start_state_t::started;
    }
    auto started = start_send_on_lane (target_routing_id, messages, false, {});
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
    std::vector<zlink::message_t> messages;
    try {
        messages = detail::backend::materialize_binding_parts (std::move (parts));
    }
    catch (...) {
        auto failure = detail::backend::raw_send_submission_failure (std::current_exception (), {});
        co_return failure.immediate_result->value () == zlink::submit_result_t::ok;
    }
    co_return co_await submit_owned_send_result (target_routing_id, messages, {})
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
          std::move (callback)))
        co_return false;
    signal_activity ();
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
        if (!_operations->register_operation (
              id, foundation::operation_registry_t::clock_t::time_point::max (),
              [completion] (auto terminal, auto packed) {
                  completion->complete (
                    result_t<completion_t>::success ({terminal, std::move (packed)}));
              },
              target_routing_id))
            co_return relocation_prepare_response_t{};
    }
    detail::backend::raw_message_t parts{protocol::encode_relocation_control (prepare)};
    (void) co_await submit_registered_infrastructure_request_with_retry (
      std::move (port), _topology.observe_peer (target_routing_id), _operations, id, std::nullopt,
      target_routing_id, std::move (parts), protocol::pack_infrastructure_reply, deadline);
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
    const auto port = _lane
                        .run ([this] {
                            std::lock_guard lifecycle_lock (_lifecycle_mutex);
                            return _port;
                        })
                        .get ();
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
    const auto port = _lane
                        .run ([this] {
                            std::lock_guard lifecycle_lock (_lifecycle_mutex);
                            return _port;
                        })
                        .get ();
    if (!port)
        return false;
    detail::backend::raw_message_t parts{
      protocol::encode_reply_header (*request.correlation, terminal_result, failure_code)};
    return port->reply (
      detail::backend::raw_received_t{request.source_routing_id, request.reply_token, {}},
      std::move (parts));
}

std::size_t
raw_mesh_node_owner_t::expire_requests (foundation::operation_registry_t::clock_t::time_point now)
{
    static_cast<void> (now);
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
            : std::nullopt))
        return false;
    reservation.second->signal_activity ();
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
    co_return co_await send_with_header_result (
      std::move (selected.value ()),
      detail::backend::materialize_binding_part (
        protocol::encode_channel_send_header (channel_name)),
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
      detail::backend::materialize_binding_part (protocol::encode_spot_message_header (
        protocol::command::spotSend, source_spot_id, target,
        operation.value_or (protocol::wire_operation_id_t{local.lifecycle_generation, *sequence}),
        std::nullopt, message_follow_hop_count)),
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
    const auto local = _topology.local_descriptor ();
    co_return co_await request_with_header (
      target_routing_id,
      [source_spot_id, target, local, operation,
       message_follow_hop_count] (std::uint64_t correlation) {
          const auto exact = operation.value_or (
            protocol::wire_operation_id_t{local.lifecycle_generation, correlation});
          return detail::backend::materialize_binding_part (protocol::encode_spot_message_header (
            protocol::command::spotRequest, source_spot_id, target, exact, correlation,
            message_follow_hop_count));
      },
      application_payload, timeout, std::move (callback), correlation, false,
      mesh_request_metric_t (_request_metrics, mesh_request_surface_t::spot), {},
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
      detail::backend::materialize_binding_part (protocol::encode_actor_message_header (
        protocol::command::actorSend, source_actor, target,
        operation.value_or (protocol::wire_operation_id_t{local.lifecycle_generation, *sequence}),
        std::nullopt, message_follow_hop_count, std::move (bound_session_source))),
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
    const auto local = _topology.local_descriptor ();
    co_return co_await request_with_header (
      target_routing_id,
      [source_actor, target, local, operation, message_follow_hop_count,
       bound_session_source = std::move (bound_session_source)] (std::uint64_t correlation) {
          const auto exact = operation.value_or (
            protocol::wire_operation_id_t{local.lifecycle_generation, correlation});
          return detail::backend::materialize_binding_part (protocol::encode_actor_message_header (
            protocol::command::actorRequest, source_actor, target, exact, correlation,
            message_follow_hop_count, bound_session_source));
      },
      application_payload, timeout, std::move (callback), correlation, false,
      mesh_request_metric_t (_request_metrics, mesh_request_surface_t::actor), {},
      request_identity (operation));
}

task_t<bool> raw_mesh_node_owner_t::request_user_spot_create (
  const std::vector<std::uint8_t> &target_routing_id,
  protocol::user_spot_create_header_t request,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback)
{
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
      timeout, std::move (callback));
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
    co_return co_await send_with_header_result (
      session_owner_routing_id,
      detail::backend::materialize_binding_part (protocol::encode_bound_session_send (record)),
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

    const auto deadline = foundation::operation_registry_t::clock_t::now () + timeout;
    const auto local = _topology.local_descriptor ();
    const auto operations = _operations;
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    std::uint64_t correlation = 0;
    foundation::call_id_t id{};
    detail::backend::raw_message_t request_parts;
    bool registered = false;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
        if (port) {
            correlation = take_reply_route_id_locked ();
            id = operation_id (local.lifecycle_generation, correlation);
            record.correlation = correlation;
            request_parts = {protocol::encode_bound_session_bind (record)};
            registered = operations->register_operation (
              id, foundation::operation_registry_t::clock_t::time_point::max (),
              std::move (callback), actor_owner_routing_id);
        }
    }
    if (!port) {
        callback (foundation::operation_terminal_t::shutdown, {});
        co_return true;
    }
    if (!registered)
        co_return false;

    co_return co_await submit_registered_infrastructure_request_with_retry (
      std::move (port), _topology.observe_peer (actor_owner_routing_id), operations, id,
      correlation, actor_owner_routing_id, std::move (request_parts),
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
      deadline, std::move (admitted));
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
      timeout,
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
    if (timeout <= std::chrono::milliseconds::zero ()) {
        throw std::invalid_argument ("Actor create request timeout must be positive");
    }
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
          timeout, std::move (callback));
    }

    const auto deadline = foundation::operation_registry_t::clock_t::now () + timeout;
    const auto operations = _operations;
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    std::uint64_t correlation = 0;
    foundation::call_id_t id{};
    detail::backend::raw_message_t request_parts;
    bool registered = false;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
        if (port) {
            correlation = take_reply_route_id_locked ();
            id = operation_id (local.lifecycle_generation, correlation);
            request.correlation = correlation;
            request_parts = {protocol::encode_actor_create_header (request)};
            registered = operations->register_operation (
              id, foundation::operation_registry_t::clock_t::time_point::max (),
              std::move (callback), target_routing_id);
        }
    }
    if (!port) {
        callback (foundation::operation_terminal_t::shutdown, {});
        co_return true;
    }
    if (!registered)
        co_return false;

    co_return co_await submit_registered_infrastructure_request_with_retry (
      std::move (port), _topology.observe_peer (target_routing_id), operations, id, correlation,
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
      deadline);
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
        // The durable sender owns the deadline and its admission-dependent
        // terminal. Registry expiry must not race that classification.
        if (!_operations->register_operation (
              id, foundation::operation_registry_t::clock_t::time_point::max (),
              [completion] (auto terminal, auto packed) {
                  completion->complete (
                    result_t<completion_t>::success ({terminal, std::move (packed)}));
              },
              target_routing_id)) {
            outcome.failure = actor_join_wire_failure_t::unavailable;
            co_return outcome;
        }
    }
    (void) co_await submit_registered_infrastructure_request_with_retry (
      std::move (port), _topology.observe_peer (target_routing_id), _operations, id,
      request.correlation, target_routing_id, std::move (parts),
      protocol::pack_infrastructure_reply, deadline);
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
          target_routing_id,
          detail::backend::materialize_binding_part (
            protocol::encode_instance_spot_activation_header (request)),
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
    if (!_operations->register_operation (
          id, foundation::operation_registry_t::clock_t::now () + timeout, std::move (callback), {},
          mesh_request_metric_t (_request_metrics, mesh_request_surface_t::instance_spot))) {
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
    trace_mesh ("infrastructure-request-submit correlation=" + std::to_string (correlation)
                + " targetBytes=" + std::to_string (target_routing_id.size ()));
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    auto running = std::make_shared<task_t<detail::backend::raw_request_completion_t>> (
      request_control_parts (*port, target_routing_id, std::move (parts), timeout, admitted));
    detail::observe_task_terminal (
      *running, [operations, id, correlation,
                 running] (const result_t<detail::backend::raw_request_completion_t> &settled) {
          if (!settled) {
              (void) operations->fail (id, foundation::operation_terminal_t::transport_failed);
              return;
          }
          const auto &completion = settled.value ();
          if (completion.terminal != zlink::request_result_t::ok) {
              if (const auto terminal = messaging::request_failure_mapper_t{}.transport_terminal (
                    completion.terminal)) {
                  (void) operations->fail (id, *terminal);
              } else {
                  (void) operations->complete (
                    id,
                    detail::backend::materialize_binding_part (protocol::pack_infrastructure_reply (
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
                    static_cast<std::uint32_t> (protocol::request_terminal_result::protocolError),
                    static_cast<std::uint32_t> (
                      protocol::framework_error_code::requestProtocolError))})),
                {}, foundation::operation_terminal_t::protocol_error);
          }
      });
    co_return co_await await_request_admission (admitted->task ());
}

task_t<bool> raw_mesh_node_owner_t::request_user_spot_close (
  const std::vector<std::uint8_t> &target_routing_id,
  protocol::user_spot_close_header_t request,
  std::chrono::milliseconds timeout,
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
      timeout, std::move (callback));
}

task_t<bool> raw_mesh_node_owner_t::request_infrastructure (
  const std::vector<std::uint8_t> &target_routing_id,
  const std::function<std::vector<std::uint8_t> (std::uint64_t)> &header,
  const std::function<std::vector<std::uint8_t> (const detail::backend::raw_message_t &)>
    &decode_reply,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback)
{
    if (timeout <= std::chrono::milliseconds::zero ()) {
        throw std::invalid_argument ("raw mesh infrastructure request timeout must be positive");
    }
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
    if (!_operations->register_operation (
          id, foundation::operation_registry_t::clock_t::now () + timeout, std::move (callback))) {
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
    co_return co_await submit_registered_infrastructure_request (
      std::move (port), std::move (operations), id, correlation, target_routing_id,
      std::move (request_parts), decode_reply, timeout);
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
            const auto peers = owner._topology.peers ();
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
            for (const auto &peer : owner._topology.not_required_peers ())
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

void raw_mesh_node_owner_t::publish_drop_metrics (opentelemetry::metrics::ObserverResult result,
                                                  void *state)
{
    const auto &owner = *static_cast<raw_mesh_node_owner_t *> (state);
    auto observer = opentelemetry::nostd::get<
      opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObserverResultT<double>>> (result);
    for (std::size_t index = 0; index < inbound_surfaces.size (); ++index) {
        observer->Observe (
          static_cast<double> (owner._inbound_drops[index].load (std::memory_order_relaxed)),
          {{"mesh_name", owner._options.descriptor.mesh_name},
           {"surface", inbound_surfaces[index].metric_surface},
           {"message_kind", "send"},
           {"reason", "backpressure"}});
    }
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
        readiness = port->poll (std::chrono::milliseconds::zero ());
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
    const auto admission =
      _lane
        .run ([this, &source_routing_id, &descriptor, &expectation, &connection_id, now, kind] {
            const auto result = expectation.expected_descriptor
                                  ? _topology.admit (descriptor, connection_id,
                                                     *expectation.expected_descriptor, now, kind)
                                  : _topology.admit (descriptor, connection_id, now, kind);
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
            _lane
              .run ([this, &received] {
                  const auto peer = _topology.peer (received->source_routing_id);
                  if (!peer)
                      return;
                  (void) _topology.disconnect (received->source_routing_id, peer->connection_id);
                  (void) _liveness.disconnect (received->source_routing_id, peer->connection_id);
                  refresh_admission_due_on_lane ();
              })
              .get ();
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
                const auto ack =
                  _lane
                    .run ([this, &received, &connection_id, &record] {
                        return _liveness.acknowledge_probe (received->source_routing_id,
                                                            connection_id, record.probe_id);
                    })
                    .get ();
                if (!ack)
                    return raw_mesh_pump_result_t::protocol_error;
                const auto submitted = submit_header_only (
                  received->source_routing_id,
                  protocol::encode_liveness (protocol::command::livenessAck, record.probe_id));
                if (submitted == send_start_state_t::terminated)
                    return raw_mesh_pump_result_t::protocol_error;
            } else {
                (void) _lane
                  .run ([this, &received, &connection_id, &record, now] {
                      return _liveness.acknowledge (received->source_routing_id, connection_id,
                                                    record.probe_id, now);
                  })
                  .get ();
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
        const auto next_operation_deadline = _operations->next_deadline ();
        if (next_operation_deadline
            && (!next_activity || *next_operation_deadline < *next_activity)) {
            next_activity = next_operation_deadline;
        }
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
        return port->poll (timeout, accept_application_receive) != zlink::poll_event_flag_t::none;
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
          const auto selected = _routes.find (rid);
          if (selected == _routes.end ())
              return std::pair<std::uint64_t, bool>{0, false};
          const auto peer = _topology.peer (rid);
          return std::pair{selected->second.generation,
                           peer && peer->ready
                             && (!lifecycle_generation
                                 || peer->descriptor.lifecycle_generation == *lifecycle_generation)
                             && detail::backend::is_route_connection (peer->connection_id,
                                                                      selected->second.generation)};
      })
      .get ();
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
    const auto readiness = port->poll (std::chrono::milliseconds::zero ());
    return apply_route_snapshot (readiness);
}

std::size_t raw_mesh_node_owner_t::apply_route_snapshot (zlink::poll_event_flag_t readiness)
{
    if ((static_cast<short> (readiness) & static_cast<short> (zlink::poll_event_flag_t::pollroute))
        == 0)
        return 0;
    return _lane
      .run ([this] {
          std::size_t changes = 0;
          {
              if (!_router)
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
                  observed.emplace (rid,
                                    previous != _routes.end ()
                                        && previous->second.generation == route.route_generation
                                      ? std::move (previous->second)
                                      : selected_route_t{route.route_generation, {}});
              }
              // A route that ended or was replaced loses its admission and
              // liveness; a new route admits only through a new handshake.
              for (const auto &[node_routing_id, route] : _routes) {
                  const auto still = observed.find (node_routing_id);
                  if (still != observed.end () && still->second.generation == route.generation)
                      continue;
                  ++changes;
                  const auto connection_id = route_connection_id (route.generation);
                  trace_mesh ("route-ended routing=" + owner_key (node_routing_id)
                              + " connection=" + owner_key (connection_id));
                  const auto removed = _topology.disconnect (node_routing_id, connection_id);
                  (void) _liveness.disconnect (node_routing_id, connection_id);
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
    _liveness.set_admission_pending (
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
    _liveness.set_admission_pending (pending);
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
    const auto [result, port] =
      _lane
        .run ([this, now] {
            auto tick = _liveness.tick (now);
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
                const auto removed = _topology.disconnect (timed_out, peer->connection_id);
                if (removed) {
                    {
                        std::lock_guard socket_lock (*_socket_mutex);
                        try {
                            _router->disconnect_rid (zlink::routing_id_t::from (timed_out));
                        }
                        catch (const zlink::connect_error_t &error) {
                            // Core may already have removed the selected route.
                            if (error.result () != zlink::connect_result_t::not_found)
                                throw;
                        }
                    }
                    end_peer_operations_if_disconnected_locked (timed_out);
                }
            }
            submit_pending_hellos_on_lane ();
            return std::pair{std::move (tick), port};
        })
        .get ();
    if (!port)
        co_return result;
    for (const auto &probe : result.probes) {
        (void) submit_header_only (
          probe.node_routing_id,
          protocol::encode_liveness (protocol::command::livenessProbe, probe.probe_id));
    }
    co_return result;
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

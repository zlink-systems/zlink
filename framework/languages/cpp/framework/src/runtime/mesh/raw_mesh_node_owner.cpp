/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/mesh/raw_mesh_node_owner.hpp"

#include "runtime/protocol/service_wire_codec.hpp"

#include <zlink/Contracts/Core/byte_count.hpp>
#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Eventing/events.hpp>
#include <zlink/Contracts/Eventing/monitor.hpp>
#include <zlink/Contracts/Eventing/poll_event.hpp>
#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Errors/errors.hpp>
#include <zlink/Contracts/Sockets/results.hpp>
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>

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

namespace zlink::framework::runtime::mesh
{
namespace
{

constexpr std::size_t max_completion_control_parts = 64;
constexpr std::size_t max_completion_control_bytes = 256u * 1024u;
constexpr std::size_t max_pending_completion_controls = 1024;
constexpr std::size_t max_pending_unadmitted_applications = 1024;
constexpr std::size_t max_pending_unadmitted_application_bytes =
  16u * 1024u * 1024u;
constexpr std::size_t max_pending_admissions = 64;
constexpr std::size_t max_pending_admission_bytes = 64u * 1024u;

bool mesh_trace_enabled ()
{
    const char *value = std::getenv ("ZLINK_CPP_MESH_TRACE");
    return value != nullptr && *value != '\0' && std::string_view (value) != "0";
}

void trace_mesh (const std::string &message)
{
    if (mesh_trace_enabled ())
        std::cerr << "zlink mesh " << message << '\n';
}

std::string trace_owner_key (const void *owner)
{
    std::ostringstream stream;
    stream << std::hex << reinterpret_cast<std::uintptr_t> (owner);
    return stream.str ();
}

bool completion_control_command (protocol::command kind) noexcept
{
    switch (kind) {
        case protocol::command::hello:
        case protocol::command::admit:
        case protocol::command::update:
        case protocol::command::reject:
        case protocol::command::livenessProbe:
        case protocol::command::livenessAck:
        case protocol::command::relocationPrepare:
        case protocol::command::relocationReady:
        case protocol::command::relocationReserved:
        case protocol::command::relocationAck:
        case protocol::command::relocationSeal:
        case protocol::command::relocationComplete:
        case protocol::command::replyRelay:
        case protocol::command::replyRelayAck:
            return true;
        default:
            return false;
    }
}

bool completion_control_size_is_bounded (
  const detail::backend::raw_message_t &parts) noexcept
{
    if (parts.empty () || parts.size () > max_completion_control_parts)
        return false;
    std::size_t total = 0;
    for (const auto &part : parts) {
        if (part.size () > max_completion_control_bytes - total)
            return false;
        total += part.size ();
    }
    return true;
}

bool application_command (protocol::command kind) noexcept
{
    switch (kind) {
        case protocol::command::nodeSend:
        case protocol::command::nodeRequest:
        case protocol::command::channelSend:
        case protocol::command::channelRequest:
        case protocol::command::spotSend:
        case protocol::command::spotRequest:
        case protocol::command::actorSend:
        case protocol::command::actorRequest:
            return true;
        default:
            return false;
    }
}

std::size_t raw_received_bytes (
  const detail::backend::raw_received_t &received) noexcept
{
    std::size_t total = 0;
    for (const auto &part : received.parts) {
        if (part.size () > std::numeric_limits<std::size_t>::max () - total)
            return std::numeric_limits<std::size_t>::max ();
        total += part.size ();
    }
    return total;
}

std::string advertised_endpoint (
  std::string bound_endpoint,
  const std::optional<std::string> &advertise_host)
{
    if (!advertise_host)
        return bound_endpoint;
    const auto port = bound_endpoint.rfind (':');
    if (!bound_endpoint.starts_with ("tcp://")
        || port == std::string::npos || port < 6) {
        throw std::invalid_argument (
          "MeshNode advertise host requires a TCP bind endpoint");
    }
    const auto host =
      advertise_host->find (':') == std::string::npos
        ? *advertise_host
        : "[" + *advertise_host + "]";
    return "tcp://" + host + bound_endpoint.substr (port);
}

std::vector<std::uint8_t> pack_infrastructure_reply (
  const detail::backend::raw_message_t &parts)
{
    std::size_t size = 1;
    for (const auto &part : parts) {
        if (part.size () > std::numeric_limits<std::uint32_t>::max ())
            throw protocol::service_wire_error_t (
              "infrastructure reply part is too large");
        size += 4 + part.size ();
    }
    std::vector<std::uint8_t> packed;
    packed.reserve (size);
    packed.push_back (static_cast<std::uint8_t> (parts.size ()));
    for (const auto &part : parts) {
        const auto length = static_cast<std::uint32_t> (part.size ());
        packed.push_back (
          static_cast<std::uint8_t> ((length >> 24u) & 0xffu));
        packed.push_back (
          static_cast<std::uint8_t> ((length >> 16u) & 0xffu));
        packed.push_back (
          static_cast<std::uint8_t> ((length >> 8u) & 0xffu));
        packed.push_back (static_cast<std::uint8_t> (length & 0xffu));
        packed.insert (packed.end (), part.begin (), part.end ());
    }
    return packed;
}

} // namespace

bool raw_mesh_byte_vector_less_t::operator() (
  const std::vector<std::uint8_t> &left,
  const std::vector<std::uint8_t> &right) const noexcept
{
    return std::lexicographical_compare (
      left.begin (), left.end (), right.begin (), right.end ());
}

void raw_mesh_connection_candidates_t::ready (
  const std::vector<std::uint8_t> &node_routing_id,
  std::vector<std::uint8_t> connection_id,
  service_connection_direction_t direction,
  std::string remote_endpoint)
{
    if (node_routing_id.empty () || connection_id.empty ())
        return;
    auto &physical = _candidates[node_routing_id];
    auto key = connection_id;
    physical.insert_or_assign (
      std::move (key),
      raw_mesh_connection_candidate_t{
        std::move (connection_id), std::move (remote_endpoint), direction,
        _next_ready_sequence++});
    if (_next_ready_sequence == 0)
        _next_ready_sequence = 1;
}

std::optional<raw_mesh_connection_candidate_t>
raw_mesh_connection_candidates_t::for_handshake (
  const std::vector<std::uint8_t> &node_routing_id,
  service_connection_direction_t preferred_direction) const
{
    const auto found = _candidates.find (node_routing_id);
    if (found == _candidates.end ())
        return std::nullopt;
    const raw_mesh_connection_candidate_t *preferred = nullptr;
    const raw_mesh_connection_candidate_t *newest = nullptr;
    for (const auto &[_, candidate] : found->second) {
        if (newest == nullptr
            || newest->ready_sequence < candidate.ready_sequence)
            newest = &candidate;
        if (candidate.direction == preferred_direction
            && (preferred == nullptr
                || preferred->ready_sequence
                     < candidate.ready_sequence))
            preferred = &candidate;
    }
    /* The public routed receive contract exposes the peer RID but not the
     * physical connection ID. The admission command supplies the connection
     * direction: hello selects the inbound candidate and admit/update selects
     * the outbound candidate. A unilateral connection has only the opposite
     * local direction for one half of the exchange, so it falls back to that
     * sole direction. Within one direction, ROUTER handover makes the most
     * recently ready physical candidate the active route. */
    return preferred != nullptr
      ? std::optional<raw_mesh_connection_candidate_t> (*preferred)
      : newest != nullptr
          ? std::optional<raw_mesh_connection_candidate_t> (*newest)
          : std::nullopt;
}

bool raw_mesh_connection_candidates_t::disconnect (
  const std::vector<std::uint8_t> &node_routing_id,
  const std::vector<std::uint8_t> &connection_id)
{
    const auto found = _candidates.find (node_routing_id);
    if (found == _candidates.end ())
        return false;
    const auto removed = found->second.erase (connection_id) != 0;
    if (found->second.empty ())
        _candidates.erase (found);
    return removed;
}

std::size_t raw_mesh_connection_candidates_t::size (
  const std::vector<std::uint8_t> &node_routing_id) const
{
    const auto found = _candidates.find (node_routing_id);
    return found == _candidates.end () ? 0 : found->second.size ();
}

raw_mesh_node_owner_t::raw_mesh_node_owner_t (raw_mesh_node_options_t options) :
    _options (std::move (options)),
    _topology (_options.descriptor),
    _mailbox (_options.application_message_budget,
              _options.application_byte_budget,
              _options.infrastructure_message_budget,
              _options.infrastructure_byte_budget),
    _operations (
      std::make_shared<foundation::operation_registry_t> (4096))
{
}

raw_mesh_node_owner_t::~raw_mesh_node_owner_t () noexcept
{
    close ();
}

void raw_mesh_node_owner_t::start ()
{
    std::lock_guard lifecycle_lock (_lifecycle_mutex);
    if (_port) {
        return;
    }
    if (_closed) {
        throw std::logic_error ("raw mesh node owner cannot restart after close");
    }
    auto context = std::make_unique<zlink::context_t> ();
    context->options ().auto_hwm_enabled (true);
    context->options ().auto_hwm_profile (
      _options.auto_hwm_profile);
    auto router = std::make_unique<zlink::router_socket_t> (*context);
    router->options ().handover (true);
    router->options ().mandatory (true);
    router->options ().linger (std::chrono::milliseconds (0));
    router->options ().send_hwm (
      zlink::byte_count_t::bytes (_options.send_high_water_mark));
    router->options ().recv_hwm (
      zlink::byte_count_t::bytes (_options.receive_high_water_mark));
    router->set_send_ready_handler ([this] {
        std::function<void ()> handler;
        {
            std::lock_guard lifecycle_lock (_lifecycle_mutex);
            handler = _send_ready_handler;
        }
        if (handler)
            handler ();
    });
    router->set_routing_id (
      zlink::routing_id_t::from (_options.descriptor.node_routing_id));
    auto monitor = std::make_unique<zlink::socket_monitor_t> (
      router->monitor_open (zlink::monitor_event::connection_ready
                            | zlink::monitor_event::disconnected));
    router->bind (_options.descriptor.advertised_endpoint);

    auto descriptor = _topology.local_descriptor ();
    if (descriptor.descriptor_revision
        == std::numeric_limits<std::uint64_t>::max ()) {
        throw std::overflow_error ("service descriptor revision is exhausted");
    }
    descriptor.advertised_endpoint = advertised_endpoint (
      router->options ().last_endpoint (), _options.advertise_host);
    ++descriptor.descriptor_revision;

    _port = std::make_shared<detail::backend::raw_route_port_t> (
      *router, &_socket_mutex,
      zlink::poll_event_flag_t::pollin
        | zlink::poll_event_flag_t::pollcompletion);
    {
        std::lock_guard control_lock (_completion_control_mutex);
        _accept_completion_controls = true;
    }
    _port->set_completion_control_handler (
      [this] (detail::backend::raw_bytes_t source,
              detail::backend::raw_message_t parts) {
          accept_completion_control (
            std::move (source), std::move (parts));
      });
    auto monitor_poller = std::make_unique<zlink::poller_t> ();
    monitor_poller->add (*monitor, zlink::poll_event_flag_t::pollin, 1);
    _monitor_poller = std::move (monitor_poller);
    _monitor = std::move (monitor);
    _router = std::move (router);
    _context = std::move (context);

    // Keep the descriptor in preparing until the receive port, completion
    // control, and monitor path can accept the first admitted message.
    descriptor.state = service_node_state_t::serving;
    _topology.publish_local (descriptor);
    _options.descriptor = descriptor;
}

void raw_mesh_node_owner_t::close () noexcept
{
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    std::unique_ptr<zlink::router_socket_t> router;
    std::unique_ptr<zlink::poller_t> monitor_poller;
    std::unique_ptr<zlink::socket_monitor_t> monitor;
    std::unique_ptr<zlink::context_t> context;
    {
        std::lock_guard control_lock (_completion_control_mutex);
        _accept_completion_controls = false;
        _completion_controls.clear ();
        _completion_control_failure.reset ();
    }
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        _closed = true;
        _pending_unadmitted_applications.clear ();
        _pending_unadmitted_application_bytes = 0;
        _pending_admissions.clear ();
        _pending_admission_bytes = 0;
        port = std::move (_port);
        monitor = std::move (_monitor);
        monitor_poller = std::move (_monitor_poller);
        router = std::move (_router);
        context = std::move (_context);
    }
    _mailbox.close ();
    _operations->shutdown ();
    if (port) {
        port->close ();
    }
    if (monitor_poller) {
        try {
            monitor_poller->close ();
        }
        catch (...) {
        }
    }
    if (monitor) {
        try {
            monitor->close ();
        }
        catch (...) {
        }
    }
    router.reset ();
    if (context) {
        try {
            context->shutdown ();
            context->term ();
        }
        catch (...) {
        }
    }
}

bool raw_mesh_node_owner_t::started () const noexcept
{
    std::lock_guard lifecycle_lock (_lifecycle_mutex);
    return static_cast<bool> (_port);
}

void raw_mesh_node_owner_t::set_send_ready_handler (
  std::function<void ()> handler)
{
    std::lock_guard lifecycle_lock (_lifecycle_mutex);
    _send_ready_handler = std::move (handler);
}

std::string raw_mesh_node_owner_t::endpoint () const
{
    return _topology.local_descriptor ().advertised_endpoint;
}

zlink::context_t &raw_mesh_node_owner_t::context ()
{
    std::lock_guard lifecycle_lock (_lifecycle_mutex);
    if (!_context) {
        throw std::logic_error ("raw mesh node owner is not started");
    }
    return *_context;
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
    std::lock_guard lifecycle_lock (_lifecycle_mutex);
    if (!_router || endpoint.empty ()) {
        return false;
    }
    try {
        std::lock_guard socket_lock (_socket_mutex);
        _router->connect (endpoint);
        _outbound_endpoints.insert (endpoint);
        return true;
    }
    catch (...) {
        return false;
    }
}

bool raw_mesh_node_owner_t::connect_peer (
  const std::string &endpoint,
  service_node_descriptor_t expected_descriptor)
{
    std::lock_guard lifecycle_lock (_lifecycle_mutex);
    if (!_router || endpoint.empty ()) {
        return false;
    }
    _expected_peers.insert_or_assign (
      expected_descriptor.node_routing_id, expected_descriptor);
    try {
        std::lock_guard socket_lock (_socket_mutex);
        _router->connect (endpoint);
        _outbound_endpoints.insert (endpoint);
        return true;
    }
    catch (...) {
        return false;
    }
}

void raw_mesh_node_owner_t::expect_peer (
  service_node_descriptor_t expected_descriptor)
{
    if (expected_descriptor.node_routing_id.empty ()
        || expected_descriptor.advertised_endpoint.empty ())
        return;
    std::lock_guard lifecycle_lock (_lifecycle_mutex);
    _expected_peers.insert_or_assign (
      expected_descriptor.node_routing_id,
      std::move (expected_descriptor));
}

void raw_mesh_node_owner_t::forget_peer (
  const std::vector<std::uint8_t> &node_routing_id,
  const std::string &endpoint)
{
    std::lock_guard lifecycle_lock (_lifecycle_mutex);
    const auto found = _expected_peers.find (node_routing_id);
    if (found != _expected_peers.end ()
        && found->second.advertised_endpoint == endpoint)
        _expected_peers.erase (found);
}

peer_admission_result_t raw_mesh_node_owner_t::admit_peer (
  service_node_descriptor_t descriptor,
  std::vector<std::uint8_t> connection_id,
  service_liveness_registry_t::clock_t::time_point now)
{
    std::lock_guard lifecycle_lock (_lifecycle_mutex);
    if (!_router) {
        return peer_admission_result_t::invalid_descriptor;
    }
    auto node_routing_id = descriptor.node_routing_id;
    const auto lifecycle_generation = descriptor.lifecycle_generation;
    auto liveness_connection_id = connection_id;
    const auto admitted =
      _topology.admit (std::move (descriptor), std::move (connection_id));
    if (admitted != peer_admission_result_t::admitted) {
        return admitted;
    }
    _liveness.admit (std::move (node_routing_id),
                     std::move (liveness_connection_id), now);
    return admitted;
}

bool raw_mesh_node_owner_t::send_to_node (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::application_payload_t &application_payload)
{
    return send_with_header (
      target_routing_id, protocol::encode_node_send_header (),
      application_payload);
}

bool raw_mesh_node_owner_t::request_to_node (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::application_payload_t &application_payload,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback)
{
    return request_to_target (
      target_routing_id, application_payload, timeout, std::move (callback),
      std::nullopt);
}

bool raw_mesh_node_owner_t::request_to_channel (
  const std::string &channel_name,
  const protocol::application_payload_t &application_payload,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback)
{
    const auto selected = _topology.select (channel_name);
    if (!selected) {
        return false;
    }
    return request_to_target (
      selected->descriptor.node_routing_id, application_payload, timeout,
      std::move (callback), channel_name);
}

bool raw_mesh_node_owner_t::request_to_target (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::application_payload_t &application_payload,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback,
  const std::optional<std::string> &channel_name)
{
    return request_with_header (
      target_routing_id,
      [channel_name] (std::uint64_t correlation) {
          return channel_name
                   ? protocol::encode_channel_request_header (
                       correlation, *channel_name)
                   : protocol::encode_node_request_header (correlation);
      },
      application_payload, timeout, std::move (callback));
}

bool raw_mesh_node_owner_t::submit_request (
  const std::shared_ptr<detail::backend::raw_route_port_t> &port,
  const pending_request_t &request,
  std::chrono::milliseconds timeout)
{
    if (!port || timeout <= std::chrono::milliseconds::zero ()) {
        (void) _operations->fail (
          request.operation, foundation::operation_terminal_t::transport_failed);
        return false;
    }
    const auto operations = _operations;
    trace_mesh (
      "request-submit correlation="
        + std::to_string (request.correlation)
        + " targetBytes="
        + std::to_string (request.target_routing_id.size ()));
    const auto submitted = port->request (
      request.target_routing_id, request.wire, timeout,
      [operations, id = request.operation, correlation = request.correlation] (
        detail::backend::raw_request_result_t result,
        detail::backend::raw_message_t parts) {
          trace_mesh (
            "request-completion correlation="
              + std::to_string (correlation)
              + " result="
              + std::to_string (static_cast<int> (result))
              + " parts=" + std::to_string (parts.size ()));
          if (result != detail::backend::raw_request_result_t::ok) {
              const auto terminal =
                result == detail::backend::raw_request_result_t::timed_out
                  ? foundation::operation_terminal_t::timed_out
                : result == detail::backend::raw_request_result_t::terminated
                  ? foundation::operation_terminal_t::shutdown
                  : foundation::operation_terminal_t::transport_failed;
              (void) operations->fail (id, terminal);
              return;
          }
          try {
              if (parts.empty () || parts.size () > 2) {
                  throw protocol::service_wire_error_t (
                    "request reply has an invalid part count");
              }
              const auto reply =
                protocol::decode_reply_header (parts.front ());
              if (reply.correlation != correlation) {
                  throw protocol::service_wire_error_t (
                    "request reply correlation does not match");
              }
              if (reply.terminal_result != 0) {
                  if (parts.size () != 1) {
                      throw protocol::service_wire_error_t (
                        "failed request reply cannot carry a payload");
                  }
                  (void) operations->fail (
                    id, foundation::operation_terminal_t::transport_failed);
                  return;
              }
              if (parts.size () != 2) {
                  throw protocol::service_wire_error_t (
                    "successful request reply must carry a payload");
              }
              (void) protocol::decode_application_payload (parts[1]);
              (void) operations->complete (id, std::move (parts[1]));
          }
          catch (const protocol::service_wire_error_t &) {
              (void) operations->fail (
                id, foundation::operation_terminal_t::transport_failed);
          }
      });
    if (!submitted) {
        (void) _operations->fail (
          request.operation, foundation::operation_terminal_t::transport_failed);
    }
    return submitted;
}

void raw_mesh_node_owner_t::trace_admission_phase (
  const std::vector<std::uint8_t> &node_routing_id,
  std::uint64_t lifecycle_generation,
  protocol::command command,
  peer_admission_result_t result)
{
    if (result == peer_admission_result_t::not_required) {
        return;
    }
    const auto peer = _topology.peer (node_routing_id);
    const auto peer_generation =
      peer ? peer->descriptor.lifecycle_generation : 0;
    const auto admission_context =
      " owner=" + trace_owner_key (this)
      + " source=" + owner_key (node_routing_id)
      + " result=" + std::to_string (static_cast<int> (result))
      + " peer=" + (peer ? std::string ("present") : "absent")
      + " peerGeneration=" + std::to_string (peer_generation);
    if (command == protocol::command::hello
        && result == peer_admission_result_t::admitted) {
        trace_mesh (
          "handshake phase=local-admission-awaiting-remote-admit"
          + admission_context);
        return;
    }
    if (command != protocol::command::admit
        || (result != peer_admission_result_t::admitted
            && result != peer_admission_result_t::duplicate_connection))
        return;
    if (lifecycle_generation != 0)
        trace_mesh ("handshake phase=bilateral-ready" + admission_context);
}

void raw_mesh_node_owner_t::discard_pending_unadmitted_applications (
  const std::vector<std::uint8_t> &node_routing_id)
{
    std::lock_guard lifecycle_lock (_lifecycle_mutex);
    std::size_t discarded = 0;
    for (auto pending = _pending_unadmitted_applications.begin ();
         pending != _pending_unadmitted_applications.end ();) {
        if (pending->received.source_routing_id != node_routing_id) {
            ++pending;
            continue;
        }
        _pending_unadmitted_application_bytes -= pending->bytes;
        pending = _pending_unadmitted_applications.erase (pending);
        ++discarded;
    }
    for (auto pending = _pending_admissions.begin ();
         pending != _pending_admissions.end ();) {
        if (pending->received.source_routing_id != node_routing_id) {
            ++pending;
            continue;
        }
        _pending_admission_bytes -= pending->bytes;
        pending = _pending_admissions.erase (pending);
        ++discarded;
    }
    if (discarded != 0)
        trace_mesh (
          "application-discard reason=peer-disconnected count="
            + std::to_string (discarded)
            + " pending="
            + std::to_string (_pending_unadmitted_applications.size ()));
}

bool raw_mesh_node_owner_t::request_with_header (
  const std::vector<std::uint8_t> &target_routing_id,
  const std::function<std::vector<std::uint8_t> (std::uint64_t)> &header,
  const protocol::application_payload_t &application_payload,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback)
{
    if (timeout <= std::chrono::milliseconds::zero ()) {
        throw std::invalid_argument ("raw mesh request timeout must be positive");
    }
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    std::uint64_t correlation = 0;
    const auto local = _topology.local_descriptor ();
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
        if (!port) {
            return false;
        }
        correlation = _next_correlation++;
        if (correlation == 0 || _next_correlation == 0) {
            _next_correlation = 1;
            throw std::overflow_error ("raw mesh request correlation is exhausted");
        }
    }
    const auto id =
      operation_id (local.lifecycle_generation, correlation);
    if (!_operations->register_operation (
          id, foundation::operation_registry_t::clock_t::now () + timeout,
          std::move (callback))) {
        return false;
    }
    pending_request_t request{
      target_routing_id,
      {header (correlation),
       protocol::encode_application_payload (application_payload)},
      id,
      correlation};
    return submit_request (port, request, timeout);
}

bool raw_mesh_node_owner_t::send_with_header (
  const std::vector<std::uint8_t> &target_routing_id,
  std::vector<std::uint8_t> header,
  const protocol::application_payload_t &application_payload)
{
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port) {
        return false;
    }
    return port->send (
      target_routing_id,
      {std::move (header),
       protocol::encode_application_payload (application_payload)});
}

bool raw_mesh_node_owner_t::send_header_only (
  const std::vector<std::uint8_t> &target_routing_id,
  std::vector<std::uint8_t> header)
{
    bool use_completion = false;
    try {
        const auto decoded = protocol::decode_header (header);
        const auto admission =
          decoded.kind == protocol::command::hello
          || decoded.kind == protocol::command::admit
          || decoded.kind == protocol::command::update
          || decoded.kind == protocol::command::reject;
        use_completion =
          completion_control_command (decoded.kind)
          && (admission || _topology.peer (target_routing_id));
    }
    catch (const protocol::service_wire_error_t &) {
    }
    if (use_completion)
        return send_completion_control (
          target_routing_id, {std::move (header)});
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    return port && port->send (target_routing_id, {std::move (header)});
}

bool raw_mesh_node_owner_t::send_completion_control (
  const std::vector<std::uint8_t> &target_routing_id,
  detail::backend::raw_message_t parts)
{
    if (!completion_control_size_is_bounded (parts))
        return false;
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    return port
           && port->send_completion_control (
             target_routing_id, parts);
}

bool raw_mesh_node_owner_t::send_session_relocation_route (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::session_relocation_route_t &route)
{
    return send_header_only (
      target_routing_id,
      protocol::encode_session_relocation_route (route));
}

bool raw_mesh_node_owner_t::send_session_relocation_seal (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::session_relocation_seal_t &seal)
{
    return send_header_only (
      target_routing_id,
      protocol::encode_session_relocation_seal (seal));
}

bool raw_mesh_node_owner_t::request_session_relocation_seal (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::session_relocation_seal_t &seal,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback)
{
    if (timeout <= std::chrono::milliseconds::zero ())
        throw std::invalid_argument (
          "Session relocation seal timeout must be positive");
    const auto operation = operation_id (
      seal.relocation.high, seal.relocation.low);
    if (!_operations->register_operation (
          operation,
          foundation::operation_registry_t::clock_t::now () + timeout,
          std::move (callback)))
        return false;
    if (send_session_relocation_seal (target_routing_id, seal))
        return true;
    (void) _operations->fail (
      operation, foundation::operation_terminal_t::transport_failed);
    return false;
}

bool raw_mesh_node_owner_t::send_session_relocation_sealed (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::session_relocation_sealed_t &sealed)
{
    return send_header_only (
      target_routing_id,
      protocol::encode_session_relocation_sealed (sealed));
}

bool raw_mesh_node_owner_t::request_session_relocation_route (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::session_relocation_route_t &route,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback)
{
    if (timeout <= std::chrono::milliseconds::zero ())
        throw std::invalid_argument (
          "Session relocation route timeout must be positive");
    const auto operation = operation_id (
      route.relocation.high, route.relocation.low);
    if (!_operations->register_operation (
          operation,
          foundation::operation_registry_t::clock_t::now () + timeout,
          std::move (callback))) {
        return false;
    }
    if (send_session_relocation_route (target_routing_id, route))
        return true;
    (void) _operations->fail (
      operation, foundation::operation_terminal_t::transport_failed);
    return false;
}

bool raw_mesh_node_owner_t::send_session_relocation_routed (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::session_relocation_routed_t &routed)
{
    return send_header_only (
      target_routing_id,
      protocol::encode_session_relocation_routed (routed));
}

bool raw_mesh_node_owner_t::send_reply_relay (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::reply_relay_t &relay,
  std::optional<protocol::application_payload_t> application_reply)
{
    if (relay.terminal_result != 0 && application_reply) {
        throw std::invalid_argument (
          "failed reply relay cannot carry an application payload");
    }
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port) {
        return false;
    }
    std::vector<std::vector<std::uint8_t>> parts;
    parts.emplace_back (protocol::encode_reply_relay (relay));
    if (application_reply) {
        parts.emplace_back (
          protocol::encode_application_payload (*application_reply));
    }
    if (!application_reply
        && _topology.peer (target_routing_id)
        && completion_control_size_is_bounded (parts)) {
        return port->send_completion_control (
          target_routing_id, parts);
    }
    return port->send (target_routing_id, parts);
}

bool raw_mesh_node_owner_t::send_reply_relay_ack (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::reply_relay_ack_t &ack)
{
    return send_header_only (
      target_routing_id, protocol::encode_reply_relay_ack (ack));
}

bool raw_mesh_node_owner_t::send_message_follow (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::message_follow_notice_t &notice)
{
    return send_header_only (
      target_routing_id, protocol::encode_message_follow (notice));
}

bool raw_mesh_node_owner_t::send_relocation_control (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::relocation_control_t &control)
{
    return send_header_only (
      target_routing_id, protocol::encode_relocation_control (control));
}

bool raw_mesh_node_owner_t::reply (
  const service_mailbox_record_t &request,
  const protocol::application_payload_t &application_payload)
{
    if (request.source_routing_id.empty () || !request.request_sequence
        || !request.correlation) {
        throw std::invalid_argument (
          "raw mesh reply requires a request mailbox record");
    }
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port) {
        return false;
    }
    return port->reply (
      detail::backend::raw_received_t{
        request.source_routing_id, request.request_sequence, {}},
      {protocol::encode_reply_header (*request.correlation, 0, 0),
       protocol::encode_application_payload (application_payload)});
}

bool raw_mesh_node_owner_t::reply_failure (
  const service_mailbox_record_t &request,
  std::uint32_t terminal_result,
  std::uint32_t failure_code)
{
    if (request.source_routing_id.empty () || !request.request_sequence
        || !request.correlation || terminal_result == 0) {
        throw std::invalid_argument (
          "raw mesh failed reply requires a request and terminal result");
    }
    const auto local = _topology.local_descriptor ();
    if (request.source_routing_id == local.node_routing_id) {
        return _operations->fail (
          operation_id (
            local.lifecycle_generation,
            *request.correlation),
          foundation::operation_terminal_t::transport_failed);
    }
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port) {
        return false;
    }
    return port->reply (
      detail::backend::raw_received_t{
        request.source_routing_id, request.request_sequence, {}},
      {protocol::encode_reply_header (
        *request.correlation, terminal_result, failure_code)});
}

std::size_t raw_mesh_node_owner_t::expire_requests (
  foundation::operation_registry_t::clock_t::time_point now)
{
    static_cast<void> (now);
    return _operations->expire (now);
}

bool raw_mesh_node_owner_t::send_to_channel (
  const std::string &channel_name,
  const protocol::application_payload_t &application_payload)
{
    const auto selected = _topology.select (channel_name);
    if (!selected) {
        return false;
    }
    return send_with_header (
      selected->descriptor.node_routing_id,
      protocol::encode_channel_send_header (channel_name),
      application_payload);
}

bool raw_mesh_node_owner_t::send_to_spot (
  const std::vector<std::uint8_t> &target_routing_id,
  const std::string &source_spot_id,
  const protocol::spot_route_fence_t &target,
  const protocol::application_payload_t &application_payload)
{
    const auto sequence = next_operation_sequence ();
    const auto local = _topology.local_descriptor ();
    return send_with_header (
      target_routing_id,
      protocol::encode_spot_message_header (
        protocol::command::spotSend, source_spot_id, target,
        {local.lifecycle_generation, sequence}),
      application_payload);
}

bool raw_mesh_node_owner_t::request_to_spot (
  const std::vector<std::uint8_t> &target_routing_id,
  const std::string &source_spot_id,
  const protocol::spot_route_fence_t &target,
  const protocol::application_payload_t &application_payload,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback,
  std::optional<protocol::wire_operation_id_t> operation)
{
    const auto local = _topology.local_descriptor ();
    return request_with_header (
      target_routing_id,
      [source_spot_id, target, local, operation] (std::uint64_t correlation) {
          const auto exact = operation.value_or (
            protocol::wire_operation_id_t{
              local.lifecycle_generation, correlation});
          return protocol::encode_spot_message_header (
            protocol::command::spotRequest, source_spot_id,
            target, exact, correlation);
      },
      application_payload, timeout, std::move (callback));
}

bool raw_mesh_node_owner_t::send_to_actor (
  const std::vector<std::uint8_t> &target_routing_id,
  const std::optional<std::pair<std::string, std::uint64_t>> &source_actor,
  const protocol::actor_route_fence_t &target,
  const protocol::application_payload_t &application_payload)
{
    const auto sequence = next_operation_sequence ();
    const auto local = _topology.local_descriptor ();
    return send_with_header (
      target_routing_id,
      protocol::encode_actor_message_header (
        protocol::command::actorSend, source_actor, target,
        {local.lifecycle_generation, sequence}),
      application_payload);
}

bool raw_mesh_node_owner_t::request_to_actor (
  const std::vector<std::uint8_t> &target_routing_id,
  const std::optional<std::pair<std::string, std::uint64_t>> &source_actor,
  const protocol::actor_route_fence_t &target,
  const protocol::application_payload_t &application_payload,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback,
  std::optional<protocol::wire_operation_id_t> operation)
{
    const auto local = _topology.local_descriptor ();
    return request_with_header (
      target_routing_id,
      [source_actor, target, local, operation] (std::uint64_t correlation) {
          const auto exact = operation.value_or (
            protocol::wire_operation_id_t{
              local.lifecycle_generation, correlation});
          return protocol::encode_actor_message_header (
            protocol::command::actorRequest, source_actor, target,
            exact, correlation);
      },
      application_payload, timeout, std::move (callback));
}

bool raw_mesh_node_owner_t::request_user_spot_create (
  const std::vector<std::uint8_t> &target_routing_id,
  protocol::user_spot_create_header_t request,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback)
{
    const auto local = _topology.local_descriptor ();
    if (request.source_node_routing_id != local.node_routing_id
        || request.source_node_generation
             != local.lifecycle_generation
        || request.reservation.target_node_routing_id
             != target_routing_id) {
        throw std::invalid_argument (
          "User Spot create source or target fence is inconsistent");
    }
    return request_infrastructure (
      target_routing_id,
      [request = std::move (request)] (
        std::uint64_t correlation) mutable {
          request.correlation = correlation;
          return protocol::encode_user_spot_create_header (request);
      },
      [] (const detail::backend::raw_message_t &parts) {
          if (parts.empty () || parts.size () > 2)
              throw protocol::service_wire_error_t (
                "User Spot create reply has an invalid part count");
          const auto reply =
            protocol::decode_user_spot_create_reply (parts.front ());
          if (reply.header.terminal_result != 0
              && parts.size () != 1)
              throw protocol::service_wire_error_t (
                "failed User Spot create reply carries a payload");
          if (parts.size () == 2)
              (void) protocol::decode_application_payload (parts[1]);
          return pack_infrastructure_reply (parts);
      },
      timeout, std::move (callback));
}

bool raw_mesh_node_owner_t::request_actor_create (
  const std::vector<std::uint8_t> &target_routing_id,
  protocol::actor_create_header_t request,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback)
{
    const auto local = _topology.local_descriptor ();
    if (request.source_node_routing_id != local.node_routing_id
        || request.source_node_generation != local.lifecycle_generation
        || request.reservation.target_node_routing_id
             != target_routing_id)
        throw std::invalid_argument (
          "Actor create source or target fence is inconsistent");
    return request_infrastructure (
      target_routing_id,
      [request = std::move (request)] (
        std::uint64_t correlation) mutable {
          request.correlation = correlation;
          return protocol::encode_actor_create_header (request);
      },
      [] (const detail::backend::raw_message_t &parts) {
          if (parts.empty () || parts.size () > 2)
              throw protocol::service_wire_error_t (
                "Actor create reply has an invalid part count");
          const auto reply =
            protocol::decode_actor_create_reply (parts.front ());
          if (reply.header.terminal_result != 0
              && parts.size () != 1)
              throw protocol::service_wire_error_t (
                "failed Actor create reply carries a payload");
          if (parts.size () == 2)
              (void) protocol::decode_application_payload (parts[1]);
          return pack_infrastructure_reply (parts);
      }, timeout, std::move (callback));
}

bool raw_mesh_node_owner_t::send_instance_spot_activation (
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
    detail::backend::raw_message_t parts{
      protocol::encode_instance_spot_activation_header (request)};
    if (metadata)
        parts.push_back (std::move (*metadata));
    parts.push_back (
      protocol::encode_application_payload (application_payload));
    if (target_routing_id == local.node_routing_id) {
        return _mailbox.try_enqueue (
          service_mailbox_record_t{
            owner_key (local.node_routing_id),
            service_mailbox_domain_t::infrastructure,
            std::move (parts), local.node_routing_id,
            std::nullopt, std::nullopt});
    }
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    return port && port->send (target_routing_id, std::move (parts));
}

bool raw_mesh_node_owner_t::request_instance_spot_activation (
  const std::vector<std::uint8_t> &target_routing_id,
  protocol::instance_spot_activation_header_t request,
  std::optional<std::vector<std::uint8_t>> metadata,
  protocol::application_payload_t application_payload,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback)
{
    if (timeout <= std::chrono::milliseconds::zero ()) {
        throw std::invalid_argument (
          "Instance Spot activation timeout must be positive");
    }
    const auto local = _topology.local_descriptor ();
    if (request.source_node_routing_id != local.node_routing_id
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
            return false;
        }
        correlation = _next_correlation++;
        if (correlation == 0 || _next_correlation == 0) {
            _next_correlation = 1;
            throw std::overflow_error (
              "raw mesh request correlation is exhausted");
        }
    }
    request.reply_route_id = request.request ? correlation : 0;
    detail::backend::raw_message_t parts{
      protocol::encode_instance_spot_activation_header (request)};
    if (metadata)
        parts.push_back (std::move (*metadata));
    parts.push_back (
      protocol::encode_application_payload (application_payload));
    const auto id = operation_id (local.lifecycle_generation, correlation);
    if (!_operations->register_operation (
          id, foundation::operation_registry_t::clock_t::now () + timeout,
          std::move (callback))) {
        return false;
    }
    if (target_routing_id == local.node_routing_id) {
        const auto accepted = _mailbox.try_enqueue (
          service_mailbox_record_t{
            owner_key (local.node_routing_id),
            service_mailbox_domain_t::infrastructure,
            std::move (parts), local.node_routing_id,
            correlation, correlation});
        if (!accepted)
            (void) _operations->fail (
              id, foundation::operation_terminal_t::transport_failed);
        return accepted;
    }
    const auto operations = _operations;
    trace_mesh (
      "infrastructure-request-submit correlation="
        + std::to_string (correlation)
        + " targetBytes=" + std::to_string (target_routing_id.size ()));
    const auto submitted = port->request (
      target_routing_id, std::move (parts), timeout,
      [operations, id, correlation] (
        detail::backend::raw_request_result_t result,
        detail::backend::raw_message_t reply_parts) mutable {
          if (result != detail::backend::raw_request_result_t::ok) {
              const auto terminal =
                result == detail::backend::raw_request_result_t::timed_out
                  ? foundation::operation_terminal_t::timed_out
                  : result == detail::backend::raw_request_result_t::terminated
                    ? foundation::operation_terminal_t::shutdown
                    : foundation::operation_terminal_t::transport_failed;
              (void) operations->fail (id, terminal);
              return;
          }
          try {
              if (reply_parts.empty () || reply_parts.size () > 2)
                  throw protocol::service_wire_error_t (
                    "Instance Spot activation reply has an invalid part count");
              const auto reply =
                protocol::decode_reply_header (reply_parts.front ());
              if (reply.correlation != correlation)
                  throw protocol::service_wire_error_t (
                    "Instance Spot activation reply correlation does not match");
              if (reply.terminal_result != 0 && reply_parts.size () != 1)
                  throw protocol::service_wire_error_t (
                    "failed Instance Spot activation reply carries a payload");
              if (reply_parts.size () == 2)
                  (void) protocol::decode_application_payload (reply_parts[1]);
              (void) operations->complete (
                id, pack_infrastructure_reply (reply_parts));
          }
          catch (const protocol::service_wire_error_t &) {
              (void) operations->fail (
                id, foundation::operation_terminal_t::transport_failed);
          }
      });
    if (!submitted)
        (void) _operations->fail (
          id, foundation::operation_terminal_t::transport_failed);
    return submitted;
}

bool raw_mesh_node_owner_t::request_user_spot_close (
  const std::vector<std::uint8_t> &target_routing_id,
  protocol::user_spot_close_header_t request,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback)
{
    const auto local = _topology.local_descriptor ();
    if (request.source_node_routing_id != local.node_routing_id
        || request.source_node_generation
             != local.lifecycle_generation
        || request.target.target_node_routing_id != target_routing_id) {
        throw std::invalid_argument (
          "User Spot close source or target fence is inconsistent");
    }
    return request_infrastructure (
      target_routing_id,
      [request = std::move (request)] (
        std::uint64_t correlation) mutable {
          request.correlation = correlation;
          return protocol::encode_user_spot_close_header (request);
      },
      [] (const detail::backend::raw_message_t &parts) {
          if (parts.size () != 1)
              throw protocol::service_wire_error_t (
                "User Spot close reply must contain one header");
          (void) protocol::decode_user_spot_close_reply (parts.front ());
          return pack_infrastructure_reply (parts);
      },
      timeout, std::move (callback));
}

bool raw_mesh_node_owner_t::request_infrastructure (
  const std::vector<std::uint8_t> &target_routing_id,
  const std::function<std::vector<std::uint8_t> (std::uint64_t)> &header,
  const std::function<std::vector<std::uint8_t> (
    const detail::backend::raw_message_t &)> &decode_reply,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback)
{
    if (timeout <= std::chrono::milliseconds::zero ()) {
        throw std::invalid_argument (
          "raw mesh infrastructure request timeout must be positive");
    }
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    std::uint64_t correlation = 0;
    const auto local = _topology.local_descriptor ();
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
        if (!port) {
            return false;
        }
        correlation = _next_correlation++;
        if (correlation == 0 || _next_correlation == 0) {
            _next_correlation = 1;
            throw std::overflow_error (
              "raw mesh request correlation is exhausted");
        }
    }
    const auto id =
      operation_id (local.lifecycle_generation, correlation);
    if (!_operations->register_operation (
          id, foundation::operation_registry_t::clock_t::now () + timeout,
          std::move (callback))) {
        return false;
    }
    if (target_routing_id == local.node_routing_id) {
        const auto accepted = _mailbox.try_enqueue (
          service_mailbox_record_t{
            owner_key (local.node_routing_id),
            service_mailbox_domain_t::infrastructure,
            {header (correlation)},
            local.node_routing_id,
            correlation,
            correlation});
        if (!accepted)
            (void) _operations->fail (
              id,
              foundation::operation_terminal_t::transport_failed);
        return accepted;
    }
    const auto operations = _operations;
    const auto submitted = port->request (
      target_routing_id, {header (correlation)}, timeout,
      [operations, id, correlation, decode_reply] (
        detail::backend::raw_request_result_t result,
        detail::backend::raw_message_t parts) {
          if (result != detail::backend::raw_request_result_t::ok) {
              const auto terminal =
                result == detail::backend::raw_request_result_t::timed_out
                  ? foundation::operation_terminal_t::timed_out
                : result == detail::backend::raw_request_result_t::terminated
                  ? foundation::operation_terminal_t::shutdown
                  : foundation::operation_terminal_t::transport_failed;
              (void) operations->fail (id, terminal);
              return;
          }
          try {
              if (parts.empty ()) {
                  throw protocol::service_wire_error_t (
                    "infrastructure reply has no header");
              }
              const auto prefix =
                protocol::decode_reply_header (
                  std::span<const std::uint8_t> (
                    parts.front ().data (),
                    std::min<std::size_t> (
                      parts.front ().size (), 21)));
              if (prefix.correlation != correlation) {
                  throw protocol::service_wire_error_t (
                    "infrastructure reply correlation does not match");
              }
              auto payload = decode_reply (parts);
              (void) operations->complete (
                id, std::move (payload));
          }
          catch (const protocol::service_wire_error_t &) {
              (void) operations->fail (
                id, foundation::operation_terminal_t::transport_failed);
          }
      });
    trace_mesh (
      "infrastructure-request-result correlation="
        + std::to_string (correlation)
        + " accepted="
        + (submitted ? std::string ("true") : std::string ("false")));
    if (!submitted) {
        (void) _operations->fail (
          id, foundation::operation_terminal_t::transport_failed);
    }
    return submitted;
}

bool raw_mesh_node_owner_t::reply_infrastructure (
  const service_mailbox_record_t &request,
  std::vector<std::uint8_t> header)
{
    if (request.source_routing_id.empty () || !request.request_sequence
        || !request.correlation) {
        throw std::invalid_argument (
          "raw mesh infrastructure reply requires a request record");
    }
    const auto local = _topology.local_descriptor ();
    if (request.source_routing_id == local.node_routing_id) {
        return _operations->complete (
          operation_id (
            local.lifecycle_generation,
            *request.correlation),
          pack_infrastructure_reply (
            {std::move (header)}));
    }
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    return port
           && port->reply (
             detail::backend::raw_received_t{
               request.source_routing_id, request.request_sequence, {}},
             {std::move (header)});
}

bool raw_mesh_node_owner_t::reply_user_spot_create (
  const service_mailbox_record_t &request,
  const protocol::user_spot_create_reply_t &reply,
  std::optional<protocol::application_payload_t> application_reply)
{
    if (!request.correlation
        || reply.header.correlation != *request.correlation) {
        throw std::invalid_argument (
          "User Spot create reply correlation does not match");
    }
    if (reply.header.terminal_result != 0 && application_reply)
        throw std::invalid_argument (
          "failed User Spot create reply cannot carry a payload");
    const auto local = _topology.local_descriptor ();
    if (request.source_routing_id == local.node_routing_id) {
        detail::backend::raw_message_t parts{
          protocol::encode_user_spot_create_reply (
            reply.header.correlation,
            reply.header.terminal_result,
            reply.header.failure_code,
            reply.result,
            reply.spot_id,
            reply.object_generation)};
        if (application_reply)
            parts.push_back (
              protocol::encode_application_payload (
                *application_reply));
        return _operations->complete (
          operation_id (
            local.lifecycle_generation,
            *request.correlation),
          pack_infrastructure_reply (parts));
    }
    if (!application_reply)
        return reply_infrastructure (
          request,
          protocol::encode_user_spot_create_reply (
            reply.header.correlation, reply.header.terminal_result,
            reply.header.failure_code, reply.result,
            reply.spot_id, reply.object_generation));
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    return port
           && port->reply (
             detail::backend::raw_received_t{
               request.source_routing_id, request.request_sequence, {}},
             {protocol::encode_user_spot_create_reply (
                reply.header.correlation, reply.header.terminal_result,
                reply.header.failure_code, reply.result,
                reply.spot_id, reply.object_generation),
              protocol::encode_application_payload (*application_reply)});
}

bool raw_mesh_node_owner_t::reply_actor_create (
  const service_mailbox_record_t &request,
  const protocol::actor_create_reply_t &reply,
  std::optional<protocol::application_payload_t> application_reply)
{
    if (!request.correlation
        || reply.header.correlation != *request.correlation)
        throw std::invalid_argument (
          "Actor create reply correlation does not match");
    if (reply.header.terminal_result != 0 && application_reply)
        throw std::invalid_argument (
          "failed Actor create reply cannot carry a payload");
    detail::backend::raw_message_t parts{
      protocol::encode_actor_create_reply (
        reply.header.correlation,
        reply.header.terminal_result,
        reply.header.failure_code,
        reply.result,
        reply.node_routing_id,
        reply.actor_id,
        reply.object_generation)};
    if (application_reply)
        parts.push_back (protocol::encode_application_payload (
          *application_reply));
    const auto local = _topology.local_descriptor ();
    if (request.source_routing_id == local.node_routing_id)
        return _operations->complete (
          operation_id (local.lifecycle_generation,
                        *request.correlation),
          pack_infrastructure_reply (parts));
    if (parts.size () == 1)
        return reply_infrastructure (
          request, std::move (parts.front ()));
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    return port && port->reply (
      detail::backend::raw_received_t{
        request.source_routing_id, request.request_sequence, {}},
      std::move (parts));
}

bool raw_mesh_node_owner_t::reply_instance_spot_activation (
  const service_mailbox_record_t &request,
  std::uint32_t terminal_result,
  std::uint32_t failure_code,
  std::optional<protocol::application_payload_t> application_reply)
{
    if (!request.correlation) {
        throw std::invalid_argument (
          "Instance Spot activation reply requires correlation");
    }
    if (terminal_result != 0 && application_reply) {
        throw std::invalid_argument (
          "failed Instance Spot activation reply cannot carry a payload");
    }
    detail::backend::raw_message_t parts{
      protocol::encode_reply_header (*request.correlation,
                                     terminal_result,
                                     failure_code)};
    if (application_reply)
        parts.push_back (
          protocol::encode_application_payload (*application_reply));
    const auto local = _topology.local_descriptor ();
    if (request.source_routing_id == local.node_routing_id) {
        return _operations->complete (
          operation_id (local.lifecycle_generation,
                        *request.correlation),
          pack_infrastructure_reply (parts));
    }
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    return port
           && port->reply (
             detail::backend::raw_received_t{
               request.source_routing_id,
               request.request_sequence, {}},
             std::move (parts));
}

bool raw_mesh_node_owner_t::reply_user_spot_close (
  const service_mailbox_record_t &request,
  const protocol::user_spot_close_reply_t &reply)
{
    if (!request.correlation
        || reply.header.correlation != *request.correlation) {
        throw std::invalid_argument (
          "User Spot close reply correlation does not match");
    }
    return reply_infrastructure (
      request,
      protocol::encode_user_spot_close_reply (
        reply.header.correlation, reply.header.terminal_result,
        reply.header.failure_code, reply.closed));
}

raw_mesh_pump_result_t raw_mesh_node_owner_t::enqueue_received_or_retain (
  service_mailbox_record_t record,
  raw_mesh_pump_result_t accepted_result)
{
    if (_mailbox.try_enqueue (std::move (record))) {
        return accepted_result;
    }
    if (_pending_received) {
        throw std::logic_error (
          "raw mesh owner already retains a received mailbox record");
    }
    _pending_received.emplace (
      pending_received_mailbox_record_t{
        std::move (record), accepted_result});
    return raw_mesh_pump_result_t::backpressured;
}

void raw_mesh_node_owner_t::accept_completion_control (
  detail::backend::raw_bytes_t source_routing_id,
  detail::backend::raw_message_t parts)
{
    const auto mark_failure = [this, &source_routing_id] (
                                raw_mesh_pump_result_t result) {
        std::lock_guard lock (_completion_control_mutex);
        if (_accept_completion_controls && !_completion_control_failure) {
            _completion_control_failure.emplace (
              completion_control_failure_t{
                source_routing_id, result});
        }
    };
    if (!completion_control_size_is_bounded (parts)) {
        mark_failure (raw_mesh_pump_result_t::protocol_error);
        return;
    }
    try {
        const auto header = protocol::decode_header (parts.front ());
        if (!completion_control_command (header.kind)) {
            mark_failure (raw_mesh_pump_result_t::protocol_error);
            return;
        }
        if (parts.size () != 1) {
            mark_failure (raw_mesh_pump_result_t::protocol_error);
            return;
        }

        std::optional<std::uint64_t> admitted_generation;
        if (header.kind != protocol::command::hello
            && header.kind != protocol::command::admit
            && header.kind != protocol::command::update
            && header.kind != protocol::command::reject) {
            const auto peer = _topology.peer (source_routing_id);
            if (!peer)
                return;
            admitted_generation =
              peer->descriptor.lifecycle_generation;
        }

        std::lock_guard lock (_completion_control_mutex);
        if (!_accept_completion_controls)
            return;
        if (_completion_controls.size () >= max_pending_completion_controls) {
            if (!_completion_control_failure) {
                _completion_control_failure.emplace (
                  completion_control_failure_t{
                    source_routing_id,
                    raw_mesh_pump_result_t::capacity_exceeded});
            }
            return;
        }
        _completion_controls.push_back (
          pending_completion_control_t{
            detail::backend::raw_received_t{
              std::move (source_routing_id),
              std::nullopt,
              std::move (parts)},
            admitted_generation});
    }
    catch (const protocol::service_wire_error_t &) {
        mark_failure (raw_mesh_pump_result_t::protocol_error);
    }
    catch (const std::exception &) {
        mark_failure (raw_mesh_pump_result_t::protocol_error);
    }
}

void raw_mesh_node_owner_t::disconnect_completion_control_source (
  const detail::backend::raw_bytes_t &source_routing_id) noexcept
{
    if (source_routing_id.empty ()) {
        return;
    }
    const auto peer = _topology.peer (source_routing_id);
    if (!peer) {
        return;
    }
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        try {
            std::lock_guard socket_lock (_socket_mutex);
            if (_router) {
                _router->disconnect (peer->descriptor.advertised_endpoint);
            }
        }
        catch (...) {
        }
        (void) _connections.disconnect (
          source_routing_id, peer->connection_id);
    }
    (void) _topology.disconnect (source_routing_id, peer->connection_id);
    (void) _liveness.disconnect (source_routing_id, peer->connection_id);
}

raw_mesh_pump_result_t raw_mesh_node_owner_t::pump_one (
  service_liveness_registry_t::clock_t::time_point now,
  bool accept_application_receive)
{
    _last_pump_bytes = 0;
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port) {
        return raw_mesh_pump_result_t::no_data;
    }
    zlink::poll_event_flag_t readiness = zlink::poll_event_flag_t::none;
    try {
        /* One poller owns both ROUTER receive readiness and request
         * completion processing. The completion callback can enqueue a
         * control record before application admission is considered. */
        readiness = port->poll (std::chrono::milliseconds::zero ());
    }
    catch (...) {
        return raw_mesh_pump_result_t::protocol_error;
    }
    std::optional<completion_control_failure_t> completion_control_failure;
    {
        std::lock_guard lock (_completion_control_mutex);
        completion_control_failure =
          std::move (_completion_control_failure);
    }
    if (completion_control_failure) {
        disconnect_completion_control_source (
          completion_control_failure->source_routing_id);
        return completion_control_failure->result;
    }
    std::optional<detail::backend::raw_received_t> received;
    std::optional<std::uint64_t> completion_peer_generation;
    {
        std::lock_guard lock (_completion_control_mutex);
        if (!_completion_controls.empty ()) {
            auto control = std::move (_completion_controls.front ());
            _completion_controls.pop_front ();
            received.emplace (std::move (control.received));
            completion_peer_generation =
              control.admitted_peer_generation;
        }
    }
    if (!received) {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        if (!_pending_admissions.empty ()) {
            auto pending = std::move (_pending_admissions.front ());
            _pending_admissions.pop_front ();
            _pending_admission_bytes -= pending.bytes;
            received.emplace (std::move (pending.received));
            trace_mesh (
              "admission-retry reason=connection-ready pending="
                + std::to_string (_pending_admissions.size ()));
        }
    }
    if (!received && _pending_received
        && accept_application_receive) {
        const auto accepted_result =
          _pending_received->accepted_result;
        for (const auto &part : _pending_received->record.parts)
            _last_pump_bytes += part.size ();
        if (!_mailbox.try_enqueue (
              std::move (_pending_received->record))) {
            return raw_mesh_pump_result_t::backpressured;
        }
        _pending_received.reset ();
        return accepted_result;
    }
    if (!received && accept_application_receive) {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        if (!_pending_unadmitted_applications.empty ()) {
            const auto &pending = _pending_unadmitted_applications.front ();
            const auto peer =
              _topology.peer (pending.received.source_routing_id);
            trace_mesh (
              "application-pending-check count="
                + std::to_string (_pending_unadmitted_applications.size ())
                + " owner="
                + trace_owner_key (this)
                + " source="
                + owner_key (pending.received.source_routing_id)
                + " peer="
                + (peer ? std::string ("present") : std::string ("absent"))
                + " received=0 accept=1");
        }
        for (auto pending = _pending_unadmitted_applications.begin ();
             pending != _pending_unadmitted_applications.end (); ++pending) {
            const auto peer = _topology.peer (pending->received.source_routing_id);
            if (!peer)
                continue;
            trace_mesh (
              "application-drain reason=peer-admitted owner="
                + trace_owner_key (this)
                + " source="
                + owner_key (pending->received.source_routing_id)
                + " peerGeneration="
                + std::to_string (peer->descriptor.lifecycle_generation)
                + " pending="
                + std::to_string (_pending_unadmitted_applications.size ()));
            received.emplace (std::move (pending->received));
            _pending_unadmitted_application_bytes -= pending->bytes;
            _pending_unadmitted_applications.erase (pending);
            break;
        }
    }
    if (!received && accept_application_receive)
        received = port->receive_if_ready (readiness);
    if (!received) {
        return raw_mesh_pump_result_t::no_data;
    }
    _last_pump_bytes = raw_received_bytes (*received);
    if (received->parts.empty ()) {
        return raw_mesh_pump_result_t::protocol_error;
    }
    try {
        const auto header = protocol::decode_header (received->parts.front ());
        trace_mesh (
          "receive kind="
            + std::to_string (static_cast<int> (header.kind))
            + " parts=" + std::to_string (received->parts.size ())
            + " requestSeq="
            + (received->request_sequence
                 ? std::to_string (*received->request_sequence)
                 : std::string ("-")));
        if (header.kind == protocol::command::hello
            || header.kind == protocol::command::admit
            || header.kind == protocol::command::update) {
            if (received->parts.size () != 1) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto descriptor = protocol::decode_route_mesh_admission (
              received->parts.front (), header.kind,
              received->source_routing_id);
            const auto preferred_direction =
              header.kind == protocol::command::hello
                ? service_connection_direction_t::inbound
                : service_connection_direction_t::outbound;
            std::vector<std::uint8_t> connection_id;
            service_connection_direction_t direction =
              preferred_direction;
            std::string remote_endpoint;
            std::optional<service_node_descriptor_t>
              expected_descriptor;
            {
                std::lock_guard lifecycle_lock (_lifecycle_mutex);
                const auto connection =
                  _connections.for_handshake (
                    received->source_routing_id,
                    preferred_direction);
                if (!connection) {
                    const auto bytes = raw_received_bytes (*received);
                    if (_pending_admissions.size () >= max_pending_admissions
                        || bytes > max_pending_admission_bytes
                        || bytes > max_pending_admission_bytes
                             - _pending_admission_bytes) {
                        trace_mesh (
                          "admission-drop reason=pending-queue-full kind="
                            + std::to_string (static_cast<int> (header.kind)));
                        return raw_mesh_pump_result_t::capacity_exceeded;
                    }
                    _pending_admission_bytes += bytes;
                    _pending_admissions.push_back (
                      pending_admission_t{std::move (*received), bytes});
                    trace_mesh (
                      "admission-deferred reason=no-physical-candidate kind="
                        + std::to_string (static_cast<int> (header.kind))
                        + " pending="
                        + std::to_string (_pending_admissions.size ()));
                    // The monitor poller is drained by the host before the
                    // next dispatch pass. Returning no_data prevents this
                    // same pass from repeatedly retrying the queued frame
                    // before connection_ready has populated the candidate.
                    return raw_mesh_pump_result_t::no_data;
                }
                connection_id = connection->connection_id;
                direction = connection->direction;
                remote_endpoint = connection->remote_endpoint;
                const auto expected =
                  _expected_peers.find (received->source_routing_id);
                if (expected != _expected_peers.end ()
                    && (expected->second.mesh_name != descriptor.mesh_name
                        || expected->second.node_routing_id
                             != descriptor.node_routing_id
                        || expected->second.advertised_endpoint
                             != descriptor.advertised_endpoint
                        || expected->second.security_identity
                             != descriptor.security_identity
                        || (expected->second.lifecycle_generation != 0
                            && expected->second.lifecycle_generation
                                 != descriptor.lifecycle_generation))) {
                    (void) port->send_completion_control (
                      received->source_routing_id,
                      {protocol::encode_reject (3)});
                    return raw_mesh_pump_result_t::infrastructure;
                }
                if (expected != _expected_peers.end ()
                    && expected->second.lifecycle_generation != 0)
                    expected_descriptor = expected->second;
            }
            const auto admission =
              expected_descriptor
                ? _topology.admit (
                    descriptor, connection_id, direction,
                    *expected_descriptor)
                : _topology.admit (
                    descriptor, connection_id, direction);
            if (admission == peer_admission_result_t::not_required) {
                trace_admission_phase (
                  received->source_routing_id,
                  descriptor.lifecycle_generation,
                  header.kind, admission);
                if (header.kind == protocol::command::hello) {
                    (void) send_completion_control (
                      received->source_routing_id,
                      {protocol::encode_route_mesh_admission (
                        protocol::command::admit,
                        _topology.local_descriptor ())});
                } else {
                    std::lock_guard lifecycle_lock (_lifecycle_mutex);
                    try {
                        std::lock_guard socket_lock (_socket_mutex);
                        _router->disconnect (
                          remote_endpoint.empty ()
                            ? descriptor.advertised_endpoint
                            : remote_endpoint);
                    }
                    catch (...) {
                    }
                    (void) _connections.disconnect (
                      received->source_routing_id,
                      connection_id);
                }
                return raw_mesh_pump_result_t::infrastructure;
            }
            if (admission
                == peer_admission_result_t::duplicate_connection) {
                trace_admission_phase (
                  received->source_routing_id,
                  descriptor.lifecycle_generation,
                  header.kind, admission);
                if (header.kind == protocol::command::hello) {
                    (void) send_completion_control (
                      received->source_routing_id,
                      {protocol::encode_route_mesh_admission (
                        protocol::command::admit,
                        _topology.local_descriptor ())});
                }
                return raw_mesh_pump_result_t::infrastructure;
            }
            if (admission != peer_admission_result_t::admitted) {
                const auto reason =
                  admission == peer_admission_result_t::mesh_mismatch ? 2u
                  : admission == peer_admission_result_t::stale_descriptor
                    ? 7u
                    : 11u;
                (void) send_completion_control (
                  received->source_routing_id,
                  {protocol::encode_reject (reason)});
                return raw_mesh_pump_result_t::infrastructure;
            }
            trace_admission_phase (
              received->source_routing_id,
              descriptor.lifecycle_generation,
              header.kind, admission);
            _liveness.admit (
              descriptor.node_routing_id, connection_id, now);
            if (header.kind == protocol::command::hello) {
                (void) send_completion_control (
                  received->source_routing_id,
                  {protocol::encode_route_mesh_admission (
                    protocol::command::admit,
                    _topology.local_descriptor ())});
            }
            return raw_mesh_pump_result_t::infrastructure;
        }
        if (header.kind == protocol::command::reject) {
            if (received->parts.size () != 1) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            (void) protocol::decode_reject (received->parts.front ());
            const auto peer = _topology.peer (received->source_routing_id);
            if (peer) {
                (void) _topology.disconnect (
                  received->source_routing_id, peer->connection_id);
                (void) _liveness.disconnect (
                  received->source_routing_id, peer->connection_id);
            }
            return raw_mesh_pump_result_t::infrastructure;
        }
        const auto admitted = _topology.peer (received->source_routing_id);
        if (!admitted) {
            if (application_command (header.kind)
                && header.flags == 0
                && received->parts.size () == 2) {
                const auto bytes = raw_received_bytes (*received);
                std::lock_guard lifecycle_lock (_lifecycle_mutex);
                if (bytes <= max_pending_unadmitted_application_bytes
                    && _pending_unadmitted_applications.size ()
                         < max_pending_unadmitted_applications
                    && bytes <= max_pending_unadmitted_application_bytes
                         - _pending_unadmitted_application_bytes) {
                    _pending_unadmitted_application_bytes += bytes;
                    _pending_unadmitted_applications.push_back (
                      pending_unadmitted_application_t{
                        std::move (*received), bytes});
                    trace_mesh (
                      "application-deferred reason=peer-not-admitted kind="
                        + std::to_string (static_cast<int> (header.kind))
                        + " pending="
                        + std::to_string (
                          _pending_unadmitted_applications.size ()));
                    return raw_mesh_pump_result_t::application;
                }
                trace_mesh (
                  "application-drop reason=unadmitted-queue-full kind="
                    + std::to_string (static_cast<int> (header.kind)));
                return raw_mesh_pump_result_t::backpressured;
            }
            trace_mesh (
              "application-drop reason=peer-not-admitted kind="
                + std::to_string (static_cast<int> (header.kind))
                + " sourceBytes="
                + std::to_string (received->source_routing_id.size ()));
            return raw_mesh_pump_result_t::protocol_error;
        }
        if (completion_peer_generation
            && admitted->descriptor.lifecycle_generation
                 != *completion_peer_generation) {
            trace_mesh (
              "application-drop reason=peer-generation-mismatch kind="
                + std::to_string (static_cast<int> (header.kind)));
            return raw_mesh_pump_result_t::protocol_error;
        }
        if (header.kind == protocol::command::livenessProbe
            || header.kind == protocol::command::livenessAck) {
            if (received->parts.size () != 1) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto record =
              protocol::decode_liveness (received->parts.front ());
            if (record.kind == protocol::command::livenessProbe) {
                const auto ack = _liveness.acknowledge_probe (
                  received->source_routing_id, admitted->connection_id,
                  record.probe_id);
                if (!ack
                    || !send_completion_control (
                      received->source_routing_id,
                      {protocol::encode_liveness (
                        protocol::command::livenessAck, record.probe_id)})) {
                    return raw_mesh_pump_result_t::protocol_error;
                }
            } else {
                (void) _liveness.acknowledge (
                  received->source_routing_id, admitted->connection_id,
                  record.probe_id, now);
            }
            return raw_mesh_pump_result_t::infrastructure;
        }
        if (header.kind == protocol::command::messageFollow) {
            if (header.flags != 0 || received->parts.size () != 1
                || received->request_sequence) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto notice = protocol::decode_message_follow (
              received->parts.front ());
            const auto source_matches_peer = std::visit (
              [&received, &admitted] (const auto &source) {
                  return source.target_node_routing_id
                           == received->source_routing_id
                         && source.target_node_generation
                              == admitted->descriptor.lifecycle_generation;
              },
              notice.source);
            const auto same_object = [&notice] {
                if (const auto *source = std::get_if<
                      protocol::actor_route_fence_t> (&notice.source)) {
                    const auto *target = std::get_if<
                      protocol::actor_route_fence_t> (&notice.target);
                    return target != nullptr
                           && source->actor_id == target->actor_id
                           && source->object_generation
                                == target->object_generation;
                }
                const auto *source = std::get_if<
                  protocol::spot_route_fence_t> (&notice.source);
                const auto *target = std::get_if<
                  protocol::spot_route_fence_t> (&notice.target);
                return target != nullptr && source != nullptr
                       && source->spot_id == target->spot_id
                       && source->object_generation
                            == target->object_generation;
            }();
            const auto authority_generation_increases = std::visit (
              [] (const auto &source, const auto &target) {
                  using source_type = std::decay_t<decltype (source)>;
                  using target_type = std::decay_t<decltype (target)>;
                  if constexpr (!std::is_same_v<source_type, target_type>) {
                      return false;
                  } else {
                      return target.authority_owner_generation
                             > source.authority_owner_generation;
                  }
              },
              notice.source, notice.target);
            const auto local = _topology.local_descriptor ();
            const auto target_matches_topology = std::visit (
              [&local, this] (const auto &target) {
                  if (target.target_node_routing_id == local.node_routing_id) {
                      return target.target_node_generation
                             == local.lifecycle_generation;
                  }
                  const auto admitted_target = _topology.peer (
                    target.target_node_routing_id);
                  return admitted_target
                         && target.target_node_generation
                              == admitted_target->descriptor.lifecycle_generation;
              },
              notice.target);
            if (!source_matches_peer || !same_object
                || !authority_generation_increases
                || !target_matches_topology) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            return enqueue_received_or_retain (
              service_mailbox_record_t{
                owner_key (local.node_routing_id),
                service_mailbox_domain_t::infrastructure,
                std::move (received->parts),
                std::move (received->source_routing_id),
                std::nullopt, std::nullopt,
                admitted->descriptor.lifecycle_generation,
                std::pair{notice.original_operation.high,
                           notice.original_operation.low}},
              raw_mesh_pump_result_t::infrastructure);
        }
        if (header.kind == protocol::command::instanceSpot) {
            const auto activation =
              protocol::decode_instance_spot_activation_header (
                received->parts.front ());
            const auto expected_parts = activation.has_metadata ? 3u : 2u;
            if (received->parts.size () != expected_parts
                || activation.source_node_routing_id
                     != received->source_routing_id
                || activation.source_node_routing_id
                     != admitted->descriptor.node_routing_id
                || activation.source_node_generation
                     != admitted->descriptor.lifecycle_generation) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto local = _topology.local_descriptor ();
            if (activation.target.target_node_routing_id
                  != local.node_routing_id
                || activation.target.target_node_generation
                     != local.lifecycle_generation
                || (activation.request
                      != received->request_sequence.has_value ())
                || (activation.request
                    && activation.reply_route_id
                         != *received->request_sequence)
                || (!activation.request
                    && activation.reply_route_id != 0)) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            (void) protocol::decode_application_payload (
              received->parts.back ());
            return enqueue_received_or_retain (
              service_mailbox_record_t{
                owner_key (local.node_routing_id),
                service_mailbox_domain_t::infrastructure,
                std::move (received->parts),
                std::move (received->source_routing_id),
                received->request_sequence,
                activation.request
                  ? std::make_optional (
                      activation.reply_route_id)
                  : std::nullopt},
              raw_mesh_pump_result_t::infrastructure);
        }
        if (header.kind == protocol::command::actorCreate
            || header.kind == protocol::command::userSpotCreate
            || header.kind == protocol::command::userSpotClose) {
            if (header.flags != 0 || received->parts.size () != 1
                || !received->request_sequence) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto local = _topology.local_descriptor ();
            std::vector<std::uint8_t> source_node_routing_id;
            std::uint64_t source_node_generation = 0;
            std::vector<std::uint8_t> target_node_routing_id;
            std::uint64_t target_node_generation = 0;
            std::uint64_t correlation = 0;
            if (header.kind == protocol::command::actorCreate) {
                const auto create =
                  protocol::decode_actor_create_header (
                    received->parts.front ());
                source_node_routing_id = create.source_node_routing_id;
                source_node_generation = create.source_node_generation;
                target_node_routing_id =
                  create.reservation.target_node_routing_id;
                target_node_generation =
                  create.reservation.target_node_generation;
                correlation = create.correlation;
            } else if (header.kind
                       == protocol::command::userSpotCreate) {
                const auto create =
                  protocol::decode_user_spot_create_header (
                    received->parts.front ());
                source_node_routing_id =
                  create.source_node_routing_id;
                source_node_generation =
                  create.source_node_generation;
                target_node_routing_id =
                  create.reservation.target_node_routing_id;
                target_node_generation =
                  create.reservation.target_node_generation;
                correlation = create.correlation;
            } else {
                const auto close =
                  protocol::decode_user_spot_close_header (
                    received->parts.front ());
                source_node_routing_id =
                  close.source_node_routing_id;
                source_node_generation =
                  close.source_node_generation;
                target_node_routing_id =
                  close.target.target_node_routing_id;
                target_node_generation =
                  close.target.target_node_generation;
                correlation = close.correlation;
            }
            if (source_node_routing_id
                  != received->source_routing_id
                || source_node_routing_id
                     != admitted->descriptor.node_routing_id
                || source_node_generation
                     != admitted->descriptor.lifecycle_generation
                || target_node_routing_id
                     != local.node_routing_id
                || target_node_generation
                     != local.lifecycle_generation) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            return enqueue_received_or_retain (
              service_mailbox_record_t{
                owner_key (local.node_routing_id),
                service_mailbox_domain_t::infrastructure,
                std::move (received->parts),
                std::move (received->source_routing_id),
                received->request_sequence,
                correlation},
              raw_mesh_pump_result_t::infrastructure);
        }
        if (header.kind == protocol::command::relocationPrepare
            || header.kind == protocol::command::relocationReady
            || header.kind == protocol::command::relocationReserved
            || header.kind == protocol::command::relocationData
            || header.kind == protocol::command::relocationAck
            || header.kind == protocol::command::relocationSeal
            || header.kind == protocol::command::relocationComplete) {
            if (received->parts.size () != 1
                || received->request_sequence) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto control = protocol::decode_relocation_control (
              received->parts.front ());
            const auto source_fence = std::visit ([] (const auto &value)
              -> std::optional<std::pair<std::vector<std::uint8_t>,
                                         std::uint64_t>> {
                using record_t = std::decay_t<decltype (value)>;
                if constexpr (std::is_same_v<record_t,
                                             protocol::relocation_prepare_t>) {
                    if (value.initiator_role
                        == protocol::relocation_role_t::source)
                        return std::pair{value.source_node_routing_id,
                                         value.source_node_generation};
                    if (value.initiator_role
                        == protocol::relocation_role_t::target)
                        return std::pair{value.candidate.node_routing_id,
                                         value.candidate.node_generation};
                    return std::pair{value.coordinator.node_routing_id,
                                     value.coordinator.node_generation};
                }
                else if constexpr (std::is_same_v<
                                     record_t,
                                     protocol::relocation_ready_t>) {
                    return std::pair{value.candidate.node_routing_id,
                                     value.candidate.node_generation};
                }
                else if constexpr (std::is_same_v<
                                     record_t,
                                     protocol::relocation_reserved_t>) {
                    return std::pair{
                      value.coordinator.node_routing_id,
                      value.coordinator.node_generation};
                }
                else if constexpr (std::is_same_v<record_t,
                                                  protocol::relocation_data_t>) {
                    // The record source is the original request source, not
                    // the relocation transport peer. The registered target
                    // validates the exact relocation source peer.
                    return std::nullopt;
                }
                else if constexpr (std::is_same_v<record_t,
                                                  protocol::relocation_complete_t>) {
                    return std::pair{value.source.node_routing_id,
                                     value.source.node_generation};
                }
                else {
                    if (value.sender_role
                        == protocol::relocation_role_t::coordinator)
                        return std::pair{value.coordinator.node_routing_id,
                                         value.coordinator.node_generation};
                    return std::nullopt;
                }
              }, control);
            if (source_fence
                && (source_fence->first != received->source_routing_id
                    || source_fence->first
                         != admitted->descriptor.node_routing_id
                    || source_fence->second
                         != admitted->descriptor.lifecycle_generation)) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto local = _topology.local_descriptor ();
            return enqueue_received_or_retain (
              service_mailbox_record_t{
                owner_key (local.node_routing_id),
                service_mailbox_domain_t::infrastructure,
                std::move (received->parts),
                std::move (received->source_routing_id),
                std::nullopt, std::nullopt,
                admitted->descriptor.lifecycle_generation},
              raw_mesh_pump_result_t::infrastructure);
        }
        if (header.kind == protocol::command::replyRelay
            || header.kind == protocol::command::replyRelayAck) {
            if (header.flags != 0 || received->request_sequence) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto local = _topology.local_descriptor ();
            if (header.kind == protocol::command::replyRelay) {
                if (received->parts.empty ()
                    || received->parts.size () > 2) {
                    return raw_mesh_pump_result_t::protocol_error;
                }
                const auto relay = protocol::decode_reply_relay (
                  received->parts.front ());
                if (relay.terminal_result != 0
                    && received->parts.size () == 2) {
                    return raw_mesh_pump_result_t::protocol_error;
                }
                if (received->parts.size () == 2) {
                    (void) protocol::decode_application_payload (
                      received->parts.back ());
                }
            } else {
                if (received->parts.size () != 1) {
                    return raw_mesh_pump_result_t::protocol_error;
                }
                const auto ack = protocol::decode_reply_relay_ack (
                  received->parts.front ());
                if (ack.request_source.node_routing_id
                      != received->source_routing_id
                    || ack.request_source.node_routing_id
                         != admitted->descriptor.node_routing_id
                    || ack.request_source.node_generation
                         != admitted->descriptor.lifecycle_generation) {
                    return raw_mesh_pump_result_t::protocol_error;
                }
            }
            return enqueue_received_or_retain (
              service_mailbox_record_t{
                owner_key (local.node_routing_id),
                service_mailbox_domain_t::infrastructure,
                std::move (received->parts),
                std::move (received->source_routing_id),
                std::nullopt, std::nullopt,
                admitted->descriptor.lifecycle_generation},
              raw_mesh_pump_result_t::infrastructure);
        }
        if (header.kind
              == protocol::command::sessionRelocationSeal
            || header.kind
                 == protocol::command::sessionRelocationSealed
            || header.kind
                 == protocol::command::sessionRelocationRoute
            || header.kind
                 == protocol::command::sessionRelocationRouted) {
            if (header.flags != 0 || received->parts.size () != 1
                || received->request_sequence) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto local = _topology.local_descriptor ();
            if (header.kind
                == protocol::command::sessionRelocationSeal) {
                const auto seal =
                  protocol::decode_session_relocation_seal (
                    received->parts.front ());
                const auto expected_source_rid =
                  seal.sender_role == protocol::relocation_role_t::source
                    ? seal.actor.target_node_routing_id
                    : seal.coordinator.node_routing_id;
                const auto expected_source_generation =
                  seal.sender_role == protocol::relocation_role_t::source
                    ? seal.actor.target_node_generation
                    : seal.coordinator.node_generation;
                if (seal.session_owner_node_routing_id
                      != local.node_routing_id
                    || seal.session_owner_node_generation
                         != local.lifecycle_generation
                    || expected_source_rid
                         != received->source_routing_id
                    || expected_source_rid
                         != admitted->descriptor.node_routing_id
                    || expected_source_generation
                         != admitted->descriptor.lifecycle_generation) {
                    return raw_mesh_pump_result_t::protocol_error;
                }
                return enqueue_received_or_retain (
                  service_mailbox_record_t{
                    owner_key (local.node_routing_id),
                    service_mailbox_domain_t::infrastructure,
                    std::move (received->parts),
                    std::move (received->source_routing_id),
                    std::nullopt, std::nullopt},
                  raw_mesh_pump_result_t::infrastructure);
            }
            if (header.kind
                == protocol::command::sessionRelocationRoute) {
                const auto route =
                  protocol::decode_session_relocation_route (
                    received->parts.front ());
                const auto expected_source_rid =
                  route.sender_role == protocol::relocation_role_t::target
                    ? route.route.target_node_routing_id
                  : route.sender_role
                      == protocol::relocation_role_t::coordinator
                    ? route.coordinator.node_routing_id
                    : received->source_routing_id;
                const auto expected_source_generation =
                  route.sender_role == protocol::relocation_role_t::target
                    ? route.route.target_node_generation
                  : route.sender_role
                      == protocol::relocation_role_t::coordinator
                    ? route.coordinator.node_generation
                    : admitted->descriptor.lifecycle_generation;
                if (route.session_owner_node_routing_id
                      != local.node_routing_id
                    || route.session_owner_node_generation
                         != local.lifecycle_generation
                    || expected_source_rid
                         != received->source_routing_id
                    || expected_source_rid
                         != admitted->descriptor.node_routing_id
                    || expected_source_generation
                         != admitted->descriptor.lifecycle_generation) {
                    return raw_mesh_pump_result_t::protocol_error;
                }
                return enqueue_received_or_retain (
                  service_mailbox_record_t{
                    owner_key (local.node_routing_id),
                    service_mailbox_domain_t::infrastructure,
                    std::move (received->parts),
                    std::move (received->source_routing_id),
                    std::nullopt, std::nullopt},
                  raw_mesh_pump_result_t::infrastructure);
            }
            if (header.kind
                == protocol::command::sessionRelocationSealed) {
                const auto sealed =
                  protocol::decode_session_relocation_sealed (
                    received->parts.front ());
                if (sealed.session_owner_node_routing_id
                      != received->source_routing_id
                    || sealed.session_owner_node_routing_id
                         != admitted->descriptor.node_routing_id
                    || sealed.session_owner_node_generation
                         != admitted->descriptor.lifecycle_generation) {
                    return raw_mesh_pump_result_t::protocol_error;
                }
                const auto operation = operation_id (
                  sealed.relocation.high, sealed.relocation.low);
                if (!_operations->complete (
                      operation,
                      std::move (received->parts.front ()))) {
                    return raw_mesh_pump_result_t::protocol_error;
                }
                return raw_mesh_pump_result_t::infrastructure;
            }
            const auto routed =
              protocol::decode_session_relocation_routed (
                received->parts.front ());
            if (routed.session_owner_node_routing_id
                  != received->source_routing_id
                || routed.session_owner_node_routing_id
                     != admitted->descriptor.node_routing_id
                || routed.session_owner_node_generation
                     != admitted->descriptor.lifecycle_generation) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto operation = operation_id (
              routed.relocation.high, routed.relocation.low);
            if (!_operations->complete (
                  operation, std::move (received->parts.front ()))) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            return raw_mesh_pump_result_t::infrastructure;
        }
        if ((header.kind != protocol::command::nodeSend
             && header.kind != protocol::command::nodeRequest
             && header.kind != protocol::command::channelSend
             && header.kind != protocol::command::channelRequest
             && header.kind != protocol::command::spotSend
             && header.kind != protocol::command::spotRequest
             && header.kind != protocol::command::actorSend
             && header.kind != protocol::command::actorRequest)
            || header.flags != 0 || received->parts.size () != 2) {
            trace_mesh (
              "application-drop reason=invalid-application-shape kind="
                + std::to_string (static_cast<int> (header.kind))
                + " parts=" + std::to_string (received->parts.size ()));
            return raw_mesh_pump_result_t::protocol_error;
        }
        (void) protocol::decode_application_payload (received->parts[1]);
        const auto local = _topology.local_descriptor ();
        std::string mailbox_owner;
        std::optional<std::uint64_t> correlation;
        std::optional<std::pair<std::uint64_t, std::uint64_t>> operation;
        if (header.kind == protocol::command::nodeSend
            || header.kind == protocol::command::nodeRequest) {
            if (header.kind == protocol::command::nodeSend
                && received->parts.front ().size () != 5) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            if (header.kind == protocol::command::nodeRequest) {
                if (!received->request_sequence) {
                    return raw_mesh_pump_result_t::protocol_error;
                }
                correlation = protocol::decode_node_request_header (
                  received->parts.front ());
            }
            mailbox_owner = owner_key (local.node_routing_id);
        } else if (header.kind == protocol::command::channelSend
                   || header.kind == protocol::command::channelRequest) {
            std::string channel_name;
            if (header.kind == protocol::command::channelSend) {
                channel_name = protocol::decode_channel_send_header (
                  received->parts.front ());
            } else {
                if (!received->request_sequence) {
                    return raw_mesh_pump_result_t::protocol_error;
                }
                auto channel_request =
                  protocol::decode_channel_request_header (
                    received->parts.front ());
                correlation = channel_request.correlation;
                channel_name = std::move (channel_request.channel_name);
            }
            const auto channel = std::lower_bound (
              local.channels.begin (), local.channels.end (), channel_name,
              [] (const service_channel_descriptor_t &entry,
                  const std::string &name) {
                  return entry.name < name;
              });
            if (channel == local.channels.end () || channel->name != channel_name) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            mailbox_owner = "channel:" + channel_name;
        } else if (header.kind == protocol::command::spotSend
                   || header.kind == protocol::command::spotRequest) {
            if (header.kind == protocol::command::spotRequest
                && !received->request_sequence) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto spot = protocol::decode_spot_message_header (
              received->parts.front (), header.kind);
            if (spot.target.target_node_routing_id
                  != local.node_routing_id
                || spot.target.target_node_generation
                     != local.lifecycle_generation) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            correlation = spot.correlation;
            operation = std::pair{
              spot.operation.high, spot.operation.low};
            mailbox_owner = spot.target.spot_id;
            mailbox_owner.insert (0, "spot:");
        } else {
            if (header.kind == protocol::command::actorRequest
                && !received->request_sequence) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            const auto actor = protocol::decode_actor_message_header (
              received->parts.front (), header.kind);
            if (actor.target.target_node_routing_id
                  != local.node_routing_id
                || actor.target.target_node_generation
                     != local.lifecycle_generation) {
                return raw_mesh_pump_result_t::protocol_error;
            }
            correlation = actor.correlation;
            operation = std::pair{
              actor.operation.high, actor.operation.low};
            mailbox_owner = "actor:" + actor.target.actor_id;
        }
        auto result = enqueue_received_or_retain (
          service_mailbox_record_t{
            std::move (mailbox_owner),
            service_mailbox_domain_t::application,
            std::move (received->parts),
            std::move (received->source_routing_id),
            received->request_sequence,
            correlation,
            admitted->descriptor.lifecycle_generation,
            operation},
          raw_mesh_pump_result_t::application);
        trace_mesh (
          "application-enqueue result="
            + std::to_string (static_cast<int> (result))
            + " pending="
            + std::to_string (
              _mailbox.pending_messages (service_mailbox_domain_t::application)));
        return result;
    }
    catch (const protocol::service_wire_error_t &) {
        return raw_mesh_pump_result_t::protocol_error;
    }
}

bool raw_mesh_node_owner_t::wait_for_activity (
  std::chrono::milliseconds timeout,
  bool accept_application_receive) noexcept
{
    if (_mailbox.pending_messages (service_mailbox_domain_t::infrastructure)
        != 0
        || (accept_application_receive
            && _mailbox.pending_messages (
                 service_mailbox_domain_t::application)
                 != 0))
        return true;

    {
        std::lock_guard lock (_completion_control_mutex);
        if (_completion_control_failure || !_completion_controls.empty ())
            return true;
    }

    {
        std::lock_guard lock (_lifecycle_mutex);
        if (!_pending_unadmitted_applications.empty ()
            && accept_application_receive)
            return true;
    }

    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port)
        return false;
    try {
        return port->poll (timeout) != zlink::poll_event_flag_t::none;
    }
    catch (...) {
        return false;
    }
}

std::uint64_t raw_mesh_node_owner_t::next_operation_sequence ()
{
    std::lock_guard lifecycle_lock (_lifecycle_mutex);
    if (!_port)
        throw std::logic_error ("raw mesh node is not started");
    const auto sequence = _next_correlation++;
    if (sequence == 0 || _next_correlation == 0) {
        _next_correlation = 1;
        throw std::overflow_error ("raw mesh operation sequence is exhausted");
    }
    return sequence;
}

std::size_t raw_mesh_node_owner_t::drain_monitor_events (
  service_liveness_registry_t::clock_t::time_point now)
{
    std::size_t count = 0;
    for (;;) {
        std::optional<zlink::monitor_event_t> event;
        {
            std::lock_guard lifecycle_lock (_lifecycle_mutex);
            if (!_monitor || !_monitor->valid ()) {
                return count;
            }
            if (!_monitor_poller) {
                return count;
            }
            zlink::poll_event_t readiness;
            try {
                if (_monitor_poller->wait (
                      &readiness, 1, std::chrono::milliseconds::zero ())
                      != 1
                    || readiness.slot != 1
                    || (static_cast<short> (readiness.revents)
                        & static_cast<short> (zlink::poll_event_flag_t::pollin))
                         == 0) {
                    return count;
                }
            }
            catch (...) {
                return count;
            }
            try {
                event = _monitor->recv (zlink::recv_flags_t::dontwait);
            }
            catch (...) {
                return count;
            }
        }
        if (!event) {
            return count;
        }
        ++count;
        if (!event->routing_id) {
            continue;
        }
        const auto node_routing_id = event->routing_id->to_bytes ();
        const std::vector<std::uint8_t> connection_id{
          static_cast<std::uint8_t> ((event->value >> 24u) & 0xffu),
          static_cast<std::uint8_t> ((event->value >> 16u) & 0xffu),
          static_cast<std::uint8_t> ((event->value >> 8u) & 0xffu),
          static_cast<std::uint8_t> (event->value & 0xffu)};
        if (event->event == zlink::monitor_event::connection_ready) {
            std::shared_ptr<detail::backend::raw_route_port_t> port;
            {
                std::lock_guard lifecycle_lock (_lifecycle_mutex);
                const auto outbound =
                  _outbound_endpoints.contains (
                    event->remote_addr);
                /* connect_peer records every locally initiated endpoint.
                 * A ready event whose remote endpoint is in that set is the
                 * outbound physical candidate; accepted connections retain
                 * inbound direction. dispatch_ready drains these monitor
                 * events before it pumps admission messages. */
                _connections.ready (
                  node_routing_id, connection_id,
                  outbound
                    ? service_connection_direction_t::outbound
                    : service_connection_direction_t::inbound,
                  event->remote_addr);
                port = _port;
            }
            if (port) {
                bool hello_sent = false;
                try {
                    hello_sent = send_completion_control (
                      node_routing_id,
                      {protocol::encode_route_mesh_admission (
                        protocol::command::hello,
                        _topology.local_descriptor ())});
                }
                catch (const zlink::submit_error_t &) {
                }
                if (!hello_sent) {
                    std::lock_guard lifecycle_lock (
                      _lifecycle_mutex);
                    (void) _connections.disconnect (
                      node_routing_id, connection_id);
                }
            }
        } else if (event->event == zlink::monitor_event::disconnected) {
            bool known = false;
            {
                std::lock_guard lifecycle_lock (_lifecycle_mutex);
                known = _connections.disconnect (
                  node_routing_id, connection_id);
            }
            if (known) {
                const auto removed = _topology.disconnect (
                  node_routing_id, connection_id);
                (void) _liveness.disconnect (
                  node_routing_id, connection_id);
                if (removed) {
                    discard_pending_unadmitted_applications (
                      node_routing_id);
                }
            }
        }
        static_cast<void> (now);
    }
}

service_liveness_tick_t raw_mesh_node_owner_t::tick_liveness (
  service_liveness_registry_t::clock_t::time_point now)
{
    auto result = _liveness.tick (now);
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port) {
        return result;
    }
    for (const auto &probe : result.probes) {
        (void) send_completion_control (
          probe.node_routing_id,
          {protocol::encode_liveness (
            protocol::command::livenessProbe, probe.probe_id)});
    }
    for (const auto &timed_out : result.timed_out_nodes) {
        const auto peer = _topology.peer (timed_out);
        if (peer) {
            const auto removed = _topology.disconnect (
              timed_out, peer->connection_id);
            if (removed) {
                discard_pending_unadmitted_applications (timed_out);
            }
        }
    }
    return result;
}

std::string raw_mesh_node_owner_t::owner_key (
  const std::vector<std::uint8_t> &routing_id)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill ('0');
    for (const auto byte : routing_id) {
        stream << std::setw (2) << static_cast<unsigned int> (byte);
    }
    return stream.str ();
}

foundation::operation_id_t raw_mesh_node_owner_t::operation_id (
  std::uint64_t lifecycle_generation,
  std::uint64_t correlation)
{
    return foundation::operation_id_t{lifecycle_generation, correlation};
}

} // namespace zlink::framework::runtime::mesh

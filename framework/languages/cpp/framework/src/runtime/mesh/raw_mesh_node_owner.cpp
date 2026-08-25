/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/dispatch/application_job_receive_flow.hpp"
#include "runtime/transport/listener_identity.hpp"

#include "runtime/protocol/service_wire_codec.hpp"

#include <service_wire_pilot_codec.hpp>

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

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/system_executor.hpp>

#include <nlohmann/json.hpp>

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

constexpr std::size_t max_pending_admissions = 64;
constexpr std::size_t max_pending_admission_bytes = 64u * 1024u;
constexpr auto infrastructure_not_connected_retry_interval =
  std::chrono::milliseconds (75);
bool mesh_trace_enabled ()
{
    const char *value = std::getenv ("ZLINK_CPP_MESH_TRACE");
    return value != nullptr && *value != '\0' && std::string_view (value) != "0";
}

void trace_mesh_enabled (const std::string &message)
{
    std::cerr << "zlink mesh " << message << '\n';
}

// Keep all string composition behind the opt-in gate. A function call would
// evaluate concatenation arguments before entering the function.
#define trace_mesh(message)                                                     \
    do {                                                                        \
        if (mesh_trace_enabled ())                                              \
            trace_mesh_enabled (message);                                       \
    } while (false)

std::string trace_owner_key (const void *owner)
{
    std::ostringstream stream;
    stream << std::hex << reinterpret_cast<std::uintptr_t> (owner);
    return stream.str ();
}

std::vector<std::uint8_t> monitor_connection_id (std::uint64_t value)
{
    std::vector<std::uint8_t> result (sizeof (value));
    for (std::size_t index = 0; index < result.size (); ++index) {
        const auto shift =
          static_cast<unsigned int> ((result.size () - index - 1) * 8);
        result[index] = static_cast<std::uint8_t> ((value >> shift) & 0xffu);
    }
    return result;
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
        case protocol::command::boundSessionSend:
            return true;
        default:
            return false;
    }
}

std::optional<std::uint64_t> application_request_correlation (
  protocol::command kind, std::span<const std::uint8_t> header)
{
    switch (kind) {
        case protocol::command::nodeRequest:
            return protocol::decode_node_request_header (header);
        case protocol::command::channelRequest:
            return protocol::decode_channel_request_header (header).correlation;
        case protocol::command::spotRequest:
            return protocol::decode_spot_message_header (header, kind).correlation;
        case protocol::command::actorRequest:
            return protocol::decode_actor_message_header (header, kind).correlation;
        default:
            return std::nullopt;
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

class infrastructure_request_retry_state_t final :
    public std::enable_shared_from_this<infrastructure_request_retry_state_t>
{
  public:
    using clock_t = foundation::operation_registry_t::clock_t;

    infrastructure_request_retry_state_t (
      std::shared_ptr<detail::backend::raw_route_port_t> port,
      std::shared_ptr<foundation::operation_registry_t> operations,
      foundation::call_id_t operation,
      std::uint64_t correlation,
      std::vector<std::uint8_t> target_routing_id,
      detail::backend::raw_message_t request_parts,
      std::function<std::vector<std::uint8_t> (
        const detail::backend::raw_message_t &)> decode_reply,
      clock_t::time_point deadline,
      foundation::operation_terminal_t deadline_terminal =
        foundation::operation_terminal_t::transport_failed) :
        _port (std::move (port)),
        _operations (std::move (operations)),
        _operation (operation),
        _correlation (correlation),
        _target_routing_id (std::move (target_routing_id)),
        _request_parts (std::move (request_parts)),
        _decode_reply (std::move (decode_reply)),
        _deadline (deadline),
        _deadline_terminal (deadline_terminal),
        _retry_timer (boost::asio::system_executor ())
    {
    }

    void start ()
    {
        submit ();
    }

  private:
    void submit ()
    {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds> (
          _deadline - clock_t::now ());
        if (remaining <= std::chrono::milliseconds::zero ()) {
            fail (_deadline_terminal);
            return;
        }
        auto running = std::make_shared<
          task_t<detail::backend::raw_request_completion_t>> (
            _port->request (
              _target_routing_id, _request_parts, remaining));
        detail::observe_task_completion (
          *running,
          [self = shared_from_this (), running] (
            const result_t<detail::backend::raw_request_completion_t>
              &settled) {
              self->settle (settled);
          });
    }

    void settle (
      const result_t<detail::backend::raw_request_completion_t> &settled)
    {
        if (!settled) {
            fail (foundation::operation_terminal_t::transport_failed);
            return;
        }
        auto completion = settled.value ();
        trace_mesh (
          "infrastructure-request-result correlation="
          + std::to_string (_correlation)
          + " result="
          + std::to_string (static_cast<int> (completion.result)));
        if (completion.result
              == detail::backend::raw_request_result_t::not_connected
            || completion.result
                 == detail::backend::raw_request_result_t::route_unavailable) {
            schedule_retry ();
            return;
        }
        if (completion.result
            != detail::backend::raw_request_result_t::ok) {
            const auto terminal =
              completion.result
                  == detail::backend::raw_request_result_t::timed_out
                ? foundation::operation_terminal_t::timed_out
              : completion.result
                    == detail::backend::raw_request_result_t::terminated
                ? foundation::operation_terminal_t::shutdown
                : foundation::operation_terminal_t::transport_failed;
            fail (terminal);
            return;
        }
        try {
            const auto &parts = completion.parts;
            if (parts.empty ()) {
                throw protocol::service_wire_error_t (
                  "infrastructure reply has no header");
            }
            const auto prefix = protocol::decode_reply_header (
              std::span<const std::uint8_t> (
                parts.front ().data (),
                std::min<std::size_t> (parts.front ().size (), 21)));
            if (prefix.correlation != _correlation) {
                throw protocol::service_wire_error_t (
                  "infrastructure reply correlation does not match");
            }
            auto payload = _decode_reply (parts);
            (void) _operations->complete (
              _operation, std::move (payload));
        }
        catch (const protocol::service_wire_error_t &) {
            fail (foundation::operation_terminal_t::transport_failed);
        }
    }

    void schedule_retry ()
    {
        const auto now = clock_t::now ();
        if (now >= _deadline) {
            fail (_deadline_terminal);
            return;
        }
        _retry_timer.expires_at (std::min (
          _deadline, now + infrastructure_not_connected_retry_interval));
        _retry_timer.async_wait (
          [self = shared_from_this ()] (
            const boost::system::error_code &error) {
              if (error) {
                  self->fail (
                    foundation::operation_terminal_t::transport_failed);
                  return;
              }
              self->submit ();
          });
    }

    void fail (foundation::operation_terminal_t terminal)
    {
        (void) _operations->fail (_operation, terminal);
    }

    std::shared_ptr<detail::backend::raw_route_port_t> _port;
    std::shared_ptr<foundation::operation_registry_t> _operations;
    foundation::call_id_t _operation;
    std::uint64_t _correlation;
    std::vector<std::uint8_t> _target_routing_id;
    detail::backend::raw_message_t _request_parts;
    std::function<std::vector<std::uint8_t> (
      const detail::backend::raw_message_t &)> _decode_reply;
    clock_t::time_point _deadline;
    foundation::operation_terminal_t _deadline_terminal;
    boost::asio::steady_timer _retry_timer;
};

task_t<bool> submit_registered_infrastructure_request_with_retry (
  std::shared_ptr<detail::backend::raw_route_port_t> port,
  std::shared_ptr<foundation::operation_registry_t> operations,
  foundation::call_id_t operation,
  std::uint64_t correlation,
  std::vector<std::uint8_t> target_routing_id,
  detail::backend::raw_message_t request_parts,
  std::function<std::vector<std::uint8_t> (
    const detail::backend::raw_message_t &)> decode_reply,
  foundation::operation_registry_t::clock_t::time_point deadline,
  foundation::operation_terminal_t deadline_terminal =
    foundation::operation_terminal_t::transport_failed)
{
    if (!port
        || deadline <= foundation::operation_registry_t::clock_t::now ()) {
        (void) operations->fail (
          operation, deadline_terminal);
        co_return false;
    }
    auto retry = std::make_shared<infrastructure_request_retry_state_t> (
      std::move (port), std::move (operations), operation, correlation,
      std::move (target_routing_id), std::move (request_parts),
      std::move (decode_reply), deadline, deadline_terminal);
    retry->start ();
    co_return true;
}

task_t<bool> submit_registered_infrastructure_request (
  std::shared_ptr<detail::backend::raw_route_port_t> port,
  std::shared_ptr<foundation::operation_registry_t> operations,
  foundation::call_id_t operation,
  std::uint64_t correlation,
  std::vector<std::uint8_t> target_routing_id,
  detail::backend::raw_message_t request_parts,
  std::function<std::vector<std::uint8_t> (
    const detail::backend::raw_message_t &)> decode_reply,
  std::chrono::milliseconds timeout)
{
    if (!port || timeout <= std::chrono::milliseconds::zero ()) {
        (void) operations->fail (
          operation, foundation::operation_terminal_t::transport_failed);
        co_return false;
    }
    auto running = std::make_shared<
      task_t<detail::backend::raw_request_completion_t>> (
        port->request (
          target_routing_id, std::move (request_parts), timeout));
    detail::observe_task_completion (
      *running,
      [operations = std::move (operations), operation, correlation,
       decode_reply = std::move (decode_reply), running] (
        const result_t<detail::backend::raw_request_completion_t> &settled) {
          if (!settled) {
              (void) operations->fail (
                operation,
                foundation::operation_terminal_t::transport_failed);
              return;
          }
          auto completion = settled.value ();
          trace_mesh (
            "infrastructure-request-result correlation="
            + std::to_string (correlation)
            + " result="
            + std::to_string (static_cast<int> (completion.result)));
          if (completion.result
              != detail::backend::raw_request_result_t::ok) {
              const auto terminal =
                completion.result
                    == detail::backend::raw_request_result_t::timed_out
                  ? foundation::operation_terminal_t::timed_out
                : completion.result
                      == detail::backend::raw_request_result_t::terminated
                  ? foundation::operation_terminal_t::shutdown
                  : foundation::operation_terminal_t::transport_failed;
              (void) operations->fail (operation, terminal);
              return;
          }
          try {
              const auto &parts = completion.parts;
              if (parts.empty ()) {
                  throw protocol::service_wire_error_t (
                    "infrastructure reply has no header");
              }
              const auto prefix = protocol::decode_reply_header (
                std::span<const std::uint8_t> (
                  parts.front ().data (),
                  std::min<std::size_t> (parts.front ().size (), 21)));
              if (prefix.correlation != correlation) {
                  throw protocol::service_wire_error_t (
                    "infrastructure reply correlation does not match");
              }
              auto payload = decode_reply (parts);
              (void) operations->complete (
                operation, std::move (payload));
          }
          catch (const protocol::service_wire_error_t &) {
              (void) operations->fail (
                operation,
                foundation::operation_terminal_t::transport_failed);
          }
      });
    co_return true;
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

std::vector<std::vector<std::uint8_t>>
raw_mesh_connection_candidates_t::disconnect_all (
  const std::vector<std::uint8_t> &node_routing_id)
{
    std::vector<std::vector<std::uint8_t>> removed;
    const auto found = _candidates.find (node_routing_id);
    if (found == _candidates.end ())
        return removed;
    removed.reserve (found->second.size ());
    for (const auto &[connection_id, _] : found->second)
        removed.push_back (connection_id);
    _candidates.erase (found);
    return removed;
}

std::optional<std::vector<std::uint8_t>>
raw_mesh_connection_candidates_t::disconnect_by_connection_id (
  const std::vector<std::uint8_t> &connection_id,
  std::string_view remote_endpoint)
{
    for (auto node = _candidates.begin (); node != _candidates.end (); ++node) {
        const auto candidate = std::find_if (
          node->second.begin (), node->second.end (),
          [&] (const auto &entry) {
              return entry.first == connection_id
                     && (remote_endpoint.empty ()
                         || entry.second.remote_endpoint == remote_endpoint);
          });
        if (candidate == node->second.end ())
            continue;
        const auto node_routing_id = node->first;
        node->second.erase (candidate);
        if (node->second.empty ())
            _candidates.erase (node);
        return node_routing_id;
    }
    return std::nullopt;
}

std::vector<std::pair<std::vector<std::uint8_t>,
                      std::vector<std::uint8_t>>>
raw_mesh_connection_candidates_t::disconnect_by_endpoint (
  std::string_view remote_endpoint)
{
    std::vector<std::pair<std::vector<std::uint8_t>,
                          std::vector<std::uint8_t>>> removed;
    if (remote_endpoint.empty ())
        return removed;
    for (auto node = _candidates.begin (); node != _candidates.end ();) {
        for (auto candidate = node->second.begin ();
             candidate != node->second.end ();) {
            if (candidate->second.remote_endpoint != remote_endpoint) {
                ++candidate;
                continue;
            }
            removed.emplace_back (node->first, candidate->first);
            candidate = node->second.erase (candidate);
        }
        if (node->second.empty ())
            node = _candidates.erase (node);
        else
            ++node;
    }
    return removed;
}

std::size_t raw_mesh_connection_candidates_t::size (
  const std::vector<std::uint8_t> &node_routing_id) const
{
    const auto found = _candidates.find (node_routing_id);
    return found == _candidates.end () ? 0 : found->second.size ();
}

bool raw_mesh_connection_candidates_t::contains (
  const std::vector<std::uint8_t> &node_routing_id,
  const std::vector<std::uint8_t> &connection_id) const
{
    const auto found = _candidates.find (node_routing_id);
    return found != _candidates.end () && found->second.contains (connection_id);
}

bool raw_mesh_connection_candidates_t::endpoint_in_use_by_other (
  std::string_view remote_endpoint,
  const std::vector<std::uint8_t> &excluded_node_routing_id) const
{
    if (remote_endpoint.empty ())
        return false;
    for (const auto &[node_routing_id, candidates] : _candidates) {
        if (node_routing_id == excluded_node_routing_id)
            continue;
        for (const auto &[_, candidate] : candidates) {
            if (candidate.remote_endpoint == remote_endpoint)
                return true;
        }
    }
    return false;
}

raw_mesh_node_owner_t::raw_mesh_node_owner_t (
  raw_mesh_node_options_t options,
  std::shared_ptr<zlink::context_t> context) :
    _options (std::move (options)),
    _context (
      context ? std::move (context) : std::make_shared<zlink::context_t> ()),
    _topology (_options.descriptor),
    _mailbox (_options.application_message_budget,
              _options.application_byte_budget,
              _options.infrastructure_message_budget,
              _options.infrastructure_byte_budget),
    _operations (
      std::make_shared<foundation::operation_registry_t> (
        foundation::default_operation_capacity))
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
    auto router = std::make_unique<zlink::router_socket_t> (*_context);
    router->options ().handover (true);
    router->options ().mandatory (true);
    router->options ().linger (std::chrono::milliseconds (0));
    router->set_routing_id (
      zlink::routing_id_t::from (_options.descriptor.node_routing_id));
    application_job_queue_t::receive_flow_registration_t
      receive_flow_registration;
    if (_options.application_jobs) {
        receive_flow_registration =
          _options.application_jobs->register_receive_flow_socket (
            [socket = router.get ()] (
              application_job_queue_pressure_state_t state) {
                return apply_application_job_receive_flow_state (
                  *socket, state);
            });
    }
    auto monitor = std::make_unique<zlink::socket_monitor_t> (
      router->monitor_open (zlink::monitor_event::connection_ready
                            | zlink::monitor_event::disconnected));
    router->bind (_options.descriptor.advertised_endpoint);

    auto descriptor = _topology.local_descriptor ();
    if (descriptor.descriptor_revision
        == std::numeric_limits<std::uint64_t>::max ()) {
        throw std::overflow_error ("service descriptor revision is exhausted");
    }
    descriptor.advertised_endpoint = transport::advertised_tcp_endpoint (
      router->options ().last_endpoint (), _options.advertise_host, "MeshNode");
    ++descriptor.descriptor_revision;

    _port = std::make_shared<detail::backend::raw_route_port_t> (
      *router, &_socket_mutex,
      zlink::poll_event_flag_t::pollin);
    auto monitor_poller = std::make_unique<zlink::poller_t> ();
    monitor_poller->add (*monitor, zlink::poll_event_flag_t::pollin, 1);
    _monitor_poller = std::move (monitor_poller);
    _monitor = std::move (monitor);
    _router = std::move (router);
    _receive_flow_registration =
      std::move (receive_flow_registration);
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
    application_job_queue_t::receive_flow_registration_t
      receive_flow_registration;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        _closed = true;
        _pending_admissions.clear ();
        _pending_admission_bytes = 0;
        port = std::move (_port);
        monitor = std::move (_monitor);
        monitor_poller = std::move (_monitor_poller);
        receive_flow_registration =
          std::move (_receive_flow_registration);
        router = std::move (_router);
    }
    receive_flow_registration.close ();
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
}

bool raw_mesh_node_owner_t::started () const noexcept
{
    std::lock_guard lifecycle_lock (_lifecycle_mutex);
    return static_cast<bool> (_port);
}

std::string raw_mesh_node_owner_t::endpoint () const
{
    return _topology.local_descriptor ().advertised_endpoint;
}

zlink::context_t &raw_mesh_node_owner_t::context ()
{
    std::lock_guard lifecycle_lock (_lifecycle_mutex);
    if (!_router || !_context) {
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
        trace_mesh ("connect endpoint=" + endpoint);
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
        trace_mesh ("connect endpoint=" + endpoint
                    + " expected=" + owner_key (expected_descriptor.node_routing_id));
        _router->options ().connect_routing_id (
          zlink::routing_id_t::from (
            expected_descriptor.node_routing_id));
        _router->connect (endpoint);
        _outbound_endpoints.insert (endpoint);
        return true;
    }
    catch (...) {
        return false;
    }
}

void raw_mesh_node_owner_t::disconnect_peer (const std::string &endpoint) noexcept
{
    (void) disconnect_peer ({}, endpoint);
}

bool raw_mesh_node_owner_t::disconnect_peer (
  const std::vector<std::uint8_t> &expected_routing_id,
  const std::string &endpoint) noexcept
{
    if (endpoint.empty ())
        return false;
    std::lock_guard lifecycle_lock (_lifecycle_mutex);
    if (!_router)
        return false;
    try {
        trace_mesh ("disconnect endpoint=" + endpoint
                    + " expected=" + owner_key (expected_routing_id));
        std::optional<admitted_peer_t> admitted;
        if (!expected_routing_id.empty ()) {
            admitted = _topology.peer (expected_routing_id);
        }
        else {
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
                        + " connection="
                        + owner_key (admitted->connection_id));
            if (expected_routing_id.empty ())
                (void) _connections.disconnect (
                  admitted->descriptor.node_routing_id,
                  admitted->connection_id);
            (void) _topology.disconnect (admitted->descriptor.node_routing_id,
                                         admitted->connection_id);
            (void) _liveness.disconnect (admitted->descriptor.node_routing_id,
                                         admitted->connection_id);
            discard_pending_admissions_locked (
              admitted->descriptor.node_routing_id);
        }
        else if (expected_routing_id.empty ()) {
            const auto candidates = _connections.disconnect_by_endpoint (endpoint);
            trace_mesh ("disconnect admitted=none candidates="
                        + std::to_string (candidates.size ()));
            for (const auto &[node_routing_id, connection_id] : candidates) {
                (void) _topology.disconnect (node_routing_id, connection_id);
                (void) _liveness.disconnect (node_routing_id, connection_id);
                discard_pending_admissions_locked (node_routing_id);
            }
        }
        if (!expected_routing_id.empty ()) {
            const auto candidates =
              _connections.disconnect_all (expected_routing_id);
            for (const auto &connection_id : candidates) {
                (void) _topology.disconnect (
                  expected_routing_id, connection_id);
                (void) _liveness.disconnect (
                  expected_routing_id, connection_id);
            }
            discard_pending_admissions_locked (expected_routing_id);
        }
        bool endpoint_in_use_by_other = false;
        if (!expected_routing_id.empty ()) {
            endpoint_in_use_by_other =
              _connections.endpoint_in_use_by_other (
                endpoint, expected_routing_id);
            if (!endpoint_in_use_by_other) {
                for (const auto &candidate : _topology.peers ()) {
                    if (candidate.descriptor.node_routing_id
                          != expected_routing_id
                        && candidate.descriptor.advertised_endpoint
                             == endpoint) {
                        endpoint_in_use_by_other = true;
                        break;
                    }
                }
            }
        }
        if (!expected_routing_id.empty ()) {
            std::lock_guard socket_lock (_socket_mutex);
            try {
                _router->disconnect_rid (
                  zlink::routing_id_t::from (expected_routing_id));
            }
            catch (...) {
                /* The routing-id index can already be gone after a monitor
                 * replacement event. Endpoint teardown remains required to
                 * disable reconnect for every pipe configured by this owner. */
            }
        }
        if (!endpoint_in_use_by_other) {
            try {
                std::lock_guard socket_lock (_socket_mutex);
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
    peer_admission_result_t admitted;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        if (!_router) {
            return peer_admission_result_t::invalid_descriptor;
        }
        auto node_routing_id = descriptor.node_routing_id;
        auto liveness_connection_id = connection_id;
        admitted =
          _topology.admit (std::move (descriptor), std::move (connection_id));
        if (admitted != peer_admission_result_t::admitted) {
            return admitted;
        }
        _liveness.admit (std::move (node_routing_id),
                         std::move (liveness_connection_id), now);
    }
    return admitted;
}

task_t<bool> raw_mesh_node_owner_t::send_to_node (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::application_payload_t &application_payload)
{
    co_return co_await send_to_node_result (
                target_routing_id, application_payload)
              == zlink::submit_result_t::ok;
}

task_t<zlink::submit_result_t> raw_mesh_node_owner_t::send_to_node_result (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::application_payload_t &application_payload)
{
    co_return co_await send_with_header_result (
      target_routing_id, protocol::encode_node_send_header (),
      application_payload);
}

task_t<bool> raw_mesh_node_owner_t::request_to_node (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::application_payload_t &application_payload,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback,
  std::optional<std::uint64_t> correlation)
{
    co_return co_await request_to_target (
      target_routing_id, application_payload, timeout, std::move (callback),
      std::nullopt, correlation);
}

task_t<bool> raw_mesh_node_owner_t::request_to_channel (
  const std::string &channel_name,
  const protocol::application_payload_t &application_payload,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback,
  std::optional<std::uint64_t> correlation)
{
    const auto selected = _topology.select (channel_name);
    if (!selected) {
        co_return false;
    }
    co_return co_await request_to_target (
      selected->descriptor.node_routing_id, application_payload, timeout,
      std::move (callback), channel_name, correlation);
}

task_t<bool> raw_mesh_node_owner_t::request_to_target (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::application_payload_t &application_payload,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback,
  const std::optional<std::string> &channel_name,
  std::optional<std::uint64_t> correlation)
{
    co_return co_await request_with_header (
      target_routing_id,
      [channel_name] (std::uint64_t correlation) {
          return channel_name
                   ? protocol::encode_channel_request_header (
                       correlation, *channel_name)
                   : protocol::encode_node_request_header (correlation);
      },
      application_payload, timeout, std::move (callback), correlation);
}

task_t<bool> raw_mesh_node_owner_t::submit_request (
  const std::shared_ptr<detail::backend::raw_route_port_t> &port,
  const pending_request_t &request,
  std::chrono::milliseconds timeout)
{
    if (!port || timeout <= std::chrono::milliseconds::zero ()) {
        (void) _operations->fail (
          request.operation, foundation::operation_terminal_t::transport_failed);
        co_return false;
    }
    const auto operations = _operations;
    trace_mesh (
      "request-submit correlation="
        + std::to_string (request.correlation)
        + " targetBytes="
        + std::to_string (request.target_routing_id.size ()));
    auto running = std::make_shared<
      task_t<detail::backend::raw_request_completion_t>> (
        port->request (request.target_routing_id, request.wire, timeout));
    detail::observe_task_completion (
      *running, [operations, request, running] (
                  const result_t<detail::backend::raw_request_completion_t> &
                    settled) {
          if (!settled) {
              (void) operations->fail (
                request.operation,
                foundation::operation_terminal_t::transport_failed);
              return;
          }
          auto completion = settled.value ();
          trace_mesh (
            "request-completion correlation="
              + std::to_string (request.correlation)
              + " result="
              + std::to_string (static_cast<int> (completion.result))
              + " parts=" + std::to_string (completion.parts.size ()));
          if (completion.result
              != detail::backend::raw_request_result_t::ok) {
              const auto terminal =
                completion.result
                    == detail::backend::raw_request_result_t::timed_out
                  ? foundation::operation_terminal_t::timed_out
                : completion.result
                      == detail::backend::raw_request_result_t::terminated
                  ? foundation::operation_terminal_t::shutdown
                  : foundation::operation_terminal_t::transport_failed;
              (void) operations->fail (request.operation, terminal);
              return;
          }
          try {
              auto &parts = completion.parts;
              if (parts.empty () || parts.size () > 2) {
                  throw protocol::service_wire_error_t (
                    "request reply has an invalid part count");
              }
              const auto reply = protocol::decode_reply_header (parts.front ());
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
              if (parts.front ().size () != 21) {
                  throw protocol::service_wire_error_t (
                    "generic request reply carries an operation-specific "
                    "tail");
              }
              if (reply.correlation != request.correlation) {
                  throw protocol::service_wire_error_t (
                    "request reply correlation does not match");
              }
              if (reply.terminal_result != 0) {
                  if (parts.size () != 1) {
                      throw protocol::service_wire_error_t (
                        "failed request reply cannot carry a payload");
                  }
                  (void) operations->fail (
                    request.operation,
                    foundation::operation_terminal_t::transport_failed,
                    parts.front ());
                  return;
              }
              if (parts.size () != 2) {
                  throw protocol::service_wire_error_t (
                    "successful request reply must carry a payload");
              }
              (void) protocol::decode_application_payload (parts[1], false);
              (void) operations->complete (
                request.operation, std::move (parts[1]));
          }
          catch (const protocol::service_wire_error_t &) {
              //  Spec 32-framework-error-model:91-92 — a malformed reply is
              //  ProtocolError, not a transport failure. Carry a synthesized
              //  protocolError header; complete_operation decodes it into
              //  terminal 104 instead of collapsing to internal_error.
              (void) operations->fail (
                request.operation,
                foundation::operation_terminal_t::transport_failed,
                protocol::encode_reply_header (
                  request.correlation, 104, 16));
          }
      });
    co_return true;
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

void raw_mesh_node_owner_t::discard_pending_admissions (
  const std::vector<std::uint8_t> &node_routing_id)
{
    std::lock_guard lifecycle_lock (_lifecycle_mutex);
    discard_pending_admissions_locked (node_routing_id);
}

void raw_mesh_node_owner_t::discard_pending_admissions_locked (
  const std::vector<std::uint8_t> &node_routing_id)
{
    std::size_t discarded = 0;
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
            + std::to_string (_pending_admissions.size ()));
}

task_t<bool> raw_mesh_node_owner_t::request_with_header (
  const std::vector<std::uint8_t> &target_routing_id,
  const std::function<std::vector<std::uint8_t> (std::uint64_t)> &header,
  const protocol::application_payload_t &application_payload,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback,
  std::optional<std::uint64_t> requested_correlation)
{
    if (timeout <= std::chrono::milliseconds::zero ()) {
        throw std::invalid_argument ("raw mesh request timeout must be positive");
    }
    if (!_topology.peer (target_routing_id)) {
        co_return false;
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
        correlation = take_reply_route_id_locked (requested_correlation);
    }
    const auto id =
      operation_id (local.lifecycle_generation, correlation);
    if (!_operations->register_operation (
          id, foundation::operation_registry_t::clock_t::now () + timeout,
          std::move (callback))) {
        co_return false;
    }
    pending_request_t request{
      target_routing_id,
      {header (correlation),
       protocol::encode_application_payload (application_payload)},
      id,
      correlation};
    co_return co_await submit_request (port, request, timeout);
}

task_t<zlink::submit_result_t> raw_mesh_node_owner_t::send_with_header_result (
  const std::vector<std::uint8_t> &target_routing_id,
  std::vector<std::uint8_t> header,
  const protocol::application_payload_t &application_payload,
  detail::backend::raw_send_stage_trace_t trace)
{
    if (!_topology.peer (target_routing_id)) {
        if (trace)
            trace ("router_admission_submit", "not_connected");
        co_return zlink::submit_result_t::not_connected;
    }
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port) {
        if (trace)
            trace ("router_admission_submit", "terminated");
        co_return zlink::submit_result_t::terminated;
    }
    detail::backend::raw_message_t parts{
      std::move (header),
      protocol::encode_application_payload (application_payload)};
    const auto result = co_await port->send_result (
      target_routing_id, parts, std::move (trace));
    co_return result;
}

task_t<bool> raw_mesh_node_owner_t::send_with_header (
  const std::vector<std::uint8_t> &target_routing_id,
  std::vector<std::uint8_t> header,
  const protocol::application_payload_t &application_payload)
{
    co_return co_await send_with_header_result (
                target_routing_id, std::move (header), application_payload)
              == zlink::submit_result_t::ok;
}

task_t<bool> raw_mesh_node_owner_t::send_header_only (
  const std::vector<std::uint8_t> &target_routing_id,
  std::vector<std::uint8_t> header)
{
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port)
        co_return false;
    detail::backend::raw_message_t parts{std::move (header)};
    const auto sent = co_await port->send (target_routing_id, parts);
    co_return sent;
}

task_t<bool> raw_mesh_node_owner_t::send_session_relocation_route (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::session_relocation_route_t &route)
{
    const auto local = _topology.local_descriptor ();
    if (target_routing_id == local.node_routing_id) {
        if (route.session_owner_node_routing_id
              != local.node_routing_id
            || route.session_owner_node_generation
                 != local.lifecycle_generation
            || route.sender_role
                 != protocol::relocation_role_t::target
            || route.route.target_node_routing_id
                 != local.node_routing_id
            || route.route.target_node_generation
                 != local.lifecycle_generation) {
            co_return false;
        }
        co_return _mailbox.try_enqueue (
          service_mailbox_record_t{
            owner_key (local.node_routing_id),
            service_mailbox_domain_t::infrastructure,
            {protocol::encode_session_relocation_route (route)},
            local.node_routing_id,
            std::nullopt,
            std::nullopt,
            local.lifecycle_generation});
    }
    co_return co_await send_header_only (
      target_routing_id,
      protocol::encode_session_relocation_route (route));
}

task_t<bool> raw_mesh_node_owner_t::send_session_relocation_seal (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::session_relocation_seal_t &seal)
{
    co_return co_await send_header_only (
      target_routing_id,
      protocol::encode_session_relocation_seal (seal));
}

task_t<bool> raw_mesh_node_owner_t::request_session_relocation_seal (
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
        co_return false;
    if (co_await send_session_relocation_seal (target_routing_id, seal))
        co_return true;
    (void) _operations->fail (
      operation, foundation::operation_terminal_t::transport_failed);
    co_return false;
}

task_t<bool> raw_mesh_node_owner_t::send_session_relocation_sealed (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::session_relocation_sealed_t &sealed)
{
    co_return co_await send_header_only (
      target_routing_id,
      protocol::encode_session_relocation_sealed (sealed));
}

task_t<bool> raw_mesh_node_owner_t::send_reply_relay (
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
        co_return false;
    }
    std::vector<std::vector<std::uint8_t>> parts;
    parts.emplace_back (protocol::encode_reply_relay (relay));
    if (application_reply) {
        parts.emplace_back (
          protocol::encode_application_payload (*application_reply));
    }
    co_return co_await port->send (target_routing_id, parts);
}

task_t<bool> raw_mesh_node_owner_t::send_reply_relay_ack (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::reply_relay_ack_t &ack)
{
    co_return co_await send_header_only (
      target_routing_id, protocol::encode_reply_relay_ack (ack));
}

task_t<bool> raw_mesh_node_owner_t::send_message_follow (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::message_follow_notice_t &notice)
{
    co_return co_await send_header_only (
      target_routing_id, protocol::encode_message_follow (notice));
}

task_t<bool> raw_mesh_node_owner_t::send_relocation_control (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::relocation_control_t &control)
{
    co_return co_await send_header_only (
      target_routing_id, protocol::encode_relocation_control (control));
}

task_t<relocation_prepare_response_t>
raw_mesh_node_owner_t::request_relocation_prepare (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::relocation_prepare_t &prepare,
  std::chrono::milliseconds timeout,
  std::vector<protocol::session_relocation_route_t> session_routes)
{
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port || timeout <= std::chrono::milliseconds::zero ())
        co_return relocation_prepare_response_t{};
    detail::backend::raw_message_t parts;
    parts.emplace_back (
      protocol::encode_relocation_control (prepare));
    for (const auto &route : session_routes)
        parts.emplace_back (
          protocol::encode_session_relocation_route (route));
    auto pending = port->request (target_routing_id, parts, timeout);
    const auto completed = co_await pending;
    if (completed.result != detail::backend::raw_request_result_t::ok
        || completed.parts.size () != 1)
        co_return relocation_prepare_response_t{};
    // Exact-identity fencing (spec 15 §4.2 / spec 28): a reply whose
    // identity fields do not match the prepare this call sent is a stale
    // or wrong-attempt reply and must not resolve this call either way —
    // neither as ready nor as an explicit failure.
    const auto identity_matches =
      [&prepare] (const auto &relocation,
                  std::uint64_t target_attempt_generation,
                  const auto &coordinator, const auto &target,
                  const auto &object) {
          return relocation == prepare.relocation
                 && target_attempt_generation
                      == prepare.target_attempt_generation
                 && coordinator == prepare.coordinator
                 && target == prepare.target && object == prepare.object;
      };
    relocation_prepare_response_t response;
    try {
        const auto control =
          protocol::decode_relocation_control (completed.parts.front ());
        if (const auto *ready =
              std::get_if<protocol::relocation_ready_t> (&control)) {
            if (identity_matches (
                  ready->relocation, ready->target_attempt_generation,
                  ready->coordinator, ready->target, ready->object))
                response.ready = *ready;
        } else if (const auto *failed =
                     std::get_if<protocol::relocation_failed_t> (&control)) {
            // The bug this closes: previously only relocation_ready_t was
            // ever extracted here — an explicit, identity-matched
            // relocationFailed(53) reply fell through to the same "no
            // result" the caller also observes on a genuine timeout,
            // discarding both the fast, explicit rejection and its
            // failure_code.
            if (identity_matches (
                  failed->relocation, failed->target_attempt_generation,
                  failed->coordinator, failed->target, failed->object))
                response.failed = *failed;
        }
    }
    catch (const protocol::service_wire_error_t &) {
    }
    co_return response;
}

bool raw_mesh_node_owner_t::reply_relocation_ready (
  const service_mailbox_record_t &request,
  const protocol::relocation_ready_t &ready)
{
    if (request.source_routing_id.empty () || !request.request_sequence)
        return false;
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port)
        return false;
    detail::backend::raw_message_t parts{
      protocol::encode_relocation_control (ready)};
    return port->reply (
      detail::backend::raw_received_t{
        request.source_routing_id, request.request_sequence, {},
        request.retained},
      parts);
}

bool raw_mesh_node_owner_t::reply_relocation_failed (
  const service_mailbox_record_t &request,
  const protocol::relocation_failed_t &failure)
{
    if (request.source_routing_id.empty () || !request.request_sequence)
        return false;
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port)
        return false;
    detail::backend::raw_message_t parts{
      protocol::encode_relocation_control (failure)};
    return port->reply (
      detail::backend::raw_received_t{
        request.source_routing_id, request.request_sequence, {},
        request.retained},
      parts);
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
    if (!port)
        return false;
    detail::backend::raw_message_t parts{
      protocol::encode_reply_header (*request.correlation, 0, 0),
      protocol::encode_application_payload (application_payload)};
    return port->reply (
      detail::backend::raw_received_t{
        request.source_routing_id, request.request_sequence, {},
        request.retained},
      parts);
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
    if (!port)
        return false;
    detail::backend::raw_message_t parts{
      protocol::encode_reply_header (
        *request.correlation, terminal_result, failure_code)};
    return port->reply (
      detail::backend::raw_received_t{
        request.source_routing_id, request.request_sequence, {},
        request.retained},
      parts);
}

std::size_t raw_mesh_node_owner_t::expire_requests (
  foundation::operation_registry_t::clock_t::time_point now)
{
    static_cast<void> (now);
    return _operations->expire (now);
}

task_t<bool> raw_mesh_node_owner_t::send_to_channel (
  const std::string &channel_name,
  const protocol::application_payload_t &application_payload)
{
    co_return co_await send_to_channel_result (
                channel_name, application_payload)
              == zlink::submit_result_t::ok;
}

task_t<zlink::submit_result_t> raw_mesh_node_owner_t::send_to_channel_result (
  const std::string &channel_name,
  const protocol::application_payload_t &application_payload)
{
    const auto selected = _topology.select (channel_name);
    if (!selected) {
        co_return zlink::submit_result_t::not_found;
    }
    co_return co_await send_with_header_result (
      selected->descriptor.node_routing_id,
      protocol::encode_channel_send_header (channel_name),
      application_payload);
}

task_t<bool> raw_mesh_node_owner_t::send_to_spot (
  const std::vector<std::uint8_t> &target_routing_id,
  const std::string &source_spot_id,
  const protocol::spot_route_fence_t &target,
  const protocol::application_payload_t &application_payload)
{
    co_return co_await send_to_spot_result (
                target_routing_id, source_spot_id, target,
                application_payload)
              == zlink::submit_result_t::ok;
}

task_t<zlink::submit_result_t> raw_mesh_node_owner_t::send_to_spot_result (
  const std::vector<std::uint8_t> &target_routing_id,
  const std::string &source_spot_id,
  const protocol::spot_route_fence_t &target,
  const protocol::application_payload_t &application_payload)
{
    const auto sequence = next_operation_sequence ();
    const auto local = _topology.local_descriptor ();
    co_return co_await send_with_header_result (
      target_routing_id,
      protocol::encode_spot_message_header (
        protocol::command::spotSend, source_spot_id, target,
        {local.lifecycle_generation, sequence}),
      application_payload);
}

task_t<bool> raw_mesh_node_owner_t::request_to_spot (
  const std::vector<std::uint8_t> &target_routing_id,
  const std::string &source_spot_id,
  const protocol::spot_route_fence_t &target,
  const protocol::application_payload_t &application_payload,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback,
  std::optional<protocol::wire_operation_id_t> operation,
  std::optional<std::uint64_t> correlation)
{
    const auto local = _topology.local_descriptor ();
    co_return co_await request_with_header (
      target_routing_id,
      [source_spot_id, target, local, operation] (std::uint64_t correlation) {
          const auto exact = operation.value_or (
            protocol::wire_operation_id_t{
              local.lifecycle_generation, correlation});
          return protocol::encode_spot_message_header (
            protocol::command::spotRequest, source_spot_id,
            target, exact, correlation);
      },
      application_payload, timeout, std::move (callback), correlation);
}

task_t<bool> raw_mesh_node_owner_t::send_to_actor (
  const std::vector<std::uint8_t> &target_routing_id,
  const std::optional<std::pair<std::string, std::uint64_t>> &source_actor,
  const protocol::actor_route_fence_t &target,
  const protocol::application_payload_t &application_payload,
  std::optional<protocol::actor_message_header_t::bound_session_source_t>
    bound_session_source)
{
    co_return co_await send_to_actor_result (
                target_routing_id, source_actor, target, application_payload,
                std::move (bound_session_source))
              == zlink::submit_result_t::ok;
}

task_t<zlink::submit_result_t> raw_mesh_node_owner_t::send_to_actor_result (
  const std::vector<std::uint8_t> &target_routing_id,
  const std::optional<std::pair<std::string, std::uint64_t>> &source_actor,
  const protocol::actor_route_fence_t &target,
  const protocol::application_payload_t &application_payload,
  std::optional<protocol::actor_message_header_t::bound_session_source_t>
    bound_session_source)
{
    const auto sequence = next_operation_sequence ();
    const auto local = _topology.local_descriptor ();
    co_return co_await send_with_header_result (
      target_routing_id,
      protocol::encode_actor_message_header (
        protocol::command::actorSend, source_actor, target,
        {local.lifecycle_generation, sequence}, std::nullopt, 0,
        std::move (bound_session_source)),
      application_payload);
}

task_t<bool> raw_mesh_node_owner_t::request_to_actor (
  const std::vector<std::uint8_t> &target_routing_id,
  const std::optional<std::pair<std::string, std::uint64_t>> &source_actor,
  const protocol::actor_route_fence_t &target,
  const protocol::application_payload_t &application_payload,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback,
  std::optional<protocol::wire_operation_id_t> operation,
  std::optional<protocol::actor_message_header_t::bound_session_source_t>
    bound_session_source,
  std::optional<std::uint64_t> correlation)
{
    const auto local = _topology.local_descriptor ();
    co_return co_await request_with_header (
      target_routing_id,
      [source_actor, target, local, operation,
       bound_session_source = std::move (bound_session_source)] (
        std::uint64_t correlation) {
          const auto exact = operation.value_or (
            protocol::wire_operation_id_t{
              local.lifecycle_generation, correlation});
          return protocol::encode_actor_message_header (
            protocol::command::actorRequest, source_actor, target,
            exact, correlation, 0, bound_session_source);
      },
      application_payload, timeout, std::move (callback), correlation);
}

task_t<bool> raw_mesh_node_owner_t::request_user_spot_create (
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
    co_return co_await request_infrastructure (
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
              (void) protocol::decode_application_payload (parts[1], false);
          return pack_infrastructure_reply (parts);
      },
      timeout, std::move (callback));
}

task_t<bool> raw_mesh_node_owner_t::send_bound_session (
  const std::vector<std::uint8_t> &session_owner_routing_id,
  const protocol::bound_session_send_t &record,
  const protocol::application_payload_t &application_payload)
{
    co_return co_await send_bound_session_result (
                session_owner_routing_id, record, application_payload)
              == zlink::submit_result_t::ok;
}

task_t<zlink::submit_result_t> raw_mesh_node_owner_t::send_bound_session_result (
  const std::vector<std::uint8_t> &session_owner_routing_id,
  const protocol::bound_session_send_t &record,
  const protocol::application_payload_t &application_payload,
  detail::backend::raw_send_stage_trace_t trace)
{
    const auto local = _topology.local_descriptor ();
    if (record.actor.target_node_routing_id != local.node_routing_id
        || record.actor.target_node_generation
             != local.lifecycle_generation) {
        throw std::invalid_argument (
          "bound Session send Actor authority source is inconsistent");
    }
    co_return co_await send_with_header_result (
      session_owner_routing_id,
      protocol::encode_bound_session_send (record),
      application_payload, std::move (trace));
}

task_t<bool> raw_mesh_node_owner_t::request_bound_session_bind (
  const std::vector<std::uint8_t> &actor_owner_routing_id,
  protocol::bound_session_bind_t record,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback)
{
    if (record.actor.target_node_routing_id
          != actor_owner_routing_id) {
        throw std::invalid_argument (
          "bound Session bind Actor authority target is inconsistent");
    }
    if (timeout <= std::chrono::milliseconds::zero ()) {
        throw std::invalid_argument (
          "bound Session bind request timeout must be positive");
    }

    const auto deadline = foundation::operation_registry_t::clock_t::now ()
                          + timeout;
    const auto local = _topology.local_descriptor ();
    const auto operations = _operations;
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    std::uint64_t correlation = 0;
    foundation::call_id_t id{};
    bool registered = false;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
        if (port) {
            correlation = take_reply_route_id_locked ();
            id = operation_id (local.lifecycle_generation, correlation);
            registered = operations->register_operation (
              id, deadline + infrastructure_not_connected_retry_interval,
              std::move (callback));
        }
    }
    if (!port) {
        callback (foundation::operation_terminal_t::shutdown, {});
        co_return true;
    }
    if (!registered)
        co_return false;

    record.correlation = correlation;
    detail::backend::raw_message_t request_parts{
      protocol::encode_bound_session_bind (record)};
    co_return co_await submit_registered_infrastructure_request_with_retry (
      std::move (port), operations, id, correlation,
      actor_owner_routing_id, std::move (request_parts),
      [] (const detail::backend::raw_message_t &parts) {
          if (parts.size () != 1)
              throw protocol::service_wire_error_t (
                "bound Session bind reply must contain one header");
          const auto reply =
            protocol::decode_reply_header (parts.front ());
          if (!protocol::valid_terminal_failure (
                reply.terminal_result,
                static_cast<protocol::framework_error_code> (
                  reply.failure_code))) {
              throw protocol::service_wire_error_t (
                "bound Session bind reply terminal is inconsistent");
          }
          return parts.front ();
      }, deadline, foundation::operation_terminal_t::timed_out);
}

task_t<bool> raw_mesh_node_owner_t::send_bound_session_replaced (
  const std::vector<std::uint8_t> &retired_session_owner_routing_id,
  const protocol::bound_session_replaced_t &record)
{
    const auto local = _topology.local_descriptor ();
    if (record.actor_authority.target_node_routing_id
          != local.node_routing_id
        || record.actor_authority.target_node_generation
             != local.lifecycle_generation
        || record.retired_session.session_owner_node_routing_id
             != retired_session_owner_routing_id) {
        throw std::invalid_argument (
          "bound Session replacement source or target is inconsistent");
    }
    co_return co_await send_header_only (
      retired_session_owner_routing_id,
      protocol::encode_bound_session_replaced (record));
}

bool raw_mesh_node_owner_t::reply_bound_session_bind (
  const service_mailbox_record_t &request,
  std::uint32_t terminal_result,
  std::uint32_t failure_code)
{
    if (!request.correlation)
        throw std::invalid_argument (
          "bound Session bind reply requires a correlation");
    return reply_infrastructure (
      request,
      protocol::encode_reply_header (
        *request.correlation, terminal_result, failure_code));
}

task_t<bool> raw_mesh_node_owner_t::request_actor_create (
  std::vector<std::uint8_t> target_routing_id,
  protocol::actor_create_header_t request,
  std::chrono::milliseconds timeout,
  foundation::operation_registry_t::callback_t callback)
{
    if (timeout <= std::chrono::milliseconds::zero ()) {
        throw std::invalid_argument (
          "Actor create request timeout must be positive");
    }
    const auto local = _topology.local_descriptor ();
    if (request.source_node_routing_id != local.node_routing_id
        || request.source_node_generation != local.lifecycle_generation
        || request.reservation.target_node_routing_id
             != target_routing_id)
        throw std::invalid_argument (
          "Actor create source or target fence is inconsistent");

    if (target_routing_id == local.node_routing_id) {
        co_return co_await request_infrastructure (
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
                  (void) protocol::decode_application_payload (parts[1], false);
              return pack_infrastructure_reply (parts);
          }, timeout, std::move (callback));
    }

    // The ROUTER route can become available after the location reservation.
    // Submit first because topology admission is not required in every
    // deployment. Transient route absence (including host/network unreachable)
    // is retried asynchronously within the original request deadline.
    const auto deadline = foundation::operation_registry_t::clock_t::now ()
                          + timeout;
    const auto operations = _operations;
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    std::uint64_t correlation = 0;
    foundation::call_id_t id{};
    bool registered = false;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
        if (port) {
            correlation = take_reply_route_id_locked ();
            id = operation_id (local.lifecycle_generation, correlation);
            registered = operations->register_operation (
              id, deadline + infrastructure_not_connected_retry_interval,
              std::move (callback));
        }
    }
    if (!port) {
        callback (foundation::operation_terminal_t::shutdown, {});
        co_return true;
    }
    if (!registered)
        co_return false;

    detail::backend::raw_message_t request_parts{
      [request = std::move (request)] (
        std::uint64_t correlation) mutable {
          request.correlation = correlation;
          return protocol::encode_actor_create_header (request);
      } (correlation)};
    co_return co_await submit_registered_infrastructure_request_with_retry (
      std::move (port), operations, id, correlation,
      std::move (target_routing_id), std::move (request_parts),
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
              (void) protocol::decode_application_payload (parts[1], false);
          return pack_infrastructure_reply (parts);
      }, deadline);
}

task_t<actor_join_wire_outcome_t>
raw_mesh_node_owner_t::request_actor_join (
  const std::vector<std::uint8_t> &target_routing_id,
  const protocol::actor_join_request_t &request,
  const std::optional<protocol::application_payload_t> &payload,
  std::chrono::milliseconds timeout)
{
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
        parts = protocol::encode_actor_join_28 ({
          request.correlation,
          {request.actor.actor_id, request.actor.object_generation,
           request.actor.target_node_routing_id,
           request.actor.target_node_generation,
           request.actor.authority_owner_generation,
           request.actor.owner_lease_generation},
          request.entry,
          {request.target_spot.spot_id, request.target_spot.object_generation,
           request.target_spot.target_node_routing_id,
           request.target_spot.target_node_generation,
           request.target_spot.authority_owner_generation,
           request.target_spot.owner_lease_generation},
          payload
            ? std::optional<
                protocol::service_wire_pilot_application_payload_envelope_v1>{
                {payload->packet_name, payload->content_type, payload->payload}}
            : std::nullopt});
    }
    catch (const std::invalid_argument &) {
        throw protocol::service_wire_error_t ("invalid Actor join request");
    }
    auto pending = port->request (target_routing_id, parts, timeout);
    const auto completed = co_await pending;
    if (completed.result != detail::backend::raw_request_result_t::ok) {
        outcome.failure =
          completed.result == detail::backend::raw_request_result_t::timed_out
            ? actor_join_wire_failure_t::deadline_exceeded
            : actor_join_wire_failure_t::unavailable;
        co_return outcome;
    }
    if (completed.parts.empty () || completed.parts.size () > 2) {
        //  A conformant peer never produces this part count for a
        //  reply(20)+actorJoin tail — malformed wire, not unavailability.
        outcome.failure = actor_join_wire_failure_t::protocol_error;
        co_return outcome;
    }
    try {
        auto reply =
          protocol::decode_actor_join_reply (completed.parts.front ());
        // Exact-identity fencing: a reply whose correlation does not match
        // the request this call sent is a stale or misrouted reply and must
        // not resolve this call either way.
        if (reply.header.correlation != request.correlation) {
            outcome.failure = actor_join_wire_failure_t::protocol_error;
            co_return outcome;
        }
        if (completed.parts.size () == 2) {
            //  Surface the decoded application reply to the caller instead
            //  of validating and discarding it.
            outcome.application_reply = protocol::decode_application_payload (
              completed.parts[1], false);
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
    detail::backend::raw_message_t parts{
      protocol::encode_instance_spot_activation_header (request)};
    if (metadata)
        parts.push_back (std::move (*metadata));
    parts.push_back (
      protocol::encode_application_payload (application_payload));
    if (target_routing_id == local.node_routing_id) {
        co_return _mailbox.try_enqueue (
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
    if (!port)
        co_return false;
    co_return co_await port->send (
      target_routing_id, std::move (parts));
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
            co_return false;
        }
        correlation = take_reply_route_id_locked ();
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
        co_return false;
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
        co_return accepted;
    }
    const auto operations = _operations;
    trace_mesh (
      "infrastructure-request-submit correlation="
        + std::to_string (correlation)
        + " targetBytes=" + std::to_string (target_routing_id.size ()));
    auto running = std::make_shared<
      task_t<detail::backend::raw_request_completion_t>> (
        port->request (target_routing_id, std::move (parts), timeout));
    detail::observe_task_completion (
      *running, [operations, id, correlation, running] (
                  const result_t<detail::backend::raw_request_completion_t> &
                    settled) {
          if (!settled) {
              (void) operations->fail (
                id, foundation::operation_terminal_t::transport_failed);
              return;
          }
          auto completion = settled.value ();
          if (completion.result
              != detail::backend::raw_request_result_t::ok) {
              const auto terminal =
                completion.result
                    == detail::backend::raw_request_result_t::timed_out
                  ? foundation::operation_terminal_t::timed_out
                : completion.result
                      == detail::backend::raw_request_result_t::terminated
                  ? foundation::operation_terminal_t::shutdown
                  : foundation::operation_terminal_t::transport_failed;
              (void) operations->fail (id, terminal);
              return;
          }
          try {
              auto &reply_parts = completion.parts;
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
                  (void) protocol::decode_application_payload (
                    reply_parts[1], false);
              (void) operations->complete (
                id, pack_infrastructure_reply (reply_parts));
          }
          catch (const protocol::service_wire_error_t &) {
              //  Spec 32-framework-error-model:91-92 — a reply that can't be
              //  processed is ProtocolError, not a transport failure. Complete
              //  with a synthesized protocolError header so the upper adapter
              //  and sink classify it via reply_header_exception.
              (void) operations->complete (
                id,
                pack_infrastructure_reply (
                  detail::backend::raw_message_t{
                    protocol::encode_reply_header (correlation, 104, 16)}));
          }
      });
    co_return true;
}

task_t<bool> raw_mesh_node_owner_t::request_user_spot_close (
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
    co_return co_await request_infrastructure (
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

task_t<bool> raw_mesh_node_owner_t::request_infrastructure (
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
            co_return false;
        }
        correlation = take_reply_route_id_locked ();
    }
    const auto id =
      operation_id (local.lifecycle_generation, correlation);
    if (!_operations->register_operation (
          id, foundation::operation_registry_t::clock_t::now () + timeout,
          std::move (callback))) {
        co_return false;
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
        co_return accepted;
    }
    const auto operations = _operations;
    detail::backend::raw_message_t request_parts{header (correlation)};
    co_return co_await submit_registered_infrastructure_request (
      std::move (port), std::move (operations), id, correlation,
      target_routing_id, std::move (request_parts), decode_reply, timeout);
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
          pack_infrastructure_reply ({std::move (header)}));
    }
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port)
        return false;
    detail::backend::raw_message_t parts{std::move (header)};
    return port->reply (
      detail::backend::raw_received_t{
        request.source_routing_id, request.request_sequence, {},
        request.retained},
      parts);
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
    detail::backend::raw_message_t parts{
      protocol::encode_user_spot_create_reply (
        reply.header.correlation,
        reply.header.terminal_result,
        reply.header.failure_code,
        reply.result,
        reply.spot_id,
        reply.object_generation)};
    if (application_reply) {
        parts.push_back (
          protocol::encode_application_payload (*application_reply));
    }
    const auto local = _topology.local_descriptor ();
    if (request.source_routing_id == local.node_routing_id) {
        return _operations->complete (
          operation_id (
            local.lifecycle_generation,
            *request.correlation),
          pack_infrastructure_reply (parts));
    }
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port)
        return false;
    return port->reply (
      detail::backend::raw_received_t{
        request.source_routing_id, request.request_sequence, {},
        request.retained},
      parts);
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
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port)
        return false;
    return port->reply (
      detail::backend::raw_received_t{
        request.source_routing_id, request.request_sequence, {},
        request.retained},
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
    if (!request.correlation || request.source_routing_id.empty ()
        || !request.request_sequence)
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
    detail::backend::raw_message_t parts{
      protocol::encode_actor_join_reply (
        *request.correlation, terminal_result, failure_code, join_result, spot, membership_epoch,
        receive_chunk_limit_bytes)};
    if (application_reply) {
        parts.push_back (protocol::encode_application_payload (*application_reply));
    }
    return port->reply (
      detail::backend::raw_received_t{
        request.source_routing_id, request.request_sequence, {},
        request.retained},
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
      protocol::encode_reply_header (
        *request.correlation, terminal_result, failure_code)};
    if (application_reply) {
        parts.push_back (
          protocol::encode_application_payload (*application_reply));
    }
    const auto local = _topology.local_descriptor ();
    if (request.source_routing_id == local.node_routing_id) {
        return _operations->complete (
          operation_id (
            local.lifecycle_generation,
            *request.correlation),
          pack_infrastructure_reply (parts));
    }
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port)
        return false;
    return port->reply (
      detail::backend::raw_received_t{
        request.source_routing_id,
        request.request_sequence,
        {},
        request.retained},
      parts);
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
        reply.header.correlation,
        reply.header.terminal_result,
        reply.header.failure_code,
        reply.closed));
}

raw_mesh_pump_result_t raw_mesh_node_owner_t::enqueue_received_or_retain (
  service_mailbox_record_t record,
  raw_mesh_pump_result_t accepted_result)
{
    record.retained = std::move (_received_owner_for_enqueue);
    const auto enqueue_result =
      _mailbox.try_enqueue_result (std::move (record));
    if (enqueue_result == service_mailbox_enqueue_result_t::accepted) {
        return accepted_result;
    }
    if (enqueue_result == service_mailbox_enqueue_result_t::closed)
        return raw_mesh_pump_result_t::backpressured;
    if (record.domain == service_mailbox_domain_t::application) {
        bool replied = false;
        if (record.request_sequence && record.correlation) {
            try {
                replied = reply_failure (
                  record,
                  static_cast<std::uint32_t> (
                    protocol::request_terminal_result::rejected),
                  static_cast<std::uint32_t> (
                    protocol::framework_error_code::workerQueueFull));
            }
            catch (...) {
            }
        }
        trace_mesh (
          "application-reject reason=owner-capacity action="
          + std::string (replied ? "reply-worker-queue-full"
                                 : "drop"));
        return raw_mesh_pump_result_t::backpressured;
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

task_t<raw_mesh_pump_result_t> raw_mesh_node_owner_t::pump_one (
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
        co_return raw_mesh_pump_result_t::no_data;
    }
    zlink::poll_event_flag_t readiness = zlink::poll_event_flag_t::none;
    try {
        /* The ROUTER's normal receive pump owns both infrastructure and
         * application traffic. */
        readiness = port->poll (std::chrono::milliseconds::zero ());
    }
    catch (...) {
        co_return raw_mesh_pump_result_t::protocol_error;
    }
    std::optional<detail::backend::raw_received_t> received;
    {
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
            co_return raw_mesh_pump_result_t::backpressured;
        }
        _pending_received.reset ();
        co_return accepted_result;
    }
    if (!received && !accept_application_receive) {
        // The ROUTER carries every ordinary record, including control and
        // malformed input.  Without the host-wide supply permit none of
        // those records may be dequeued and classified after receive;
        // terminal reply/error completion progresses on its separate path.
        co_return raw_mesh_pump_result_t::no_data;
    }
    if (!received)
        received = port->receive_if_ready (readiness);
    if (!received) {
        co_return raw_mesh_pump_result_t::no_data;
    }
    _received_owner_for_enqueue = std::move (received->retained);
    struct received_owner_reset_t
    {
        raw_mesh_node_owner_t *owner;
        ~received_owner_reset_t () { owner->_received_owner_for_enqueue.reset (); }
    } received_owner_reset{this};
    _last_pump_bytes = raw_received_bytes (*received);
    if (received->parts.empty ()) {
        co_return raw_mesh_pump_result_t::protocol_error;
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
                co_return raw_mesh_pump_result_t::protocol_error;
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
            bool expected_descriptor_mismatch = false;
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
                        co_return raw_mesh_pump_result_t::capacity_exceeded;
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
                    co_return raw_mesh_pump_result_t::no_data;
                }
                connection_id = connection->connection_id;
                direction = connection->direction;
                remote_endpoint = connection->remote_endpoint;
                const auto expected =
                  _expected_peers.find (received->source_routing_id);
                expected_descriptor_mismatch =
                  expected != _expected_peers.end ()
                  && (expected->second.mesh_name != descriptor.mesh_name
                      || expected->second.node_routing_id
                           != descriptor.node_routing_id
                      || expected->second.advertised_endpoint
                           != descriptor.advertised_endpoint
                      || expected->second.security_identity
                           != descriptor.security_identity
                      || (expected->second.lifecycle_generation != 0
                          && expected->second.lifecycle_generation
                               != descriptor.lifecycle_generation));
                if (expected != _expected_peers.end ()
                    && expected->second.lifecycle_generation != 0)
                    expected_descriptor = expected->second;
            }
            if (expected_descriptor_mismatch) {
                (void) co_await send_header_only (
                  received->source_routing_id,
                  protocol::encode_reject (3));
                co_return raw_mesh_pump_result_t::infrastructure;
            }
            peer_admission_result_t admission;
            {
                std::lock_guard lifecycle_lock (_lifecycle_mutex);
                if (!_connections.contains (received->source_routing_id,
                                            connection_id)) {
                    trace_mesh ("admission-discard reason=connection-disconnected");
                    co_return raw_mesh_pump_result_t::infrastructure;
                }
                admission = expected_descriptor
                  ? _topology.admit (
                      descriptor, connection_id, direction,
                      *expected_descriptor)
                  : _topology.admit (
                      descriptor, connection_id, direction);
            }
            if (admission == peer_admission_result_t::not_required) {
                trace_admission_phase (
                  received->source_routing_id,
                  descriptor.lifecycle_generation,
                  header.kind, admission);
                if (header.kind == protocol::command::hello) {
                    (void) co_await send_header_only (
                      received->source_routing_id,
                      protocol::encode_route_mesh_admission (
                        protocol::command::admit,
                        _topology.local_descriptor ()));
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
                co_return raw_mesh_pump_result_t::infrastructure;
            }
            if (admission
                == peer_admission_result_t::duplicate_connection) {
                trace_admission_phase (
                  received->source_routing_id,
                  descriptor.lifecycle_generation,
                  header.kind, admission);
                if (header.kind == protocol::command::hello) {
                    (void) co_await send_header_only (
                      received->source_routing_id,
                      protocol::encode_route_mesh_admission (
                        protocol::command::admit,
                        _topology.local_descriptor ()));
                }
                co_return raw_mesh_pump_result_t::infrastructure;
            }
            if (admission != peer_admission_result_t::admitted) {
                const auto reason =
                  admission == peer_admission_result_t::mesh_mismatch ? 2u
                  : admission == peer_admission_result_t::stale_descriptor
                    ? 7u
                    : 11u;
                (void) co_await send_header_only (
                  received->source_routing_id,
                  protocol::encode_reject (reason));
                co_return raw_mesh_pump_result_t::infrastructure;
            }
            trace_admission_phase (
              received->source_routing_id,
              descriptor.lifecycle_generation,
              header.kind, admission);
            _liveness.admit (
              descriptor.node_routing_id, connection_id, now);
            if (header.kind == protocol::command::hello) {
                (void) co_await send_header_only (
                  received->source_routing_id,
                  protocol::encode_route_mesh_admission (
                    protocol::command::admit,
                    _topology.local_descriptor ()));
            }
            co_return raw_mesh_pump_result_t::infrastructure;
        }
        if (header.kind == protocol::command::reject) {
            if (received->parts.size () != 1) {
                co_return raw_mesh_pump_result_t::protocol_error;
            }
            (void) protocol::decode_reject (received->parts.front ());
            const auto peer = _topology.peer (received->source_routing_id);
            if (peer) {
                (void) _topology.disconnect (
                  received->source_routing_id, peer->connection_id);
                (void) _liveness.disconnect (
                  received->source_routing_id, peer->connection_id);
            }
            co_return raw_mesh_pump_result_t::infrastructure;
        }
        const auto admitted = _topology.peer (received->source_routing_id);
        if (!admitted) {
            if (application_command (header.kind)
                && header.flags == 0
                && received->parts.size () == 2) {
                const auto correlation = application_request_correlation (
                  header.kind, received->parts.front ());
                bool rejected = false;
                if (correlation && received->request_sequence) {
                    try {
                        rejected = reply_failure (
                          service_mailbox_record_t{
                            owner_key (received->source_routing_id),
                            service_mailbox_domain_t::application,
                            {},
                            received->source_routing_id,
                            received->request_sequence,
                            correlation,
                            0,
                            std::nullopt,
                            std::nullopt,
                            _received_owner_for_enqueue},
                          static_cast<std::uint32_t> (
                            protocol::request_terminal_result::notConnected),
                          static_cast<std::uint32_t> (
                            protocol::framework_error_code::none));
                    }
                    catch (...) {
                    }
                }
                trace_mesh (
                  "application-drop reason=peer-not-admitted action="
                  + std::string (rejected ? "reply-not-connected" : "drop"));
            }
            trace_mesh (
              "application-drop reason=peer-not-admitted kind="
                + std::to_string (static_cast<int> (header.kind))
                + " sourceBytes="
                + std::to_string (received->source_routing_id.size ()));
            co_return raw_mesh_pump_result_t::protocol_error;
        }
        if (header.kind == protocol::command::livenessProbe
            || header.kind == protocol::command::livenessAck) {
            if (received->parts.size () != 1) {
                co_return raw_mesh_pump_result_t::protocol_error;
            }
            const auto record =
              protocol::decode_liveness (received->parts.front ());
            if (record.kind == protocol::command::livenessProbe) {
                const auto ack = _liveness.acknowledge_probe (
                  received->source_routing_id, admitted->connection_id,
                  record.probe_id);
                if (!ack
                    || !co_await send_header_only (
                      received->source_routing_id,
                      protocol::encode_liveness (
                        protocol::command::livenessAck, record.probe_id))) {
                    co_return raw_mesh_pump_result_t::protocol_error;
                }
            } else {
                (void) _liveness.acknowledge (
                  received->source_routing_id, admitted->connection_id,
                  record.probe_id, now);
            }
            co_return raw_mesh_pump_result_t::infrastructure;
        }
        if (header.kind == protocol::command::messageFollow) {
            if (header.flags != 0 || received->parts.size () != 1
                || received->request_sequence) {
                co_return raw_mesh_pump_result_t::protocol_error;
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
                co_return raw_mesh_pump_result_t::protocol_error;
            }
            co_return enqueue_received_or_retain (
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
        if (header.kind == protocol::command::boundSessionSend) {
            if (header.flags != 0 || received->parts.size () != 2
                || received->request_sequence) {
                co_return raw_mesh_pump_result_t::protocol_error;
            }
            const auto send = protocol::decode_bound_session_send (
              received->parts.front ());
            if (send.actor.target_node_routing_id
                  != received->source_routing_id
                || send.actor.target_node_routing_id
                     != admitted->descriptor.node_routing_id
                || send.actor.target_node_generation
                     != admitted->descriptor.lifecycle_generation) {
                co_return raw_mesh_pump_result_t::protocol_error;
            }
            (void) protocol::decode_application_payload (
              received->parts.back (), false);
            co_return enqueue_received_or_retain (
              service_mailbox_record_t{
                "bound-session:" + send.actor.actor_id,
                service_mailbox_domain_t::application,
                std::move (received->parts),
                std::move (received->source_routing_id),
                std::nullopt, std::nullopt,
                admitted->descriptor.lifecycle_generation},
              raw_mesh_pump_result_t::application);
        }
        if (header.kind == protocol::command::boundSessionBind) {
            if (header.flags != 0 || received->parts.size () != 1
                || !received->request_sequence) {
                co_return raw_mesh_pump_result_t::protocol_error;
            }
            const auto bind = protocol::decode_bound_session_bind (
              received->parts.front ());
            const auto local = _topology.local_descriptor ();
            if (bind.actor.target_node_routing_id
                  != local.node_routing_id
                || bind.actor.target_node_generation
                     != local.lifecycle_generation) {
                co_return raw_mesh_pump_result_t::protocol_error;
            }
            co_return enqueue_received_or_retain (
              service_mailbox_record_t{
                owner_key (local.node_routing_id),
                service_mailbox_domain_t::infrastructure,
                std::move (received->parts),
                std::move (received->source_routing_id),
                received->request_sequence,
                bind.correlation,
                admitted->descriptor.lifecycle_generation},
              raw_mesh_pump_result_t::infrastructure);
        }
        if (header.kind == protocol::command::boundSessionReplaced) {
            if (header.flags != 0 || received->parts.size () != 1
                || received->request_sequence) {
                co_return raw_mesh_pump_result_t::protocol_error;
            }
            const auto replacement =
              protocol::decode_bound_session_replaced (
                received->parts.front ());
            const auto local = _topology.local_descriptor ();
            if (replacement.actor_authority.target_node_routing_id
                  != received->source_routing_id
                || replacement.actor_authority.target_node_routing_id
                     != admitted->descriptor.node_routing_id
                || replacement.actor_authority.target_node_generation
                     != admitted->descriptor.lifecycle_generation
                || replacement.retired_session
                     .session_owner_node_routing_id
                     != local.node_routing_id
                || replacement.retired_session
                     .session_owner_node_generation
                     != local.lifecycle_generation) {
                co_return raw_mesh_pump_result_t::protocol_error;
            }
            co_return enqueue_received_or_retain (
              service_mailbox_record_t{
                owner_key (local.node_routing_id),
                service_mailbox_domain_t::infrastructure,
                std::move (received->parts),
                std::move (received->source_routing_id),
                std::nullopt, std::nullopt,
                admitted->descriptor.lifecycle_generation},
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
                co_return raw_mesh_pump_result_t::protocol_error;
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
                co_return raw_mesh_pump_result_t::protocol_error;
            }
            (void) protocol::decode_application_payload (
              received->parts.back (), false);
            co_return enqueue_received_or_retain (
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
                co_return raw_mesh_pump_result_t::protocol_error;
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
                co_return raw_mesh_pump_result_t::protocol_error;
            }
            co_return enqueue_received_or_retain (
              service_mailbox_record_t{
                owner_key (local.node_routing_id),
                service_mailbox_domain_t::infrastructure,
                std::move (received->parts),
                std::move (received->source_routing_id),
                received->request_sequence,
                correlation},
              raw_mesh_pump_result_t::infrastructure);
        }
        if (header.kind == protocol::command::actorJoin) {
            if (header.flags != 0 || received->parts.empty ()
                || received->parts.size () > 2
                || !received->request_sequence) {
                co_return raw_mesh_pump_result_t::protocol_error;
            }
            const auto decoded_canonical = [&] {
                try {
                    return protocol::decode_actor_join_28 (received->parts);
                }
                catch (const std::invalid_argument &) {
                    throw protocol::service_wire_error_t (
                      "invalid Actor join request");
                }
            } ();
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
            if (request.actor.target_node_routing_id
                  != received->source_routing_id
                || request.actor.target_node_routing_id
                     != admitted->descriptor.node_routing_id
                || request.actor.target_node_generation
                     != admitted->descriptor.lifecycle_generation) {
                co_return raw_mesh_pump_result_t::protocol_error;
            }
            co_return enqueue_received_or_retain (
              service_mailbox_record_t{
                owner_key (local.node_routing_id),
                service_mailbox_domain_t::infrastructure,
                std::move (received->parts),
                std::move (received->source_routing_id),
                received->request_sequence, request.correlation},
              raw_mesh_pump_result_t::infrastructure);
        }
        if (header.kind == protocol::command::relocationPrepare
            || header.kind == protocol::command::relocationData
            || header.kind == protocol::command::relocationCutover
            || header.kind == protocol::command::relocationState) {
            if (received->parts.empty ()
                || (received->parts.size () != 1
                    && header.kind
                         != protocol::command::relocationPrepare)
                || (received->request_sequence
                    && header.kind
                         != protocol::command::relocationPrepare)) {
                co_return raw_mesh_pump_result_t::protocol_error;
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
                        return std::pair{
                          value.target.target_node_routing_id,
                          value.target.target_node_generation};
                    return std::pair{value.coordinator.node_routing_id,
                                     value.coordinator.node_generation};
                }
                else if constexpr (std::is_same_v<record_t,
                                                  protocol::relocation_data_t>) {
                    // The record source is the original request source, not
                    // the relocation transport peer. The registered target
                    // validates the exact relocation source peer.
                    return std::nullopt;
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
                co_return raw_mesh_pump_result_t::protocol_error;
            }
            const auto local = _topology.local_descriptor ();
            co_return enqueue_received_or_retain (
              service_mailbox_record_t{
                owner_key (local.node_routing_id),
                service_mailbox_domain_t::infrastructure,
                std::move (received->parts),
                std::move (received->source_routing_id),
                received->request_sequence, std::nullopt,
                admitted->descriptor.lifecycle_generation},
              raw_mesh_pump_result_t::infrastructure);
        }
        if (header.kind == protocol::command::replyRelay
            || header.kind == protocol::command::replyRelayAck) {
            if (header.flags != 0 || received->request_sequence) {
                co_return raw_mesh_pump_result_t::protocol_error;
            }
            const auto local = _topology.local_descriptor ();
            if (header.kind == protocol::command::replyRelay) {
                if (received->parts.empty ()
                    || received->parts.size () > 2) {
                    co_return raw_mesh_pump_result_t::protocol_error;
                }
                const auto relay = protocol::decode_reply_relay (
                  received->parts.front ());
                if (relay.terminal_result != 0
                    && received->parts.size () == 2) {
                    co_return raw_mesh_pump_result_t::protocol_error;
                }
                if (received->parts.size () == 2) {
                    (void) protocol::decode_application_payload (
                      received->parts.back (), false);
                }
            } else {
                if (received->parts.size () != 1) {
                    co_return raw_mesh_pump_result_t::protocol_error;
                }
                const auto ack = protocol::decode_reply_relay_ack (
                  received->parts.front ());
                if (ack.request_source.node_routing_id
                      != received->source_routing_id
                    || ack.request_source.node_routing_id
                         != admitted->descriptor.node_routing_id
                    || ack.request_source.node_generation
                         != admitted->descriptor.lifecycle_generation) {
                    co_return raw_mesh_pump_result_t::protocol_error;
                }
            }
            co_return enqueue_received_or_retain (
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
                 == protocol::command::sessionRelocationRoute) {
            if (header.flags != 0 || received->parts.size () != 1
                || received->request_sequence) {
                co_return raw_mesh_pump_result_t::protocol_error;
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
                    co_return raw_mesh_pump_result_t::protocol_error;
                }
                co_return enqueue_received_or_retain (
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
                == protocol::command::sessionRelocationRoute) {
                const auto route =
                  protocol::decode_session_relocation_route (
                    received->parts.front ());
                const auto expected_source_rid =
                  route.sender_role == protocol::relocation_role_t::target
                    ? route.route.target_node_routing_id
                    : route.coordinator.node_routing_id;
                const auto expected_source_generation =
                  route.sender_role == protocol::relocation_role_t::target
                    ? route.route.target_node_generation
                    : route.coordinator.node_generation;
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
                    co_return raw_mesh_pump_result_t::protocol_error;
                }
                co_return enqueue_received_or_retain (
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
                    co_return raw_mesh_pump_result_t::protocol_error;
                }
                const auto operation = operation_id (
                  sealed.relocation.high, sealed.relocation.low);
                if (!_operations->complete (
                      operation,
                      std::move (received->parts.front ()))) {
                    co_return raw_mesh_pump_result_t::protocol_error;
                }
                co_return raw_mesh_pump_result_t::infrastructure;
            }
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
            trace_mesh (
              "application-drop reason=invalid-application-shape kind="
                + std::to_string (static_cast<int> (header.kind))
                + " parts=" + std::to_string (received->parts.size ()));
            co_return raw_mesh_pump_result_t::protocol_error;
        }
        (void) protocol::decode_application_payload (received->parts[1], false);
        const auto local = _topology.local_descriptor ();
        std::string mailbox_owner;
        std::optional<std::uint64_t> correlation;
        std::optional<std::pair<std::uint64_t, std::uint64_t>> operation;
        std::optional<service_bound_session_source_t> bound_session_source;
        if (header.kind == protocol::command::nodeSend
            || header.kind == protocol::command::nodeRequest) {
            if (header.kind == protocol::command::nodeSend
                && received->parts.front ().size () != 5) {
                co_return raw_mesh_pump_result_t::protocol_error;
            }
            if (header.kind == protocol::command::nodeRequest) {
                if (!received->request_sequence) {
                    co_return raw_mesh_pump_result_t::protocol_error;
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
                    co_return raw_mesh_pump_result_t::protocol_error;
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
                co_return raw_mesh_pump_result_t::protocol_error;
            }
            mailbox_owner = "channel:" + channel_name;
        } else if (header.kind == protocol::command::spotSend
                   || header.kind == protocol::command::spotRequest) {
            if (header.kind == protocol::command::spotRequest
                && !received->request_sequence) {
                co_return raw_mesh_pump_result_t::protocol_error;
            }
            const auto spot = protocol::decode_spot_message_header (
              received->parts.front (), header.kind);
            if (spot.target.target_node_routing_id
                  != local.node_routing_id
                || spot.target.target_node_generation
                     != local.lifecycle_generation) {
                co_return raw_mesh_pump_result_t::protocol_error;
            }
            correlation = spot.correlation;
            operation = std::pair{
              spot.operation.high, spot.operation.low};
            mailbox_owner = spot.target.spot_id;
            mailbox_owner.insert (0, "spot:");
        } else {
            if (header.kind == protocol::command::actorRequest
                && !received->request_sequence) {
                co_return raw_mesh_pump_result_t::protocol_error;
            }
            auto actor = protocol::decode_actor_message_header (
              received->parts.front (), header.kind);
            if (actor.target.target_node_routing_id
                  != local.node_routing_id
                || actor.target.target_node_generation
                     != local.lifecycle_generation) {
                co_return raw_mesh_pump_result_t::protocol_error;
            }
            correlation = actor.correlation;
            operation = std::pair{
              actor.operation.high, actor.operation.low};
            if (actor.bound_session_source) {
                bound_session_source = service_bound_session_source_t{
                  std::move (actor.bound_session_source->session_routing_id),
                  actor.bound_session_source->binding_generation,
                  actor.bound_session_source->session_sequence};
            }
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
            operation,
            std::move (bound_session_source)},
          raw_mesh_pump_result_t::application);
        trace_mesh (
          "application-enqueue result="
            + std::to_string (static_cast<int> (result))
            + " pending="
            + std::to_string (
              _mailbox.pending_messages (service_mailbox_domain_t::application)));
        co_return result;
    }
    catch (const protocol::service_wire_error_t &) {
        co_return raw_mesh_pump_result_t::protocol_error;
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

void raw_mesh_node_owner_t::signal_activity () noexcept
{
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lock (_lifecycle_mutex);
        port = _port;
    }
    if (port)
        port->signal_activity ();
}

std::uint64_t raw_mesh_node_owner_t::next_operation_sequence ()
{
    std::lock_guard lifecycle_lock (_lifecycle_mutex);
    if (!_port)
        throw std::logic_error ("raw mesh node is not started");
    if (_next_operation_sequence == 0) {
        throw std::overflow_error ("raw mesh operation sequence is exhausted");
    }
    const auto sequence = _next_operation_sequence;
    _next_operation_sequence =
      sequence == std::numeric_limits<std::uint64_t>::max ()
        ? 0
        : sequence + 1;
    return sequence;
}

std::uint64_t raw_mesh_node_owner_t::take_reply_route_id_locked (
  std::optional<std::uint64_t> requested)
{
    if (_next_reply_route_id == 0) {
        throw std::overflow_error (
          "raw mesh reply route id is exhausted");
    }
    if (requested && (*requested == 0
                      || *requested < _next_reply_route_id)) {
        throw std::invalid_argument (
          "raw mesh reply route id must be unique in the current lifecycle");
    }
    const auto reply_route_id = requested.value_or (_next_reply_route_id);
    _next_reply_route_id =
      reply_route_id == std::numeric_limits<std::uint64_t>::max ()
        ? 0
        : reply_route_id + 1;
    return reply_route_id;
}

task_t<std::size_t> raw_mesh_node_owner_t::drain_monitor_events (
  service_liveness_registry_t::clock_t::time_point now)
{
    std::size_t count = 0;
    for (;;) {
        std::optional<zlink::monitor_event_t> event;
        {
            std::lock_guard lifecycle_lock (_lifecycle_mutex);
            if (!_monitor || !_monitor->valid ()) {
                co_return count;
            }
            if (!_monitor_poller) {
                co_return count;
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
                    co_return count;
                }
            }
            catch (...) {
                co_return count;
            }
            try {
                event = _monitor->recv (zlink::recv_flags_t::dontwait);
            }
            catch (...) {
                co_return count;
            }
        }
        if (!event) {
            co_return count;
        }
        ++count;
        const auto connection_id = monitor_connection_id (event->connection_id);
        trace_mesh (
          "monitor event="
            + std::to_string (static_cast<int> (event->event))
            + " remote=" + event->remote_addr
            + " connection=" + owner_key (connection_id)
            + " routing="
            + (event->routing_id
                 ? owner_key (event->routing_id->to_bytes ())
                 : std::string ("-")));
        if (event->event == zlink::monitor_event::disconnected) {
            std::optional<std::vector<std::uint8_t>> disconnected_node;
            {
                std::lock_guard lifecycle_lock (_lifecycle_mutex);
                if (event->routing_id) {
                    const auto node_routing_id = event->routing_id->to_bytes ();
                    if (_connections.disconnect (node_routing_id, connection_id))
                        disconnected_node = node_routing_id;
                }
                if (!disconnected_node)
                    disconnected_node = _connections.disconnect_by_connection_id (
                      connection_id, event->remote_addr);
                if (!disconnected_node)
                    disconnected_node = _connections.disconnect_by_connection_id (
                      connection_id);
            }
            if (disconnected_node) {
                const auto removed = _topology.disconnect (
                  *disconnected_node, connection_id);
                (void) _liveness.disconnect (
                  *disconnected_node, connection_id);
                if (removed) {
                    discard_pending_admissions (*disconnected_node);
                }
            }
            static_cast<void> (now);
            continue;
        }
        if (!event->routing_id) {
            continue;
        }
        const auto node_routing_id = event->routing_id->to_bytes ();
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
                try {
                    (void) co_await send_header_only (
                      node_routing_id,
                      protocol::encode_route_mesh_admission (
                        protocol::command::hello,
                        _topology.local_descriptor ()));
                }
                catch (const zlink::submit_error_t &) {
                }
            }
        }
        static_cast<void> (now);
    }
}

task_t<service_liveness_tick_t> raw_mesh_node_owner_t::tick_liveness (
  service_liveness_registry_t::clock_t::time_point now)
{
    auto result = _liveness.tick (now);
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lifecycle_lock (_lifecycle_mutex);
        port = _port;
    }
    if (!port) {
        co_return result;
    }
    for (const auto &probe : result.probes) {
        (void) co_await send_header_only (
          probe.node_routing_id,
          protocol::encode_liveness (
            protocol::command::livenessProbe, probe.probe_id));
    }
    for (const auto &timed_out : result.timed_out_nodes) {
        const auto peer = _topology.peer (timed_out);
        if (peer) {
            const auto removed = _topology.disconnect (
              timed_out, peer->connection_id);
            if (removed) {
                discard_pending_admissions (timed_out);
            }
        }
    }
    co_return result;
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

foundation::call_id_t raw_mesh_node_owner_t::operation_id (
  std::uint64_t lifecycle_generation,
  std::uint64_t correlation)
{
    return foundation::call_id_t{lifecycle_generation, correlation};
}

} // namespace zlink::framework::runtime::mesh

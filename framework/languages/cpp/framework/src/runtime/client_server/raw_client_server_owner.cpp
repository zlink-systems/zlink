/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/client_server/raw_client_server_owner.hpp"
#include "runtime/transport/listener_identity.hpp"

#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Core/byte_count.hpp>
#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Eventing/events.hpp>
#include <zlink/Contracts/Eventing/monitor.hpp>
#include <zlink/Contracts/Eventing/poll_event.hpp>
#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/results.hpp>
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace zlink::framework::runtime::client_server
{
namespace
{

// The socket HWM counts frame metadata in addition to application bytes.
// Leave a protocol margin so one message at the advertised limit remains
// admissible when Auto HWM selected a smaller default.
constexpr std::uint64_t client_server_wire_hwm_margin = 64u * 1024u;

bool client_server_trace_enabled ()
{
    const char *value = std::getenv ("ZLINK_CPP_CLIENT_SERVER_TRACE");
    return value != nullptr && std::string_view (value) != "0"
           && std::string_view (value) != "";
}

void trace_client_server (std::string_view stage, std::string details = {})
{
    if (!client_server_trace_enabled ())
        return;
    std::clog << "zlink-cpp-client-server-trace stage=" << stage;
    if (!details.empty ())
        std::clog << ' ' << details;
    std::clog << '\n';
}

template <typename DetailBuilder>
void trace_client_server_lazy (std::string_view stage, DetailBuilder &&builder)
{
    if (!client_server_trace_enabled ())
        return;
    trace_client_server (stage, std::forward<DetailBuilder> (builder) ());
}

std::string routing_id_label (const std::vector<std::uint8_t> &routing_id)
{
    return zlink::routing_id_t::from (routing_id).to_string ();
}

std::size_t raw_message_bytes (
  const detail::backend::raw_message_t &parts) noexcept
{
    std::size_t result = 0;
    for (const auto &part : parts)
        result += part.size ();
    return result;
}

std::vector<std::uint8_t> liveness_connection_identity (
  const protocol::client_server_server_admission_t &server)
{
    if (!server.server_routing_id.empty ())
        return server.server_routing_id;
    return {server.advertised_endpoint.begin (),
            server.advertised_endpoint.end ()};
}

foundation::operation_terminal_t request_failure (
  detail::backend::raw_request_result_t result)
{
    if (result == detail::backend::raw_request_result_t::timed_out) {
        return foundation::operation_terminal_t::timed_out;
    }
    if (result == detail::backend::raw_request_result_t::terminated) {
        return foundation::operation_terminal_t::shutdown;
    }
    return foundation::operation_terminal_t::transport_failed;
}

class pending_request_guard_t
{
  public:
    explicit pending_request_guard_t (std::atomic_size_t &pending) :
        _pending (&pending)
    {
        _pending->fetch_add (1, std::memory_order_relaxed);
    }

    ~pending_request_guard_t ()
    {
        _pending->fetch_sub (1, std::memory_order_relaxed);
    }

    pending_request_guard_t (const pending_request_guard_t &) = delete;
    pending_request_guard_t &operator= (const pending_request_guard_t &) = delete;

  private:
    std::atomic_size_t *_pending;
};

} // namespace

raw_client_server_server_t::raw_client_server_server_t (
  raw_client_server_server_options_t options,
  std::shared_ptr<zlink::context_t> context) :
    _options (std::move (options)),
    _context (
      context ? std::move (context) : std::make_shared<zlink::context_t> ()),
    _mailbox (dispatch_limits::application_mailbox_messages,
              dispatch_limits::application_mailbox_bytes,
              dispatch_limits::control_mailbox_messages,
              dispatch_limits::control_mailbox_bytes)
{
    if (_options.descriptor.channel_name.empty ()
        || _options.descriptor.server_routing_id.empty ()) {
        throw std::invalid_argument (
          "ClientServer server channel and routing id are required");
    }
}

raw_client_server_server_t::~raw_client_server_server_t () noexcept
{
    close ();
}

void raw_client_server_server_t::start ()
{
    std::lock_guard lock (_mutex);
    if (_port) {
        return;
    }
    if (_closed) {
        throw std::logic_error (
          "ClientServer server cannot restart after close");
    }
    auto router = std::make_unique<zlink::router_socket_t> (*_context);
    router->options ().handover (true);
    router->options ().mandatory (true);
    router->options ().linger (std::chrono::milliseconds (0));
    router->options ().send_timeout (std::chrono::seconds (1));
    router->options ().max_message_size (
      zlink::byte_size_t::bytes (
        _options.descriptor.effective_max_message_bytes));
    trace_client_server_lazy (
      "server-socket-options",
      [&] {
          return "endpoint=" + _options.descriptor.advertised_endpoint
                 + " max_message_bytes="
                 + std::to_string (
                     router->options ().max_message_size ().bytes ());
      });
    router->set_routing_id (
      zlink::routing_id_t::from (
        _options.descriptor.server_routing_id));
    auto monitor = std::make_unique<zlink::socket_monitor_t> (
      router->monitor_open (zlink::monitor_event::connection_ready
                            | zlink::monitor_event::disconnected));
    router->bind (_options.descriptor.advertised_endpoint);
    _options.descriptor.advertised_endpoint =
      transport::advertised_tcp_endpoint (
        router->options ().last_endpoint (), _options.advertise_host, "ClientServer");
    if (_options.descriptor.descriptor_revision
        == std::numeric_limits<std::uint64_t>::max ()) {
        throw std::overflow_error (
          "ClientServer descriptor revision is exhausted");
    }
    ++_options.descriptor.descriptor_revision;
    _options.descriptor.state = mesh::service_node_state_t::serving;
    auto monitor_poller = std::make_unique<zlink::poller_t> ();
    monitor_poller->add (*monitor, zlink::poll_event_flag_t::pollin, 1);
    _port = std::make_shared<detail::backend::raw_route_port_t> (
      *router,
      &_socket_mutex,
      zlink::poll_event_flag_t::pollin,
      _options.transport_poller,
      _options.transport_poller_slot);
    _monitor_poller = std::move (monitor_poller);
    _monitor = std::move (monitor);
    _router = std::move (router);
}

void raw_client_server_server_t::close () noexcept
{
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    std::unique_ptr<zlink::router_socket_t> router;
    std::unique_ptr<zlink::poller_t> monitor_poller;
    std::unique_ptr<zlink::socket_monitor_t> monitor;
    {
        std::lock_guard lock (_mutex);
        if (_closed) {
            return;
        }
        _closed = true;
        port = std::move (_port);
        router = std::move (_router);
        monitor_poller = std::move (_monitor_poller);
        monitor = std::move (_monitor);
    }
    _mailbox.close ();
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

std::string raw_client_server_server_t::endpoint () const
{
    std::lock_guard lock (_mutex);
    return _options.descriptor.advertised_endpoint;
}

protocol::client_server_server_admission_t
raw_client_server_server_t::descriptor () const
{
    std::lock_guard lock (_mutex);
    return _options.descriptor;
}

void raw_client_server_server_t::update_descriptor (
  protocol::client_server_server_admission_t descriptor)
{
    std::lock_guard lock (_mutex);
    if (descriptor.channel_name
          != _options.descriptor.channel_name
        || descriptor.server_routing_id
             != _options.descriptor.server_routing_id
        || descriptor.lifecycle_generation
             != _options.descriptor.lifecycle_generation
        || descriptor.security_identity
             != _options.descriptor.security_identity
        || descriptor.advertised_endpoint
             != _options.descriptor.advertised_endpoint
        || descriptor.descriptor_revision
             <= _options.descriptor.descriptor_revision) {
        throw std::invalid_argument (
          "ClientServer descriptor update violates its immutable fence");
    }
    _options.descriptor = std::move (descriptor);
    _descriptor_update_pending = true;
}

mesh::service_mailbox_t &
raw_client_server_server_t::mailbox () noexcept
{
    return _mailbox;
}

std::size_t raw_client_server_server_t::drain_monitor_events (
  mesh::service_liveness_registry_t::clock_t::time_point now)
{
    static_cast<void> (now);
    std::size_t count = 0;
    for (;;) {
        std::optional<zlink::monitor_event_t> event;
        {
            std::lock_guard lock (_mutex);
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
            event = _monitor->recv (zlink::recv_flags_t::dontwait);
        }
        if (!event) {
            return count;
        }
        ++count;
        trace_client_server_lazy (
          "server-monitor-event",
          [&] {
              return "event="
                     + std::to_string (static_cast<int> (event->event))
                     + " routing_id="
                     + (event->routing_id
                          ? event->routing_id->to_string ()
                          : std::string ("-"))
                     + " value=" + std::to_string (event->value)
                     + " local=" + event->local_addr + " remote="
                     + event->remote_addr;
          });
        if (!event->routing_id) {
            continue;
        }
        const auto client = event->routing_id->to_bytes ();
        if (event->event == zlink::monitor_event::connection_ready) {
            trace_client_server_lazy (
              "server-connection-ready",
              [&] {
                  return "channel=" + _options.descriptor.channel_name
                         + " client=" + routing_id_label (client);
              });
            std::lock_guard lock (_mutex);
            // The monitor value is a ready-count, not a physical connection
            // identity.  The route id is the stable identity available at
            // this framework boundary.
            _connections.insert_or_assign (client, client);
        } else if (event->event == zlink::monitor_event::disconnected) {
            trace_client_server_lazy (
              "server-disconnected",
              [&] {
                  return "channel=" + _options.descriptor.channel_name
                         + " client=" + routing_id_label (client);
              });
            {
                std::lock_guard lock (_mutex);
                _connections.erase (client);
            }
            (void) _liveness.disconnect (client, client);
        }
    }
}

task_t<client_server_pump_result_t> raw_client_server_server_t::pump_one (
  mesh::service_liveness_registry_t::clock_t::time_point now,
  std::shared_ptr<application_job_queue_t::permit_t> application_permit)
{
    _last_pump_bytes = 0;
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lock (_mutex);
        port = _port;
    }
    if (!port) {
        co_return client_server_pump_result_t::no_data;
    }
    if (_pending_received) {
        for (const auto &part : _pending_received->parts)
            _last_pump_bytes += part.size ();
        if (!_mailbox.try_enqueue (
              std::move (*_pending_received))) {
            co_return client_server_pump_result_t::backpressured;
        }
        _pending_received.reset ();
        co_return client_server_pump_result_t::application;
    }
    std::optional<detail::backend::raw_received_t> received;
    received = port->try_receive ();
    if (!received) {
        co_return client_server_pump_result_t::no_data;
    }
    for (const auto &part : received->parts)
        _last_pump_bytes += part.size ();
    if (received->parts.empty ()) {
        co_return client_server_pump_result_t::protocol_error;
    }
    try {
        const auto header =
          protocol::decode_header (received->parts.front ());
        if (header.kind == protocol::command::channelRequest
            || header.kind == protocol::command::channelSend) {
            trace_client_server_lazy (
              "server-received",
              [&] {
                  return "channel=" + _options.descriptor.channel_name
                         + " kind="
                         + std::to_string (static_cast<int> (header.kind))
                         + " client="
                         + routing_id_label (received->source_routing_id)
                         + " endpoint="
                         + _options.descriptor.advertised_endpoint
                         + " request_seq="
                         + (received->request_sequence
                              ? std::to_string (*received->request_sequence)
                              : std::string ("-"))
                         + " part0_bytes="
                         + std::to_string (received->parts.front ().size ())
                         + " part1_bytes="
                         + (received->parts.size () > 1
                              ? std::to_string (received->parts[1].size ())
                              : std::string ("-"));
              });
        }
        if (header.kind == protocol::command::hello) {
            if (received->parts.size () != 1) {
                co_return client_server_pump_result_t::protocol_error;
            }
            const auto client =
              protocol::decode_client_server_client_admission (
                received->parts.front (), protocol::command::hello);
            trace_client_server_lazy (
              "server-hello",
              [&] {
                  return "channel=" + client.channel_name
                         + " client="
                         + routing_id_label (received->source_routing_id)
                         + " endpoint="
                         + _options.descriptor.advertised_endpoint;
              });
            if (client.channel_name != _options.descriptor.channel_name
                || client.security_identity
                     != _options.descriptor.security_identity) {
                const detail::backend::raw_message_t reject_message{
                  protocol::encode_reject (3)};
                (void) co_await port->send (
                  received->source_routing_id,
                  reject_message);
                co_return client_server_pump_result_t::infrastructure;
            }
            {
                std::lock_guard lock (_mutex);
                _connections.insert_or_assign (
                  received->source_routing_id,
                  received->source_routing_id);
            }
            _liveness.admit (
              received->source_routing_id, received->source_routing_id, now);
            const detail::backend::raw_message_t admit_message{
              protocol::encode_client_server_server_admission (
                protocol::command::admit, _options.descriptor)};
            const auto admitted = co_await port->send (
              received->source_routing_id, admit_message);
            if (!admitted) {
                co_return client_server_pump_result_t::protocol_error;
            }
            trace_client_server_lazy (
              "server-admit-sent",
              [&] {
                  return "channel=" + _options.descriptor.channel_name
                         + " client="
                         + routing_id_label (received->source_routing_id)
                         + " endpoint="
                         + _options.descriptor.advertised_endpoint;
              });
            co_return client_server_pump_result_t::infrastructure;
        }
        std::optional<std::vector<std::uint8_t>> connection;
        {
            std::lock_guard lock (_mutex);
            const auto found =
              _connections.find (received->source_routing_id);
            if (found != _connections.end ())
                connection = found->second;
        }
        if (!connection)
            co_return client_server_pump_result_t::protocol_error;
        if (header.kind == protocol::command::livenessProbe
            || header.kind == protocol::command::livenessAck) {
            if (received->parts.size () != 1) {
                co_return client_server_pump_result_t::protocol_error;
            }
            const auto liveness =
              protocol::decode_liveness (received->parts.front ());
            if (liveness.kind == protocol::command::livenessProbe) {
                const auto acknowledged = _liveness.acknowledge_probe (
                  received->source_routing_id, *connection,
                  liveness.probe_id);
                const detail::backend::raw_message_t ack_message{
                  protocol::encode_liveness (
                    protocol::command::livenessAck,
                    liveness.probe_id)};
                if (!acknowledged)
                    co_return client_server_pump_result_t::protocol_error;
                const auto ack_sent = co_await port->send (
                  received->source_routing_id, ack_message);
                if (!ack_sent) {
                    co_return client_server_pump_result_t::protocol_error;
                }
            } else {
                (void) _liveness.acknowledge (
                  received->source_routing_id, *connection,
                  liveness.probe_id, now);
            }
            co_return client_server_pump_result_t::infrastructure;
        }
        if ((header.kind != protocol::command::channelSend
             && header.kind != protocol::command::channelRequest)
            || received->parts.size () != 2) {
            co_return client_server_pump_result_t::protocol_error;
        }
        std::optional<std::uint64_t> correlation;
        std::string channel;
        if (header.kind == protocol::command::channelSend) {
            channel = protocol::decode_channel_send_header (
              received->parts.front ());
        } else {
            if (!received->request_sequence) {
                co_return client_server_pump_result_t::protocol_error;
            }
            auto request = protocol::decode_channel_request_header (
              received->parts.front ());
            correlation = request.correlation;
            channel = std::move (request.channel_name);
        }
        if (channel != _options.descriptor.channel_name) {
            co_return client_server_pump_result_t::protocol_error;
        }
        (void) protocol::decode_application_payload (received->parts[1]);
        if (application_permit)
            application_permit->mark_queued ();
        mesh::service_mailbox_record_t record{
          std::move (channel),
          mesh::service_mailbox_domain_t::application,
          std::move (received->parts),
          std::move (received->source_routing_id),
          received->request_sequence,
          correlation,
          0,
          std::nullopt,
          std::nullopt,
          std::move (received->retained),
          [permit = std::move (application_permit)] () mutable {
              if (!permit)
                  return;
              permit->release_for_handler_entry ();
              permit.reset ();
          }};
        if (!_mailbox.try_enqueue (std::move (record))) {
            _pending_received.emplace (std::move (record));
            co_return client_server_pump_result_t::backpressured;
        }
        co_return client_server_pump_result_t::application;
    }
    catch (const protocol::service_wire_error_t &) {
        co_return client_server_pump_result_t::protocol_error;
    }
}

bool raw_client_server_server_t::has_pending_application () const noexcept
{
    return _pending_received.has_value ();
}

task_t<mesh::service_liveness_tick_t>
raw_client_server_server_t::tick_liveness (
  mesh::service_liveness_registry_t::clock_t::time_point now)
{
    auto result = _liveness.tick (now);
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    std::optional<protocol::client_server_server_admission_t> update;
    std::vector<std::vector<std::uint8_t>> clients;
    {
        std::lock_guard lock (_mutex);
        port = _port;
        if (_descriptor_update_pending) {
            update = _options.descriptor;
            clients.reserve (_connections.size ());
            for (const auto &[client, _] : _connections)
                clients.push_back (client);
            _descriptor_update_pending = false;
        }
    }
    if (port) {
        if (update) {
            const detail::backend::raw_message_t update_message{
              protocol::encode_client_server_server_admission (
                protocol::command::update, *update)};
            for (const auto &client : clients)
                (void) co_await port->send (client, update_message);
        }
        for (const auto &probe : result.probes) {
            const detail::backend::raw_message_t probe_message{
              protocol::encode_liveness (
                protocol::command::livenessProbe, probe.probe_id)};
            (void) co_await port->send (
              probe.node_routing_id, probe_message);
        }
    }
    co_return result;
}

std::optional<mesh::service_liveness_registry_t::clock_t::time_point>
raw_client_server_server_t::next_liveness_activity () const
{
    return _liveness.next_activity ();
}

bool raw_client_server_server_t::reply (
  const mesh::service_mailbox_record_t &request,
  const protocol::application_payload_t &payload)
{
    if (request.source_routing_id.empty () || !request.request_sequence
        || !request.correlation) {
        throw std::invalid_argument (
          "ClientServer reply requires request context");
    }
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lock (_mutex);
        port = _port;
    }
    if (!port)
        return false;
    detail::backend::raw_message_t parts{
      protocol::encode_reply_header (*request.correlation, 0, 0),
      protocol::encode_application_payload (payload)};
    return port->reply (
      {request.source_routing_id, request.request_sequence, {},
       request.retained},
      parts);
}

bool raw_client_server_server_t::reply (
  const mesh::service_mailbox_record_t &request,
  std::uint32_t terminal_result,
  protocol::framework_error_code failure_code)
{
    if (request.source_routing_id.empty () || !request.request_sequence
        || !request.correlation) {
        throw std::invalid_argument (
          "ClientServer reply requires request context");
    }
    if (terminal_result == 0) {
        throw std::invalid_argument (
          "ClientServer failure reply requires terminal fields");
    }
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    {
        std::lock_guard lock (_mutex);
        port = _port;
    }
    if (!port)
        return false;
    detail::backend::raw_message_t parts{
      protocol::encode_reply_header (
        *request.correlation,
        terminal_result,
        static_cast<std::uint32_t> (failure_code))};
    return port->reply (
      {request.source_routing_id, request.request_sequence, {},
       request.retained},
      parts);
}

bool raw_client_server_server_t::byte_vector_less_t::operator() (
  const std::vector<std::uint8_t> &left,
  const std::vector<std::uint8_t> &right) const noexcept
{
    return std::lexicographical_compare (
      left.begin (), left.end (), right.begin (), right.end ());
}

raw_client_server_client_t::raw_client_server_client_t (
  raw_client_server_client_options_t options,
  std::shared_ptr<zlink::context_t> context) :
    _options (std::move (options)),
    _context (
      context ? std::move (context) : std::make_shared<zlink::context_t> ())
{
    if (_options.client_routing_id.empty ()
        || _options.admission.channel_name.empty ()
        || _options.expected_server.advertised_endpoint.empty ()) {
        throw std::invalid_argument (
          "ClientServer client identity, channel and endpoint are required");
    }
}

raw_client_server_client_t::~raw_client_server_client_t () noexcept
{
    close ();
}

void raw_client_server_client_t::start ()
{
    std::lock_guard lock (_mutex);
    if (_port) {
        return;
    }
    if (_closed) {
        throw std::logic_error (
          "ClientServer client cannot restart after close");
    }
    auto dealer = std::make_unique<zlink::dealer_socket_t> (*_context);
    dealer->options ().linger (std::chrono::milliseconds (0));
    dealer->options ().send_timeout (std::chrono::seconds (1));
    dealer->options ().max_message_size (
      zlink::byte_size_t::bytes (
        _options.admission.effective_max_message_bytes));
    trace_client_server_lazy (
      "client-socket-options",
      [&] {
          return "endpoint=" + _options.expected_server.advertised_endpoint
                 + " max_message_bytes="
                 + std::to_string (
                     dealer->options ().max_message_size ().bytes ());
      });
    dealer->set_routing_id (
      zlink::routing_id_t::from (_options.client_routing_id));
    auto monitor = std::make_unique<zlink::socket_monitor_t> (
      dealer->monitor_open (zlink::monitor_event::connection_ready
                            | zlink::monitor_event::disconnected));
    dealer->connect (_options.expected_server.advertised_endpoint);
    auto monitor_poller = std::make_unique<zlink::poller_t> ();
    monitor_poller->add (*monitor, zlink::poll_event_flag_t::pollin, 1);
    _port = std::make_shared<detail::backend::raw_dealer_port_t> (
      *dealer,
      &_socket_mutex,
      _options.transport_poller,
      _options.transport_poller_slot);
    _monitor_poller = std::move (monitor_poller);
    _monitor = std::move (monitor);
    _dealer = std::move (dealer);
}

void raw_client_server_client_t::close () noexcept
{
    std::shared_ptr<detail::backend::raw_dealer_port_t> port;
    std::unique_ptr<zlink::dealer_socket_t> dealer;
    std::unique_ptr<zlink::poller_t> monitor_poller;
    std::unique_ptr<zlink::socket_monitor_t> monitor;
    {
        std::lock_guard lock (_mutex);
        if (_closed) {
            return;
        }
        _closed = true;
        _ready = false;
        port = std::move (_port);
        dealer = std::move (_dealer);
        monitor_poller = std::move (_monitor_poller);
        monitor = std::move (_monitor);
    }
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
    dealer.reset ();
}

bool raw_client_server_client_t::ready () const noexcept
{
    std::lock_guard lock (_mutex);
    return _ready;
}

task_t<std::size_t> raw_client_server_client_t::drain_monitor_events (
  mesh::service_liveness_registry_t::clock_t::time_point now)
{
    static_cast<void> (now);
    std::size_t count = 0;
    for (;;) {
        std::optional<zlink::monitor_event_t> event;
        std::shared_ptr<detail::backend::raw_dealer_port_t> port;
        {
            std::lock_guard lock (_mutex);
            if (_monitor && _monitor->valid () && _monitor_poller) {
                zlink::poll_event_t readiness;
                bool readable = false;
                try {
                    readable =
                      _monitor_poller->wait (
                        &readiness, 1, std::chrono::milliseconds::zero ())
                        == 1
                      && readiness.slot == 1
                      && (static_cast<short> (readiness.revents)
                          & static_cast<short> (
                            zlink::poll_event_flag_t::pollin))
                           != 0;
                }
                catch (...) {
                }
                if (readable) {
                    event = _monitor->recv (
                      zlink::recv_flags_t::dontwait);
                    port = _port;
                }
            }
        }
        if (!event) {
            co_return count;
        }
        ++count;
        trace_client_server_lazy (
          "client-monitor-event",
          [&] {
              return "event="
                     + std::to_string (static_cast<int> (event->event))
                     + " value=" + std::to_string (event->value)
                     + " local=" + event->local_addr + " remote="
                     + event->remote_addr;
          });
        if (event->event == zlink::monitor_event::connection_ready) {
            trace_client_server_lazy (
              "client-connection-ready",
              [&] {
                  return "endpoint="
                         + _options.expected_server.advertised_endpoint
                         + " channel=" + _options.admission.channel_name
                         + " client="
                         + routing_id_label (
                             _options.admission.channel_name.empty ()
                               ? std::vector<std::uint8_t>{}
                               : _options.client_routing_id);
              });
            {
                std::lock_guard lock (_mutex);
                _connection_id = liveness_connection_identity (
                  _options.expected_server);
                _ready = false;
            }
            if (port) {
                const detail::backend::raw_message_t hello_message{
                  protocol::encode_client_server_client_admission (
                    protocol::command::hello, _options.admission)};
                (void) co_await port->send (hello_message);
            }
        } else if (event->event == zlink::monitor_event::disconnected) {
            trace_client_server_lazy (
              "client-disconnected",
              [&] {
                  return "endpoint="
                         + _options.expected_server.advertised_endpoint
                         + " channel=" + _options.admission.channel_name
                         + " client="
                         + routing_id_label (_options.client_routing_id);
              });
            bool current = false;
            std::vector<std::uint8_t> connection;
            {
                std::lock_guard lock (_mutex);
                connection = liveness_connection_identity (
                  _options.expected_server);
                current = _connection_id == connection;
                if (current) {
                    _ready = false;
                    _connection_id.clear ();
                }
            }
            if (current) {
                (void) _liveness.disconnect (
                  _options.expected_server.server_routing_id, connection);
            }
        }
    }
}

task_t<client_server_pump_result_t> raw_client_server_client_t::pump_one (
  mesh::service_liveness_registry_t::clock_t::time_point now)
{
    _last_pump_bytes = 0;
    std::shared_ptr<detail::backend::raw_dealer_port_t> port;
    {
        std::lock_guard lock (_mutex);
        port = _port;
    }
    if (!port) {
        co_return client_server_pump_result_t::no_data;
    }
    const auto received = port->try_receive ();
    if (!received) {
        co_return client_server_pump_result_t::no_data;
    }
    for (const auto &part : *received)
        _last_pump_bytes += part.size ();
    if (received->empty ()) {
        co_return client_server_pump_result_t::protocol_error;
    }
    try {
        const auto header = protocol::decode_header (received->front ());
        if (header.kind == protocol::command::admit
            || header.kind == protocol::command::update) {
            if (received->size () != 1) {
                co_return client_server_pump_result_t::protocol_error;
            }
            const auto server =
              protocol::decode_client_server_server_admission (
                received->front (), header.kind);
            std::vector<std::uint8_t> connection;
            bool invalid = false;
            bool ready = false;
            {
                std::lock_guard lock (_mutex);
                const auto identity_is_not_pinned =
                  _options.expected_server.server_routing_id.empty ()
                  && _options.expected_server.lifecycle_generation == 0;
                invalid =
                  server.channel_name
                      != _options.expected_server.channel_name
                    || (!identity_is_not_pinned
                        && (server.server_routing_id
                              != _options.expected_server.server_routing_id
                            || server.lifecycle_generation
                                 != _options.expected_server.lifecycle_generation))
                    || server.security_identity
                         != _options.expected_server.security_identity
                    || server.advertised_endpoint
                         != _options.expected_server.advertised_endpoint
                    || server.descriptor_revision
                         < _options.expected_server.descriptor_revision
                    || server.server_routing_id.empty ()
                    || server.lifecycle_generation == 0
                    || _connection_id.empty ();
                if (!invalid) {
                    _options.expected_server = server;
                    _ready =
                      server.state == mesh::service_node_state_t::serving
                      && server.weight > 0;
                    ready = _ready;
                    connection = _connection_id;
                }
            }
            if (invalid)
                co_return client_server_pump_result_t::protocol_error;
            trace_client_server_lazy (
              "client-admitted",
              [&] {
                  return "endpoint=" + server.advertised_endpoint
                         + " channel=" + server.channel_name + " client="
                         + routing_id_label (_options.client_routing_id)
                         + " ready=" + (ready ? "true" : "false");
              });
            _liveness.admit (
              server.server_routing_id, connection, now);
            co_return client_server_pump_result_t::infrastructure;
        }
        if (header.kind == protocol::command::reject) {
            (void) protocol::decode_reject (received->front ());
            {
                std::lock_guard lock (_mutex);
                _ready = false;
            }
            co_return client_server_pump_result_t::infrastructure;
        }
        if (header.kind == protocol::command::livenessProbe
            || header.kind == protocol::command::livenessAck) {
            if (received->size () != 1) {
                co_return client_server_pump_result_t::protocol_error;
            }
            std::vector<std::uint8_t> connection;
            {
                std::lock_guard lock (_mutex);
                connection = _connection_id;
            }
            const auto liveness =
              protocol::decode_liveness (received->front ());
            if (liveness.kind == protocol::command::livenessProbe) {
                const auto acknowledged = _liveness.acknowledge_probe (
                  _options.expected_server.server_routing_id,
                  connection, liveness.probe_id);
                const detail::backend::raw_message_t ack_message{
                  protocol::encode_liveness (
                    protocol::command::livenessAck,
                    liveness.probe_id)};
                if (!acknowledged)
                    co_return client_server_pump_result_t::protocol_error;
                const auto ack_sent = co_await port->send (ack_message);
                if (!ack_sent) {
                    co_return client_server_pump_result_t::protocol_error;
                }
            } else {
                (void) _liveness.acknowledge (
                  _options.expected_server.server_routing_id,
                  connection, liveness.probe_id, now);
            }
            co_return client_server_pump_result_t::infrastructure;
        }
        co_return client_server_pump_result_t::protocol_error;
    }
    catch (const protocol::service_wire_error_t &) {
        co_return client_server_pump_result_t::protocol_error;
    }
}

task_t<mesh::service_liveness_tick_t>
raw_client_server_client_t::tick_liveness (
  mesh::service_liveness_registry_t::clock_t::time_point now)
{
    auto result = _liveness.tick (now);
    std::shared_ptr<detail::backend::raw_dealer_port_t> port;
    {
        std::lock_guard lock (_mutex);
        port = _port;
    }
    if (port) {
        for (const auto &probe : result.probes) {
            const detail::backend::raw_message_t probe_message{
              protocol::encode_liveness (
                protocol::command::livenessProbe, probe.probe_id)};
            (void) co_await port->send (probe_message);
        }
    }
    if (!result.timed_out_nodes.empty ()) {
        std::lock_guard lock (_mutex);
        _ready = false;
    }
    co_return result;
}

std::optional<mesh::service_liveness_registry_t::clock_t::time_point>
raw_client_server_client_t::next_liveness_activity () const
{
    return _liveness.next_activity ();
}

task_t<zlink::submit_result_t> raw_client_server_client_t::send (
  const protocol::application_payload_t &payload,
  std::chrono::milliseconds timeout)
{
    if (timeout <= std::chrono::milliseconds::zero ()) {
        throw std::invalid_argument (
          "ClientServer send timeout must be positive");
    }
    std::shared_ptr<detail::backend::raw_dealer_port_t> port;
    std::string channel;
    bool ready = false;
    {
        std::lock_guard lock (_mutex);
        ready = _ready;
        if (ready) {
            port = _port;
            channel = _options.admission.channel_name;
        }
    }
    if (!ready)
        co_return zlink::submit_result_t::not_connected;
    const auto wire = detail::backend::raw_message_t{
      protocol::encode_channel_send_header (channel),
      protocol::encode_application_payload (payload)};
    trace_client_server_lazy (
      "client-send-wire",
      [&] {
          return "endpoint=" + _options.expected_server.advertised_endpoint
                 + " packet=" + payload.packet_name
                 + " payload_bytes=" + std::to_string (wire[1].size ())
                 + " wire_bytes=" + std::to_string (raw_message_bytes (wire));
      });
    if (!port)
        co_return zlink::submit_result_t::terminated;
    const auto submitted = co_await port->send (wire, timeout);
    trace_client_server_lazy (
      "client-send-result",
      [&] {
          return "endpoint=" + _options.expected_server.advertised_endpoint
                 + " packet=" + payload.packet_name
                 + " submitted="
                 + std::to_string (static_cast<int> (submitted));
      });
    co_return submitted;
}

task_t<client_server_request_completion_t>
raw_client_server_client_t::request (
  const protocol::application_payload_t &payload,
  std::chrono::milliseconds timeout)
{
    if (timeout <= std::chrono::milliseconds::zero ()) {
        throw std::invalid_argument (
          "ClientServer request timeout must be positive");
    }
    std::shared_ptr<detail::backend::raw_dealer_port_t> port;
    std::string channel;
    std::string endpoint;
    std::uint64_t correlation = 0;
    bool ready = false;
    {
        std::lock_guard lock (_mutex);
        ready = _ready && static_cast<bool> (_port);
        if (ready) {
            port = _port;
            channel = _options.admission.channel_name;
            endpoint = _options.expected_server.advertised_endpoint;
            if (_next_correlation == 0) {
                throw std::overflow_error (
                  "ClientServer correlation is exhausted");
            }
            correlation = _next_correlation;
            _next_correlation =
              correlation == std::numeric_limits<std::uint64_t>::max ()
                ? 0
                : correlation + 1;
        }
    }
    if (!ready) {
        co_return client_server_request_completion_t{
          foundation::operation_terminal_t::transport_failed, {}, {}};
    }
    trace_client_server_lazy (
      "client-request-submit",
      [&] {
          return "endpoint=" + endpoint + " channel=" + channel
                 + " client=" + routing_id_label (_options.client_routing_id)
                 + " correlation=" + std::to_string (correlation);
      });
    const auto wire = detail::backend::raw_message_t{
      protocol::encode_channel_request_header (correlation, channel),
      protocol::encode_application_payload (payload)};
    trace_client_server_lazy (
      "client-request-wire",
      [&] {
          return "endpoint=" + endpoint + " correlation="
                 + std::to_string (correlation) + " packet="
                 + payload.packet_name + " payload_bytes="
                 + std::to_string (wire[1].size ()) + " wire_bytes="
                 + std::to_string (raw_message_bytes (wire));
      });
    pending_request_guard_t pending_request (_pending_requests);
    auto completion = co_await port->request (wire, timeout);
    trace_client_server_lazy (
      "client-request-complete",
      [&] {
          return "endpoint=" + endpoint + " correlation="
                 + std::to_string (correlation) + " result="
                 + std::to_string (static_cast<int> (completion.result))
                 + " parts=" + std::to_string (completion.parts.size ());
      });
    if (completion.result != detail::backend::raw_request_result_t::ok) {
        co_return client_server_request_completion_t{
          request_failure (completion.result), {}, {}};
    }
    try {
        auto &parts = completion.parts;
        if (parts.empty () || parts.size () > 2) {
            throw protocol::service_wire_error_t (
              "ClientServer reply has an invalid part count");
        }
        const auto reply = protocol::decode_reply_header (parts.front ());
        trace_client_server_lazy (
          "client-request-header",
          [&] {
              return "correlation=" + std::to_string (correlation)
                     + " terminal_result="
                     + std::to_string (reply.terminal_result)
                     + " failure_code="
                     + std::to_string (reply.failure_code);
          });
        if (reply.correlation != correlation) {
            throw protocol::service_wire_error_t (
              "ClientServer reply correlation does not match");
        }
        if (reply.terminal_result != 0) {
            if (parts.size () != 1) {
                throw protocol::service_wire_error_t (
                  "failed ClientServer reply cannot carry payload");
            }
            co_return client_server_request_completion_t{
              foundation::operation_terminal_t::completed, reply, {}};
        }
        if (parts.size () != 2) {
            throw protocol::service_wire_error_t (
              "successful ClientServer reply requires payload");
        }
        (void) protocol::decode_application_payload (parts[1]);
        co_return client_server_request_completion_t{
          foundation::operation_terminal_t::completed,
          reply,
          std::move (parts[1])};
    }
    catch (const protocol::service_wire_error_t &) {
        co_return client_server_request_completion_t{
          foundation::operation_terminal_t::transport_failed, {}, {}};
    }
}

std::size_t raw_client_server_client_t::pending_request_count () const noexcept
{
    return _pending_requests.load (std::memory_order_relaxed);
}

} // namespace zlink::framework::runtime::client_server

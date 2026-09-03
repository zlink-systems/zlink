/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/client_server/raw_client_server_owner.hpp"
#include "runtime/dispatch/application_job_receive_flow.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/messaging/client_call_codec.hpp"
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

// Spec 51 §4 (ClientServer direction): the client starts hello and
// livenessProbe as Core dealer requests; admit/reject and the probe ACK
// travel back only as the matching request reply. These bounds cap how long
// one control request may stay outstanding before the client retries (hello)
// or lets the liveness deadline decide (probe).
constexpr std::chrono::milliseconds client_server_admission_timeout{5000};
constexpr std::chrono::milliseconds client_server_probe_request_timeout{5000};

// Spec 51 §2/§4: only infrastructure control records (hello/admit/reject/
// update/liveness) use the binary service-wire framing on a ClientServer
// connection. Application records ride the cross-language JSON channel
// envelope, whose first frame is a JSON object — never the 'ZM' magic.
bool is_service_control_frame (
  const detail::backend::raw_bytes_t &frame) noexcept
{
    return frame.size () >= 5 && frame[0] == 0x5A && frame[1] == 0x4D;
}

detail::backend::raw_message_t envelope_wire_parts (
  runtime::messaging::message_parts_t parts)
{
    detail::backend::raw_message_t wire;
    auto items = std::move (parts).take_items ();
    wire.reserve (items.size ());
    for (auto &item : items)
        wire.push_back (item.to_bytes ());
    return wire;
}

// Owns the control record and port for the lifetime of one asynchronous
// dealer control request so the caller does not need to keep them alive.
task_t<detail::backend::raw_request_completion_t> request_control_record (
  std::shared_ptr<detail::backend::raw_dealer_port_t> port,
  detail::backend::raw_message_t parts,
  std::chrono::milliseconds timeout)
{
    co_return co_await port->request (parts, timeout);
}

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
    return _lane.run ([this] {
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
    _receive_flow_registration =
      std::move (receive_flow_registration);
    }).get ();
}

void raw_client_server_server_t::close () noexcept
{
    std::shared_ptr<detail::backend::raw_route_port_t> port;
    std::unique_ptr<zlink::router_socket_t> router;
    std::unique_ptr<zlink::poller_t> monitor_poller;
    std::unique_ptr<zlink::socket_monitor_t> monitor;
    application_job_queue_t::receive_flow_registration_t
      receive_flow_registration;
    try {
        _lane.run ([this, &port, &router, &monitor_poller, &monitor,
                    &receive_flow_registration] {
        if (_closed) {
            return;
        }
        _closed = true;
        port = std::move (_port);
        router = std::move (_router);
        monitor_poller = std::move (_monitor_poller);
        monitor = std::move (_monitor);
        receive_flow_registration =
          std::move (_receive_flow_registration);
        }).get ();
    }
    catch (...) {
        return;
    }
    receive_flow_registration.close ();
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
    return _lane.run ([this] {
        return _options.descriptor.advertised_endpoint;
    }).get ();
}

protocol::client_server_server_admission_t
raw_client_server_server_t::descriptor () const
{
    return _lane.run ([this] {
        return _options.descriptor;
    }).get ();
}

void raw_client_server_server_t::update_descriptor (
  protocol::client_server_server_admission_t descriptor)
{
    return _lane.run ([this, descriptor = std::move (descriptor)] () mutable {
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
    }).get ();
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
        const auto event = _lane.run ([this] {
            if (!_monitor || !_monitor->valid ()) {
                return std::optional<zlink::monitor_event_t>{};
            }
            if (!_monitor_poller) {
                return std::optional<zlink::monitor_event_t>{};
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
                    return std::optional<zlink::monitor_event_t>{};
                }
            }
            catch (...) {
                return std::optional<zlink::monitor_event_t>{};
            }
            return _monitor->recv (zlink::recv_flags_t::dontwait);
        }).get ();
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
            _lane.run ([this, &client] {
                // The monitor value is a ready-count, not a physical connection
                // identity.  The route id is the stable identity available at
                // this framework boundary.
                _connections.insert_or_assign (client, client);
            }).get ();
        } else if (event->event == zlink::monitor_event::disconnected) {
            trace_client_server_lazy (
              "server-disconnected",
              [&] {
                  return "channel=" + _options.descriptor.channel_name
                         + " client=" + routing_id_label (client);
              });
            _lane.run ([this, &client] {
                _connections.erase (client);
            }).get ();
            (void) _lane.run ([this, &client] {
                return _liveness.disconnect (client, client);
            }).get ();
        }
    }
}

task_t<client_server_pump_result_t> raw_client_server_server_t::pump_one (
  mesh::service_liveness_registry_t::clock_t::time_point now,
  std::shared_ptr<application_job_queue_t::permit_t> application_permit)
{
    const auto port = _lane.run ([this] {
        _last_pump_bytes = 0;
        return _port;
    }).get ();
    if (!port) {
        co_return client_server_pump_result_t::no_data;
    }
    const auto pending = _lane.run ([this] {
        if (!_pending_received)
            return std::optional<client_server_pump_result_t>{};
        for (const auto &part : _pending_received->parts)
            _last_pump_bytes += part.size ();
        if (!_mailbox.try_enqueue (std::move (*_pending_received))) {
            return std::optional{client_server_pump_result_t::backpressured};
        }
        _pending_received.reset ();
        return std::optional{client_server_pump_result_t::application};
    }).get ();
    if (pending)
        co_return *pending;
    std::optional<detail::backend::raw_received_t> received;
    received = port->try_receive ();
    if (!received) {
        co_return client_server_pump_result_t::no_data;
    }
    _lane.run ([this, &received] {
        for (const auto &part : received->parts)
            _last_pump_bytes += part.size ();
    }).get ();
    if (received->parts.empty ()) {
        co_return client_server_pump_result_t::protocol_error;
    }
    try {
        if (!is_service_control_frame (received->parts.front ())) {
            //  Application record: [JSON channel-envelope header, payload].
            co_return enqueue_application_record (
              std::move (*received), std::move (application_permit));
        }
        const auto header =
          protocol::decode_header (received->parts.front ());
        if (header.kind == protocol::command::hello) {
            //  Spec 51 §4 (ClientServer direction): the server sends only
            //  reply, liveness, update, and reject. Admission therefore
            //  travels as the reply of the client's hello request — a hello
            //  without a request sequence cannot be answered and is a
            //  protocol error.
            if (received->parts.size () != 1
                || !received->reply_token) {
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
                (void) port->reply (*received, reject_message);
                co_return client_server_pump_result_t::infrastructure;
            }
            _lane.run ([this, &received, now] {
                _connections.insert_or_assign (
                  received->source_routing_id,
                  received->source_routing_id);
                _liveness.admit (received->source_routing_id,
                                 received->source_routing_id, now);
            }).get ();
            const detail::backend::raw_message_t admit_message{
              protocol::encode_client_server_server_admission (
                protocol::command::admit, _options.descriptor)};
            const auto admitted = port->reply (*received, admit_message);
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
        const auto connection = _lane.run ([this, &received] {
            const auto found =
              _connections.find (received->source_routing_id);
            if (found != _connections.end ())
                return std::optional<std::vector<std::uint8_t>>{found->second};
            return std::optional<std::vector<std::uint8_t>>{};
        }).get ();
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
                const auto acknowledged = _lane.run ([this, &received, &connection,
                                                       &liveness] {
                    return _liveness.acknowledge_probe (
                      received->source_routing_id, *connection,
                      liveness.probe_id);
                }).get ();
                const detail::backend::raw_message_t ack_message{
                  protocol::encode_liveness (
                    protocol::command::livenessAck,
                    liveness.probe_id)};
                if (!acknowledged)
                    co_return client_server_pump_result_t::protocol_error;
                //  A request-framed probe is acknowledged on its reply leg;
                //  a raw probe keeps the raw routed ACK.
                bool ack_sent = false;
                if (received->reply_token) {
                    ack_sent = port->reply (*received, ack_message);
                } else {
                    ack_sent = co_await port->send (
                      received->source_routing_id, ack_message);
                }
                if (!ack_sent) {
                    co_return client_server_pump_result_t::protocol_error;
                }
            } else {
                (void) _lane.run ([this, &received, &connection, &liveness, now] {
                    return _liveness.acknowledge (
                      received->source_routing_id, *connection,
                      liveness.probe_id, now);
                }).get ();
            }
            co_return client_server_pump_result_t::infrastructure;
        }
        //  Any other service-wire command is not valid on a ClientServer
        //  connection (application records ride the channel envelope).
        co_return client_server_pump_result_t::protocol_error;
    }
    catch (const protocol::service_wire_error_t &) {
        co_return client_server_pump_result_t::protocol_error;
    }
}

client_server_pump_result_t
raw_client_server_server_t::enqueue_application_record (
  detail::backend::raw_received_t received,
  std::shared_ptr<application_job_queue_t::permit_t> application_permit)
{
    if (received.parts.size () != 2) {
        return client_server_pump_result_t::protocol_error;
    }
    const auto accepted = _lane.run ([this, &received] {
        if (_connections.find (received.source_routing_id)
            == _connections.end ()) {
            return false;
        }
        return true;
    }).get ();
    if (!accepted)
        return client_server_pump_result_t::protocol_error;
    const auto header = messaging::envelope_codec_t{}.decode_header (
      zlink::message_t::from (received.parts.front ()), false);
    if (!header) {
        return client_server_pump_result_t::protocol_error;
    }
    const auto &envelope = header.value ();
    const auto matches_channel = _lane.run ([this, &envelope] {
        return envelope.channel_name == _options.descriptor.channel_name;
    }).get ();
    if (!matches_channel) {
        return client_server_pump_result_t::protocol_error;
    }
    if (envelope.kind == messaging::message_kind_t::request) {
        if (!received.reply_token) {
            return client_server_pump_result_t::protocol_error;
        }
    } else if (envelope.kind != messaging::message_kind_t::command) {
        return client_server_pump_result_t::protocol_error;
    }
    trace_client_server_lazy (
      "server-received",
      [&] {
          return "channel=" + envelope.channel_name
                 + " kind="
                 + std::to_string (static_cast<int> (envelope.kind))
                 + " packet=" + envelope.message_name + " client="
                 + routing_id_label (received.source_routing_id)
                 + " reply_token="
                 + (received.reply_token ? std::string ("present")
                                         : std::string ("-"));
      });
    if (application_permit)
        application_permit->mark_queued ();
    mesh::service_mailbox_record_t record{
      envelope.channel_name,
      mesh::service_mailbox_domain_t::application,
      std::move (received.parts),
      std::move (received.source_routing_id),
      received.reply_token,
      std::nullopt,
      0,
      std::nullopt,
      std::nullopt,
      [permit = std::move (application_permit)] () mutable {
          if (!permit)
              return;
          permit->release_for_handler_entry ();
          permit.reset ();
      }};
    return _lane.run ([this, &record] {
        if (!_mailbox.try_enqueue (std::move (record))) {
            _pending_received.emplace (std::move (record));
            return client_server_pump_result_t::backpressured;
        }
        return client_server_pump_result_t::application;
    }).get ();
}

bool raw_client_server_server_t::has_pending_application () const
{
    return _lane.run ([this] { return _pending_received.has_value (); }).get ();
}

std::size_t raw_client_server_server_t::last_pump_bytes () const
{
    return _lane.run ([this] { return _last_pump_bytes; }).get ();
}

task_t<mesh::service_liveness_tick_t>
raw_client_server_server_t::tick_liveness (
  mesh::service_liveness_registry_t::clock_t::time_point now)
{
    auto prepared = _lane.run ([this, now] {
        struct prepared_t
        {
            mesh::service_liveness_tick_t result;
            std::shared_ptr<detail::backend::raw_route_port_t> port;
            std::optional<protocol::client_server_server_admission_t> update;
            std::vector<std::vector<std::uint8_t>> clients;
        } value{_liveness.tick (now), _port, std::nullopt, {}};
        if (_descriptor_update_pending) {
            value.update = _options.descriptor;
            value.clients.reserve (_connections.size ());
            for (const auto &[client, _] : _connections)
                value.clients.push_back (client);
            _descriptor_update_pending = false;
        }
        return value;
    }).get ();
    if (prepared.port) {
        if (prepared.update) {
            const detail::backend::raw_message_t update_message{
              protocol::encode_client_server_server_admission (
                protocol::command::update, *prepared.update)};
            for (const auto &client : prepared.clients)
                (void) co_await prepared.port->send (client, update_message);
        }
        for (const auto &probe : prepared.result.probes) {
            const detail::backend::raw_message_t probe_message{
              protocol::encode_liveness (
                protocol::command::livenessProbe, probe.probe_id)};
            (void) co_await prepared.port->send (
              probe.node_routing_id, probe_message);
        }
    }
    co_return prepared.result;
}

std::optional<mesh::service_liveness_registry_t::clock_t::time_point>
raw_client_server_server_t::next_liveness_activity () const
{
    return _lane.run ([this] { return _liveness.next_activity (); }).get ();
}

bool raw_client_server_server_t::reply (
  const mesh::service_mailbox_record_t &request,
  const protocol::application_payload_t &payload)
{
    if (request.source_routing_id.empty () || !request.reply_token)
        return false;
    if (request.parts.empty ()) {
        throw std::invalid_argument (
          "ClientServer reply requires request context");
    }
    const auto request_header = messaging::envelope_codec_t{}.decode_header (
      zlink::message_t::from (request.parts.front ()), false);
    if (!request_header) {
        throw std::invalid_argument (
          "ClientServer reply requires a decodable request envelope");
    }
    const auto port = _lane.run ([this] { return _port; }).get ();
    if (!port)
        return false;
    zlink::framework::detail::channel_reply_writer_t writer;
    auto header = writer.create_reply_header (
      messaging::message_kind_t::response,
      request_header.value ().channel_name,
      request_header.value ());
    header.content_type = payload.content_type;
    auto parts = envelope_wire_parts (
      writer.reply_raw_envelope (
        header, zlink::message_t::from (payload.payload)));
    const auto delivered = port->reply (
      {request.source_routing_id, request.reply_token, {}},
      parts);
    trace_client_server_lazy (
      "server-reply",
      [&] {
          return "channel=" + request_header.value ().channel_name
                 + " packet=" + request_header.value ().message_name
                 + " client=" + routing_id_label (request.source_routing_id)
                 + " reply_token="
                 + (request.reply_token ? std::string ("present")
                                        : std::string ("-"))
                 + " delivered=" + (delivered ? "true" : "false");
      });
    return delivered;
}

bool raw_client_server_server_t::reply (
  const mesh::service_mailbox_record_t &request,
  const framework_exception_t &error)
{
    if (request.source_routing_id.empty () || !request.reply_token)
        return false;
    if (request.parts.empty ()) {
        throw std::invalid_argument (
          "ClientServer reply requires request context");
    }
    const auto request_header = messaging::envelope_codec_t{}.decode_header (
      zlink::message_t::from (request.parts.front ()), false);
    if (!request_header) {
        throw std::invalid_argument (
          "ClientServer reply requires a decodable request envelope");
    }
    const auto port = _lane.run ([this] { return _port; }).get ();
    if (!port)
        return false;
    zlink::framework::detail::channel_reply_writer_t writer;
    auto header = writer.create_error_header (
      request_header.value ().channel_name, request_header.value (), error);
    //  The error reply body is the JSON literal `null`, matching the other
    //  language runtimes' error envelope emission.
    auto parts = envelope_wire_parts (
      writer.reply_raw_envelope (
        header,
        zlink::message_t::from (
          std::vector<std::uint8_t>{'n', 'u', 'l', 'l'})));
    return port->reply (
      {request.source_routing_id, request.reply_token, {}},
      parts);
}

bool raw_client_server_server_t::byte_vector_less_t::operator() (
  const std::vector<std::uint8_t> &left,
  const std::vector<std::uint8_t> &right) const noexcept
{
    return std::lexicographical_compare (
      left.begin (), left.end (), right.begin (), right.end ());
}

// Completed admission/probe request replies parked by the asynchronous
// completion callbacks until the pump loop applies them. The callbacks
// capture only this shared state (never the owning client), so a completion
// arriving after the client is destroyed writes into an orphaned state and
// is dropped.
struct raw_client_server_client_t::control_reply_state_t
{
    /* Spec 51 §4 physical-connection replacement: a late event from a
     * previous pair must not alter the new connection's admission or
     * liveness. Each parked completion therefore carries the connection
     * identity its request was started on; apply discards a mismatch. */
    struct parked_reply_t
    {
        std::vector<std::uint8_t> connection;
        std::uint64_t connection_generation = 0;
        detail::backend::raw_request_completion_t completion;
    };

    runtime::offload_executor_t lane_executor;
    runtime::state_lane_t lane{lane_executor};
    bool admission_in_flight = false;
    std::optional<parked_reply_t> admission;
    std::vector<std::pair<std::uint64_t, parked_reply_t>> probes;
};

raw_client_server_client_t::raw_client_server_client_t (
  raw_client_server_client_options_t options,
  std::shared_ptr<zlink::context_t> context) :
    _options (std::move (options)),
    _context (
      context ? std::move (context) : std::make_shared<zlink::context_t> ()),
    _control_replies (std::make_shared<control_reply_state_t> ())
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
    return _lane.run ([this] {
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
    application_job_queue_t::receive_flow_registration_t
      receive_flow_registration;
    if (_options.application_jobs) {
        receive_flow_registration =
          _options.application_jobs->register_receive_flow_socket (
            [socket = dealer.get ()] (
              application_job_queue_pressure_state_t state) {
                return apply_application_job_receive_flow_state (
                  *socket, state);
            });
    }
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
    _receive_flow_registration =
      std::move (receive_flow_registration);
    }).get ();
}

void raw_client_server_client_t::close () noexcept
{
    std::shared_ptr<detail::backend::raw_dealer_port_t> port;
    std::unique_ptr<zlink::dealer_socket_t> dealer;
    std::unique_ptr<zlink::poller_t> monitor_poller;
    std::unique_ptr<zlink::socket_monitor_t> monitor;
    application_job_queue_t::receive_flow_registration_t
      receive_flow_registration;
    try {
        _lane.run ([this, &port, &dealer, &monitor_poller, &monitor,
                    &receive_flow_registration] {
        if (_closed) {
            return;
        }
        _closed = true;
        _ready = false;
        port = std::move (_port);
        dealer = std::move (_dealer);
        monitor_poller = std::move (_monitor_poller);
        monitor = std::move (_monitor);
        receive_flow_registration =
          std::move (_receive_flow_registration);
        }).get ();
    }
    catch (...) {
        return;
    }
    receive_flow_registration.close ();
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

bool raw_client_server_client_t::ready () const
{
    return _lane.run ([this] { return _ready; }).get ();
}

task_t<std::size_t> raw_client_server_client_t::drain_monitor_events (
  mesh::service_liveness_registry_t::clock_t::time_point now)
{
    static_cast<void> (now);
    std::size_t count = 0;
    for (;;) {
        const auto event_and_port = _lane.run ([this] {
            std::pair<std::optional<zlink::monitor_event_t>,
                      std::shared_ptr<detail::backend::raw_dealer_port_t>> value;
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
                    value.first = _monitor->recv (
                      zlink::recv_flags_t::dontwait);
                    value.second = _port;
                }
            }
            return value;
        }).get ();
        const auto &event = event_and_port.first;
        const auto &port = event_and_port.second;
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
            _lane.run ([this] {
                _connection_id = liveness_connection_identity (
                  _options.expected_server);
                ++_connection_generation;
                _ready = false;
            }).get ();
            if (port) {
                //  Spec 51 §4 (ClientServer direction): hello starts as a
                //  Core dealer request; the admit/reject decision arrives
                //  only as that request's reply.
                begin_admission_request (port);
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
            current = _lane.run ([this, &connection] {
                connection = liveness_connection_identity (
                  _options.expected_server);
                const auto current = _connection_id == connection;
                if (current) {
                    _ready = false;
                    _connection_id.clear ();
                    ++_connection_generation;
                }
                return current;
            }).get ();
            if (current) {
                (void) _lane.run ([this, &connection] {
                    return _liveness.disconnect (
                      _options.expected_server.server_routing_id, connection);
                }).get ();
            }
        }
    }
}

task_t<client_server_pump_result_t> raw_client_server_client_t::pump_one (
  mesh::service_liveness_registry_t::clock_t::time_point now)
{
    const auto port = _lane.run ([this] {
        _last_pump_bytes = 0;
        return _port;
    }).get ();
    if (!port) {
        co_return client_server_pump_result_t::no_data;
    }
    if (apply_pending_control_replies (now)) {
        co_return client_server_pump_result_t::infrastructure;
    }
    const auto received = port->try_receive ();
    if (!received) {
        co_return client_server_pump_result_t::no_data;
    }
    _lane.run ([this, &received] {
        for (const auto &part : *received)
            _last_pump_bytes += part.size ();
    }).get ();
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
            co_return accept_server_admission (
              received->front (), header.kind, now);
        }
        if (header.kind == protocol::command::reject) {
            (void) protocol::decode_reject (received->front ());
            _lane.run ([this] {
                _ready = false;
            }).get ();
            co_return client_server_pump_result_t::infrastructure;
        }
        if (header.kind == protocol::command::livenessProbe
            || header.kind == protocol::command::livenessAck) {
            if (received->size () != 1) {
                co_return client_server_pump_result_t::protocol_error;
            }
            const auto connection = _lane.run ([this] {
                return _connection_id;
            }).get ();
            const auto liveness =
              protocol::decode_liveness (received->front ());
            if (liveness.kind == protocol::command::livenessProbe) {
                const auto acknowledged = _lane.run ([this, &connection, &liveness] {
                    return _liveness.acknowledge_probe (
                      _options.expected_server.server_routing_id,
                      connection, liveness.probe_id);
                }).get ();
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
                (void) _lane.run ([this, &connection, &liveness, now] {
                    return _liveness.acknowledge (
                      _options.expected_server.server_routing_id,
                      connection, liveness.probe_id, now);
                }).get ();
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
    const auto result = _lane.run ([this, now] {
        return _liveness.tick (now);
    }).get ();
    const auto port = _lane.run ([this] { return _port; }).get ();
    if (port) {
        for (const auto &probe : result.probes) {
            //  Spec 51 §4 (ClientServer direction): the client-initiated
            //  probe rides the Core dealer request envelope; its ACK is the
            //  matching reply.
            begin_probe_request (port, probe.probe_id);
        }
    }
    if (!result.timed_out_nodes.empty ()) {
        _lane.run ([this] { _ready = false; }).get ();
    }
    co_return result;
}

std::optional<mesh::service_liveness_registry_t::clock_t::time_point>
raw_client_server_client_t::next_liveness_activity () const
{
    return _lane.run ([this] { return _liveness.next_activity (); }).get ();
}

client_server_pump_result_t
raw_client_server_client_t::accept_server_admission (
  const detail::backend::raw_bytes_t &frame,
  protocol::command kind,
  mesh::service_liveness_registry_t::clock_t::time_point now)
{
    const auto server =
      protocol::decode_client_server_server_admission (frame, kind);
    const auto state = _lane.run ([this, &server] {
        struct state_t
        {
            std::vector<std::uint8_t> connection;
            bool invalid = false;
            bool ready = false;
        } value;
        const auto identity_is_not_pinned =
          _options.expected_server.server_routing_id.empty ()
          && _options.expected_server.lifecycle_generation == 0;
        value.invalid =
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
        if (!value.invalid) {
            _options.expected_server = server;
            _ready =
              server.state == mesh::service_node_state_t::serving
              && server.weight > 0;
            value.ready = _ready;
            value.connection = _connection_id;
        }
        return value;
    }).get ();
    if (state.invalid)
        return client_server_pump_result_t::protocol_error;
    trace_client_server_lazy (
      "client-admitted",
      [&] {
          return "endpoint=" + server.advertised_endpoint
                 + " channel=" + server.channel_name + " client="
                 + routing_id_label (_options.client_routing_id)
                 + " ready=" + (state.ready ? "true" : "false");
      });
    _lane.run ([this, &server, &state, now] {
        _liveness.admit (server.server_routing_id, state.connection, now);
    }).get ();
    return client_server_pump_result_t::infrastructure;
}

void raw_client_server_client_t::begin_admission_request (
  const std::shared_ptr<detail::backend::raw_dealer_port_t> &port)
{
    const auto replies = _control_replies;
    const auto admitted = replies->lane.run ([replies] {
        if (replies->admission_in_flight)
            return false;
        replies->admission_in_flight = true;
        return true;
    }).get ();
    if (!admitted)
        return;
    const auto state = _lane.run ([this] {
        struct state_t
        {
            protocol::client_server_client_admission_t admission;
            std::vector<std::uint8_t> connection;
            std::uint64_t connection_generation = 0;
        } value;
        value.admission = _options.admission;
        value.connection = _connection_id;
        value.connection_generation = _connection_generation;
        return value;
    }).get ();
    auto admission = state.admission;
    auto connection = state.connection;
    const auto connection_generation = state.connection_generation;
    if (connection.empty ()) {
        replies->lane.run ([replies] {
            replies->admission_in_flight = false;
        }).get ();
        return;
    }
    trace_client_server_lazy (
      "client-admission-request",
      [&] { return "channel=" + admission.channel_name; });
    detail::backend::raw_message_t hello_message{
      protocol::encode_client_server_client_admission (
        protocol::command::hello, admission)};
    auto running = std::make_shared<
      task_t<detail::backend::raw_request_completion_t>> (
        request_control_record (port,
                                std::move (hello_message),
                                client_server_admission_timeout));
    detail::observe_task_completion (
      *running,
      [state = replies, running, connection,
       connection_generation] (
        const result_t<detail::backend::raw_request_completion_t> &settled) {
          trace_client_server_lazy (
            "client-admission-complete",
            [&] {
                return std::string ("settled=")
                       + (settled ? "true" : "false")
                       + (settled
                            ? " result="
                                + std::to_string (static_cast<int> (
                                    settled.value ().result))
                                + " parts="
                                + std::to_string (
                                    settled.value ().parts.size ())
                            : std::string ());
            });
          (void) state->lane.try_post (
            [state, connection, connection_generation, settled] {
                state->admission_in_flight = false;
                if (settled) {
                    state->admission.emplace (
                      control_reply_state_t::parked_reply_t{
                        connection, connection_generation, settled.value ()});
                } else {
                    state->admission.emplace (
                      control_reply_state_t::parked_reply_t{
                        connection, connection_generation,
                        detail::backend::raw_request_completion_t{
                          detail::backend::raw_request_result_t::failed, {}}});
                }
            });
      });
}

void raw_client_server_client_t::begin_probe_request (
  const std::shared_ptr<detail::backend::raw_dealer_port_t> &port,
  std::uint64_t probe_id)
{
    const auto state = _lane.run ([this] {
        return std::pair{_connection_id, _connection_generation};
    }).get ();
    const auto &connection = state.first;
    const auto connection_generation = state.second;
    if (connection.empty ())
        return;
    detail::backend::raw_message_t probe_message{
      protocol::encode_liveness (
        protocol::command::livenessProbe, probe_id)};
    auto running = std::make_shared<
      task_t<detail::backend::raw_request_completion_t>> (
        request_control_record (port,
                                std::move (probe_message),
                                client_server_probe_request_timeout));
    detail::observe_task_completion (
      *running,
      [state = _control_replies, running, probe_id, connection,
       connection_generation] (
        const result_t<detail::backend::raw_request_completion_t> &settled) {
          if (!settled)
              return;
          (void) state->lane.try_post (
            [state, probe_id, connection, connection_generation, settled] {
                state->probes.emplace_back (
                  probe_id,
                  control_reply_state_t::parked_reply_t{
                    connection, connection_generation, settled.value ()});
            });
      });
}

bool raw_client_server_client_t::apply_pending_control_replies (
  mesh::service_liveness_registry_t::clock_t::time_point now)
{
    const auto replies = _control_replies;
    auto pending = replies->lane.run ([replies] {
        std::pair<std::optional<control_reply_state_t::parked_reply_t>,
                  std::vector<std::pair<std::uint64_t,
                                        control_reply_state_t::parked_reply_t>>>
          value;
        value.first.swap (replies->admission);
        value.second.swap (replies->probes);
        return value;
    }).get ();
    auto admission = std::move (pending.first);
    auto probes = std::move (pending.second);
    const auto current = _lane.run ([this] {
        return std::pair{_connection_id, _connection_generation};
    }).get ();
    const auto &current_connection = current.first;
    const auto current_generation = current.second;
    bool progressed = false;
    if (admission) {
        progressed = true;
        const bool stale =
          admission->connection != current_connection
          || admission->connection_generation != current_generation;
        const auto &completion = admission->completion;
        if (stale) {
            //  Spec 51 §4: a late admission reply from a previous physical
            //  pair cannot admit or reject the current connection.
            trace_client_server_lazy (
              "client-admission-stale-discard",
              [&] {
                  return "channel=" + _options.admission.channel_name
                         + " result="
                         + std::to_string (
                             static_cast<int> (completion.result));
              });
            //  The current (new) connection still needs its own admission.
            const auto retry_state = _lane.run ([this] {
                std::pair<bool, std::shared_ptr<detail::backend::raw_dealer_port_t>> value;
                value.first = !_closed && !_ready && !_connection_id.empty ();
                if (value.first)
                    value.second = _port;
                return value;
            }).get ();
            const auto request = retry_state.first;
            const auto &port = retry_state.second;
            if (request && port)
                begin_admission_request (port);
        } else if (completion.result
                     == detail::backend::raw_request_result_t::ok
                   && completion.parts.size () == 1) {
            try {
                const auto header =
                  protocol::decode_header (completion.parts.front ());
                if (header.kind == protocol::command::admit) {
                    (void) accept_server_admission (
                      completion.parts.front (), header.kind, now);
                } else if (header.kind == protocol::command::reject) {
                    (void) protocol::decode_reject (
                      completion.parts.front ());
                    _lane.run ([this] { _ready = false; }).get ();
                } else {
                    trace_client_server (
                      "client-admission-invalid-reply");
                }
            }
            catch (const protocol::service_wire_error_t &) {
                trace_client_server ("client-admission-malformed-reply");
            }
        } else {
            //  The request failed or timed out. Retry while the physical
            //  connection is still current and admission has not happened.
            const auto retry_state = _lane.run ([this] {
                std::pair<bool, std::shared_ptr<detail::backend::raw_dealer_port_t>> value;
                value.first = !_closed && !_ready && !_connection_id.empty ();
                if (value.first)
                    value.second = _port;
                return value;
            }).get ();
            const auto retry = retry_state.first;
            const auto &port = retry_state.second;
            if (retry && port)
                begin_admission_request (port);
        }
    }
    for (const auto &probe : probes) {
        progressed = true;
        if (probe.second.connection != current_connection
            || probe.second.connection_generation != current_generation) {
            //  Spec 51 §4/§5: a probe ACK from a previous physical pair
            //  cannot extend the new connection's liveness deadline.
            trace_client_server_lazy (
              "client-probe-stale-discard",
              [&] { return "probe=" + std::to_string (probe.first); });
            continue;
        }
        const auto &completion = probe.second.completion;
        if (completion.result != detail::backend::raw_request_result_t::ok
            || completion.parts.size () != 1) {
            continue;
        }
        try {
            const auto liveness =
              protocol::decode_liveness (completion.parts.front ());
            if (liveness.kind != protocol::command::livenessAck
                || liveness.probe_id != probe.first) {
                continue;
            }
            (void) _lane.run ([this, &current_connection, &liveness, now] {
                return _liveness.acknowledge (
                  _options.expected_server.server_routing_id,
                  current_connection, liveness.probe_id, now);
            }).get ();
        }
        catch (const protocol::service_wire_error_t &) {
            trace_client_server ("client-probe-malformed-reply");
        }
    }
    return progressed;
}

task_t<zlink::submit_result_t> raw_client_server_client_t::send (
  const protocol::application_payload_t &payload,
  std::chrono::milliseconds timeout)
{
    if (timeout <= std::chrono::milliseconds::zero ()) {
        throw std::invalid_argument (
          "ClientServer send timeout must be positive");
    }
    const auto state = _lane.run ([this] {
        struct state_t
        {
            std::shared_ptr<detail::backend::raw_dealer_port_t> port;
            std::string channel;
            bool ready = false;
        } value;
        value.ready = _ready;
        if (value.ready) {
            value.port = _port;
            value.channel = _options.admission.channel_name;
        }
        return value;
    }).get ();
    const auto &port = state.port;
    const auto &channel = state.channel;
    const auto ready = state.ready;
    if (!ready)
        co_return zlink::submit_result_t::not_connected;
    //  A ClientServer one-way rides the channel envelope as a Command
    //  record; per the shared dialect a Command carries no correlation id.
    messaging::envelope_header_t header;
    header.kind = messaging::message_kind_t::command;
    header.channel_name = channel;
    header.message_name = payload.packet_name;
    header.content_type = payload.content_type;
    const auto wire = envelope_wire_parts (
      messaging::envelope_codec_t{}.encode_raw_body_parts (
        header, zlink::message_t::from (payload.payload)));
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
    const auto state = _lane.run ([this] {
        struct state_t
        {
            std::shared_ptr<detail::backend::raw_dealer_port_t> port;
            std::string channel;
            std::string endpoint;
            bool ready = false;
        } value;
        value.ready = _ready && static_cast<bool> (_port);
        if (value.ready) {
            value.port = _port;
            value.channel = _options.admission.channel_name;
            value.endpoint = _options.expected_server.advertised_endpoint;
        }
        return value;
    }).get ();
    const auto &port = state.port;
    const auto &channel = state.channel;
    const auto &endpoint = state.endpoint;
    const auto ready = state.ready;
    if (!ready) {
        co_return client_server_request_completion_t{
          foundation::operation_terminal_t::transport_failed};
    }
    //  A ClientServer request rides the channel envelope as a Request
    //  record with a required correlation id; the deadline mirrors the
    //  caller timeout like the other language runtimes.
    messaging::client_call_codec_t codec;
    auto header = codec.create_envelope (
      messaging::message_kind_t::request, channel, payload.packet_name,
      timeout);
    header.content_type = payload.content_type;
    const auto correlation_id = header.correlation_id;
    trace_client_server_lazy (
      "client-request-submit",
      [&] {
          return "endpoint=" + endpoint + " channel=" + channel
                 + " client=" + routing_id_label (_options.client_routing_id)
                 + " correlation=" + correlation_id;
      });
    const auto wire = envelope_wire_parts (
      messaging::envelope_codec_t{}.encode_raw_body_parts (
        header, zlink::message_t::from (payload.payload)));
    trace_client_server_lazy (
      "client-request-wire",
      [&] {
          return "endpoint=" + endpoint + " correlation=" + correlation_id
                 + " packet=" + payload.packet_name + " payload_bytes="
                 + std::to_string (wire[1].size ()) + " wire_bytes="
                 + std::to_string (raw_message_bytes (wire));
      });
    pending_request_guard_t pending_request (_pending_requests);
    auto completion = co_await port->request (wire, timeout);
    trace_client_server_lazy (
      "client-request-complete",
      [&] {
          return "endpoint=" + endpoint + " correlation=" + correlation_id
                 + " result="
                 + std::to_string (static_cast<int> (completion.result))
                 + " parts=" + std::to_string (completion.parts.size ());
      });
    if (completion.result != detail::backend::raw_request_result_t::ok) {
        co_return client_server_request_completion_t{
          request_failure (completion.result)};
    }
    //  Spec 32-framework-error-model:91-92 — a malformed ClientServer reply
    //  is ProtocolError, not a transport failure; report it as a completed
    //  terminal carrying the protocol_error code for the consumer's mapper.
    const auto protocol_failure =
      [] (std::string message) {
          client_server_request_completion_t failure{
            foundation::operation_terminal_t::completed};
          failure.error_code = "protocol_error";
          failure.error_message = std::move (message);
          return failure;
      };
    auto &parts = completion.parts;
    if (parts.size () != 2) {
        co_return protocol_failure (
          "ClientServer reply has an invalid part count");
    }
    const auto reply_header = messaging::envelope_codec_t{}.decode_header (
      zlink::message_t::from (parts.front ()), false);
    if (!reply_header) {
        co_return protocol_failure (
          reply_header.error () != nullptr
            ? reply_header.error ()->what ()
            : "ClientServer reply envelope is malformed");
    }
    const auto &reply = reply_header.value ();
    if (reply.kind == messaging::message_kind_t::error) {
        client_server_request_completion_t failure{
          foundation::operation_terminal_t::completed};
        failure.error_code = reply.error_code.value_or ("request_failed");
        failure.error_message = reply.error_message.value_or (
          "ClientServer request failed");
        co_return failure;
    }
    if (reply.kind != messaging::message_kind_t::response
        || reply.correlation_id != correlation_id) {
        co_return protocol_failure (
          "ClientServer reply kind or correlation does not match");
    }
    client_server_request_completion_t success{
      foundation::operation_terminal_t::completed};
    success.content_type = reply.content_type;
    success.payload = std::move (parts[1]);
    co_return success;
}

std::size_t raw_client_server_client_t::pending_request_count () const noexcept
{
    return _pending_requests.load (std::memory_order_relaxed);
}

std::size_t raw_client_server_client_t::last_pump_bytes () const
{
    return _lane.run ([this] { return _last_pump_bytes; }).get ();
}

} // namespace zlink::framework::runtime::client_server

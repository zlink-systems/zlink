/* SPDX-License-Identifier: FSL-1.1-ALv2 */

/* C++ cross-language host (G6).
 *
 * One binary with the same role modes the .NET `Zlink.Framework.TestHost`
 * exposes, so every producer/consumer direction runs against the other
 * language's real public package: channel request/send, fanout publish and
 * subscribe, and STREAM session frames. Payload shape and packet identity
 * follow the common contract — a JSON body with the DTO's field names and the
 * packet name from `static constexpr packet_name`, which is the type name the
 * other languages resolve by default. */

#include <zlink/framework.hpp>
#include <zlink/stream_connector.hpp>
#include <zlink/stream_connector/codecs/auto_codec.hpp>

#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace fw = zlink::framework;
namespace runtime = zlink::framework::runtime;

namespace
{

std::string host_from_tcp_endpoint (const std::string &endpoint)
{
    const auto start = endpoint.rfind ("tcp://", 0) == 0 ? 6U : 0U;
    const auto separator = endpoint.rfind (':');
    if (separator == std::string::npos || separator <= start)
        throw std::invalid_argument ("ClientServer endpoint must use tcp://host:port");
    return endpoint.substr (start, separator - start);
}

std::uint16_t port_from_tcp_endpoint (const std::string &endpoint)
{
    const auto separator = endpoint.rfind (':');
    if (separator == std::string::npos || separator + 1 >= endpoint.size ())
        throw std::invalid_argument ("ClientServer endpoint must use tcp://host:port");
    const auto value = std::stoul (endpoint.substr (separator + 1));
    if (value == 0 || value > 65535)
        throw std::invalid_argument ("ClientServer endpoint port is out of range");
    return static_cast<std::uint16_t> (value);
}

/* Cross-language DTOs. The names match the .NET TestHost records and the Node
 * classes: both languages derive the packet name from the type name. */
struct test_host_profile_request_t
{
    static constexpr const char *packet_name = "TestHostProfileRequest";
    std::string value;
};

struct test_host_profile_reply_t
{
    static constexpr const char *packet_name = "TestHostProfileReply";
    std::string value;
};

struct test_host_profile_send_t
{
    static constexpr const char *packet_name = "TestHostProfileSend";
    std::string value;
};

struct test_host_published_event_t
{
    static constexpr const char *packet_name = "TestHostPublishedEvent";
    std::string value;
};

/* Cross-language spot route wire scenario DTOs: (a) echo request/reply,
 * (b) a packet no host registers (framework NOT_FOUND), (c) a request whose
 * handler fails with a typed application error kind. */
struct test_host_spot_route_request_t
{
    static constexpr const char *packet_name = "TestHostSpotRouteRequest";
    std::string value;
};

struct test_host_spot_route_reply_t
{
    static constexpr const char *packet_name = "TestHostSpotRouteReply";
    std::string value;
};

struct test_host_spot_route_fail_request_t
{
    static constexpr const char *packet_name = "TestHostSpotRouteFailRequest";
    std::string value;
};

struct test_host_spot_route_missing_request_t
{
    static constexpr const char *packet_name = "TestHostSpotRouteMissingRequest";
    std::string value;
};

inline void to_json (nlohmann::json &json, const test_host_profile_request_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}
/* Peer languages emit the field with their own naming policy; accept both so
 * the codec row does not depend on a case convention. */
inline std::string read_value_field (const nlohmann::json &json)
{
    if (json.contains ("value")) {
        return json.at ("value").get<std::string> ();
    }
    if (json.contains ("Value")) {
        return json.at ("Value").get<std::string> ();
    }
    throw std::runtime_error ("cross-language payload has no value field");
}

inline void from_json (const nlohmann::json &json, test_host_profile_request_t &value)
{
    value.value = read_value_field (json);
}
inline void to_json (nlohmann::json &json, const test_host_profile_reply_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}
inline void from_json (const nlohmann::json &json, test_host_profile_reply_t &value)
{
    value.value = read_value_field (json);
}
inline void to_json (nlohmann::json &json, const test_host_profile_send_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}
inline void from_json (const nlohmann::json &json, test_host_profile_send_t &value)
{
    value.value = read_value_field (json);
}
inline void to_json (nlohmann::json &json, const test_host_published_event_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}
inline void from_json (const nlohmann::json &json, test_host_published_event_t &value)
{
    value.value = read_value_field (json);
}
inline void to_json (nlohmann::json &json, const test_host_spot_route_request_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}
inline void from_json (const nlohmann::json &json, test_host_spot_route_request_t &value)
{
    value.value = read_value_field (json);
}
inline void to_json (nlohmann::json &json, const test_host_spot_route_reply_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}
inline void from_json (const nlohmann::json &json, test_host_spot_route_reply_t &value)
{
    value.value = read_value_field (json);
}
inline void to_json (nlohmann::json &json, const test_host_spot_route_fail_request_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}
inline void from_json (const nlohmann::json &json, test_host_spot_route_fail_request_t &value)
{
    value.value = read_value_field (json);
}
inline void to_json (nlohmann::json &json, const test_host_spot_route_missing_request_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}
inline void from_json (const nlohmann::json &json, test_host_spot_route_missing_request_t &value)
{
    value.value = read_value_field (json);
}

/* Snake_case error-code table (mirrors channel_reply_writer.cpp
 * error_code_name) so the recorded client markers use the wire names. */
std::string error_kind_wire_name (fw::framework_error_kind_t kind)
{
    switch (kind) {
        case fw::framework_error_kind_t::not_found:
            return "not_found";
        case fw::framework_error_kind_t::already_exists:
            return "already_exists";
        case fw::framework_error_kind_t::type_mismatch:
            return "type_mismatch";
        case fw::framework_error_kind_t::not_configured:
            return "not_configured";
        case fw::framework_error_kind_t::rejected:
            return "rejected";
        case fw::framework_error_kind_t::unavailable:
            return "unavailable";
        case fw::framework_error_kind_t::capacity_exceeded:
            return "capacity_exceeded";
        case fw::framework_error_kind_t::deadline_exceeded:
            return "deadline_exceeded";
        case fw::framework_error_kind_t::shutting_down:
            return "shutting_down";
        case fw::framework_error_kind_t::protocol_error:
            return "protocol_error";
        case fw::framework_error_kind_t::invalid_operation:
            return "invalid_operation";
        case fw::framework_error_kind_t::data_lost:
            return "data_lost";
        case fw::framework_error_kind_t::internal_failure:
            return "internal_failure";
    }
    return "internal_failure";
}

std::string error_origin_wire_name (const fw::framework_exception_t &error)
{
    switch (fw::detail::error_origin (error)) {
        case fw::detail::error_origin_t::framework:
            return "framework";
        case fw::detail::error_origin_t::application:
            return "application";
        case fw::detail::error_origin_t::unspecified:
            break;
    }
    return "unspecified";
}

class event_sink_t
{
  public:
    explicit event_sink_t (std::string path) : _path (std::move (path)) {}

    void append (const std::string &line)
    {
        const std::lock_guard<std::mutex> lock (_mutex);
        std::cout << line << std::endl;
        if (_path.empty ()) {
            return;
        }
        std::ofstream file (_path, std::ios::app);
        file << line << "\n";
    }

  private:
    std::mutex _mutex;
    std::string _path;
};

std::map<std::string, std::string> host_args;

std::string option (const std::string &name, const std::string &fallback = {})
{
    const auto found = host_args.find (name);
    return found == host_args.end () ? fallback : found->second;
}

std::string require (const std::string &name)
{
    const auto value = option (name);
    if (value.empty ()) {
        throw std::runtime_error ("--" + name + " is required");
    }
    return value;
}

void write_ready ()
{
    const auto path = option ("ready-file");
    if (path.empty ()) {
        return;
    }
    std::ofstream file (path, std::ios::trunc);
    file << "ready\n";
}

std::vector<std::uint8_t> routing_id_bytes (const std::string &value)
{
    return {value.begin (), value.end ()};
}

std::string routing_id_string (const std::vector<std::uint8_t> &value)
{
    return {value.begin (), value.end ()};
}

runtime::protocol::message_follow_notice_t make_message_follow_notice (
  const runtime::mesh::service_node_descriptor_t &source_node,
  const runtime::mesh::service_node_descriptor_t &target_node,
  std::uint64_t operation_low)
{
    const auto actor_route = [] (
      const runtime::mesh::service_node_descriptor_t &node,
      std::uint64_t authority_owner_generation) {
        return runtime::protocol::actor_route_fence_t{
          "cross-language-message-follow",
          1,
          node.node_routing_id,
          node.lifecycle_generation,
          authority_owner_generation,
          8};
    };
    return runtime::protocol::message_follow_notice_t{
      actor_route (source_node, 7),
      actor_route (target_node, 8),
      1,
      1,
      64,
      runtime::protocol::wire_operation_id_t{3, operation_low},
      11};
}

int run_message_follow_host ()
{
    using runtime::mesh::raw_mesh_node_options_t;
    using runtime::mesh::raw_mesh_node_owner_t;
    using runtime::mesh::service_mailbox_domain_t;
    using runtime::mesh::service_node_state_t;
    using runtime::protocol::decode_message_follow;

    const auto local_name = option ("node-rid", "cpp-message-follow");
    const auto peer_name = require ("peer-rid");
    const auto local_rid = routing_id_bytes (local_name);
    const auto peer_rid = routing_id_bytes (peer_name);
    if (local_rid.empty () || peer_rid.empty ())
        throw std::invalid_argument ("message-follow routing IDs must be non-empty");

    raw_mesh_node_options_t options;
    options.descriptor.mesh_name = option ("mesh-name", "message-follow");
    options.descriptor.node_routing_id = local_rid;
    options.descriptor.lifecycle_generation = 1;
    options.descriptor.descriptor_revision = 1;
    options.descriptor.advertised_endpoint = require ("bind-endpoint");
    options.descriptor.state = service_node_state_t::preparing;
    options.descriptor.security_identity = "default";
    options.descriptor.protocol_capabilities = {
      "framework-service-v12", runtime::protocol::required_capability};

    raw_mesh_node_owner_t owner (std::move (options));
    owner.start ();
    if (!owner.connect_peer (require ("peer-endpoint")))
        throw std::runtime_error ("message-follow peer connection was not submitted");

    event_sink_t sink (option ("event-file"));
    write_ready ();
    const auto deadline = std::chrono::steady_clock::now ()
                          + std::chrono::seconds (60);
    bool sent = false;
    bool received = false;
    while (std::chrono::steady_clock::now () < deadline) {
        const auto now =
          runtime::mesh::service_liveness_registry_t::clock_t::now ();
        (void) owner.drain_monitor_events (now);
        (void) owner.pump_one (now);
        (void) owner.tick_liveness (now);

        while (true) {
            auto claim = owner.mailbox ().try_claim (
              service_mailbox_domain_t::infrastructure, 16, 1024 * 1024);
            if (!claim)
                break;
            for (const auto &record : claim->records) {
                if (record.parts.size () != 1)
                    continue;
                const auto notice = decode_message_follow (record.parts.front ());
                const auto source = std::visit (
                  [] (const auto &route) {
                      return routing_id_string (route.target_node_routing_id);
                  },
                  notice.source);
                const auto target = std::visit (
                  [] (const auto &route) {
                      return routing_id_string (route.target_node_routing_id);
                  },
                  notice.target);
                sink.append (
                  "message-follow-received|source-node=" + source
                  + "|target-node=" + target
                  + "|operation-low="
                  + std::to_string (notice.original_operation.low));
                received = true;
            }
            (void) owner.mailbox ().release (*claim);
        }

        const auto peer = owner.topology ().peer (peer_rid);
        if (!sent && received && peer) {
            sent = owner
                     .send_message_follow (
                       peer_rid,
                       make_message_follow_notice (
                         owner.topology ().local_descriptor (),
                         peer->descriptor, 101))
                     .result ()
                     .value ();
        }
        if (sent && received) {
            std::this_thread::sleep_for (std::chrono::seconds (2));
            return 0;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    throw std::runtime_error (
      "message-follow process exchange timed out (sent="
      + std::string (sent ? "true" : "false")
      + ", received=" + std::string (received ? "true" : "false") + ")");
}

/* Channel server handlers: mirror TestHostProfileRequestHandler/SendHandler so
 * a peer-language client sees the same markers it sees against its own host. */
class profile_request_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<event_sink_t>;
    using request_type = test_host_profile_request_t;
    using reply_type = test_host_profile_reply_t;

    explicit profile_request_handler_t (event_sink_t &sink) : _sink (sink) {}

    test_host_profile_reply_t handle (const test_host_profile_request_t &request)
    {
        _sink.append ("channel-server-request|" + request.value);
        return test_host_profile_reply_t{request.value};
    }

  private:
    event_sink_t &_sink;
};

class profile_send_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<event_sink_t>;
    using message_type = test_host_profile_send_t;

    explicit profile_send_handler_t (event_sink_t &sink) : _sink (sink) {}

    void handle (const test_host_profile_send_t &message)
    {
        _sink.append ("channel-server-send|" + message.value);
    }

  private:
    event_sink_t &_sink;
};

/* Fanout subscriber: records "<topic>:<value>", the marker the peer hosts write. */
class published_event_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<event_sink_t>;
    using event_type = test_host_published_event_t;
    static constexpr const char *topic_name = "profile.changed";

    explicit published_event_handler_t (event_sink_t &sink) : _sink (sink) {}

    void handle (
      const test_host_published_event_t &event,
      const fw::publish_message_context_t &context)
    {
        _sink.append (std::string (context.topic) + ":" + event.value);
    }

  private:
    event_sink_t &_sink;
};

/* Spot route wire host handlers: (a) echo with the host language tag and
 * (c) an application failure with a typed framework kind. (b) needs no
 * handler — the missing packet is the scenario. */
class spot_route_request_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<event_sink_t>;
    using request_type = test_host_spot_route_request_t;
    using reply_type = test_host_spot_route_reply_t;

    explicit spot_route_request_handler_t (event_sink_t &sink) : _sink (sink) {}

    test_host_spot_route_reply_t handle (const test_host_spot_route_request_t &request,
                                         const fw::route_message_context_t &context)
    {
        _sink.append ("spot-route-server|" + request.value + "|"
                      + context.source_node_rid.to_string ());
        return test_host_spot_route_reply_t{request.value + "|cpp"};
    }

  private:
    event_sink_t &_sink;
};

class spot_route_fail_handler_t
{
  public:
    using request_type = test_host_spot_route_fail_request_t;
    using reply_type = test_host_spot_route_reply_t;

    test_host_spot_route_reply_t handle (const test_host_spot_route_fail_request_t &request,
                                         const fw::route_message_context_t &context)
    {
        (void) request;
        (void) context;
        /* Application-origin failure with a typed kind: the wire must
         * preserve "rejected" and must NOT carry zlink.origin=framework. */
        throw fw::framework_exception_t (fw::framework_error_kind_t::rejected,
                                         "application spot route failure");
    }
};

/* Spot route wire client: runs the common (a)/(b)/(c) scenario against a peer
 * host and records the observed wire kind/origin markers for the runner. */
class spot_route_client_service_t final : public fw::hosted_service_t
{
  public:
    spot_route_client_service_t (std::string channel, std::string peer_rid, std::string value) :
        _channel (std::move (channel)),
        _peer_rid (std::move (peer_rid)),
        _value (std::move (value))
    {
    }

    void start (fw::service_provider_t &services) override
    {
        auto &routes = services.get_required<fw::route_client_t> ();
        auto &sink = services.get_required<event_sink_t> ();
        const auto target = zlink::routing_id_t::from (_peer_rid);

        /* (a) round trip; the peer handshake is asynchronous, so transient
         * unavailability is retried until the deadline. */
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (60);
        while (true) {
            auto reply = routes
                           .request_to_node (_channel, target,
                                             test_host_spot_route_request_t{_value})
                           .timeout (std::chrono::seconds (5))
                           .submit<test_host_spot_route_reply_t> ()
                           .result ();
            if (reply) {
                sink.append ("spot-route-reply|" + reply.value ().value);
                break;
            }
            const auto kind = reply.error_kind ();
            /* not_found before the first success is the local "target peer is
             * not in the route table yet" state while the handshake settles;
             * (b) below observes the remote handler-missing not_found only
             * after (a) proved the link. */
            if ((kind == fw::framework_error_kind_t::unavailable
                 || kind == fw::framework_error_kind_t::deadline_exceeded
                 || kind == fw::framework_error_kind_t::not_found)
                && std::chrono::steady_clock::now () < deadline) {
                std::this_thread::sleep_for (std::chrono::milliseconds (50));
                continue;
            }
            sink.append (std::string ("spot-route-error|") + error_kind_wire_name (kind) + "|"
                         + (reply.error () ? reply.error ()->what () : "request failed"));
            return;
        }

        /* (b) framework error: no host registers this packet. */
        record_failure (sink, routes, target, "spot-route-missing",
                        test_host_spot_route_missing_request_t{_value});
        /* (c) application handler failure with a typed kind. */
        record_failure (sink, routes, target, "spot-route-app-error",
                        test_host_spot_route_fail_request_t{_value});
        write_ready ();
    }

    void stop () noexcept override {}

  private:
    template <typename TRequest>
    void record_failure (event_sink_t &sink,
                         fw::route_client_t &routes,
                         const zlink::routing_id_t &target,
                         const std::string &marker,
                         TRequest request)
    {
        auto reply = routes.request_to_node (_channel, target, std::move (request))
                       .timeout (std::chrono::seconds (5))
                       .template submit<test_host_spot_route_reply_t> ()
                       .result ();
        if (reply) {
            sink.append (marker + "|unexpected-success");
            return;
        }
        sink.append (marker + "|kind=" + error_kind_wire_name (reply.error_kind ())
                     + "|origin="
                     + (reply.error () ? error_origin_wire_name (*reply.error ())
                                       : std::string ("unspecified")));
    }

    std::string _channel;
    std::string _peer_rid;
    std::string _value;
};

/* STREAM raw session: records the payload and replies with the request packet
 * name, as required by the request/reply wire contract. */
class raw_stream_session_t final : public fw::packet_stream_session_t
{
  public:
    using dependency_types = fw::dependency_list_t<event_sink_t>;

    explicit raw_stream_session_t (event_sink_t &sink) : _sink (sink) {}

    fw::task_t<void> on_connected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_disconnected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &) override { co_return; }

    fw::task_t<void> on_packet (fw::stream_t &stream,
                                const fw::session_message_context_t &dispatch,
                                const zlink::message_t &payload) override
    {
        _sink.append ("raw|" + std::string (dispatch.packet_name) + "|"
                      + payload.to_string ());
        stream.reply_packet (zlink::message_t::from_json (std::string ("pong"))).submit ();
        co_return;
    }

  private:
    event_sink_t &_sink;
};

/* Client-side one-shot work: a channel request/send or a fanout publish runs
 * once the host is up, so the peer server records the marker and the runner can
 * assert on this host's own event file too. */
class channel_client_service_t final : public fw::hosted_service_t
{
  public:
    channel_client_service_t (std::string channel, std::string value) :
        _channel (std::move (channel)), _value (std::move (value))
    {
    }

    void start (fw::service_provider_t &services) override
    {
        auto &client = services.get_required<fw::channel_client_t> ();
        auto &sink = services.get_required<event_sink_t> ();
        auto reply = client.request (_channel, test_host_profile_request_t{_value})
                       .timeout (std::chrono::seconds (5))
                       .submit<test_host_profile_reply_t> ()
                       .result ();
        if (!reply) {
            sink.append (std::string ("channel-client-error|")
                         + (reply.error () ? reply.error ()->what () : "request failed"));
            return;
        }
        sink.append ("channel-client-reply|" + reply.value ().value);
        client.send (_channel, test_host_profile_send_t{_value + "-send"}).submit ();
        sink.append ("channel-client-sent|" + _value + "-send");
        write_ready ();
    }

    void stop () noexcept override {}

  private:
    std::string _channel;
    std::string _value;
};

class fanout_publish_service_t final : public fw::hosted_service_t
{
  public:
    fanout_publish_service_t (std::string channel, std::string topic, std::string value) :
        _channel (std::move (channel)), _topic (std::move (topic)), _value (std::move (value))
    {
    }

    void start (fw::service_provider_t &services) override
    {
        auto &publisher = services.get_required<fw::publisher_t> ();
        auto &sink = services.get_required<event_sink_t> ();
        /* The subscriber connects asynchronously; a fanout publish before the
         * subscription lands is silently dropped by the wire, so the publisher
         * repeats until the runner sees the marker or the deadline passes. */
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (15);
        while (std::chrono::steady_clock::now () < deadline) {
            publisher.publish (_channel, _topic, test_host_published_event_t{_value}).submit ();
            std::this_thread::sleep_for (std::chrono::milliseconds (250));
        }
        sink.append ("channel-publisher-done|" + _topic + ":" + _value);
    }

    void stop () noexcept override {}

  private:
    std::string _channel;
    std::string _topic;
    std::string _value;
};

int run_stream_connector ()
{
    /* Connector against a peer-language STREAM server: proves the frame/codec
     * wire and, with --expect-close, the session-closing reason contract. */
    event_sink_t sink (option ("event-file"));
    zlink::stream_connector::connector_options_t options;
    options.endpoint = require ("stream-endpoint");
    options.connect_timeout = std::chrono::milliseconds (5000);
    options.request_timeout = std::chrono::milliseconds (5000);
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto client = zlink::stream_connector::connector_factory_t::create (options);
    if (!client.connect ()) {
        throw std::runtime_error ("stream connect failed");
    }

    std::mutex gate;
    std::optional<zlink::stream_connector::close_reason_t> observed;
    client.on_connection_state_changed (
      [&] (const zlink::stream_connector::connection_state_changed_t &event) {
          if (!event.close_reason) {
              return;
          }
          if (event.current == zlink::stream_connector::connection_state_t::disconnected
              || event.current == zlink::stream_connector::connection_state_t::closed) {
              const std::lock_guard<std::mutex> lock (gate);
              if (!observed) {
                  observed = event.close_reason;
              }
          }
      });

    /* The peer raw-stream sessions answer a "RawPing" packet whose payload is a
     * plain JSON string and reply with a JSON string, so the STREAM row does
     * not depend on a shared DTO. */
    const auto value = option ("value", "cpp-to-peer");
    auto reply = client.request (std::string (value))
                   .packet_name ("RawPing")
                   .timeout (std::chrono::milliseconds (5000))
                   .submit<std::string> ();
    if (!reply) {
        throw std::runtime_error (
          std::string ("stream request failed: ")
          + (reply.error () ? reply.error ()->message : std::string ("unknown error")));
    }
    sink.append ("connector-reply|" + reply.value ());
    write_ready ();

    if (option ("expect-close") == "true") {
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (30);
        while (std::chrono::steady_clock::now () < deadline) {
            (void) client.dispatch ();
            {
                const std::lock_guard<std::mutex> lock (gate);
                if (observed) {
                    break;
                }
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
        }
        const std::lock_guard<std::mutex> lock (gate);
        if (!observed) {
            throw std::runtime_error ("no close reason observed");
        }
        using reason_t = zlink::stream_connector::close_reason_t;
        const auto reason = *observed;
        sink.append (std::string ("connector-close|")
                     + (reason == reason_t::server_drain           ? "server_drain"
                        : reason == reason_t::idle_timeout         ? "idle_timeout"
                        : reason == reason_t::heartbeat_timeout    ? "heartbeat_timeout"
                        : reason == reason_t::client_close         ? "client_close"
                        : reason == reason_t::protocol_error       ? "protocol_error"
                                                                   : "transport_error"));
    }
    return 0;
}

} // namespace

int main (int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: cross_language_host <mode> [--option value ...]" << std::endl;
        return 2;
    }
    const std::string mode = argv[1];
    for (int index = 2; index + 1 < argc; index++) {
        const std::string argument = argv[index];
        if (argument.rfind ("--", 0) == 0) {
            host_args[argument.substr (2)] = argv[index + 1];
            index++;
        }
    }

    try {
        if (mode == "stream-connector") {
            return run_stream_connector ();
        }
        if (mode == "message-follow") {
            return run_message_follow_host ();
        }

        auto app = fw::app_t::create ();
        app.add_zlink_framework ([&] (fw::zlink_framework_options_t &options) {
            options.services ().add_singleton<event_sink_t> (
              std::make_unique<event_sink_t> (option ("event-file")));

            if (mode == "channel-server") {
                const auto endpoint = require ("server-endpoint");
                options.add_client_server_channel (require ("channel-name"))
                  .server ()
                  .set_bind_host (host_from_tcp_endpoint (endpoint))
                  .set_advertise_host (host_from_tcp_endpoint (endpoint))
                  .listen (port_from_tcp_endpoint (endpoint))
                  .add_handler_group ("cross-language");
                options.handlers ()
                  .group ("cross-language")
                  .add<profile_request_handler_t> ()
                  .add_send<profile_send_handler_t> ();
                return;
            }
            if (mode == "channel-client") {
                options.add_client_server_channel (require ("channel-name"))
                  .client ()
                  .connect (require ("server-endpoint"));
                return;
            }
            if (mode == "channel-publisher") {
                /* A manually-bound publisher without a routing identity
                 * runs on the native path and needs no Location Store. */
                options.add_fanout_channel (require ("channel-name"))
                  .enable_publisher (require ("publisher-endpoint"));
                return;
            }
            if (mode == "stream-server") {
                options.add_stream_node ("stream.raw")
                  .bind (require ("stream-endpoint"))
                  .register_session<raw_stream_session_t> ();
                return;
            }
            if (mode == "spot-route-server") {
                const auto channel = require ("channel-name");
                auto mesh = options.add_route_mesh (channel);
                /* Route-only mesh: no spot hosting, so no object role and no
                 * Location Store requirement. */
                mesh.set_object_role (fw::object_role_t::none);
                mesh.listen (require ("server-endpoint"));
                mesh.set_routing_id (
                  zlink::routing_id_t::from (option ("node-rid", "cpp-spot-route")));
                mesh.channel_name (channel).server ();
                mesh.add_route_request_handler<spot_route_request_handler_t,
                                               test_host_spot_route_request_t,
                                               test_host_spot_route_reply_t> (
                  test_host_spot_route_request_t::packet_name);
                mesh.add_route_request_handler<spot_route_fail_handler_t,
                                               test_host_spot_route_fail_request_t,
                                               test_host_spot_route_reply_t> (
                  test_host_spot_route_fail_request_t::packet_name);
                return;
            }
            if (mode == "spot-route-client") {
                const auto channel = require ("channel-name");
                auto mesh = options.add_route_mesh (channel);
                mesh.set_object_role (fw::object_role_t::none);
                mesh.listen (require ("bind-endpoint"));
                mesh.set_routing_id (
                  zlink::routing_id_t::from (option ("node-rid", "cpp-spot-route-client")));
                /* RegistryMessaging e2e convention: every route mesh member
                 * exposes the channel server role, and peers connect by
                 * endpoint; the routing id is learned from admission. */
                mesh.channel_name (channel).server ();
                mesh.peer_connections ().connect (require ("server-endpoint"));
                return;
            }
            if (mode == "channel-subscriber") {
                auto channel = options.add_fanout_channel (require ("channel-name"));
                channel.connect (require ("publisher-endpoint"));
                channel.use_handler_group ("cross-language");
                options.handlers ()
                  .group ("cross-language")
                  .add_publish<published_event_handler_t> ();
                return;
            }
            throw std::runtime_error ("unsupported mode '" + mode + "'");
        });

        if (mode == "channel-client") {
            app.add_hosted_service (std::make_unique<channel_client_service_t> (
              require ("channel-name"), option ("value", "cpp-to-peer")));
        }
        if (mode == "channel-publisher") {
            app.add_hosted_service (std::make_unique<fanout_publish_service_t> (
              require ("channel-name"), require ("topic"), option ("value", "cpp-publish")));
        }
        if (mode == "spot-route-client") {
            app.add_hosted_service (std::make_unique<spot_route_client_service_t> (
              require ("channel-name"), require ("peer-rid"),
              option ("value", "cpp-spot-route")));
        }
        if (mode == "channel-server" || mode == "channel-subscriber"
            || mode == "stream-server" || mode == "spot-route-server") {
            write_ready ();
        }
        return app.run (argc, argv);
    }
    catch (const std::exception &error) {
        std::cerr << "cross-language host failed: " << error.what () << std::endl;
        return 1;
    }
}

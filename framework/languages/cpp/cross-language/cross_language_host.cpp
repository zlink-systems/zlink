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
#include <zlink/locations/redis.hpp>
#include <zlink/stream_connector.hpp>
#include <zlink/stream_connector/codecs/auto_codec.hpp>

#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
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

/* ---------------------------------------------------------------------------
 * User-Spot JoinSpot scenario (spec 15-spot-actor.ko.md section 4.2 User Spot
 * admission; spec 51 section 9 canonical actorJoin command 28 as the Core
 * request leg with command 20 as its reply leg).
 *
 * Mirrors the .NET TestHost (user-spot-source / user-spot-target), the Node
 * user_spot_join_host.js and the Java Program.java modes one-for-one: an Actor
 * created through the source's local Entry Spot defers a joinSpot() into the
 * fixed User Spot owned by the foreign peer. This host never selects the
 * admission transport -- the runtime alone decides whether the leg travels as
 * canonical command 28.
 * ------------------------------------------------------------------------ */

constexpr const char *cross_lang_actor_type = "cross-lang-relocation-actor-type";
constexpr const char *cross_lang_user_spot_type = "cross-lang-relocation-user-spot-type";

/* Peer languages emit their DTO fields with their own naming policy (.NET
 * records are PascalCase, Node/Java are camelCase), so every reader accepts
 * both spellings -- the same rule read_value_field already applies to the
 * channel DTOs. Emission stays camelCase, the proven cpp -> all-peers
 * direction. */
const nlohmann::json *find_field (const nlohmann::json &json,
                                  const char *camel,
                                  const char *pascal)
{
    if (json.contains (camel)) {
        return &json.at (camel);
    }
    if (json.contains (pascal)) {
        return &json.at (pascal);
    }
    return nullptr;
}

std::string read_string (const nlohmann::json &json, const char *camel, const char *pascal)
{
    const auto *field = find_field (json, camel, pascal);
    return field == nullptr || field->is_null () ? std::string () : field->get<std::string> ();
}

int read_int (const nlohmann::json &json, const char *camel, const char *pascal)
{
    const auto *field = find_field (json, camel, pascal);
    return field == nullptr || field->is_null () ? 0 : field->get<int> ();
}

bool read_bool (const nlohmann::json &json, const char *camel, const char *pascal)
{
    const auto *field = find_field (json, camel, pascal);
    return field != nullptr && !field->is_null () && field->get<bool> ();
}

struct cross_lang_actor_create_req_t
{
    static constexpr const char *packet_name = "CrossLangActorCreateReq";
    int state_version = 0;
    int application_state_bytes = 0;
};

struct user_spot_create_req_t
{
    static constexpr const char *packet_name = "UserSpotCreateReq";
    std::string marker;
};

struct begin_user_spot_join_req_t
{
    static constexpr const char *packet_name = "BeginUserSpotJoinReq";
    std::string target_spot_id;
    std::string marker;
};

/* Crosses the wire inside canonical command 28's application payload. */
struct user_spot_join_req_t
{
    static constexpr const char *packet_name = "UserSpotJoinReq";
    std::string marker;
};

/* Admission reply; travels back on the command-20 reply leg. */
struct user_spot_join_res_t
{
    static constexpr const char *packet_name = "UserSpotJoinRes";
    bool accepted = false;
    std::string actor_id;
    std::string spot_id;
    std::string node_rid;
    std::string marker;
};

struct user_spot_probe_req_t
{
    static constexpr const char *packet_name = "UserSpotProbeReq";
    std::string marker;
};

struct user_spot_probe_res_t
{
    static constexpr const char *packet_name = "UserSpotProbeRes";
    std::string actor_id;
    std::string spot_id;
    std::string node_rid;
    int state_version = 0;
    std::string marker;
};

/* Reciprocal-discovery probe the target sends to the source node. */
struct user_spot_discovery_probe_req_t
{
    static constexpr const char *packet_name = "UserSpotDiscoveryProbeReq";
    std::string marker;
};

struct user_spot_discovery_probe_res_t
{
    static constexpr const char *packet_name = "UserSpotDiscoveryProbeRes";
    std::string marker;
    std::string node_rid;
};

inline void to_json (nlohmann::json &json, const cross_lang_actor_create_req_t &value)
{
    json = nlohmann::json{{"stateVersion", value.state_version},
                          {"applicationStateBytes", value.application_state_bytes}};
}
inline void from_json (const nlohmann::json &json, cross_lang_actor_create_req_t &value)
{
    value.state_version = read_int (json, "stateVersion", "StateVersion");
    value.application_state_bytes = read_int (json, "applicationStateBytes",
                                              "ApplicationStateBytes");
}
inline void to_json (nlohmann::json &json, const user_spot_create_req_t &value)
{
    json = nlohmann::json{{"marker", value.marker}};
}
inline void from_json (const nlohmann::json &json, user_spot_create_req_t &value)
{
    value.marker = read_string (json, "marker", "Marker");
}
inline void to_json (nlohmann::json &json, const begin_user_spot_join_req_t &value)
{
    json = nlohmann::json{{"targetSpotId", value.target_spot_id}, {"marker", value.marker}};
}
inline void from_json (const nlohmann::json &json, begin_user_spot_join_req_t &value)
{
    value.target_spot_id = read_string (json, "targetSpotId", "TargetSpotId");
    value.marker = read_string (json, "marker", "Marker");
}
inline void to_json (nlohmann::json &json, const user_spot_join_req_t &value)
{
    json = nlohmann::json{{"marker", value.marker}};
}
inline void from_json (const nlohmann::json &json, user_spot_join_req_t &value)
{
    value.marker = read_string (json, "marker", "Marker");
}
inline void to_json (nlohmann::json &json, const user_spot_join_res_t &value)
{
    json = nlohmann::json{{"accepted", value.accepted},
                          {"actorId", value.actor_id},
                          {"spotId", value.spot_id},
                          {"nodeRid", value.node_rid},
                          {"marker", value.marker}};
}
inline void from_json (const nlohmann::json &json, user_spot_join_res_t &value)
{
    value.accepted = read_bool (json, "accepted", "Accepted");
    value.actor_id = read_string (json, "actorId", "ActorId");
    /* The Node source's local BeginUserSpotJoinReq reply names the field
     * targetSpotId; accept that spelling too so one DTO covers both. */
    value.spot_id = read_string (json, "spotId", "SpotId");
    if (value.spot_id.empty ()) {
        value.spot_id = read_string (json, "targetSpotId", "TargetSpotId");
    }
    value.node_rid = read_string (json, "nodeRid", "NodeRid");
    value.marker = read_string (json, "marker", "Marker");
}
inline void to_json (nlohmann::json &json, const user_spot_probe_req_t &value)
{
    json = nlohmann::json{{"marker", value.marker}};
}
inline void from_json (const nlohmann::json &json, user_spot_probe_req_t &value)
{
    value.marker = read_string (json, "marker", "Marker");
}
inline void to_json (nlohmann::json &json, const user_spot_probe_res_t &value)
{
    json = nlohmann::json{{"actorId", value.actor_id},
                          {"spotId", value.spot_id},
                          {"nodeRid", value.node_rid},
                          {"stateVersion", value.state_version},
                          {"marker", value.marker}};
}
inline void from_json (const nlohmann::json &json, user_spot_probe_res_t &value)
{
    value.actor_id = read_string (json, "actorId", "ActorId");
    value.spot_id = read_string (json, "spotId", "SpotId");
    value.node_rid = read_string (json, "nodeRid", "NodeRid");
    value.state_version = read_int (json, "stateVersion", "StateVersion");
    value.marker = read_string (json, "marker", "Marker");
}
inline void to_json (nlohmann::json &json, const user_spot_discovery_probe_req_t &value)
{
    json = nlohmann::json{{"marker", value.marker}};
}
inline void from_json (const nlohmann::json &json, user_spot_discovery_probe_req_t &value)
{
    value.marker = read_string (json, "marker", "Marker");
}
inline void to_json (nlohmann::json &json, const user_spot_discovery_probe_res_t &value)
{
    json = nlohmann::json{{"marker", value.marker}, {"nodeRid", value.node_rid}};
}
inline void from_json (const nlohmann::json &json, user_spot_discovery_probe_res_t &value)
{
    value.marker = read_string (json, "marker", "Marker");
    value.node_rid = read_string (json, "nodeRid", "NodeRid");
}

/* Spot/Actor instances are built by framework factories that receive only a
 * context, so the event sink and this node's own routing id travel through
 * file-scope pointers -- the same convention the C++ e2e relocation node
 * (e2e/DiscoveryRegistryHa/Server/Relocation/main.cpp) uses. */
event_sink_t *g_user_spot_sink = nullptr;
std::string g_user_spot_node_rid;

void user_spot_event (const std::string &line)
{
    if (g_user_spot_sink != nullptr) {
        g_user_spot_sink->append (line);
    }
}

/* Signals the target-side probe loop that the foreign Actor finished joining
 * the fixed User Spot (mirrors the .NET/Java UserSpotJoinObserver). */
class user_spot_join_observer_t
{
  public:
    void complete ()
    {
        {
            const std::lock_guard<std::mutex> lock (_mutex);
            _joined = true;
        }
        _changed.notify_all ();
    }

    bool wait (std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock (_mutex);
        return _changed.wait_for (lock, timeout, [&] { return _joined; });
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    bool _joined = false;
};

user_spot_join_observer_t g_user_spot_join_observer;

class relocation_actor_t final : public fw::actor_t
{
  public:
    explicit relocation_actor_t (fw::actor_context_t context) : _context (std::move (context)) {}

    fw::actor_context_t &context () noexcept override { return _context; }
    const fw::actor_context_t &context () const noexcept override { return _context; }

    std::string actor_id;
    int state_version = 0;
    std::vector<std::uint8_t> application_state;

    /* Terminal fate of a deferred joinSpot(): the only place a canonical
     * actorJoin failure surfaces on the source side. */
    fw::task_t<void> on_join_completed (const fw::actor_join_completion_t &completion) override
    {
        if (std::get_if<fw::actor_join_accepted_t> (&completion) != nullptr) {
            user_spot_event ("user-spot-join-completed|status=accepted|actor=" + actor_id);
        } else if (std::get_if<fw::actor_join_rejected_t> (&completion) != nullptr) {
            user_spot_event ("user-spot-join-completed|status=rejected|actor=" + actor_id);
        } else {
            const auto &failed = std::get<fw::actor_join_failed_t> (completion);
            user_spot_event ("user-spot-join-failed|kind="
                             + error_kind_wire_name (failed.error_kind) + "|actor=" + actor_id);
        }
        co_return;
    }

  private:
    fw::actor_context_t _context;
};

class relocation_actor_factory_t final : public fw::actor_factory_t<relocation_actor_t>
{
  public:
    fw::task_t<std::shared_ptr<relocation_actor_t>> create (fw::actor_context_t context,
                                                            std::stop_token) override
    {
        auto actor = std::make_shared<relocation_actor_t> (std::move (context));
        actor->actor_id = std::string (actor->context ().actor_id ().value ());
        co_return actor;
    }
};

/* capture()/restore() so the User-Spot join preserves stateVersion and the
 * application-state payload. The cross-language fixture pins a little-endian
 * two-int header before the opaque state bytes -- byte-for-byte the shape the
 * Java RelocationActorAdapter and the Node DataView adapter already use. */
class relocation_actor_adapter_t final : public fw::actor_relocation_adapter_t<relocation_actor_t>
{
  public:
    fw::task_t<std::vector<std::byte>> capture (relocation_actor_t &actor, std::stop_token) override
    {
        const auto length = static_cast<std::int32_t> (actor.application_state.size ());
        const auto version = static_cast<std::int32_t> (actor.state_version);
        std::vector<std::byte> encoded (sizeof (std::int32_t) * 2 + actor.application_state.size ());
        write_le_int32 (encoded.data (), version);
        write_le_int32 (encoded.data () + sizeof (std::int32_t), length);
        for (std::size_t index = 0; index < actor.application_state.size (); index++) {
            encoded[sizeof (std::int32_t) * 2 + index] =
              static_cast<std::byte> (actor.application_state[index]);
        }
        co_return encoded;
    }

    fw::task_t<void>
    restore (relocation_actor_t &actor, std::vector<std::byte> payload, std::stop_token) override
    {
        if (payload.size () < sizeof (std::int32_t) * 2) {
            throw std::runtime_error ("relocation binary payload header is truncated");
        }
        const auto version = read_le_int32 (payload.data ());
        const auto length = read_le_int32 (payload.data () + sizeof (std::int32_t));
        if (length < 0
            || payload.size () != sizeof (std::int32_t) * 2 + static_cast<std::size_t> (length)) {
            throw std::runtime_error ("relocation binary application state size is invalid");
        }
        actor.state_version = version;
        actor.application_state.clear ();
        actor.application_state.reserve (static_cast<std::size_t> (length));
        for (std::int32_t index = 0; index < length; index++) {
            actor.application_state.push_back (
              static_cast<std::uint8_t> (payload[sizeof (std::int32_t) * 2 + index]));
        }
        co_return;
    }

  private:
    static void write_le_int32 (std::byte *target, std::int32_t value)
    {
        const auto raw = static_cast<std::uint32_t> (value);
        target[0] = static_cast<std::byte> (raw & 0xffU);
        target[1] = static_cast<std::byte> ((raw >> 8) & 0xffU);
        target[2] = static_cast<std::byte> ((raw >> 16) & 0xffU);
        target[3] = static_cast<std::byte> ((raw >> 24) & 0xffU);
    }

    static std::int32_t read_le_int32 (const std::byte *source)
    {
        const auto raw = static_cast<std::uint32_t> (source[0])
                         | (static_cast<std::uint32_t> (source[1]) << 8)
                         | (static_cast<std::uint32_t> (source[2]) << 16)
                         | (static_cast<std::uint32_t> (source[3]) << 24);
        return static_cast<std::int32_t> (raw);
    }
};

/* Source-side Entry Spot: onCreateActor is the JoinEntrySpot admission-free
 * placement (spec 15:489). It both starts the deferred join and answers the
 * target's owner probe while the Actor still lives here. */
class relocation_entry_spot_t final : public fw::entry_spot_t<relocation_actor_t>
{
  public:
    explicit relocation_entry_spot_t (fw::entry_spot_context_t context) :
        _context (std::move (context))
    {
    }

    fw::entry_spot_context_t &context () noexcept override { return _context; }
    const fw::entry_spot_context_t &context () const noexcept override { return _context; }

    void configure () override
    {
        _context.handlers ().add_actor_request<&relocation_entry_spot_t::begin_join> (
          begin_user_spot_join_req_t::packet_name);
        _context.handlers ().add_actor_request<&relocation_entry_spot_t::probe> (
          user_spot_probe_req_t::packet_name);
    }

    fw::task_t<fw::actor_create_response_t>
    on_create_actor (relocation_actor_t &actor, const fw::message_t &create_request) override
    {
        if (!create_request.empty ()) {
            const auto request = create_request.decode<cross_lang_actor_create_req_t> ();
            actor.state_version = request.state_version;
            const auto length =
              static_cast<std::size_t> (std::max (0, request.application_state_bytes));
            actor.application_state.assign (length, 0);
            for (std::size_t index = 0; index < length; index++) {
                actor.application_state[index] = static_cast<std::uint8_t> (index % 251);
            }
        }
        co_return fw::actor_create_response_t::accept ();
    }

    fw::task_t<fw::spot_actor_join_result_t> on_actor_join (std::string_view actor_id,
                                                            const fw::message_t &request) override
    {
        (void) actor_id;
        co_return fw::spot_actor_join_result_t::accept (request);
    }

    fw::task_t<void> on_actor_joined (relocation_actor_t &) override { co_return; }
    fw::task_t<void> on_leave_actor (relocation_actor_t &) override { co_return; }

    /* The runtime alone selects the canonical actorJoin (command 28)
     * transport when the observed Spot route and the admitted peer's
     * service-wire capability allow it (spec 51 section 9). */
    user_spot_join_res_t begin_join (relocation_actor_t &actor,
                                     fw::message_context_t &,
                                     const begin_user_spot_join_req_t &request)
    {
        user_spot_event ("user-spot-join-deferred|actor=" + actor.actor_id + "|spot="
                         + request.target_spot_id);
        actor.context ()
          .join_spot (request.target_spot_id, user_spot_join_req_t{request.marker})
          .timeout (std::chrono::seconds (30))
          .defer ();
        return user_spot_join_res_t{true, actor.actor_id, request.target_spot_id,
                                    std::string (_context.node_rid ().value ()), request.marker};
    }

    /* Explicit "still on the source" reply instead of a NOT_FOUND retry storm
     * (mirrors Node's SourceProbeHandler and Java's
     * SourceUserSpotProbeHandler). */
    user_spot_probe_res_t probe (const relocation_actor_t &actor,
                                 fw::message_context_t &,
                                 const user_spot_probe_req_t &request)
    {
        return user_spot_probe_res_t{actor.actor_id, std::string (),
                                     std::string (_context.node_rid ().value ()),
                                     actor.state_version, request.marker};
    }

  private:
    fw::entry_spot_context_t _context;
};

/* Fixed target User Spot: a foreign Actor arrives through the canonical
 * actorJoin admission leg, on_actor_join admits it, on_actor_joined records
 * the completed membership. */
class relocation_user_spot_t final : public fw::spot_t<relocation_actor_t>
{
  public:
    explicit relocation_user_spot_t (fw::spot_context_t context) : _context (std::move (context)) {}

    fw::spot_context_t &context () noexcept override { return _context; }
    const fw::spot_context_t &context () const noexcept override { return _context; }

    void configure () override
    {
        _context.handlers ().add_actor_request<&relocation_user_spot_t::probe> (
          user_spot_probe_req_t::packet_name);
    }

    fw::task_t<fw::spot_create_response_t> on_create (const fw::message_t &request) override
    {
        if (!request.empty ()) {
            (void) request.decode<user_spot_create_req_t> ();
        }
        co_return fw::spot_create_response_t::accept ();
    }

    fw::task_t<fw::spot_actor_join_result_t> on_actor_join (std::string_view actor_id,
                                                            const fw::message_t &request) override
    {
        std::string marker;
        if (!request.empty ()) {
            try {
                marker = request.decode<user_spot_join_req_t> ().marker;
            }
            catch (const std::exception &error) {
                /* The admission payload is the foreign peer's application
                 * packet; record a decode failure as evidence rather than
                 * swallowing it, but still admit so the lifecycle assertion
                 * can distinguish "payload shape" from "admission refused". */
                user_spot_event ("user-spot-admission-decode-failed|actor="
                                 + std::string (actor_id) + "|error=" + error.what ());
                marker = "undecoded";
            }
        }
        user_spot_event ("user-spot-admission|accepted=true|actor=" + std::string (actor_id)
                         + "|spot=" + _context.spot_id ());
        co_return fw::spot_actor_join_result_t::accept (
          user_spot_join_res_t{true, std::string (actor_id), _context.spot_id (),
                               std::string (_context.node_rid ().value ()), marker});
    }

    fw::task_t<void> on_actor_joined (relocation_actor_t &actor) override
    {
        user_spot_event ("user-spot-joined|actor=" + actor.actor_id + "|spot="
                         + _context.spot_id () + "|nodeRid="
                         + std::string (_context.node_rid ().value ()));
        g_user_spot_join_observer.complete ();
        co_return;
    }

    fw::task_t<void> on_leave_actor (relocation_actor_t &) override { co_return; }

    user_spot_probe_res_t probe (const relocation_actor_t &actor,
                                 fw::message_context_t &,
                                 const user_spot_probe_req_t &request)
    {
        const auto node_rid = std::string (_context.node_rid ().value ());
        user_spot_event ("user-spot-probe-served|nodeRid=" + node_rid + "|actor="
                         + actor.actor_id);
        return user_spot_probe_res_t{actor.actor_id, _context.spot_id (), node_rid,
                                     actor.state_version, request.marker};
    }

  private:
    fw::spot_context_t _context;
};

/* Reciprocal-discovery probe: answering with this node's own routing id proves
 * the peer edge is admitted in both directions before the join runs. */
class user_spot_discovery_probe_handler_t
{
  public:
    using request_type = user_spot_discovery_probe_req_t;
    using reply_type = user_spot_discovery_probe_res_t;

    user_spot_discovery_probe_res_t handle (const user_spot_discovery_probe_req_t &request,
                                            const fw::route_message_context_t &context)
    {
        (void) context;
        return user_spot_discovery_probe_res_t{request.marker, g_user_spot_node_rid};
    }
};

/* Target role: create the fixed User Spot before this node is ready to host
 * anything else, drop the placement weight to zero so the source's Entry-Spot
 * Actor placement can never pick it, then run the reciprocal-discovery probe
 * and the post-join owner probe as background loops. */
class user_spot_target_service_t final : public fw::hosted_service_t
{
  public:
    user_spot_target_service_t (std::string mesh_name,
                                std::string spot_id,
                                std::string actor_id,
                                std::string source_node_rid) :
        _mesh_name (std::move (mesh_name)),
        _spot_id (std::move (spot_id)),
        _actor_id (std::move (actor_id)),
        _source_node_rid (std::move (source_node_rid))
    {
    }

    void start (fw::service_provider_t &services) override
    {
        auto &spots = services.get_required<fw::spot_manager_t> ();
        auto &sink = services.get_required<event_sink_t> ();
        auto &runtime_options = services.get_required<fw::route_mesh_runtime_options_t> ();
        auto &routes = services.get_required<fw::route_client_t> ();
        auto &mesh_runtime = services.get_required<fw::route_mesh_runtime_t> ();
        auto &actors = services.get_required<fw::actor_client_t> ();

        /* Spot creation runs through the Location Store, which needs the mesh
         * node to have finished its own registration; retry rather than fail
         * the whole mode on an early attempt. */
        std::string target_node_rid;
        std::string state_name;
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (60);
        while (true) {
            auto created = spots.get_or_create (_spot_id, cross_lang_user_spot_type)
                             .in_mesh (_mesh_name)
                             .creation_request (
                               user_spot_create_req_t{"cross-language-user-spot"})
                             .timeout (std::chrono::seconds (15))
                             .submit ()
                             .result ();
            if (created) {
                target_node_rid = std::string (created.value ().spot.node_rid ().value ());
                state_name = created.value ().state == fw::spot_create_state_t::created
                               ? "created"
                               : created.value ().state == fw::spot_create_state_t::existing
                                   ? "existing"
                                   : "rejected";
                break;
            }
            if (std::chrono::steady_clock::now () >= deadline) {
                sink.append (std::string ("user-spot-target-error|kind=")
                             + error_kind_wire_name (created.error_kind ()) + "|"
                             + (created.error () ? created.error ()->what () : "spot create failed"));
                return;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (250));
        }

        /* The fixed target Spot exists now. Exclude this node from the source
         * Actor's Entry-Spot placement so the join is a real cross-node
         * admission rather than a local self-join. */
        runtime_options.placement_weight (0);
        sink.append ("user-spot-created|spot=" + _spot_id + "|nodeRid=" + target_node_rid
                     + "|state=" + state_name);
        write_ready ();

        if (!_source_node_rid.empty ()) {
            _discovery = std::thread ([this, &sink, &routes, &mesh_runtime] {
                observe_source_peer (sink, routes, mesh_runtime);
            });
        }
        _probe = std::thread ([this, &sink, &actors, target_node_rid] {
            probe_joined_actor (sink, actors, target_node_rid);
        });
    }

    void stop () noexcept override
    {
        _stopping.store (true);
        if (_discovery.joinable ()) {
            _discovery.join ();
        }
        if (_probe.joinable ()) {
            _probe.join ();
        }
    }

  private:
    void observe_source_peer (event_sink_t &sink,
                              fw::route_client_t &routes,
                              fw::route_mesh_runtime_t &mesh_runtime)
    {
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (120);
        while (!_stopping.load () && std::chrono::steady_clock::now () < deadline) {
            const auto snapshot = mesh_runtime.snapshot (_mesh_name);
            if (snapshot.ready_peer_count > 0) {
                auto reply = routes
                               .request_to_node (
                                 _mesh_name, zlink::routing_id_t::from (_source_node_rid),
                                 user_spot_discovery_probe_req_t{"reciprocal-discovery"})
                               .timeout (std::chrono::seconds (2))
                               .submit<user_spot_discovery_probe_res_t> ()
                               .result ();
                if (reply && reply.value ().node_rid == _source_node_rid) {
                    sink.append ("user-spot-source-peer-ready|ready=true|peers="
                                 + std::to_string (snapshot.ready_peer_count));
                    return;
                }
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
        }
        if (!_stopping.load ()) {
            sink.append ("user-spot-source-peer-ready|ready=false|reason=probe-timeout");
        }
    }

    void probe_joined_actor (event_sink_t &sink,
                             fw::actor_client_t &actors,
                             const std::string &target_node_rid)
    {
        if (!g_user_spot_join_observer.wait (std::chrono::seconds (75))) {
            if (!_stopping.load ()) {
                sink.append ("user-spot-probe-timeout|targetRid=" + target_node_rid
                             + "|failure=join-not-observed");
            }
            return;
        }
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (90);
        std::string last_failure = "none";
        while (!_stopping.load () && std::chrono::steady_clock::now () < deadline) {
            auto reply = actors
                           .request (fw::actor_id_t (_actor_id),
                                     user_spot_probe_req_t{"target-owner-probe"})
                           .timeout (std::chrono::seconds (5))
                           .submit<user_spot_probe_res_t> ()
                           .result ();
            if (reply && reply.value ().node_rid == target_node_rid) {
                sink.append ("user-spot-probe|nodeRid=" + reply.value ().node_rid
                             + "|targetRid=" + target_node_rid + "|actor="
                             + reply.value ().actor_id + "|stateVersion="
                             + std::to_string (reply.value ().state_version));
                return;
            }
            last_failure = reply ? "unexpected-owner:" + reply.value ().node_rid
                                 : error_kind_wire_name (reply.error_kind ());
            std::this_thread::sleep_for (std::chrono::milliseconds (500));
        }
        if (!_stopping.load ()) {
            sink.append ("user-spot-probe-timeout|targetRid=" + target_node_rid + "|failure="
                         + last_failure);
        }
    }

    std::string _mesh_name;
    std::string _spot_id;
    std::string _actor_id;
    std::string _source_node_rid;
    std::atomic<bool> _stopping{false};
    std::thread _discovery;
    std::thread _probe;
};

/* Source role: create the Actor through the local Entry Spot (placement weight
 * keeps it here), then ask it to defer a joinSpot() into the foreign target
 * User Spot. */
class user_spot_source_service_t final : public fw::hosted_service_t
{
  public:
    user_spot_source_service_t (std::string mesh_name,
                                std::string spot_id,
                                std::string actor_id,
                                std::string start_file) :
        _mesh_name (std::move (mesh_name)),
        _spot_id (std::move (spot_id)),
        _actor_id (std::move (actor_id)),
        _start_file (std::move (start_file))
    {
    }

    void start (fw::service_provider_t &services) override
    {
        auto &sink = services.get_required<event_sink_t> ();
        auto &actor_manager = services.get_required<fw::actor_manager_t> ();
        auto &actors = services.get_required<fw::actor_client_t> ();
        auto &mesh_runtime = services.get_required<fw::route_mesh_runtime_t> ();
        write_ready ();
        _worker = std::thread ([this, &sink, &actor_manager, &actors, &mesh_runtime] {
            run (sink, actor_manager, actors, mesh_runtime);
        });
    }

    void stop () noexcept override
    {
        _stopping.store (true);
        if (_worker.joinable ()) {
            _worker.join ();
        }
    }

  private:
    void run (event_sink_t &sink,
              fw::actor_manager_t &actor_manager,
              fw::actor_client_t &actors,
              fw::route_mesh_runtime_t &mesh_runtime)
    {
        const auto discovery_deadline =
          std::chrono::steady_clock::now () + std::chrono::seconds (60);
        while (mesh_runtime.snapshot (_mesh_name).ready_peer_count == 0) {
            if (_stopping.load ()) {
                return;
            }
            if (std::chrono::steady_clock::now () >= discovery_deadline) {
                sink.append ("user-spot-source-peer-ready|ready=false|reason=discovery-timeout");
                return;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
        }
        sink.append ("user-spot-source-peer-ready|ready=true");

        /* Reciprocal barrier: the harness touches this file once BOTH sides
         * have observed the peer, matching the Node and Java sources. */
        if (!_start_file.empty ()) {
            const auto start_deadline =
              std::chrono::steady_clock::now () + std::chrono::seconds (60);
            while (!std::ifstream (_start_file).good ()) {
                if (_stopping.load ()) {
                    return;
                }
                if (std::chrono::steady_clock::now () >= start_deadline) {
                    sink.append ("user-spot-source-start-timeout|file=" + _start_file);
                    return;
                }
                std::this_thread::sleep_for (std::chrono::milliseconds (25));
            }
        }

        auto created = actor_manager.get_or_create (fw::actor_id_t (_actor_id), cross_lang_actor_type)
                         .in_mesh (_mesh_name)
                         .creation_request (cross_lang_actor_create_req_t{7, 4})
                         .timeout (std::chrono::seconds (15))
                         .submit ()
                         .result ();
        if (!created) {
            sink.append (std::string ("user-spot-source-error|kind=")
                         + error_kind_wire_name (created.error_kind ()) + "|"
                         + (created.error () ? created.error ()->what () : "actor create failed"));
            return;
        }
        const auto status = std::visit (
          [] (const auto &value) -> std::string {
              using value_t = std::decay_t<decltype (value)>;
              if constexpr (std::is_same_v<value_t, fw::actor_create_created_t>) {
                  return "created";
              } else if constexpr (std::is_same_v<value_t, fw::actor_create_existing_t>) {
                  return "existing";
              } else {
                  return "rejected";
              }
          },
          created.value ());
        const auto owner_node = std::visit (
          [] (const auto &value) -> std::string {
              using value_t = std::decay_t<decltype (value)>;
              if constexpr (std::is_same_v<value_t, fw::actor_create_rejected_t>) {
                  return "none";
              } else {
                  return std::string (value.actor.node_rid ().value ());
              }
          },
          created.value ());
        sink.append ("user-spot-source-actor-created|status=" + status + "|node=" + owner_node);

        auto reply = actors
                       .request (fw::actor_id_t (_actor_id),
                                 begin_user_spot_join_req_t{_spot_id, "canonical-28"})
                       .timeout (std::chrono::seconds (45))
                       .submit<user_spot_join_res_t> ()
                       .result ();
        if (!reply) {
            sink.append (std::string ("user-spot-source-error|kind=")
                         + error_kind_wire_name (reply.error_kind ()) + "|"
                         + (reply.error () ? reply.error ()->what () : "begin join failed"));
            return;
        }
        sink.append (std::string ("user-spot-join-request-reply|accepted=")
                     + (reply.value ().accepted ? "true" : "false") + "|actor="
                     + reply.value ().actor_id + "|spot=" + reply.value ().spot_id);
    }

    std::string _mesh_name;
    std::string _spot_id;
    std::string _actor_id;
    std::string _start_file;
    std::atomic<bool> _stopping{false};
    std::thread _worker;
};

/* Shared Redis Location/Relocation Store wiring: the key-prefix suffixes must
 * match the peer hosts byte-for-byte or the two nodes never see each other. */
void configure_user_spot_stores (fw::zlink_framework_options_t &options)
{
    const auto endpoint = require ("redis-endpoint");
    const auto prefix = option ("redis-key-prefix", "zlink-cross-user-spot-join");
    options.add_location_store (std::make_shared<fw::redis::redis_location_store_t> (
      fw::redis::redis_location_options_t{.connection_string = endpoint,
                                          .key_prefix = prefix + ":location"}));
    options.add_relocation_store (std::make_shared<fw::redis::redis_relocation_store_t> (
      fw::redis::redis_relocation_options_t{.connection_string = endpoint,
                                            .key_prefix = prefix + ":relocation"}));
}

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
            auto sink = std::make_unique<event_sink_t> (option ("event-file"));
            g_user_spot_sink = sink.get ();
            options.services ().add_singleton<event_sink_t> (std::move (sink));

            if (mode == "user-spot-source" || mode == "user-spot-target") {
                /* User-Spot JoinSpot (spec 15 section 4.2): an Actor created
                 * through the local Entry Spot joins a fixed User Spot owned
                 * by the foreign peer. Object-role mesh (spots are hosted
                 * here), so a Location Store is mandatory. */
                const auto target = mode == "user-spot-target";
                const auto mesh_name = require ("mesh-name");
                g_user_spot_node_rid = require ("node-rid");
                options.configure_dispatch ().message_flow (
                  fw::message_flow_log_mode_t::normal);
                configure_user_spot_stores (options);

                auto mesh = options.add_route_mesh (mesh_name);
                mesh.listen (require ("bind-endpoint"))
                  .set_routing_id (zlink::routing_id_t::from (g_user_spot_node_rid))
                  /* The target drops to zero once its fixed Spot exists, so
                   * the source always wins the Actor's initial placement. */
                  .set_placement_weight (100);
                mesh.channel_name (mesh_name).server ();
                mesh.add_route_request_handler<user_spot_discovery_probe_handler_t,
                                               user_spot_discovery_probe_req_t,
                                               user_spot_discovery_probe_res_t> (
                  user_spot_discovery_probe_req_t::packet_name);
                if (target) {
                    /* Same shape as the .NET and Node targets: a Spot factory
                     * and the Actor factory (so the arriving Actor can be
                     * materialized here), but no Entry Spot -- this node must
                     * not be an Entry-Spot placement candidate at all. */
                    mesh.add_spot_factory<relocation_user_spot_t> (
                      cross_lang_user_spot_type,
                      [] (fw::spot_context_t context) {
                          return std::make_shared<relocation_user_spot_t> (std::move (context));
                      },
                      [] (auto &factory) { factory.disable_relocation (); });
                } else {
                    mesh.add_entry_spot<relocation_entry_spot_t> (
                      [] (fw::entry_spot_context_t context) {
                          return std::make_shared<relocation_entry_spot_t> (std::move (context));
                      });
                }
                mesh.add_actor_factory<relocation_actor_t, relocation_actor_factory_t> (
                  cross_lang_actor_type, std::make_shared<relocation_actor_factory_t> (),
                  [] (auto &factory) {
                      factory.template preserve_state_with<relocation_actor_adapter_t> ();
                  });
                return;
            }
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
        if (mode == "user-spot-target") {
            /* Ready is written by the service itself, after the fixed User
             * Spot exists and the placement weight has dropped to zero. */
            app.add_hosted_service (std::make_unique<user_spot_target_service_t> (
              require ("mesh-name"), require ("spot-id"),
              option ("actor-id", "cross-lang-user-spot-actor"), option ("peer-rid")));
        }
        if (mode == "user-spot-source") {
            app.add_hosted_service (std::make_unique<user_spot_source_service_t> (
              require ("mesh-name"), require ("spot-id"),
              option ("actor-id", "cross-lang-user-spot-actor"), option ("start-file")));
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

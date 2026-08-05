/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Shared/contracts.hpp"
#include <zlink/framework.hpp>
#include <zlink/locations/redis.hpp>
#include <zlink/stream_connector.hpp>

#include <algorithm>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace sa = zlink::framework::e2e::submit_admission;

namespace
{

struct role_options_t
{
    std::string role;
    std::string rid;
    std::string http_endpoint;
    std::string mesh_endpoint;
    std::string mesh_advertise_host;
    std::string peer_rid;
    std::string peer_endpoint;
    std::string fanout_endpoint;
    std::string client_server_endpoint;
    std::string client_server_peer_endpoint;
    std::string stream_endpoint;
    std::string redis_endpoint;
    std::string redis_key_prefix;
    std::string log_dir;

    static role_options_t bind (const zlink::framework::configuration_section_t &section)
    {
        return {.role = section.require ("role"),
                .rid = section.require ("rid"),
                .http_endpoint = section.require ("httpEndpoint"),
                .mesh_endpoint = section.get ("meshEndpoint").value_or (""),
                .mesh_advertise_host =
                  section.get ("meshAdvertiseHost").value_or (""),
                .peer_rid = section.get ("peerRid").value_or (""),
                .peer_endpoint = section.get ("peerEndpoint").value_or (""),
                .fanout_endpoint = section.get ("fanoutEndpoint").value_or (""),
                .client_server_endpoint =
                  section.get ("clientServerEndpoint").value_or (""),
                .client_server_peer_endpoint =
                  section.get ("clientServerPeerEndpoint").value_or (""),
                .stream_endpoint = section.get ("streamEndpoint").value_or (""),
                .redis_endpoint = section.require ("redis.endpoint"),
                .redis_key_prefix = section.require ("redis.keyPrefix"),
                .log_dir = section.require ("logDir")};
    }
};

role_options_t read_options (zlink::framework::app_t &app, int argc, char **argv)
{
    app.config ().load_cli (argc, argv);
    const auto path = app.config ().model ().get ("config");
    if (!path) {
        throw std::runtime_error ("SubmitAdmission role requires --config=<path>");
    }
    app.config ().load_json (*path);
    return app.config ().bind_required<role_options_t> ("e2e");
}

std::uint16_t endpoint_port (const std::string &endpoint)
{
    const auto separator = endpoint.rfind (':');
    if (separator == std::string::npos || separator + 1 >= endpoint.size ()) {
        throw std::runtime_error ("TCP endpoint must end with a port: " + endpoint);
    }
    const auto port = std::stoul (endpoint.substr (separator + 1));
    if (port == 0 || port > 65535) {
        throw std::runtime_error ("TCP endpoint port is out of range: " + endpoint);
    }
    return static_cast<std::uint16_t> (port);
}

std::string terminal_name (const zlink::framework::result_t<void> &result)
{
    using error_kind_t = zlink::framework::framework_error_kind_t;
    if (result) {
            return "Submitted";
    }
    switch (result.error_kind ()) {
        case error_kind_t::deadline_exceeded:
            return "DeadlineExceeded";
        case error_kind_t::not_found:
            return "TargetNotFound";
        case error_kind_t::unavailable:
            return "RouteNotConnected";
        case error_kind_t::shutting_down:
            return "RuntimeShutdown";
        default:
            return std::string ("Exceptional:")
                   + (result.error () ? result.error ()->what () : "submit failed");
    }
}

sa::submit_response_t response_from (
  const std::string &operation_id,
  const zlink::framework::result_t<void> &result)
{
    return {.operation_id = operation_id,
            .status = terminal_name (result),
            .public_invocation_count = 1,
            .terminal_count = 1};
}

zlink::framework::task_t<sa::submit_response_t> response_after_submit (
  std::string operation_id,
  zlink::framework::task_t<void> submission)
{
    try {
        co_await submission;
        co_return response_from (
          operation_id, zlink::framework::result_t<void>::success ());
    }
    catch (const zlink::framework::framework_exception_t &error) {
        co_return response_from (
          operation_id,
          zlink::framework::result_t<void>::failure (error.kind (), error.what ()));
    }
}

class admission_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<sa::handler_gate_t, sa::evidence_store_t>;

    admission_handler_t (sa::handler_gate_t &gate, sa::evidence_store_t &evidence) :
        _gate (gate), _evidence (evidence)
    {
    }

    void handle (const sa::admission_message_t &message,
                 const zlink::framework::route_message_context_t &)
    {
        _evidence.entered (message.operation_id);
        _gate.wait ();
        _evidence.completed (message.operation_id);
    }

  private:
    sa::handler_gate_t &_gate;
    sa::evidence_store_t &_evidence;
};

class node_submit_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::route_client_t>;
    using request_type = sa::node_submit_request_t;
    using reply_type = sa::submit_response_t;

    explicit node_submit_handler_t (zlink::framework::route_client_t &routes) : _routes (routes) {}

    zlink::framework::task_t<sa::submit_response_t>
    handle (const sa::node_submit_request_t &request)
    {
        auto operation = _routes.send_to_node (
          sa::mesh_name, zlink::routing_id_t::from (request.target_rid), request.message);
        return response_after_submit (
          request.message.operation_id, operation.submit ());
    }

  private:
    zlink::framework::route_client_t &_routes;
};

class channel_submit_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::route_client_t>;
    using request_type = sa::admission_message_t;
    using reply_type = sa::submit_response_t;

    explicit channel_submit_handler_t (zlink::framework::route_client_t &routes) :
        _routes (routes)
    {
    }

    zlink::framework::task_t<sa::submit_response_t>
    handle (const sa::admission_message_t &message)
    {
        auto operation = _routes.send_to_channel (sa::channel_name, message);
        return response_after_submit (message.operation_id, operation.submit ());
    }

  private:
    zlink::framework::route_client_t &_routes;
};

class fanout_submit_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::publisher_t>;
    using request_type = sa::admission_message_t;
    using reply_type = sa::submit_response_t;

    explicit fanout_submit_handler_t (zlink::framework::publisher_t &publisher) :
        _publisher (publisher)
    {
    }

    zlink::framework::task_t<sa::submit_response_t>
    handle (const sa::admission_message_t &message)
    {
        return response_after_submit (
          message.operation_id,
          _publisher.publish (sa::fanout_channel, "admission", message).submit ());
    }

  private:
    zlink::framework::publisher_t &_publisher;
};

class client_server_admission_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<sa::handler_gate_t, sa::evidence_store_t>;
    using message_type = sa::admission_message_t;

    client_server_admission_handler_t (sa::handler_gate_t &gate,
                                       sa::evidence_store_t &evidence) :
        _gate (gate), _evidence (evidence)
    {
    }

    void handle (const sa::admission_message_t &message)
    {
        _evidence.entered (message.operation_id);
        _gate.wait ();
        _evidence.completed (message.operation_id);
    }

  private:
    sa::handler_gate_t &_gate;
    sa::evidence_store_t &_evidence;
};

class client_server_submit_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;
    using request_type = sa::admission_message_t;
    using reply_type = sa::submit_response_t;

    explicit client_server_submit_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    zlink::framework::task_t<sa::submit_response_t>
    handle (const sa::admission_message_t &message)
    {
        auto operation = _channels.send (sa::client_server_channel, message);
        return response_after_submit (message.operation_id, operation.submit ());
    }

  private:
    zlink::framework::channel_client_t &_channels;
};

class gate_close_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<sa::handler_gate_t>;
    explicit gate_close_handler_t (sa::handler_gate_t &gate) : _gate (gate) {}

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        _gate.close ();
        return {.body = R"({"status":"closed"})"};
    }

  private:
    sa::handler_gate_t &_gate;
};

class gate_open_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<sa::handler_gate_t>;
    explicit gate_open_handler_t (sa::handler_gate_t &gate) : _gate (gate) {}

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        _gate.open ();
        return {.body = R"({"status":"open"})"};
    }

  private:
    sa::handler_gate_t &_gate;
};

class evidence_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<sa::evidence_store_t>;
    explicit evidence_handler_t (sa::evidence_store_t &evidence) : _evidence (evidence) {}

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &request)
    {
        const auto found = request.query_values.find ("operationId");
        if (found == request.query_values.end () || found->second.empty ()) {
            return {.status = 400, .body = R"({"error":"operationId is required"})"};
        }
        return {.body = nlohmann::json (_evidence.get (found->second)).dump ()};
    }

  private:
    sa::evidence_store_t &_evidence;
};

class ready_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::route_mesh_runtime_t>;
    explicit ready_handler_t (zlink::framework::route_mesh_runtime_t &runtime) :
        _runtime (runtime)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &request)
    {
        const auto target = request.query_values.find ("targetRid");
        if (target == request.query_values.end () || target->second.empty ()) {
            return {.status = 400, .body = R"({"error":"targetRid is required"})"};
        }
        const auto snapshot = _runtime.snapshot (sa::mesh_name);
        const auto channel = std::find_if (
          snapshot.channels.begin (), snapshot.channels.end (),
          [] (const auto &candidate) {
              return candidate.channel_name == sa::channel_name;
          });
        const bool has_ready_channel =
          channel != snapshot.channels.end ()
          && channel->is_ready;
        nlohmann::json peers = nlohmann::json::array ();
        for (const auto &peer : snapshot.peers) {
            const bool peer_ready =
              peer.state
              == zlink::framework::peer_state_t::ready;
            peers.push_back (
              {{"rid", peer.node_rid.to_string ()},
               {"ready", peer_ready},
               {"hasReadyChannel", has_ready_channel},
               {"state", static_cast<int> (peer.state)}});
            if (peer.node_rid.to_string () == target->second
                && peer_ready && has_ready_channel) {
                return {.body = nlohmann::json{{"ready", true},
                                               {"rid", target->second},
                                               {"peerCount", snapshot.peers.size ()},
                                               {"peers", peers}}
                                  .dump ()};
            }
        }
        return {.status = 503,
                .body = nlohmann::json{{"ready", false},
                                       {"rid", target->second},
                                       {"peerCount", snapshot.peers.size ()},
                                       {"peers", peers}}
                          .dump ()};
    }

  private:
    zlink::framework::route_mesh_runtime_t &_runtime;
};

class actor_route_ready_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::route_mesh_runtime_t>;
    explicit actor_route_ready_handler_t (zlink::framework::route_mesh_runtime_t &runtime) :
        _runtime (runtime)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &request)
    {
        const auto target = request.query_values.find ("targetRid");
        if (target == request.query_values.end () || target->second.empty ()) {
            return {.status = 400, .body = R"({"error":"targetRid is required"})"};
        }
        const auto snapshot = _runtime.snapshot (sa::mesh_name + std::string (".actors"));
        for (const auto &peer : snapshot.peers) {
            if (peer.node_rid.to_string () == target->second
                && peer.state
                     == zlink::framework::peer_state_t::ready) {
                return {.body = nlohmann::json{{"ready", true},
                                               {"rid", target->second}}
                                  .dump ()};
            }
        }
        return {.status = 503,
                .body = nlohmann::json{{"ready", false}, {"rid", target->second}}.dump ()};
    }

  private:
    zlink::framework::route_mesh_runtime_t &_runtime;
};

class stream_gateway_state_t
{
  public:
    void connected (zlink::framework::stream_t stream)
    {
        std::lock_guard lock (_mutex);
        _stream = std::move (stream);
    }

    void disconnected ()
    {
        std::lock_guard lock (_mutex);
        _stream.reset ();
    }

    std::optional<zlink::framework::stream_t> stream () const
    {
        std::lock_guard lock (_mutex);
        return _stream;
    }

    void record_reply_race (std::string operation_id,
                            std::vector<std::string> terminals)
    {
        std::lock_guard lock (_mutex);
        _reply_races[std::move (operation_id)] = std::move (terminals);
    }

    nlohmann::json reply_race (const std::string &operation_id) const
    {
        std::lock_guard lock (_mutex);
        const auto found = _reply_races.find (operation_id);
        return nlohmann::json{{"operationId", operation_id},
                              {"terminals",
                               found == _reply_races.end () ? std::vector<std::string>{}
                                                            : found->second}};
    }

  private:
    mutable std::mutex _mutex;
    std::optional<zlink::framework::stream_t> _stream;
    std::map<std::string, std::vector<std::string>> _reply_races;
};

inline constexpr const char *admission_actor_type = "submit-admission-actor";

class admission_actor_t : public zlink::framework::actor_t
{
  public:
    explicit admission_actor_t (
      zlink::framework::actor_context_t context) :
        _actor_id (context.actor_ref ().actor_id ().value ()),
        _context (std::move (context))
    {
    }
    zlink::framework::actor_context_t &context () noexcept override
    { return _context; }
    const zlink::framework::actor_context_t &context () const noexcept override
    { return _context; }

    const std::string &actor_id () const noexcept { return _actor_id; }

  private:
    std::string _actor_id;
    zlink::framework::actor_context_t _context;
};

class admission_actor_factory_t final
    : public zlink::framework::actor_factory_t<admission_actor_t>
{
  public:
    zlink::framework::task_t<std::shared_ptr<admission_actor_t>>
    create (zlink::framework::actor_context_t context,
            std::stop_token) override
    {
        co_return std::make_shared<admission_actor_t> (
          std::move (context));
    }
};

class admission_actor_spot_t final
    : public zlink::framework::entry_spot_t<admission_actor_t>
{
  public:
    admission_actor_spot_t (zlink::framework::entry_spot_context_t context,
                            sa::evidence_store_t &evidence) :
        _context (std::move (context)), _evidence (evidence)
    {
    }

    zlink::framework::entry_spot_context_t &context () noexcept override
    {
        return _context;
    }
    const zlink::framework::entry_spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ().add_actor_send<&admission_actor_spot_t::on_admission> (
          "admission");
    }

    zlink::framework::task_t<zlink::framework::actor_create_response_t>
    on_create_actor (admission_actor_t &,
                     const zlink::framework::message_t &) override
    {
        co_return zlink::framework::actor_create_response_t::accept ();
    }

    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view,
                   const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::accept ();
    }

    zlink::framework::task_t<void> on_actor_joined (admission_actor_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_leave_actor (admission_actor_t &) override
    {
        co_return;
    }

    void on_admission (admission_actor_t &,
                       const zlink::framework::message_context_t &,
                       const sa::admission_message_t &message)
    {
        _evidence.entered (message.operation_id);
        _evidence.completed (message.operation_id);
    }

  private:
    zlink::framework::entry_spot_context_t _context;
    sa::evidence_store_t &_evidence;
};

class ensure_actor_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::session_actor_manager_t>;
    using request_type = sa::actor_target_t;
    using reply_type = sa::actor_target_t;

    explicit ensure_actor_handler_t (
      zlink::framework::session_actor_manager_t &actors) :
        _actors (actors)
    {
    }

    sa::actor_target_t handle (const sa::actor_target_t &request)
    {
        auto created = _actors.get_or_create (admission_actor_type, request.actor_id,
                                              zlink::framework::message_t {});
        if (!created) {
            throw *created.error ();
        }
        auto bound = _actors.bind_or_get (created.value ().ref ()).submit ().result ();
        if (!bound) {
            throw *bound.error ();
        }
        bound.value ()
          .context ()
          .join_entry_spot (zlink::framework::message_t {})
          .defer ();
        const auto &ref = bound.value ().ref ();
        return {.operation_id = request.operation_id,
                .actor_id = std::string (ref.actor_id ().value ()),
                .node_rid = std::string (ref.node_rid ().value ()),
                .generation = ref.object_generation ()};
    }

  private:
    zlink::framework::session_actor_manager_t &_actors;
};

class bound_session_submit_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::session_actor_manager_t>;
    using request_type = sa::actor_relay_request_t;
    using reply_type = sa::submit_response_t;
    explicit bound_session_submit_handler_t (zlink::framework::session_actor_manager_t &actors) :
        _actors (actors)
    {
    }

    zlink::framework::task_t<sa::submit_response_t>
    handle (const sa::actor_relay_request_t &request)
    {
        const auto actor = _actors.find (request.actor_id);
        if (!actor) {
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::not_found,
              "bound session actor was not found");
        }
        auto operation = actor->bound_session ().send (request.message);
        return response_after_submit (
          request.message.operation_id, operation.submit ());
    }

  private:
    zlink::framework::session_actor_manager_t &_actors;
};

std::string stream_terminal (
  const zlink::framework::result_t<void> &result)
{
    return terminal_name (result);
}

class submit_admission_stream_session_t final :
    public zlink::framework::packet_stream_session_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<stream_gateway_state_t,
                                          zlink::framework::session_actor_manager_t>;

    submit_admission_stream_session_t (stream_gateway_state_t &state,
                                       zlink::framework::session_actor_manager_t &actors) :
        _state (state), _actors (actors)
    {
    }

    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &stream) override
    {
        _state.connected (stream);
        co_return;
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        _state.disconnected ();
        co_return;
    }

    zlink::framework::task_t<void> on_error (zlink::framework::stream_t &,
                                             const zlink::framework::stream_error_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_packet (
      zlink::framework::stream_t &stream,
      const zlink::framework::session_message_context_t &dispatch,
      const zlink::message_t &payload) override
    {
        const auto packet_name = std::string (dispatch.packet_name);
        if (packet_name == "admission-bind-actor") {
            const auto target = payload.parse_json<sa::actor_target_t> ();
            zlink::framework::actor_ref_t ref (
              zlink::framework::actor_id_t (target.actor_id), target.generation,
              sa::mesh_name,
              zlink::framework::node_rid_t::from_string (target.node_rid));
            auto bound = co_await _actors.bind_or_get (std::move (ref)).submit ();
            const auto &bound_ref = bound.ref ();
            stream
              .reply_packet (zlink::message_t::from_json (sa::actor_target_t{
                .operation_id = target.operation_id,
                .actor_id = std::string (bound_ref.actor_id ().value ()),
                .node_rid = std::string (bound_ref.node_rid ().value ()),
                .generation = bound_ref.object_generation ()}))
              .submit ();
            co_return;
        }
        if (packet_name == "admission-session-actor-relay") {
            const auto request = payload.parse_json<sa::actor_relay_request_t> ();
            auto actor = _actors.find (request.actor_id);
            if (!actor) {
                throw zlink::framework::framework_exception_t (
                  zlink::framework::framework_error_kind_t::not_found,
                  "session actor relay target was not bound");
            }
            co_await actor->relay (
              "admission", zlink::message_t::from_json (request.message));
            stream
              .reply_packet (zlink::message_t::from_json (
                sa::submit_response_t{.operation_id = request.message.operation_id,
                                      .status = "Submitted",
                                      .public_invocation_count = 1,
                                      .terminal_count = 1}))
              .submit ();
            co_return;
        }
        if (packet_name == "admission-no-reply-token") {
            const auto message = payload.parse_json<sa::admission_message_t> ();
            auto invalid = stream.reply_packet (zlink::message_t::from_json (message));
            std::vector<std::string> terminals;
            terminals.push_back (stream_terminal (invalid.submit ().result ()));
            _state.record_reply_race (message.operation_id, std::move (terminals));
            co_return;
        }
        if (!dispatch.can_reply) {
            co_return;
        }
        const auto message = payload.parse_json<sa::admission_message_t> ();
        if (packet_name == "admission-reply-sequential") {
            auto reply = stream.reply_packet (zlink::message_t::from_json (message));
            std::vector<std::string> terminals;
            terminals.push_back (stream_terminal (reply.submit ().result ()));
            terminals.push_back (stream_terminal (reply.submit ().result ()));
            _state.record_reply_race (message.operation_id, std::move (terminals));
            co_return;
        }
        if (packet_name != "admission-reply-race") {
            co_return;
        }
        auto first = stream.reply_packet (zlink::message_t::from_json (message));
        auto second = stream.reply_packet (zlink::message_t::from_json (message));
        std::barrier start (3);
        std::vector<std::string> terminals (2);
        std::thread first_submit ([&] {
            start.arrive_and_wait ();
            terminals[0] = stream_terminal (first.submit ().result ());
        });
        std::thread second_submit ([&] {
            start.arrive_and_wait ();
            terminals[1] = stream_terminal (second.submit ().result ());
        });
        start.arrive_and_wait ();
        first_submit.join ();
        second_submit.join ();
        _state.record_reply_race (message.operation_id, std::move (terminals));
        co_return;
    }

  private:
    stream_gateway_state_t &_state;
    zlink::framework::session_actor_manager_t &_actors;
};

class stream_send_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<stream_gateway_state_t>;
    using request_type = sa::admission_message_t;
    using reply_type = sa::submit_response_t;

    explicit stream_send_handler_t (stream_gateway_state_t &state) : _state (state) {}

    zlink::framework::task_t<sa::submit_response_t>
    handle (const sa::admission_message_t &message)
    {
        auto stream = _state.stream ();
        if (!stream) {
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::unavailable,
              "STREAM peer is not connected");
        }
        auto operation = stream->write_packet (zlink::message_t::from_json (message));
        operation.packet_name ("admission");
        return response_after_submit (message.operation_id, operation.submit ());
    }

  private:
    stream_gateway_state_t &_state;
};

class stream_gateway_evidence_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<stream_gateway_state_t>;
    explicit stream_gateway_evidence_handler_t (stream_gateway_state_t &state) : _state (state) {}

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &request)
    {
        const auto found = request.query_values.find ("operationId");
        if (found == request.query_values.end () || found->second.empty ()) {
            return {.status = 400, .body = R"({"error":"operationId is required"})"};
        }
        return {.body = _state.reply_race (found->second).dump ()};
    }

  private:
    stream_gateway_state_t &_state;
};

class stream_peer_state_t
{
  public:
    explicit stream_peer_state_t (const std::string &endpoint)
    {
        zlink::stream_connector::connector_options_t options;
        options.endpoint = endpoint;
        options.connect_timeout = std::chrono::seconds (3);
        options.request_timeout = std::chrono::seconds (3);
        options.heartbeat.enabled = false;
        options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        _connector.emplace (zlink::stream_connector::connector_factory_t::create (options));
        _connector->on<sa::admission_message_t> (
          "admission", [this] (const sa::admission_message_t &message) {
              std::lock_guard lock (_mutex);
              ++_received[message.operation_id];
          });
        const auto connected = _connector->connect ();
        if (!connected) {
            throw std::runtime_error (
              "STREAM connector could not connect to the SessionGateway");
        }
    }

    ~stream_peer_state_t ()
    {
        if (_connector) {
            _connector->close ();
        }
    }

    bool request_reply (const sa::admission_message_t &message, std::string packet_name)
    {
        auto reply = _connector->request (message)
                       .packet_name (std::move (packet_name))
                       .submit<sa::admission_message_t> ();
        return reply && reply.value ().operation_id == message.operation_id;
    }

    void send_without_reply_token (const sa::admission_message_t &message)
    {
        _connector->send (message).packet_name ("admission-no-reply-token").submit ();
    }

    sa::actor_target_t bind_actor (const sa::actor_target_t &target)
    {
        auto reply = _connector->request (target)
                       .packet_name ("admission-bind-actor")
                       .submit<sa::actor_target_t> ();
        if (!reply) {
            throw std::runtime_error ("STREAM actor bind request failed");
        }
        return reply.value ();
    }

    sa::submit_response_t relay_actor (const sa::actor_relay_request_t &request)
    {
        auto reply = _connector->request (request)
                       .packet_name ("admission-session-actor-relay")
                       .submit<sa::submit_response_t> ();
        if (!reply) {
            throw std::runtime_error ("STREAM session actor relay request failed");
        }
        return reply.value ();
    }

    std::uint64_t received (const std::string &operation_id) const
    {
        std::lock_guard lock (_mutex);
        const auto found = _received.find (operation_id);
        return found == _received.end () ? 0 : found->second;
    }

  private:
    mutable std::mutex _mutex;
    std::optional<zlink::stream_connector::connector_t> _connector;
    std::map<std::string, std::uint64_t> _received;
};

class stream_peer_request_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<stream_peer_state_t>;
    using request_type = sa::admission_message_t;
    using reply_type = sa::submit_response_t;
    explicit stream_peer_request_handler_t (stream_peer_state_t &state) : _state (state) {}

    sa::submit_response_t handle (const sa::admission_message_t &message)
    {
        if (!_state.request_reply (message, "admission-reply-race")) {
            throw std::runtime_error ("STREAM reply race did not produce the winner reply");
        }
        return {.operation_id = message.operation_id,
                .status = "ReplyObserved",
                .public_invocation_count = 1,
                .terminal_count = 1};
    }

  private:
    stream_peer_state_t &_state;
};

class stream_peer_sequential_reply_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<stream_peer_state_t>;
    using request_type = sa::admission_message_t;
    using reply_type = sa::submit_response_t;
    explicit stream_peer_sequential_reply_handler_t (stream_peer_state_t &state) :
        _state (state)
    {
    }

    sa::submit_response_t handle (const sa::admission_message_t &message)
    {
        if (!_state.request_reply (message, "admission-reply-sequential")) {
            throw std::runtime_error ("STREAM sequential reply did not produce the first reply");
        }
        return {.operation_id = message.operation_id,
                .status = "ReplyObserved",
                .public_invocation_count = 1,
                .terminal_count = 1};
    }

  private:
    stream_peer_state_t &_state;
};

class stream_peer_no_token_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<stream_peer_state_t>;
    using request_type = sa::admission_message_t;
    using reply_type = sa::submit_response_t;
    explicit stream_peer_no_token_handler_t (stream_peer_state_t &state) : _state (state) {}

    sa::submit_response_t handle (const sa::admission_message_t &message)
    {
        _state.send_without_reply_token (message);
        return {.operation_id = message.operation_id,
                .status = "Sent",
                .public_invocation_count = 1,
                .terminal_count = 1};
    }

  private:
    stream_peer_state_t &_state;
};

class stream_peer_bind_actor_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<stream_peer_state_t>;
    using request_type = sa::actor_target_t;
    using reply_type = sa::actor_target_t;
    explicit stream_peer_bind_actor_handler_t (stream_peer_state_t &state) : _state (state) {}
    sa::actor_target_t handle (const sa::actor_target_t &target)
    {
        return _state.bind_actor (target);
    }

  private:
    stream_peer_state_t &_state;
};

class stream_peer_relay_actor_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<stream_peer_state_t>;
    using request_type = sa::actor_relay_request_t;
    using reply_type = sa::submit_response_t;
    explicit stream_peer_relay_actor_handler_t (stream_peer_state_t &state) : _state (state) {}
    sa::submit_response_t handle (const sa::actor_relay_request_t &request)
    {
        return _state.relay_actor (request);
    }

  private:
    stream_peer_state_t &_state;
};

class stream_peer_evidence_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<stream_peer_state_t>;
    explicit stream_peer_evidence_handler_t (stream_peer_state_t &state) : _state (state) {}

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &request)
    {
        const auto found = request.query_values.find ("operationId");
        if (found == request.query_values.end () || found->second.empty ()) {
            return {.status = 400, .body = R"({"error":"operationId is required"})"};
        }
        return {.body = nlohmann::json{{"operationId", found->second},
                                       {"receivedCount", _state.received (found->second)}}
                          .dump ()};
    }

  private:
    stream_peer_state_t &_state;
};

void configure_mesh_role (zlink::framework::zlink_framework_options_t &framework,
                          const role_options_t &options)
{
    framework.add_location_store (
      std::make_shared<zlink::framework::redis::redis_location_store_t> (
        zlink::framework::redis::redis_location_options_t{
          .connection_string = options.redis_endpoint,
          .key_prefix = options.redis_key_prefix}));
    framework.services ().add_singleton<sa::handler_gate_t> ();
    framework.services ().add_singleton<sa::evidence_store_t> ();
    framework.services ().add_transient<admission_handler_t,
                                         sa::handler_gate_t,
                                         sa::evidence_store_t> ();

    auto mesh = framework.add_route_mesh (sa::mesh_name);
    auto &socket = mesh.configure_router_socket ();
    socket.send_high_water_mark = zlink::byte_count_t::bytes (1);
    socket.receive_high_water_mark = zlink::byte_count_t::bytes (1);
    socket.mailbox_message_budget = 1;
    socket.send_timeout = std::chrono::milliseconds (300);
    auto &node = mesh.listen (options.mesh_endpoint)
                   .set_routing_id (zlink::routing_id_t::from (options.rid))
                   .set_object_role (zlink::framework::object_role_t::server);
    if (!options.mesh_advertise_host.empty ())
        node.set_advertise_host (options.mesh_advertise_host);
    node.add_route_send_handler<admission_handler_t, sa::admission_message_t> ();
    auto channel = mesh.channel_name (sa::channel_name);
    channel.server ()
      .set_weight (options.role == "target" ? 100 : 0)
      .add_send_handler<admission_handler_t, sa::admission_message_t> ();
    if (!options.peer_endpoint.empty ()) {
        mesh.peer_connections ().connect (options.peer_endpoint);
    }

    auto &http = framework.http ();
    http.listen (options.http_endpoint).map_health ("/health");
    http.map_get<evidence_handler_t> ("/evidence")
      .map_post<gate_close_handler_t> ("/gate/close")
      .map_post<gate_open_handler_t> ("/gate/open");
    if (options.role == "caller") {
        http.map_get<ready_handler_t> ("/ready")
          .map_post<node_submit_handler_t> ("/submit/node")
          .map_post<channel_submit_handler_t> ("/submit/channel");
    }
}

void configure_object_client_role (
  zlink::framework::zlink_framework_options_t &framework,
  const role_options_t &options)
{
    framework.add_location_store (
      std::make_shared<zlink::framework::redis::redis_location_store_t> (
        zlink::framework::redis::redis_location_options_t{
          .connection_string = options.redis_endpoint,
          .key_prefix = options.redis_key_prefix}));
    framework.services ().add_singleton<sa::handler_gate_t> ();
    framework.services ().add_singleton<sa::evidence_store_t> ();
    framework.services ().add_transient<admission_handler_t,
                                         sa::handler_gate_t,
                                         sa::evidence_store_t> ();

    auto mesh = framework.add_route_mesh (sa::mesh_name);
    auto &socket = mesh.configure_router_socket ();
    socket.send_high_water_mark = zlink::byte_count_t::bytes (1);
    socket.receive_high_water_mark = zlink::byte_count_t::bytes (1);
    socket.mailbox_message_budget = 1;
    socket.send_timeout = std::chrono::milliseconds (300);
    mesh.listen (options.mesh_endpoint)
      .set_routing_id (zlink::routing_id_t::from (options.rid))
      .set_object_role (zlink::framework::object_role_t::client);
    mesh.channel_name (sa::channel_name)
      .server ()
      .set_weight (0)
      .add_send_handler<admission_handler_t, sa::admission_message_t> ();
    if (!options.peer_endpoint.empty ()) {
        mesh.peer_connections ().connect (
          zlink::routing_id_t::from (options.peer_rid), options.peer_endpoint);
    }

    framework.http ()
      .listen (options.http_endpoint)
      .map_health ("/health")
      .map_get<evidence_handler_t> ("/evidence")
      .map_post<gate_close_handler_t> ("/gate/close")
      .map_post<gate_open_handler_t> ("/gate/open");
}

void configure_publisher_role (zlink::framework::zlink_framework_options_t &framework,
                               const role_options_t &options)
{
    framework.add_fanout_channel (sa::fanout_channel)
      .enable_publisher (options.fanout_endpoint)
      .set_routing_id (zlink::routing_id_t::from (options.rid));
    framework.http ()
      .listen (options.http_endpoint)
      .map_health ("/health")
      .map_post<fanout_submit_handler_t> ("/submit/fanout");
}

void configure_client_server_target_role (
  zlink::framework::zlink_framework_options_t &framework,
  const role_options_t &options)
{
    framework.services ().add_singleton<sa::handler_gate_t> ();
    framework.services ().add_singleton<sa::evidence_store_t> ();
    framework.services ().add_transient<client_server_admission_handler_t,
                                         sa::handler_gate_t,
                                         sa::evidence_store_t> ();
    framework.add_client_server_channel (sa::client_server_channel)
      .server ()
      .set_bind_host ("127.0.0.1")
      .listen (endpoint_port (options.client_server_endpoint))
      .add_send_handler<client_server_admission_handler_t,
                        sa::admission_message_t> ();
    framework.http ()
      .listen (options.http_endpoint)
      .map_health ("/health")
      .map_get<evidence_handler_t> ("/evidence")
      .map_post<gate_close_handler_t> ("/gate/close")
      .map_post<gate_open_handler_t> ("/gate/open");
}

void configure_client_server_caller_role (
  zlink::framework::zlink_framework_options_t &framework,
  const role_options_t &options)
{
    auto channel = framework.add_client_server_channel (sa::client_server_channel);
    auto client = channel.client ();
    client.connect (options.client_server_endpoint);
    if (!options.client_server_peer_endpoint.empty ()) {
        client.connect (options.client_server_peer_endpoint);
    }
    framework.http ()
      .listen (options.http_endpoint)
      .map_health ("/health")
      .map_post<client_server_submit_handler_t> ("/submit/client-server");
}

void configure_stream_gateway_role (zlink::framework::zlink_framework_options_t &framework,
                                    const role_options_t &options)
{
    framework.services ().add_singleton<stream_gateway_state_t> ();
    auto evidence = std::make_unique<sa::evidence_store_t> ();
    auto *evidence_ptr = evidence.get ();
    framework.services ().add_singleton<sa::evidence_store_t> (std::move (evidence));
    auto mesh = framework.add_route_mesh (sa::mesh_name + std::string (".actors"));
    mesh.listen (options.mesh_endpoint)
      .set_routing_id (zlink::routing_id_t::from (options.rid))
      .add_entry_spot<admission_actor_spot_t> (
        [evidence_ptr] (zlink::framework::entry_spot_context_t context) {
            return std::make_shared<admission_actor_spot_t> (
              std::move (context), *evidence_ptr);
        })
      .add_actor_factory<admission_actor_t, admission_actor_factory_t> (
        admission_actor_type,
        std::make_shared<admission_actor_factory_t> (),
        [] (auto &factory) { factory.disable_relocation (); });
    if (!options.peer_endpoint.empty ()) {
        mesh.peer_connections ().connect (
          zlink::routing_id_t::from (options.peer_rid), options.peer_endpoint);
    }
    framework.add_stream_node ("submit-admission-stream")
      .bind (options.stream_endpoint)
      .register_session<submit_admission_stream_session_t> ();
    framework.http ()
      .listen (options.http_endpoint)
      .map_health ("/health")
      .map_get<actor_route_ready_handler_t> ("/ready/actor")
      .map_post<stream_send_handler_t> ("/submit/stream")
      .map_post<ensure_actor_handler_t> ("/actors/ensure")
      .map_post<bound_session_submit_handler_t> ("/submit/bound-session")
      .map_get<evidence_handler_t> ("/evidence/actor")
      .map_get<stream_gateway_evidence_handler_t> ("/evidence/stream");
}

void configure_actor_target_role (zlink::framework::zlink_framework_options_t &framework,
                                  const role_options_t &options)
{
    auto evidence = std::make_unique<sa::evidence_store_t> ();
    auto *evidence_ptr = evidence.get ();
    framework.services ().add_singleton<sa::evidence_store_t> (std::move (evidence));
    auto mesh = framework.add_route_mesh (sa::mesh_name + std::string (".actors"));
    mesh.listen (options.mesh_endpoint)
      .set_routing_id (zlink::routing_id_t::from (options.rid))
      .add_entry_spot<admission_actor_spot_t> (
        [evidence_ptr] (zlink::framework::entry_spot_context_t context) {
            return std::make_shared<admission_actor_spot_t> (
              std::move (context), *evidence_ptr);
        })
      .add_actor_factory<admission_actor_t, admission_actor_factory_t> (
        admission_actor_type,
        std::make_shared<admission_actor_factory_t> (),
        [] (auto &factory) { factory.disable_relocation (); });
    if (!options.peer_endpoint.empty ()) {
        mesh.peer_connections ().connect (
          zlink::routing_id_t::from (options.peer_rid), options.peer_endpoint);
    }
    framework.http ()
      .listen (options.http_endpoint)
      .map_health ("/health")
      .map_post<ensure_actor_handler_t> ("/actors/ensure")
      .map_post<bound_session_submit_handler_t> ("/submit/bound-session")
      .map_get<evidence_handler_t> ("/evidence/actor");
}

void configure_stream_peer_role (zlink::framework::zlink_framework_options_t &framework,
                                 const role_options_t &options)
{
    framework.services ().add_singleton<stream_peer_state_t> (
      std::make_unique<stream_peer_state_t> (options.stream_endpoint));
    framework.http ()
      .listen (options.http_endpoint)
      .map_health ("/health")
      .map_post<stream_peer_request_handler_t> ("/request/reply-race")
      .map_post<stream_peer_sequential_reply_handler_t> ("/request/reply-sequential")
      .map_post<stream_peer_no_token_handler_t> ("/send/no-reply-token")
      .map_post<stream_peer_bind_actor_handler_t> ("/actors/bind")
      .map_post<stream_peer_relay_actor_handler_t> ("/actors/relay")
      .map_get<stream_peer_evidence_handler_t> ("/evidence/stream");
}

} // namespace

int main (int argc, char **argv)
{
    try {
        auto app = zlink::framework::app_t::create ();
        const auto options = read_options (app, argc, argv);
        app.advanced ().zlink ().max_pending (1);
        app.logging ()
          .use_file (options.log_dir + "/" + options.role + "-" + options.rid + ".log")
          .set_min_level (zlink::framework::log_level_t::debug);
        app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &framework) {
            if (options.role == "caller" || options.role == "target") {
                configure_mesh_role (framework, options);
            } else if (options.role == "object-client") {
                configure_object_client_role (framework, options);
            } else if (options.role == "publisher") {
                configure_publisher_role (framework, options);
            } else if (options.role == "client-server-target") {
                configure_client_server_target_role (framework, options);
            } else if (options.role == "client-server-caller") {
                configure_client_server_caller_role (framework, options);
            } else if (options.role == "stream-gateway") {
                configure_stream_gateway_role (framework, options);
            } else if (options.role == "stream-peer") {
                configure_stream_peer_role (framework, options);
            } else if (options.role == "actor-target") {
                configure_actor_target_role (framework, options);
            } else {
                throw std::runtime_error ("unknown SubmitAdmission role: " + options.role);
            }
        });
        return app.run (argc, argv);
    }
    catch (const std::exception &error) {
        std::cerr << "submit-admission role failed: " << error.what () << '\n';
        return 2;
    }
}

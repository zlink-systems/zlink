/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../../Shared/messages.hpp"
#include "../../Shared/configuration.hpp"

#include <zlink/framework.hpp>
#include <zlink/locations/redis.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace e2e = zlink::e2e::to_actor_messaging;

namespace
{

void add_redis_location_store (zlink::framework::zlink_framework_options_t &framework,
                               const e2e::redis_configuration_t &redis)
{
    framework.add_location_store (
      std::make_shared<zlink::framework::redis::redis_location_store_t> (
        zlink::framework::redis::redis_location_options_t{
          .connection_string = redis.endpoint, .key_prefix = redis.key_prefix}));
    auto &locations = framework.configure_locations ();
    locations.heartbeat_interval = std::chrono::seconds (1);
    locations.owner_lease_ttl = std::chrono::seconds (3);
    locations.polling_interval = std::chrono::milliseconds (500);
}

class captured_actor_refs_t
{
  public:
    void save (const std::string &actor_id, zlink::framework::actor_ref_t actor_ref)
    {
        std::lock_guard<std::mutex> lock (_mutex);
        _refs.insert_or_assign (actor_id, std::move (actor_ref));
    }

    std::optional<zlink::framework::actor_ref_t> find (const std::string &actor_id) const
    {
        std::lock_guard<std::mutex> lock (_mutex);
        const auto found = _refs.find (actor_id);
        return found == _refs.end () ? std::nullopt
                                     : std::optional<zlink::framework::actor_ref_t> (found->second);
    }

  private:
    mutable std::mutex _mutex;
    std::map<std::string, zlink::framework::actor_ref_t> _refs;
};

class actor_route_connections_t
{
  public:
    actor_route_connections_t (zlink::framework::mesh_peer_connections_t connections,
                               std::string actor_endpoint,
                               std::string actor_b_endpoint) :
        _connections (std::move (connections)),
        _actor_endpoint (std::move (actor_endpoint)),
        _actor_b_endpoint (std::move (actor_b_endpoint))
    {
    }

    void disconnect ()
    {
        _connections.disconnect (_actor_endpoint);
        _connections.disconnect (_actor_b_endpoint);
    }

    void reconnect ()
    {
        _connections.connect (_actor_endpoint);
        _connections.connect (_actor_b_endpoint);
    }

  private:
    zlink::framework::mesh_peer_connections_t _connections;
    std::string _actor_endpoint;
    std::string _actor_b_endpoint;
};

std::string kind_name (zlink::framework::framework_error_kind_t kind)
{
    switch (kind) {
        case zlink::framework::framework_error_kind_t::not_found:
            return "actor_route_not_found";
        case zlink::framework::framework_error_kind_t::unavailable:
            return "actor_location_stale";
        case zlink::framework::framework_error_kind_t::unavailable:
            return "route_not_connected";
        case zlink::framework::framework_error_kind_t::internal_failure:
            return "request_failed";
        default:
            return "framework_error_" + std::to_string (static_cast<int> (kind));
    }
}

e2e::actor_call_response_t failed (const e2e::actor_call_request_t &request,
                                   const zlink::framework::framework_exception_t &error)
{
    return {request.scenario, request.actor_id, error.what (), kind_name (error.kind ())};
}

zlink::framework::task_t<zlink::framework::actor_ref_t>
candidate_actor_ref (zlink::framework::actor_directory_t &directory,
                     const e2e::caller_configuration_t &configuration,
                     const std::string &actor_id)
{
    if (auto located = co_await directory.find (actor_id)) {
        co_return *located;
    }
    co_return zlink::framework::actor_ref_t (
      zlink::framework::actor_id_t (actor_id), 1, e2e::spot_mesh_name,
      zlink::framework::node_rid_t::from_string (configuration.actor_rid));
}

class send_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::actor_client_t,
                                          zlink::framework::actor_directory_t,
                                          e2e::caller_configuration_t>;
    using request_type = e2e::actor_call_request_t;
    using reply_type = e2e::actor_call_response_t;

    send_handler_t (zlink::framework::actor_client_t &actors,
                    zlink::framework::actor_directory_t &directory,
                    e2e::caller_configuration_t &configuration) :
        _actors (actors), _directory (directory), _configuration (configuration)
    {
    }

    zlink::framework::task_t<e2e::actor_call_response_t>
    handle (const e2e::actor_call_request_t &request)
    {
        try {
            e2e::actor_notify_t notify{request.scenario, request.actor_id, request.value};
            _actors.send (zlink::framework::actor_id_t (request.actor_id), notify)
              .submit ();
            co_return e2e::actor_call_response_t{request.scenario, request.actor_id, "sent", ""};
        }
        catch (const zlink::framework::framework_exception_t &error) {
            co_return failed (request, error);
        }
    }

  private:
    zlink::framework::actor_client_t &_actors;
    zlink::framework::actor_directory_t &_directory;
    e2e::caller_configuration_t &_configuration;
};

class request_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::actor_client_t,
                                          zlink::framework::actor_directory_t,
                                          e2e::caller_configuration_t>;
    using request_type = e2e::actor_call_request_t;
    using reply_type = e2e::actor_call_response_t;

    request_handler_t (zlink::framework::actor_client_t &actors,
                       zlink::framework::actor_directory_t &directory,
                       e2e::caller_configuration_t &configuration) :
        _actors (actors), _directory (directory), _configuration (configuration)
    {
    }

    zlink::framework::task_t<e2e::actor_call_response_t>
    handle (const e2e::actor_call_request_t &request)
    {
        try {
            e2e::actor_ask_t ask{request.scenario, request.actor_id, request.value};
            auto reply = co_await _actors.request (zlink::framework::actor_id_t (request.actor_id), ask)
                           .timeout (std::chrono::seconds (5))
                           .submit<e2e::actor_reply_t> ();
            co_return e2e::actor_call_response_t{
              request.scenario, request.actor_id, reply.value, ""};
        }
        catch (const zlink::framework::framework_exception_t &error) {
            co_return failed (request, error);
        }
    }

  private:
    zlink::framework::actor_client_t &_actors;
    zlink::framework::actor_directory_t &_directory;
    e2e::caller_configuration_t &_configuration;
};

class location_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::actor_directory_t>;
    using request_type = e2e::actor_call_request_t;
    using reply_type = e2e::actor_call_response_t;

    explicit location_handler_t (zlink::framework::actor_directory_t &directory) :
        _directory (directory)
    {
    }

    zlink::framework::task_t<e2e::actor_call_response_t>
    handle (const e2e::actor_call_request_t &request)
    {
        const auto located = co_await _directory.find (request.actor_id);
        co_return e2e::actor_call_response_t{
          request.scenario, request.actor_id, located ? "present" : "missing", ""};
    }

  private:
    zlink::framework::actor_directory_t &_directory;
};

class capture_ref_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::actor_directory_t,
                                          captured_actor_refs_t>;
    using request_type = e2e::actor_call_request_t;
    using reply_type = e2e::actor_call_response_t;

    capture_ref_handler_t (zlink::framework::actor_directory_t &directory,
                           captured_actor_refs_t &captured) :
        _directory (directory), _captured (captured)
    {
    }

    zlink::framework::task_t<e2e::actor_call_response_t>
    handle (const e2e::actor_call_request_t &request)
    {
        const auto actor_ref = co_await _directory.find (request.actor_id);
        if (!actor_ref) {
            co_return e2e::actor_call_response_t{request.scenario, request.actor_id, "",
                                                 "actor_route_not_found"};
        }
        _captured.save (request.actor_id, *actor_ref);
        co_return e2e::actor_call_response_t{request.scenario, request.actor_id, "captured", ""};
    }

  private:
    zlink::framework::actor_directory_t &_directory;
    captured_actor_refs_t &_captured;
};

class request_captured_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::actor_client_t,
                                          captured_actor_refs_t>;
    using request_type = e2e::actor_call_request_t;
    using reply_type = e2e::actor_call_response_t;

    request_captured_handler_t (zlink::framework::actor_client_t &actors,
                                captured_actor_refs_t &captured) :
        _actors (actors), _captured (captured)
    {
    }

    zlink::framework::task_t<e2e::actor_call_response_t>
    handle (const e2e::actor_call_request_t &request)
    {
        const auto actor_ref = _captured.find (request.actor_id);
        if (!actor_ref) {
            co_return e2e::actor_call_response_t{request.scenario, request.actor_id, "",
                                                 "actor_route_not_found"};
        }
        try {
            auto reply = co_await _actors
                           .request (zlink::framework::actor_id_t (request.actor_id),
                                              e2e::actor_ask_t{request.scenario,
                                                               request.actor_id, request.value})
                           .timeout (std::chrono::seconds (5))
                           .submit<e2e::actor_reply_t> ();
            co_return e2e::actor_call_response_t{request.scenario, request.actor_id, reply.value,
                                                 ""};
        }
        catch (const zlink::framework::framework_exception_t &error) {
            co_return failed (request, error);
        }
    }

  private:
    zlink::framework::actor_client_t &_actors;
    captured_actor_refs_t &_captured;
};

class disconnect_routes_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<actor_route_connections_t>;

    explicit disconnect_routes_handler_t (actor_route_connections_t &routes) :
        _routes (routes)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        _routes.disconnect ();
        zlink::framework::http_response_t response;
        response.body = nlohmann::json ({{"status", "disconnect"}}).dump ();
        return response;
    }

  private:
    actor_route_connections_t &_routes;
};

class reconnect_routes_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<actor_route_connections_t>;

    explicit reconnect_routes_handler_t (actor_route_connections_t &routes) :
        _routes (routes)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        _routes.reconnect ();
        zlink::framework::http_response_t response;
        response.body = nlohmann::json ({{"status", "reconnect"}}).dump ();
        return response;
    }

  private:
    actor_route_connections_t &_routes;
};

} // namespace

int main (int argc, char **argv)
{
    auto app = zlink::framework::app_t::create ();
    const auto configuration =
      e2e::load_role_configuration<e2e::caller_configuration_t> (app, argc, argv);
    app.logging ().use_file (configuration.log_dir + "/caller.log").set_min_level (
      zlink::framework::log_level_t::debug);
    app.add_zlink_framework ([configuration] (
                              zlink::framework::zlink_framework_options_t &framework) {
        framework.configure_dispatch ()
          .message_flow (zlink::framework::message_flow_log_mode_t::key_transitions)
          .trace_log_file (configuration.log_dir + "/caller-flow.log")
          .trace_label ("cpp-to-actor-caller");
        framework.services ().add_singleton<e2e::caller_configuration_t> (
          std::make_unique<e2e::caller_configuration_t> (configuration));
        framework.services ().add_singleton<captured_actor_refs_t> ();
        add_redis_location_store (framework, configuration.redis);
        auto mesh = framework.add_route_mesh (e2e::spot_mesh_name)
          .listen (configuration.spot_endpoint)
          .set_routing_id (zlink::routing_id_t::from (configuration.node_rid));
        if (!configuration.actor_spot_endpoint.empty ()) {
            mesh.peer_connections ().connect (
              zlink::routing_id_t::from (configuration.actor_rid),
              configuration.actor_spot_endpoint);
        }
        if (!configuration.actor_b_spot_endpoint.empty ()) {
            mesh.peer_connections ().connect (
              zlink::routing_id_t::from (configuration.actor_b_rid),
              configuration.actor_b_spot_endpoint);
        }
        framework.services ().add_singleton<actor_route_connections_t> (
          std::make_unique<actor_route_connections_t> (
            mesh.peer_connections (), configuration.actor_spot_endpoint,
            configuration.actor_b_spot_endpoint));
        framework.http ()
          .listen (configuration.http_endpoint)
          .map_health ("/health")
          .map_post<send_handler_t> ("/send")
          .map_post<request_handler_t> ("/request")
          .map_post<location_handler_t> ("/location")
          .map_post<capture_ref_handler_t> ("/capture-ref")
          .map_post<request_captured_handler_t> ("/request-captured")
          .map_post<disconnect_routes_handler_t> ("/route/disconnect")
          .map_post<reconnect_routes_handler_t> ("/route/reconnect");
    });
    return app.run (argc, argv);
}

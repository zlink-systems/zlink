/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../../Shared/location_store_registration.hpp"

#include <zlink/framework.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace rm = zlink::framework::e2e::registry_messaging;

namespace
{

inline constexpr const char *mesh_name = "registry-messaging-rm-a3";
inline constexpr const char *server_channel = "registry.messaging.rm-a3.server";

const char *peer_state_name (zlink::framework::peer_state_t state)
{
    using state_t = zlink::framework::peer_state_t;
    switch (state) {
        case state_t::connecting: return "connecting";
        case state_t::ready: return "ready";
        case state_t::draining: return "draining";
        case state_t::not_connected: return "not_connected";
        case state_t::not_required: return "not_required";
    }
    return "not_connected";
}

std::optional<std::string> unavailable_reason_name (
  const std::optional<zlink::framework::topology_reason_t> &reason)
{
    if (!reason)
        return std::nullopt;
    using reason_t = zlink::framework::topology_reason_t;
    switch (*reason) {
        case reason_t::runtime_not_ready: return "runtime_not_ready";
        case reason_t::no_ready_peer: return "no_ready_peer";
        case reason_t::no_ready_target: return "no_ready_target";
        case reason_t::location_unavailable: return "location_unavailable";
        case reason_t::capacity_exceeded: return "capacity_exceeded";
        case reason_t::draining: return "draining";
        case reason_t::internal_failure: return "internal_failure";
    }
    return "internal_failure";
}

std::vector<std::string> split_csv (const std::string &text)
{
    std::vector<std::string> result;
    std::stringstream input (text);
    std::string value;
    while (std::getline (input, value, ',')) {
        if (!value.empty ())
            result.push_back (std::move (value));
    }
    return result;
}

std::optional<int> optional_int (
  const zlink::framework::configuration_section_t &section,
  const char *name)
{
    const auto value = section.get (name);
    if (!value || value->empty ())
        return std::nullopt;
    return std::stoi (*value);
}

struct options_t
{
    std::string rid;
    std::string route_endpoint;
    std::string http_endpoint;
    std::string redis_endpoint;
    std::string redis_key_prefix;
    std::string log_dir;
    std::vector<std::string> peer_endpoints;
    std::optional<int> server_weight;

    static options_t bind (
      const zlink::framework::configuration_section_t &section)
    {
        return {
          .rid = section.require ("rid"),
          .route_endpoint = section.require ("routeEndpoint"),
          .http_endpoint = section.require ("httpEndpoint"),
          .redis_endpoint = section.require ("redis.endpoint"),
          .redis_key_prefix = section.require ("redis.keyPrefix"),
          .log_dir = section.require ("logDir"),
          .peer_endpoints =
            split_csv (section.get ("peerEndpoints").value_or ("")),
          .server_weight = optional_int (section, "serverWeight")};
    }
};

struct node_probe_t
{
    std::string value;
};

void to_json (nlohmann::json &json, const node_probe_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}

void from_json (const nlohmann::json &json, node_probe_t &value)
{
    json.at ("value").get_to (value.value);
}

class status_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<
        zlink::framework::route_mesh_runtime_t>;

    explicit status_handler_t (
      zlink::framework::route_mesh_runtime_t &runtime) :
        _runtime (runtime)
    {
    }

    zlink::framework::http_response_t handle (
      const zlink::framework::http_request_t &)
    {
        const auto snapshot = _runtime.snapshot (mesh_name);
        auto peers = nlohmann::json::array ();
        for (const auto &peer : snapshot.peers) {
            const auto unavailable_reason =
              unavailable_reason_name (peer.unavailable_reason);
            peers.push_back (
              {{"nodeRid", peer.node_rid.to_string ()},
               {"state", peer_state_name (peer.state)},
               {"unavailableReason",
                unavailable_reason.value_or ("")}});
        }
        return {
          .body =
            nlohmann::json{
              {"meshName", snapshot.mesh_name},
              {"state", static_cast<int> (snapshot.state)},
              {"isReady", snapshot.is_ready},
              {"sequence", snapshot.sequence},
              {"readyPeerCount", snapshot.ready_peer_count},
              {"peers", std::move (peers)}}
              .dump ()};
    }

  private:
    zlink::framework::route_mesh_runtime_t &_runtime;
};

class node_direct_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<zlink::framework::route_client_t>;

    explicit node_direct_handler_t (
      zlink::framework::route_client_t &routes) :
        _routes (routes)
    {
    }

    zlink::framework::task_t<zlink::framework::http_response_t>
    handle (const zlink::framework::http_request_t &request)
    {
        const auto json = nlohmann::json::parse (request.body);
        const auto target = json.at ("targetRid").get<std::string> ();
        auto call = _routes.send_to_node (
          mesh_name, zlink::routing_id_t::from (target),
          node_probe_t{.value = "rm-a3"});
        try {
            co_await call.submit ();
            co_return zlink::framework::http_response_t{
              .body =
                R"({"terminal":"Submitted","errorKind":""})"};
        }
        catch (const zlink::framework::framework_exception_t &error) {
            const auto not_found =
              error.kind ()
              == zlink::framework::framework_error_kind_t::not_found;
            co_return zlink::framework::http_response_t{
              .body =
                nlohmann::json{
                  {"terminal", not_found ? "NotFound" : "UnexpectedError"},
                  {"errorKind",
                   not_found ? "request_target_not_found"
                             : std::to_string (
                                 static_cast<int> (error.kind ()))},
                  {"message", error.what ()}}
                  .dump ()};
        }
    }

  private:
    zlink::framework::route_client_t &_routes;
};

} // namespace

int main (int argc, char **argv)
{
    auto app = zlink::framework::app_t::create ();
    app.config ().load_cli (argc, argv);
    const auto config_path = app.config ().model ().get ("config");
    if (!config_path) {
        throw std::runtime_error (
          "RegistryMessaging Object Client requires --config=<path>");
    }
    app.config ().load_json (*config_path);
    const auto options =
      app.config ().bind_required<options_t> ("e2e");
    app.logging ()
      .use_file (options.log_dir + "/" + options.rid + ".log")
      .set_min_level (zlink::framework::log_level_t::debug);

    app.add_zlink_framework (
      [&] (zlink::framework::zlink_framework_options_t &framework) {
          rm::add_redis_location_store (
            framework, options.redis_endpoint,
            options.redis_key_prefix);
          // Keep a crashed peer's descriptor visible beyond the fixed
          // 15-second liveness timeout so public monitoring can expose the
          // required-but-disconnected state.
          framework.configure_locations ().owner_lease_ttl =
            std::chrono::seconds (30);

          auto route = framework.add_route_mesh (mesh_name);
          route.listen (options.route_endpoint)
            .set_routing_id (
              zlink::routing_id_t::from (options.rid))
            .set_object_role (
              zlink::framework::object_role_t::client);
          if (options.server_weight) {
              route.channel_name (server_channel)
                .server ()
                .set_weight (*options.server_weight);
          } else {
              route.channel_name (server_channel).client ();
          }
          for (const auto &peer : options.peer_endpoints)
              route.peer_connections ().connect (peer);

          framework.http ()
            .listen (options.http_endpoint)
            .map_health ("/health")
            .map_get<status_handler_t> ("/rm-a3/status")
            .map_post<node_direct_handler_t> (
              "/rm-a3/node-direct");
      });
    return app.run (argc, argv);
}

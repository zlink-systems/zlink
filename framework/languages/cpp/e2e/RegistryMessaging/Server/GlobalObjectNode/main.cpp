/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../../Shared/location_store_registration.hpp"

#include <zlink/framework.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace
{

namespace fw = zlink::framework;
namespace rm = zlink::framework::e2e::registry_messaging;

struct probe_request_t
{
    std::string id;
};

struct probe_response_t
{
    std::string id;
    std::string owner;
    std::string kind;
};

struct mesh_name_service_t
{
    std::string value;
};

inline void to_json (nlohmann::json &json, const probe_request_t &value)
{
    json = nlohmann::json{{"id", value.id}};
}

inline void from_json (const nlohmann::json &json, probe_request_t &value)
{
    value.id = json.at ("id").get<std::string> ();
}

inline void to_json (nlohmann::json &json, const probe_response_t &value)
{
    json = nlohmann::json{{"id", value.id}, {"owner", value.owner}, {"kind", value.kind}};
}

class global_actor_t final : public fw::actor_t
{
  public:
    explicit global_actor_t (fw::actor_context_t context) : _context (std::move (context)) {}

    fw::actor_context_t &context () noexcept override { return _context; }
    const fw::actor_context_t &context () const noexcept override { return _context; }

  private:
    fw::actor_context_t _context;
};

struct global_actor_factory_t final : fw::actor_factory_t<global_actor_t>
{
    fw::task_t<std::shared_ptr<global_actor_t>> create (
      fw::actor_context_t context, std::stop_token) override
    {
        co_return std::make_shared<global_actor_t> (std::move (context));
    }
};

class global_entry_spot_t final : public fw::entry_spot_t<global_actor_t>
{
  public:
    explicit global_entry_spot_t (fw::entry_spot_context_t context) : _context (std::move (context)) {}

    fw::entry_spot_context_t &context () noexcept override { return _context; }
    const fw::entry_spot_context_t &context () const noexcept override { return _context; }
    void configure () override
    {
        _context.handlers ().add_actor_request<&global_entry_spot_t::probe> ();
    }

    fw::task_t<fw::message_t> probe (global_actor_t &actor,
                                     fw::message_context_t &,
                                     const probe_request_t &request) const
    {
        co_return fw::message_t::from (probe_response_t{
          request.id,
          std::string (actor.context ().actor_ref ().node_rid ().value ()),
          "actor"});
    }

    fw::task_t<fw::spot_actor_join_result_t> on_actor_join (
      std::string_view, const fw::message_t &) override
    {
        co_return fw::spot_actor_join_result_t::accept ();
    }

    fw::task_t<void> on_actor_joined (global_actor_t &) override { co_return; }
    fw::task_t<void> on_leave_actor (global_actor_t &) override { co_return; }

  private:
    fw::entry_spot_context_t _context;
};

class global_user_spot_t final : public fw::spot_t<global_actor_t>
{
  public:
    explicit global_user_spot_t (fw::spot_context_t context) : _context (std::move (context)) {}

    fw::spot_context_t &context () noexcept override { return _context; }
    const fw::spot_context_t &context () const noexcept override { return _context; }
    void configure () override
    {
        _context.handlers ().add_handler<&global_user_spot_t::probe> ();
    }

    fw::task_t<fw::message_t> probe (fw::message_context_t &, const probe_request_t &request) const
    {
        co_return fw::message_t::from (probe_response_t{
          request.id,
          std::string (_context.node_rid ().value ()),
          "spot"});
    }

    fw::task_t<fw::spot_actor_join_result_t> on_actor_join (
      std::string_view, const fw::message_t &) override
    {
        co_return fw::spot_actor_join_result_t::accept ();
    }

    fw::task_t<void> on_actor_joined (global_actor_t &) override { co_return; }
    fw::task_t<void> on_leave_actor (global_actor_t &) override { co_return; }

  private:
    fw::spot_context_t _context;
};

struct options_t
{
    std::string rid;
    std::string mesh_name;
    std::string route_endpoint;
    std::string bridge_mesh_name;
    std::string bridge_route_endpoint;
    std::string bridge_peer_endpoint;
    std::string http_endpoint;
    std::string redis_endpoint;
    std::string redis_key_prefix;
    std::string log_dir;
    std::string peer_endpoints;

    static options_t bind (const fw::configuration_section_t &section)
    {
        return {.rid = section.require ("rid"),
                .mesh_name = section.require ("meshName"),
                .route_endpoint = section.require ("routeEndpoint"),
                .bridge_mesh_name = section.require ("bridgeMeshName"),
                .bridge_route_endpoint = section.require ("bridgeRouteEndpoint"),
                .bridge_peer_endpoint = section.require ("bridgePeerEndpoint"),
                .http_endpoint = section.require ("httpEndpoint"),
                .redis_endpoint = section.require ("redis.endpoint"),
                .redis_key_prefix = section.require ("redis.keyPrefix"),
                .log_dir = section.require ("logDir"),
                .peer_endpoints = section.get ("peerEndpoints").value_or ("")};
    }
};

std::string first_peer (const options_t &options)
{
    const auto comma = options.peer_endpoints.find (',');
    return options.peer_endpoints.substr (0, comma);
}

class create_actor_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<fw::actor_manager_t, mesh_name_service_t>;
    create_actor_handler_t (fw::actor_manager_t &actors, mesh_name_service_t &mesh) :
        _actors (actors), _mesh (mesh) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto body = nlohmann::json::parse (request.body);
        const auto id = body.at ("id").get<std::string> ();
        const auto type = body.at ("type").get<std::string> ();
        try {
            const auto result = co_await _actors
              .get_or_create (fw::actor_id_t (id), type)
              .in_mesh (_mesh.value)
              .creation_request (fw::message_t::from (probe_request_t{id}))
              .submit ();
            const auto ref = std::visit ([] (const auto &value) -> fw::actor_ref_t {
                using result_t = std::decay_t<decltype (value)>;
                if constexpr (std::is_same_v<result_t, fw::actor_create_rejected_t>)
                    throw fw::framework_exception_t (fw::framework_error_kind_t::rejected,
                                                     "actor create rejected");
                else
                    return value.actor;
            }, result);
            co_return fw::http_response_t{.body = nlohmann::json{
              {"id", id}, {"mesh", std::string (ref.mesh_name ())},
              {"nodeRid", std::string (ref.node_rid ().value ())},
              {"generation", ref.object_generation ()}}.dump ()};
        }
        catch (const fw::framework_exception_t &error) {
            co_return fw::http_response_t{.status = 409,
              .body = nlohmann::json{{"error", static_cast<int> (error.kind ())},
                                     {"detail", error.what ()}}.dump ()};
        }
    }

  private:
    fw::actor_manager_t &_actors;
    mesh_name_service_t &_mesh;
};

class create_spot_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<fw::spot_manager_t, mesh_name_service_t>;
    create_spot_handler_t (fw::spot_manager_t &spots, mesh_name_service_t &mesh) :
        _spots (spots), _mesh (mesh) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto body = nlohmann::json::parse (request.body);
        const auto id = body.at ("id").get<std::string> ();
        const auto type = body.at ("type").get<std::string> ();
        try {
            const auto result = co_await _spots
              .get_or_create (fw::spot_id_t (id), type)
              .in_mesh (_mesh.value)
              .submit ();
            co_return fw::http_response_t{.body = nlohmann::json{
              {"id", id}, {"mesh", std::string (result.spot.mesh_name ())},
              {"nodeRid", std::string (result.spot.node_rid ().value ())},
              {"generation", result.spot.object_generation ()}}.dump ()};
        }
        catch (const fw::framework_exception_t &error) {
            co_return fw::http_response_t{.status = 409,
              .body = nlohmann::json{{"error", static_cast<int> (error.kind ())},
                                     {"detail", error.what ()}}.dump ()};
        }
    }

  private:
    fw::spot_manager_t &_spots;
    mesh_name_service_t &_mesh;
};

class probe_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<fw::actor_client_t, fw::route_client_t>;
    probe_handler_t (fw::actor_client_t &actors, fw::route_client_t &routes) :
        _actors (actors), _routes (routes) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto body = nlohmann::json::parse (request.body);
        const auto kind = body.at ("kind").get<std::string> ();
        const auto id = body.at ("id").get<std::string> ();
        try {
            if (kind == "actor") {
                auto result = co_await _actors.request (fw::actor_id_t (id), probe_request_t{id})
                                 .submit<probe_response_t> ();
                co_return fw::http_response_t{.body = nlohmann::json (result).dump ()};
            }
            auto result = co_await _routes.request_to_spot (
              fw::spot_id_t (id), probe_request_t{id}).submit<probe_response_t> ();
            co_return fw::http_response_t{.body = nlohmann::json (result).dump ()};
        }
        catch (const fw::framework_exception_t &error) {
            co_return fw::http_response_t{.status = 409,
              .body = nlohmann::json{{"error", static_cast<int> (error.kind ())},
                                     {"detail", error.what ()}}.dump ()};
        }
    }

  private:
    fw::actor_client_t &_actors;
    fw::route_client_t &_routes;
};

class find_actor_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<fw::actor_directory_t>;
    explicit find_actor_handler_t (fw::actor_directory_t &directory) : _directory (directory) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto id = nlohmann::json::parse (request.body).at ("id").get<std::string> ();
        const auto found = co_await _directory.find (id);
        if (!found)
            co_return fw::http_response_t{.status = 404, .body = R"({"error":"not_found"})"};
        const auto &ref = *found;
        co_return fw::http_response_t{.body = nlohmann::json{
          {"id", id}, {"mesh", std::string (ref.mesh_name ())},
          {"nodeRid", std::string (ref.node_rid ().value ())},
          {"generation", ref.object_generation ()}}.dump ()};
    }

  private:
    fw::actor_directory_t &_directory;
};

class find_spot_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<fw::spot_manager_t>;
    explicit find_spot_handler_t (fw::spot_manager_t &spots) : _spots (spots) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto id = nlohmann::json::parse (request.body).at ("id").get<std::string> ();
        const auto found = co_await _spots.find (fw::spot_id_t (id));
        if (!found)
            co_return fw::http_response_t{.status = 404, .body = R"({"error":"not_found"})"};
        co_return fw::http_response_t{.body = nlohmann::json{
          {"id", id}, {"mesh", std::string (found->mesh_name ())},
          {"nodeRid", std::string (found->node_rid ().value ())},
          {"generation", found->object_generation ()}}.dump ()};
    }

  private:
    fw::spot_manager_t &_spots;
};

class mesh_status_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<fw::route_mesh_runtime_t>;
    explicit mesh_status_handler_t (fw::route_mesh_runtime_t &runtime) : _runtime (runtime) {}

    fw::http_response_t handle (const fw::http_request_t &request)
    {
        const auto mesh = request.query_values.at ("mesh");
        const auto snapshot = _runtime.snapshot (mesh);
        nlohmann::json peers = nlohmann::json::array ();
        for (const auto &peer : snapshot.peers)
            peers.push_back ({{"rid", peer.node_rid.to_string ()},
                              {"state", static_cast<int> (peer.state)}});
        return fw::http_response_t{.body = nlohmann::json{
          {"mesh", mesh}, {"state", static_cast<int> (snapshot.state)},
          {"ready", snapshot.is_ready}, {"peers", std::move (peers)}}.dump ()};
    }

  private:
    fw::route_mesh_runtime_t &_runtime;
};

} // namespace

int main (int argc, char **argv)
{
    auto app = fw::app_t::create ();
    app.config ().load_cli (argc, argv);
    const auto config_path = app.config ().model ().get ("config");
    if (!config_path)
        throw std::runtime_error ("GlobalObjectNode requires --config");
    app.config ().load_json (*config_path);
    const auto options = app.config ().bind_required<options_t> ("e2e");
    app.logging ().use_file (options.log_dir + "/" + options.rid + ".log");
    app.add_zlink_framework ([&] (fw::zlink_framework_options_t &framework) {
        rm::add_redis_location_store (framework, options.redis_endpoint, options.redis_key_prefix);
        framework.services ().add_singleton<mesh_name_service_t> (
          std::make_unique<mesh_name_service_t> (mesh_name_service_t{options.mesh_name}));
        auto mesh = framework.add_route_mesh (options.mesh_name);
        mesh.listen (options.route_endpoint)
          .set_routing_id (zlink::routing_id_t::from (options.rid))
          .set_object_role (fw::object_role_t::server)
          .channel_name (options.mesh_name)
          .server ();
        if (!options.peer_endpoints.empty ())
            mesh.peer_connections ().connect (first_peer (options));
        auto bridge = framework.add_route_mesh (options.bridge_mesh_name);
        bridge.listen (options.bridge_route_endpoint)
          .set_routing_id (zlink::routing_id_t::from (options.rid + "-bridge"))
          .set_object_role (fw::object_role_t::client)
          .channel_name (options.bridge_mesh_name)
          .client ();
        bridge.peer_connections ().connect (options.bridge_peer_endpoint);
        mesh.add_entry_spot<global_entry_spot_t> ([] (fw::entry_spot_context_t context) {
            return std::make_shared<global_entry_spot_t> (std::move (context));
        });
        mesh.add_spot_factory<global_user_spot_t> (
          "global-user", [] (fw::spot_context_t context) {
              return std::make_shared<global_user_spot_t> (std::move (context));
          }, [] (auto &factory) { factory.disable_relocation (); });
        mesh.add_actor_factory<global_actor_t, global_actor_factory_t> (
          "global-actor", std::make_shared<global_actor_factory_t> (),
          [] (auto &factory) { factory.disable_relocation (); });
        framework.http ().listen (options.http_endpoint)
          .map_health ("/health")
          .map_post<create_actor_handler_t> ("/object/create-actor")
          .map_post<create_spot_handler_t> ("/object/create-spot")
          .map_post<probe_handler_t> ("/object/probe")
          .map_post<find_actor_handler_t> ("/object/find-actor")
          .map_post<find_spot_handler_t> ("/object/find-spot")
          .map_get<mesh_status_handler_t> ("/status");
    });
    return app.run (argc, argv);
}

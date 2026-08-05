/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"
#include "../Shared/Endpoints/evidence_endpoint.hpp"
#include "../Shared/scenario_state.hpp"
#include "../Shared/Support/location_store.hpp"
#include "../Shared/Support/configuration.hpp"

#include <zlink/framework.hpp>

#include <memory>
#include <string>

namespace e2e = zlink::framework::e2e::spot_service;

namespace
{

std::string gateway_error_kind_name (zlink::framework::framework_error_kind_t kind)
{
    switch (kind) {
        case zlink::framework::framework_error_kind_t::not_found:
            return "actor_route_not_found";
        case zlink::framework::framework_error_kind_t::unavailable:
            return "actor_location_stale";
        case zlink::framework::framework_error_kind_t::not_found:
            return "actor_dispatch_handler_not_found";
        default:
            return "request_failed";
    }
}

class gateway_evidence_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<scenario_state_t>;

    explicit gateway_evidence_handler_t (scenario_state_t &state) : _state (state) {}

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        zlink::framework::http_response_t response;
        response.body = nlohmann::json (_state.snapshot ()).dump ();
        return response;
    }

  private:
    scenario_state_t &_state;
};

class gateway_publish_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<
      scenario_state_t, zlink::framework::spot_publisher_client_t>;

    gateway_publish_handler_t (scenario_state_t &state,
                               zlink::framework::spot_publisher_client_t &publisher) :
        _state (state), _publisher (publisher)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &http)
    {
        const auto request =
          nlohmann::json::parse (http.body).get<e2e::spot_publish_route_req_t> ();
        auto published =
          _publisher
            .publish (e2e::publisher_channel, e2e::mesh_topic,
                      e2e::mesh_msg_t{"evt-sm-c4", request.marker})
            .result ();
        if (!published) {
            throw zlink::framework::framework_exception_t (
              published.error_kind (),
              published.error () ? published.error ()->what () : "SM-C4 publish failed");
        }
        _state.record ("SpotPublish", {}, request.spot_id,
                       "publisher=" + _state.node_rid + "|marker=" + request.marker);

        zlink::framework::http_response_t response;
        response.body =
          nlohmann::json (e2e::spot_publish_route_res_t{.accepted = true}).dump ();
        return response;
    }

  private:
    scenario_state_t &_state;
    zlink::framework::spot_publisher_client_t &_publisher;
};

class gateway_actor_push_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<scenario_state_t,
                                          zlink::framework::actor_client_t,
                                          zlink::framework::actor_directory_t>;

    gateway_actor_push_handler_t (scenario_state_t &state,
                                  zlink::framework::actor_client_t &actors,
                                  zlink::framework::actor_directory_t &actor_directory) :
        _state (state), _actors (actors), _actor_directory (actor_directory)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &http)
    {
        const auto request =
          nlohmann::json::parse (http.body).get<e2e::actor_push_by_actor_req_t> ();
        _state.record ("ActorPushRequest", request.actor_id, {}, request.value);
        zlink::framework::http_response_t response;
        try {
            auto actor_ref = _actor_directory.find (request.actor_id).result ();
            if (!actor_ref || !actor_ref.value ()) {
                const auto error_kind = gateway_error_kind_name (
                  actor_ref ? zlink::framework::framework_error_kind_t::not_found
                            : actor_ref.error_kind ());
                _state.record ("ActorPushFailed", request.actor_id, {}, error_kind);
                response.body =
                  nlohmann::json (e2e::actor_push_by_actor_res_t{
                    .actor_id = request.actor_id,
                    .value = request.value,
                    .delivered = false,
                    .error_kind = error_kind})
                    .dump ();
                return response;
            }
            auto reply =
              _actors.request (zlink::framework::actor_id_t (request.actor_id),
                               e2e::actor_push_req_t{request.value})
                .timeout (std::chrono::milliseconds (10000))
                .submit<e2e::actor_push_res_t> ()
                .result ();
            if (!reply) {
                const auto error_kind = gateway_error_kind_name (reply.error_kind ());
                _state.record ("ActorPushFailed", request.actor_id, {}, error_kind);
                response.body =
                  nlohmann::json (e2e::actor_push_by_actor_res_t{
                    .actor_id = request.actor_id,
                    .value = request.value,
                    .delivered = false,
                    .error_kind = error_kind})
                    .dump ();
                return response;
            }
            _state.record ("ActorPushDelivered", reply.value ().actor_id, {},
                           "value=" + request.value + "|node=play-a");
            response.body =
              nlohmann::json (e2e::actor_push_by_actor_res_t{
                .actor_id = reply.value ().actor_id,
                .value = request.value,
                .delivered = true,
                .error_kind = ""})
                .dump ();
            return response;
        }
        catch (const zlink::framework::framework_exception_t &error) {
            const auto error_kind = gateway_error_kind_name (error.kind ());
            _state.record ("ActorPushFailed", request.actor_id, {}, error_kind);
            response.body =
              nlohmann::json (e2e::actor_push_by_actor_res_t{
                .actor_id = request.actor_id,
                .value = request.value,
                .delivered = false,
                .error_kind = error_kind})
                .dump ();
            return response;
        }
    }

  private:
    scenario_state_t &_state;
    zlink::framework::actor_client_t &_actors;
    zlink::framework::actor_directory_t &_actor_directory;
};

void configure_gateway_codecs (zlink::framework::codec_options_builder_t codecs)
{
}

} // namespace

struct gateway_options_t
{
    std::string log_dir;
    std::string node_rid;
    std::string route_endpoint;
    std::string spot_router_endpoint;
    std::string pubsub_endpoint;
    std::string http_endpoint;
    std::string redis_endpoint;
    std::string redis_key_prefix;

    static gateway_options_t bind (const zlink::framework::configuration_section_t &section)
    {
        return {.log_dir = section.require ("logDir"),
                .node_rid = section.get ("nodeRid").value_or ("gateway"),
                .route_endpoint = section.require ("routeEndpoint"),
                .spot_router_endpoint = section.require ("spotRouterEndpoint"),
                .pubsub_endpoint = section.require ("pubsubEndpoint"),
                .http_endpoint = section.require ("httpEndpoint"),
                .redis_endpoint = section.require ("redis.endpoint"),
                .redis_key_prefix = section.require ("redis.keyPrefix")};
    }
};

inline int run_gateway_server (int argc, char **argv)
{
    auto app = zlink::framework::app_t::create ();
    load_spot_service_config (app, argc, argv, "gateway");
    const auto config = app.config ().bind_required<gateway_options_t> ("e2e");
    const auto &log_dir = config.log_dir;
    const auto &node_rid = config.node_rid;
    const auto &route_endpoint = config.route_endpoint;
    const auto &spot_router_endpoint = config.spot_router_endpoint;
    const auto &pubsub_endpoint = config.pubsub_endpoint;
    const auto &http_endpoint = config.http_endpoint;
    const auto &redis_endpoint = config.redis_endpoint;
    const auto &redis_key_prefix = config.redis_key_prefix;

    app.logging ()
      .use_file (log_dir + "/" + node_rid + ".log")
      .set_min_level (zlink::framework::log_level_t::debug);
    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        auto state = std::make_unique<scenario_state_t> (node_rid);
        options.configure_dispatch ()
          .message_flow (zlink::framework::message_flow_log_mode_t::key_transitions)
          .trace_log_file (log_dir + "/" + node_rid + "-flow.log")
          .trace_label ("cpp-sm-" + node_rid);
        options.services ()
          .add_singleton<scenario_state_t> (std::move (state))
          .add_transient<gateway_evidence_handler_t, scenario_state_t> ()
          .add_transient<evidence_wait_handler_t, scenario_state_t> ()
          .add_transient<gateway_publish_handler_t, scenario_state_t,
                         zlink::framework::spot_publisher_client_t> ()
          .add_transient<gateway_actor_push_handler_t, scenario_state_t,
                         zlink::framework::actor_client_t,
                         zlink::framework::actor_directory_t> ();
        configure_gateway_codecs (options.codecs ());
        add_redis_location_store (options, redis_endpoint, redis_key_prefix);
        auto route = options.add_route_mesh (e2e::route_channel);
        route.listen (route_endpoint)
          .set_routing_id (zlink::routing_id_t::from (node_rid))
          .channel_name (e2e::route_channel);
        auto spot = options.add_route_mesh (e2e::spot_mesh);
        spot.listen (spot_router_endpoint)
          .set_routing_id (zlink::routing_id_t::from (node_rid))
          .channel_name (e2e::spot_mesh);
        options.http ()
          .listen (http_endpoint)
          .map_health ("/health")
          .map_get<gateway_evidence_handler_t> ("/evidence")
          .map_post<evidence_wait_handler_t> ("/evidence/wait")
          .map_post<gateway_publish_handler_t> ("/spot/publish")
          .map_post<gateway_actor_push_handler_t> ("/actor/push");
    });
    return app.run (argc, argv);
}

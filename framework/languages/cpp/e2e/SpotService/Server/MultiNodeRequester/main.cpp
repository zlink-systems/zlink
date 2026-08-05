/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../MultiNode/Handlers/multi_node_handlers.hpp"
#include "../Shared/Support/codecs.hpp"
#include "../Shared/Support/configuration.hpp"
#include "../Shared/Support/location_store.hpp"

#include <zlink/framework.hpp>

#include <memory>
#include <string>

class requester_bridge_spot_t
    : public zlink::framework::spot_t<zlink::framework::actor_t>
{
  public:
    explicit requester_bridge_spot_t (
      zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::spot_context_t &context () noexcept override
    {
        return _context;
    }

    const zlink::framework::spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override {}
    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (
      std::string_view,
      const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::reject ();
    }
    zlink::framework::task_t<void>
    on_actor_joined (zlink::framework::actor_t &) override
    {
        co_return;
    }
    zlink::framework::task_t<void>
    on_leave_actor (zlink::framework::actor_t &) override
    {
        co_return;
    }

  private:
    zlink::framework::spot_context_t _context;
};

struct requester_options_t
{
    std::string log_dir;
    std::string node_rid;
    std::string route_client_endpoint;
    std::string spot_router_endpoint;
    std::string http_endpoint;
    std::string redis_endpoint;
    std::string redis_key_prefix;

    static requester_options_t bind (const zlink::framework::configuration_section_t &section)
    {
        return {.log_dir = section.require ("logDir"),
                .node_rid = section.get ("nodeRid").value_or (multi_node_a_name),
                .route_client_endpoint = section.require ("routeClientEndpoint"),
                .spot_router_endpoint = section.require ("spotRouterEndpoint"),
                .http_endpoint = section.require ("httpEndpoint"),
                .redis_endpoint = section.require ("redis.endpoint"),
                .redis_key_prefix = section.require ("redis.keyPrefix")};
    }
};

int main (int argc, char **argv)
{
    auto app = zlink::framework::app_t::create ();
    load_spot_service_config (app, argc, argv, "multi-node requester");
    const auto config = app.config ().bind_required<requester_options_t> ("e2e");
    const auto &log_dir = config.log_dir;
    const auto &node_rid = config.node_rid;
    const auto &route_client_endpoint = config.route_client_endpoint;
    const auto &spot_router_endpoint = config.spot_router_endpoint;
    const auto &http_endpoint = config.http_endpoint;
    const auto &redis_endpoint = config.redis_endpoint;
    const auto &redis_key_prefix = config.redis_key_prefix;

    app.logging ()
      .use_file (log_dir + "/" + node_rid + "-requester.log")
      .set_min_level (zlink::framework::log_level_t::debug);
    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        auto state = std::make_unique<scenario_state_t> (node_rid);
        options.configure_dispatch ()
          .message_flow (zlink::framework::message_flow_log_mode_t::key_transitions)
          .trace_log_file (log_dir + "/" + node_rid + "-requester-flow.log")
          .trace_label ("cpp-sm-" + node_rid + "-requester");
        options.services ()
          .add_singleton<scenario_state_t> (std::move (state))
          .add_transient<multi_node_route_ping_proxy_handler_t, scenario_state_t,
                         zlink::framework::route_client_t> ()
          .add_transient<multi_node_state_route_handler_t, scenario_state_t,
                         zlink::framework::route_client_t,
                         zlink::framework::spot_handle_resolver_t> ();
        configure_codecs (options.codecs ());
        add_redis_location_store (options, redis_endpoint, redis_key_prefix);
        const auto route_name = multi_node_route_channel_for (node_rid);
        auto route = options.add_route_mesh (route_name);
        route.listen (route_client_endpoint)
          .set_routing_id (zlink::routing_id_t::from ("requester-" + node_rid))
          .channel_name (route_name);
        const auto requester_mesh = "requester-" + node_rid;
        auto spot = options.add_route_mesh (requester_mesh);
        spot.listen (spot_router_endpoint)
          .set_routing_id (zlink::routing_id_t::from ("requester-spot-" + node_rid))
          .channel_name (requester_mesh);
        spot.add_spot_factory<requester_bridge_spot_t> (
          "requester-bridge",
          [] (spot_context_t context) {
              return std::make_shared<requester_bridge_spot_t> (
                std::move (context));
          },
          [] (auto &factory) {
              factory.disable_relocation ();
          });
        options.http ()
          .listen (http_endpoint)
          .map_health ("/health")
          .map_post<multi_node_route_ping_proxy_handler_t> ("/route/control-ping")
          .map_post<multi_node_state_route_handler_t> ("/spot/state/request");
    });
    return app.run (argc, argv);
}

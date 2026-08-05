/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "Configuration/publisher_options.hpp"
#include "Endpoints/publisher_endpoints.hpp"
#include "../Shared/location_store.hpp"

namespace ps_publisher = zlink::framework::e2e::pubsub::server::publisher;
namespace ps_server = zlink::framework::e2e::pubsub::server;

int main (int argc, char **argv)
{
    auto app = zlink::framework::app_t::create ();
    app.config ().load_cli (argc, argv);
    const auto config_path = app.config ().model ().get ("config");
    if (!config_path) {
        throw std::runtime_error ("PubSub publisher requires --config=<path>");
    }
    app.config ().load_json (*config_path);
    const auto pubsub = app.config ().bind_required<ps_publisher::publisher_options_t> ("e2e");
    app.logging ()
      .use_file (pubsub.log_dir + "/publisher.log")
      .set_min_level (zlink::framework::log_level_t::debug);
    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.services ().add_singleton<ps_server::operational_evidence_store_t> (
          std::make_unique<ps_server::operational_evidence_store_t> ("publisher"));
        ps_server::configure_flow (options, pubsub.log_dir, "publisher");
        ps_server::configure_codecs (options.codecs ());
        ps_server::add_redis_location_store (options, pubsub.redis_endpoint, pubsub.redis_key_prefix);
        options.add_fanout_channel (zlink::framework::e2e::pubsub::event_channel)
          .set_routing_id (zlink::routing_id_t::from ("pubsub-publisher"))
          .enable_publisher (pubsub.publisher_endpoint);
        options.http ()
          .listen (pubsub.http_endpoint)
          .map_health ("/health")
          .map_get<ps_server::operational_evidence_handler_t> ("/evidence")
          .map_post<ps_server::operational_evidence_clear_handler_t> ("/evidence/clear")
          .map_post<ps_server::operational_shutdown_handler_t> ("/shutdown")
          .map_post<ps_publisher::publish_event_handler_t> ("/publish/event")
          .map_post<ps_publisher::publish_missing_handler_t> ("/publish/missing");
    });
    return app.run (argc, argv);
}

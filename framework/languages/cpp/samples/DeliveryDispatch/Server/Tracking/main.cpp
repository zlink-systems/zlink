/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Configuration/evidence_store.hpp"
#include "../Configuration/location_store.hpp"
#include "../Configuration/sample_names.hpp"
#include "../Configuration/sample_configuration.hpp"
#include "../common_codecs.hpp"
#include "Handlers/tracking_handlers.hpp"

#include <zlink/framework.hpp>

int main (int argc, char **argv)
{
    using namespace zlink::framework;
    using namespace zlink::samples::deliverydispatch;

    auto app = app_t::create ();
    const auto configuration = load_sample_configuration (app, argc, argv);
    const auto &topology = configuration.topology;
    app.add_zlink_framework ([&] (zlink_framework_options_t &options) {
        options.configure_dispatch ()
          .message_flow (message_flow_log_mode_t::key_transitions)
          .trace_log_file (configuration.flow_log_path ())
          .trace_label ("deliverydispatch-tracking");
        options.services ().add_singleton<evidence_store_t> (std::make_unique<evidence_store_t> (configuration.evidence_path ()));
        add_deliverydispatch_json_codecs (options.codecs ());
        add_deliverydispatch_location_store (options, topology);
        options.add_client_server_channel (sample_names_t::tracking_route_channel)
          .server ()
          .set_bind_host (host_from_tcp_endpoint (topology.tracking_route_endpoint))
          .listen (port_from_http_url (topology.tracking_route_endpoint))
          .add_handler_group ("tracking");
        auto customer_mesh = options.add_route_mesh (sample_names_t::customer_actor_discovery);
        customer_mesh.set_routing_id (zlink::routing_id_t::from (
          sample_names_t::tracking_route_node));
        customer_mesh.set_object_role (object_role_t::client);
        customer_mesh.listen (topology.tracking_spot_router_endpoint)
          .channel_name (sample_names_t::customer_actor_discovery)
          .client ();
        customer_mesh.peer_connections ().connect (
          zlink::routing_id_t::from (sample_names_t::customer_gateway_route_node),
          topology.customer_spot_router_endpoint);
        options.handlers ().group ("tracking").add<delivery_status_changed_handler_t> ();
    });
    return app.run (argc, argv);
}

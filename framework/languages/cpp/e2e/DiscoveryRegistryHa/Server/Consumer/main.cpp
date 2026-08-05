/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "Configuration/consumer_options.hpp"
#include "Endpoints/consumer_endpoints.hpp"
#include "Infrastructure/socket_evidence_store.hpp"

#include "../../Shared/store_failure_contracts.hpp"
#include "../Shared/location_store.hpp"

#include <zlink/framework.hpp>

namespace sf = zlink::framework::e2e::store_failure;
namespace sf_consumer = zlink::framework::e2e::store_failure::consumer;

int main (int argc, char **argv)
{
    auto app = zlink::framework::app_t::create ();
    const auto options = sf_consumer::read_consumer_options (app, argc, argv);
    app.logging ()
      .use_file (options.log_dir + "/" + options.rid + ".log")
      .set_min_level (zlink::framework::log_level_t::debug);
    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &framework) {
        auto socket_evidence = std::make_unique<sf_consumer::socket_evidence_store_t> ();
        auto *socket_evidence_ptr = socket_evidence.get ();
        framework.services ().add_singleton<sf_consumer::socket_evidence_store_t> (
          std::move (socket_evidence));
        framework.configure_dispatch ()
          .message_flow (zlink::framework::message_flow_log_mode_t::key_transitions)
          .trace_log_file (options.log_dir + "/" + options.rid + "-flow.log")
          .trace_label ("cpp-store-failure-" + options.rid);
        sf::server::add_redis_location_store (framework, options);
        framework.add_client_server_channel (sf::api_channel).client ();
        app.logging ().use_callback_sink (
          [socket_evidence_ptr] (const zlink::framework::log_record_t &record) {
              socket_evidence_ptr->record (record);
          });
        framework.http ()
          .listen (options.http_endpoint)
          .map_health ("/health")
          .map_get<sf_consumer::query_status_handler_t> ("/query/status")
          .map_get<sf_consumer::query_peers_handler_t> ("/query/peers")
          .map_get<sf_consumer::query_connections_handler_t> ("/query/connections")
          .map_post<sf_consumer::profile_request_handler_t> ("/profile/request")
          .map_post<sf_consumer::profile_request_timeout_handler_t> (
            "/profile/request/timeout/{milliseconds}")
          .map_post<sf_consumer::shutdown_handler_t> ("/shutdown");
    });
    return app.run (argc, argv);
}

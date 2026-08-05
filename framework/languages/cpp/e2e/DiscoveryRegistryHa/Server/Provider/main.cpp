/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "Configuration/provider_options.hpp"
#include "Handlers/provider_handlers.hpp"
#include "Infrastructure/provider_evidence_store.hpp"

#include "../../Shared/store_failure_contracts.hpp"
#include "../Shared/location_store.hpp"

#include <zlink/framework.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace sf = zlink::framework::e2e::store_failure;
namespace sf_provider = zlink::framework::e2e::store_failure::provider;

namespace
{
std::uint16_t port_from_tcp_endpoint (const std::string &endpoint)
{
    const auto separator = endpoint.rfind (':');
    if (separator == std::string::npos || separator + 1 >= endpoint.size ())
        throw std::invalid_argument ("ClientServer endpoint must include a port");
    const auto value = std::stoul (endpoint.substr (separator + 1));
    if (value == 0 || value > 65535)
        throw std::invalid_argument ("ClientServer endpoint port is out of range");
    return static_cast<std::uint16_t> (value);
}
} // namespace

int main (int argc, char **argv)
{
    auto app = zlink::framework::app_t::create ();
    const auto options = sf_provider::read_provider_options (app, argc, argv);
    auto lifecycle_owner =
      std::make_unique<sf_provider::provider_lifecycle_control_t> (app);
    app.logging ()
      .use_file (options.log_dir + "/" + options.log_name + ".log")
      .set_min_level (zlink::framework::log_level_t::debug);
    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &framework) {
        framework.configure_dispatch ()
          .message_flow (zlink::framework::message_flow_log_mode_t::key_transitions)
          .trace_log_file (options.log_dir + "/" + options.log_name + "-flow.log")
          .trace_label ("cpp-store-failure-" + options.log_name);
        sf::server::add_redis_location_store (framework, options);
        framework.services ().add_singleton<sf_provider::provider_evidence_store_t> (
          std::make_unique<sf_provider::provider_evidence_store_t> (options.rid));
        framework.services ().add_singleton<sf_provider::provider_lifecycle_control_t> (
          std::move (lifecycle_owner));
        framework.add_client_server_channel (sf::api_channel)
          .server ()
          .set_bind_host ("127.0.0.1")
          .listen (port_from_tcp_endpoint (options.channel_endpoint))
          .add_handler_group (sf::handler_group);
        framework.handlers ()
          .group (sf::handler_group)
          .add<sf_provider::profile_request_handler_t> ();
        framework.http ()
          .listen (options.http_endpoint)
          .map_health ("/health")
          .map_get<sf_provider::query_status_handler_t> ("/query/status")
          .map_get<sf_provider::evidence_snapshot_handler_t> ("/evidence")
          .map_post<sf_provider::evidence_wait_handler_t> ("/evidence/wait")
          .map_post<sf_provider::drain_handler_t> ("/drain")
          .map_post<sf_provider::shutdown_handler_t> ("/shutdown")
          .map_post<sf_provider::crash_handler_t> ("/admin/crash");
    });
    return app.run (argc, argv);
}

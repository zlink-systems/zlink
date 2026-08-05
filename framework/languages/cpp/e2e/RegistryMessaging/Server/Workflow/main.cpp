/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "Configuration/workflow_options.hpp"
#include "Endpoints/workflow_endpoints.hpp"
#include "Handlers/workflow_handlers.hpp"
#include "Infrastructure/scenario_state.hpp"

#include "../../Shared/location_store_registration.hpp"
#include "../../Shared/registry_messaging_contracts.hpp"
#include "../../Shared/tcp_endpoint.hpp"

#include <zlink/framework.hpp>

#include <memory>

namespace e2e = zlink::framework::e2e::registry_messaging;
namespace rm_workflow = zlink::framework::e2e::registry_messaging::workflow;

namespace
{

void configure_common_codecs (zlink::framework::codec_options_builder_t codecs)
{
}

} // namespace

int main (int argc, char **argv)
{
    auto app = zlink::framework::app_t::create ();
    app.config ().load_cli (argc, argv);
    const auto config_path = app.config ().model ().get ("config");
    if (!config_path) {
        throw std::runtime_error ("RegistryMessaging workflow requires --config=<path>");
    }
    app.config ().load_json (*config_path);
    const auto options = app.config ().bind_required<rm_workflow::workflow_options_t> ("e2e");
    app.logging ()
      .use_file (options.log_dir + "/" + options.rid + ".log")
      .set_min_level (zlink::framework::log_level_t::debug);
    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &framework) {
        framework.configure_dispatch ()
          .message_flow (zlink::framework::message_flow_log_mode_t::key_transitions)
          .trace_log_file (options.log_dir + "/" + options.rid + "-flow.log")
          .trace_label ("cpp-rm-" + options.rid);
        framework.services ().add_singleton<rm_workflow::scenario_state_t> (
          std::make_unique<rm_workflow::scenario_state_t> (options.rid, options.instance_id));
        configure_common_codecs (framework.codecs ());
        e2e::add_redis_location_store (framework, options.redis_endpoint, options.redis_key_prefix);
        const auto endpoint = e2e::parse_tcp_endpoint (options.workflow_endpoint);
        auto channel = framework.add_client_server_channel (e2e::workflow_channel);
        channel.client ();
        channel.server ()
          .set_bind_host (endpoint.host)
          .set_advertise_host (endpoint.host)
          .listen (endpoint.port)
          .add_handler_group (e2e::handler_group);
        framework.handlers ()
          .group (e2e::handler_group)
          .add<rm_workflow::workflow_request_handler_t> ();
        if (!options.http_endpoint.empty ()) {
            framework.http ()
              .listen (options.http_endpoint)
              .map_health ("/health")
              .map_get<rm_workflow::evidence_handler_t> ("/evidence")
              .map_post<rm_workflow::http_workflow_request_handler_t> ("/workflow/request");
        }
    });
    return app.run (argc, argv);
}

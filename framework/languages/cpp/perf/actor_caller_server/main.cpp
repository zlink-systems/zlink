/* SPDX-License-Identifier: FSL-1.1-ALv2 */

// The ActorCaller source role of §10.9 and §10.10: an Object Client without a Session, addressing Actors by global ActorId.

#include "actor_no_bind_request_echo_scenario.hpp"
#include "actor_no_bind_send_send_echo_scenario.hpp"

namespace
{
using namespace perf;

template <typename TScenario> int run (std::unique_ptr<role_t> role)
{
    auto &role_ref = *role;
    auto scenario = std::make_shared<TScenario> (role_ref);
    role_ref.workload = [scenario] (const loops_t &loops) { scenario->run (loops); };
    return run_role (std::move (role), [&, scenario] (fw::zlink_framework_options_t &options, fw::app_t &app) {
        const auto &config = role_ref.config;
        auto mesh = options.add_route_mesh (*config.mesh_name);
        mesh.set_automatic_routing_id_prefix ("perf-actor-caller");
        mesh.listen (config.transport_endpoints.at ("mesh"));
        mesh.objects ().client ();
        if (config.mode == "send-send") {
            options.handlers ().group ("perf-return").add_send<correlation_return_handler_t> ();
            mesh.channel (*config.channel_name).server ().add_handler_group ("perf-return");
        }
        app.add_hosted_service (std::make_unique<prepare_service_t> (role_ref, [scenario] (const std::atomic<bool> &stopping) { scenario->prepare (stopping); }));
    });
}
} // namespace

int main (int argc, char **argv)
{
    auto config = perf::read_role_config (argc, argv);
    if (config.role != "actor-caller" || !config.source)
        throw std::invalid_argument ("ActorCallerServer runs the source role of §10.9 and §10.10.");
    const auto scenario = config.scenario;
    auto role = std::make_unique<perf::role_t> (std::move (config), true);
    role->objects->set (false, "Actors are not yet created and probed through the public API.", json::array ());
    if (scenario == "actor-no-bind-request-echo")
        return run<actor_no_bind_request_echo_scenario_t> (std::move (role));
    if (scenario == "actor-no-bind-send-send-echo") {
        role->correlations = std::make_unique<perf::send_send_correlation_t> (role->measurement, role->metrics);
        return run<actor_no_bind_send_send_echo_scenario_t> (std::move (role));
    }
    throw std::invalid_argument ("ActorCallerServer does not run scenario '" + scenario + "'.");
}

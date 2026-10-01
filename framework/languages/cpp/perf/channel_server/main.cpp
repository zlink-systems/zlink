/* SPDX-License-Identifier: FSL-1.1-ALv2 */

// The Channel role of §11.2 (source and target) and §10.3-§10.6. Which scenario runs is the role config's `scenario`; each
// scenario header states its own measured operation.

#include "channel_echo_only_scenario.hpp"
#include "s2s_channel_to_spot_request_echo_scenario.hpp"
#include "s2s_channel_to_spot_send_send_echo_scenario.hpp"
#include "s2s_return_to_spot_handler.hpp"

namespace
{
using namespace perf;

// The automatic RouteMesh node of an s2s Channel role (§10.3-§10.6); the caller adds object role and Channel membership.
fw::mesh_node_builder_t add_s2s_mesh (fw::zlink_framework_options_t &options, const role_config_t &config)
{
    auto mesh = options.add_route_mesh (*config.mesh_name);
    mesh.set_automatic_routing_id_prefix ("perf-channel");
    mesh.listen (config.transport_endpoints.at ("mesh"));
    return mesh;
}

// A source Channel role: the scenario prepares and drives; the workload is what `/app/perf/start` launches. `configure` adds the
// topology of this scenario to the shared framework options.
template <typename TScenario, typename TConfigure> int run_source (std::unique_ptr<role_t> role, TConfigure configure)
{
    auto &role_ref = *role;
    auto scenario = std::make_shared<TScenario> (role_ref);
    role_ref.workload = [scenario] (const loops_t &loops) { scenario->run (loops); };
    return run_role (std::move (role), [&, scenario, configure] (fw::zlink_framework_options_t &options, fw::app_t &app) {
        configure (options, role_ref.config);
        app.add_hosted_service (std::make_unique<prepare_service_t> (role_ref, [scenario] (const std::atomic<bool> &stopping) { scenario->prepare (stopping); }));
    });
}

// A target Channel role: it only echoes; the measured operation lives in the source process.
template <typename TConfigure> int run_target (std::unique_ptr<role_t> role, TConfigure configure)
{
    auto &role_ref = *role;
    return run_role (std::move (role), [&, configure] (fw::zlink_framework_options_t &options, fw::app_t &) { configure (options, role_ref.config); });
}

// §11.2: manual RouteMesh or ClientServer topology, no Store and no objects.
void configure_channel_echo_only (fw::zlink_framework_options_t &options, const role_config_t &config)
{
    options.handlers ().group ("perf").add<channel_echo_handler_t> ();
    if (config.topology == "routemesh") {
        auto mesh = options.add_route_mesh (*config.mesh_name);
        mesh.set_routing_id (zlink::routing_id_t::from (config.source ? "perf-channel-source" : "perf-channel-target"));
        mesh.listen (*config.listener_endpoint ());
        if (config.source) {
            mesh.channel (*config.channel_name).client ();
            mesh.peer_connections ().connect (zlink::routing_id_t::from ("perf-channel-target"), *config.peer_endpoint);
        }
        else
            mesh.channel (*config.channel_name).server ().add_handler_group ("perf");
    }
    else if (config.topology == "clientserver") {
        auto channel = options.add_client_server_channel (*config.channel_name);
        if (config.source)
            channel.client ().connect (*config.peer_endpoint);
        else
            channel.server ().listen (static_cast<std::uint16_t> (port_of (*config.listener_endpoint ()))).add_handler_group ("perf");
    }
    else
        throw std::invalid_argument ("Unsupported channel topology.");
}
} // namespace

int main (int argc, char **argv)
{
    auto config = perf::read_role_config (argc, argv);
    if (config.role != "channel")
        throw std::invalid_argument ("ChannelServer runs the channel role.");
    const auto scenario = config.scenario;
    const bool source = config.source;
    // Only a source Object Client reports objectsReady (the Spots it found); every other role has no objects of its own.
    auto role = std::make_unique<perf::role_t> (std::move (config), source && scenario != "channel-echo-only");

    if (scenario == "channel-echo-only")
        return source ? run_source<channel_echo_only_scenario_t> (std::move (role), configure_channel_echo_only)
                      : run_target (std::move (role), configure_channel_echo_only);
    if (scenario == "s2s-channel-to-spot-request-echo" && source)
        return run_source<s2s_channel_to_spot_request_echo_scenario_t> (std::move (role), [] (fw::zlink_framework_options_t &options, const role_config_t &config) {
            add_s2s_mesh (options, config).objects ().client ();
        });
    if (scenario == "s2s-channel-to-spot-send-send-echo" && source) {
        role->correlations = std::make_unique<perf::send_send_correlation_t> (role->measurement, role->metrics);
        return run_source<s2s_channel_to_spot_send_send_echo_scenario_t> (std::move (role), [] (fw::zlink_framework_options_t &options, const role_config_t &config) {
            options.handlers ().group ("perf-return").add_send<correlation_return_handler_t> ();
            auto mesh = add_s2s_mesh (options, config);
            mesh.objects ().client ();
            mesh.channel (*config.channel_name).server ().add_handler_group ("perf-return");
        });
    }
    // The echo targets of §10.5 and §10.6: the measured operation lives in the Spot process.
    if (scenario == "s2s-spot-to-channel-request-echo" && !source)
        return run_target (std::move (role), [] (fw::zlink_framework_options_t &options, const role_config_t &config) {
            options.handlers ().group ("perf").add<channel_echo_handler_t> ();
            add_s2s_mesh (options, config).channel (*config.channel_name).server ().add_handler_group ("perf");
        });
    if (scenario == "s2s-spot-to-channel-send-send-echo" && !source)
        return run_target (std::move (role), [] (fw::zlink_framework_options_t &options, const role_config_t &config) {
            options.handlers ().group ("perf-return-to-spot").add_send<s2s_return_to_spot_handler_t> ();
            auto mesh = add_s2s_mesh (options, config);
            mesh.objects ().client ();
            mesh.channel (*config.channel_name).server ().add_handler_group ("perf-return-to-spot");
        });
    throw std::invalid_argument ("ChannelServer does not run scenario " + scenario + ".");
}

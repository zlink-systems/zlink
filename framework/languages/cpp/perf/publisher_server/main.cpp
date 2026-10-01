/* SPDX-License-Identifier: FSL-1.1-ALv2 */

// The Publisher role of §10.11: automatic Classic fanout, the publisher listens on the reserved endpoint and publishes its
// descriptor to the run Store.

#include "pubsub_fanout_echo_scenario.hpp"

int main (int argc, char **argv)
{
    auto config = perf::read_role_config (argc, argv);
    if (config.scenario != "pubsub-fanout-echo" || config.role != "publisher" || !config.source)
        throw std::invalid_argument ("PublisherServer runs the publisher role of pubsub-fanout-echo.");
    auto role = std::make_unique<perf::role_t> (std::move (config), true);
    role->objects->set (false, "The Publisher host is not Ready yet.", perf::json::array ());
    auto &role_ref = *role;
    auto scenario = std::make_shared<perf::pubsub_fanout_echo_scenario_t> (role_ref, perf::cell_directory_of (argv[2]));
    role_ref.workload = [scenario] (const perf::loops_t &loops) { scenario->run (loops); };
    return perf::run_role (std::move (role), [&, scenario] (fw::zlink_framework_options_t &options, fw::app_t &app) {
        options.add_fanout_channel (*role_ref.config.channel_name)
          .enable_publisher (role_ref.config.transport_endpoints.at ("fanout"))
                    .set_automatic_routing_id_prefix ("perf-publisher");
        app.add_hosted_service (std::make_unique<perf::prepare_service_t> (role_ref, [scenario] (const std::atomic<bool> &stopping) { scenario->prepare (stopping); }));
    });
}

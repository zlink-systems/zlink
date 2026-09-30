/* SPDX-License-Identifier: FSL-1.1-ALv2 */

// The Spot role of §10.3-§10.8. Which scenario runs is the role config's `scenario`; each scenario header states its own
// measured operation.

#include "s2s_channel_to_spot_target.hpp"
#include "s2s_spot_to_channel_request_echo_scenario.hpp"
#include "s2s_spot_to_channel_send_send_echo_scenario.hpp"
#include "spot_no_await_echo_scenario.hpp"
#include "spot_worker_offload_echo_scenario.hpp"

namespace
{
using namespace perf;

// A source Spot role: the scenario prepares and drives; the workload is what `/app/perf/start` launches.
template <typename TSpot, typename TScenario>
int run_source (std::unique_ptr<role_t> role, bool calls_channel, bool worker = false)
{
    auto &role_ref = *role;
    auto scenario = std::make_shared<TScenario> (role_ref);
    role_ref.workload = [scenario] (const loops_t &loops) { scenario->run (loops); };
    return run_role (std::move (role), [&, scenario, calls_channel, worker] (fw::zlink_framework_options_t &options, fw::app_t &app) {
        if (worker) {
            // §5.2: only the public worker options; the C++ options carry no queue length.
            const auto &config = *role_ref.config.worker;
            options.worker ().max_threads (static_cast<std::size_t> (config.max_threads));
            options.worker ().min_threads (static_cast<std::size_t> (config.min_threads));
            options.worker ().idle_timeout (std::chrono::milliseconds (config.idle_timeout_ms));
        }
        add_spot_role<TSpot> (options, role_ref.config, calls_channel);
        app.add_hosted_service (std::make_unique<prepare_service_t> (role_ref, [scenario] (const std::atomic<bool> &stopping) { scenario->prepare (stopping); }));
    });
}

// A target Spot role (§10.3, §10.4): Object Server only; the source Channel process measures.
template <typename TSpot> int run_target (std::unique_ptr<role_t> role, bool calls_channel)
{
    auto &role_ref = *role;
    role_ref.metrics.spot_internals_unsupported ();
    return run_role (std::move (role), [&, calls_channel] (fw::zlink_framework_options_t &options, fw::app_t &app) {
        add_spot_role<TSpot> (options, role_ref.config, calls_channel);
        app.add_hosted_service (std::make_unique<prepare_service_t> (role_ref, [&role_ref] (const std::atomic<bool> &stopping) { prepare_spot_target (role_ref, stopping); }));
    });
}
} // namespace

int main (int argc, char **argv)
{
    auto config = perf::read_role_config (argc, argv);
    if (config.role != "spot")
        throw std::invalid_argument ("SpotServer runs the spot role.");
    const auto scenario = config.scenario;
    const bool source = config.source;
    if ((scenario == "spot-worker-offload-echo") && !config.worker)
        throw std::invalid_argument ("The Spot role with worker config is the source of this scenario.");
    auto role = std::make_unique<perf::role_t> (std::move (config), true);
    if (scenario == "s2s-channel-to-spot-request-echo")
        return run_target<perf_echo_spot_t> (std::move (role), false);
    if (scenario == "s2s-channel-to-spot-send-send-echo")
        return run_target<s2s_send_echo_spot_t> (std::move (role), true); // the echo goes to the caller return ChannelName
    if (!source)
        throw std::invalid_argument ("The Spot role is the source of this scenario.");
    if (scenario == "s2s-spot-to-channel-request-echo")
        return run_source<s2s_remote_request_spot_t, s2s_spot_to_channel_request_echo_scenario_t> (std::move (role), true);
    if (scenario == "s2s-spot-to-channel-send-send-echo") {
        role->correlations = std::make_unique<perf::send_send_correlation_t> (role->measurement, role->metrics);
        return run_source<s2s_send_send_spot_t, s2s_spot_to_channel_send_send_echo_scenario_t> (std::move (role), true);
    }
    if (scenario == "spot-no-await-echo")
        return run_source<perf_echo_spot_t, spot_no_await_echo_scenario_t> (std::move (role), false);
    if (scenario == "spot-worker-offload-echo")
        return run_source<spot_worker_offload_spot_t, spot_worker_offload_echo_scenario_t> (std::move (role), false, true);
    throw std::invalid_argument ("SpotServer does not run scenario " + scenario + ".");
}

/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "../Support/scenario_context.hpp"

namespace zlink::framework::e2e::observability_ops::client::scenarios
{
inline void run_obs_b3_scenario (const verification_input_t &input)
{
    const auto publisher = read_json (input, "publisherEvidence");
    const auto subscriber = read_json (input, "subscriberEvidence");
    require (metrics_named (publisher, "zlink.fanout.published").empty ()
               && metrics_named (publisher, "zlink.fanout.received").empty ()
               && metrics_named (publisher, "zlink.fanout.dropped").empty ()
               && metrics_named (subscriber, "zlink.fanout.published").empty ()
               && metrics_named (subscriber, "zlink.fanout.received").empty ()
               && metrics_named (subscriber, "zlink.fanout.dropped").empty (),
             "OBS-B3 exposed forbidden publish-specific metrics");
    require_bounded_metric_labels (publisher,
                                   "OBS-B3 publisher metric has a forbidden label");
    require_bounded_metric_labels (subscriber,
                                   "OBS-B3 subscriber metric has a forbidden label");
    const auto lateness = metrics_named (publisher, "zlink.location.owner_lease.renew.lateness");
    require (std::any_of (lateness.begin (), lateness.end (), [] (const auto &metric) {
                 return metric.at ("value").template get<double> () >= 0.5;
             }),
             "OBS-B3 external Redis delay did not produce lease renewal lateness");
}
} // namespace zlink::framework::e2e::observability_ops::client::scenarios

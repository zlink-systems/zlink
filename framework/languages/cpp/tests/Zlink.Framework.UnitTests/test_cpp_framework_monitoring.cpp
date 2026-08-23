/* SPDX-License-Identifier: MPL-2.0 */

#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/diagnostics/runtime_metrics.hpp"

#include <zlink/framework.hpp>

#include <cstddef>
#include <iostream>
#include <memory>
#include <string>

namespace
{

bool unsubscribed_metrics_remain_disabled ()
{
    auto state = std::make_shared<zlink::framework::detail::monitoring_runtime_state_t> ();
    const zlink::framework::runtime::runtime_metrics_t metrics (state);

    for (std::size_t index = 0; index < 10'000; ++index) {
        metrics.counter ("zlink.test.unsubscribed", "{operation}", 1,
                         {{"operation", "request"}});
    }

    // A disabled logger is the entire metric admission boundary. Emission must
    // remain a no-op without creating a second metric storage path.
    return !metrics.enabled ();
}

//  OBS-B4 unit counterpart. .NET gets this from the platform: a Meter with no
//  subscribed MeterListener records nothing, so emission cannot grow storage.
//  The C++ runtime owns its own metric path, so the same guarantee has to be
//  proven here. Emitting without a reader must not create or retain any metric
//  storage, which shows up as a stable state ownership count and a logger that
//  never becomes enabled.
bool unsubscribed_metric_storage_unchanged ()
{
    auto state = std::make_shared<zlink::framework::detail::monitoring_runtime_state_t> ();
    const zlink::framework::runtime::runtime_metrics_t metrics (state);

    const long baseline_owners = state.use_count ();
    if (metrics.enabled ()) {
        std::cerr << "metric storage baseline is not an unsubscribed reader\n";
        return false;
    }

    for (std::size_t index = 0; index < 50'000; ++index) {
        metrics.counter ("zlink.test.storage", "{operation}", 1,
                         {{"operation", "request"}, {"index", std::to_string (index)}});
        metrics.updown ("zlink.test.storage.active", "{operation}", 1,
                        {{"operation", "request"}});
    }

    //  A retained sample, sink, or observer would have to hold the state, so a
    //  changed owner count is the observable form of grown storage.
    if (state.use_count () != baseline_owners) {
        std::cerr << "unsubscribed metric emission retained state owners: "
                  << baseline_owners << " -> " << state.use_count () << '\n';
        return false;
    }
    if (metrics.enabled ()) {
        std::cerr << "unsubscribed metric emission enabled the reader path\n";
        return false;
    }
    return true;
}

bool public_spot_event_surface_reports_internal_timer_failure ()
{
    zlink::framework::monitoring_builder_t monitoring;
    std::vector<zlink::framework::spot_event_t> observed;
    monitoring
      .add_spot_events ("zone-node-1")
      .on_spot_event (
        [&observed] (const zlink::framework::spot_event_t &event) {
            observed.push_back (event);
        });
    const auto runtime = zlink::framework::detail::monitoring_runtime_t::from (
      monitoring);
    runtime.publish_timer_failure (
      "unregistered-node", zlink::framework::spot_id_t ("zone-ignored"),
      zlink::framework::timer_failure_event_t{
        "ignored", std::type_index (typeid (int)), 1, false,
        "ignored"});
    runtime.publish_timer_failure (
      "zone-node-1", zlink::framework::spot_id_t ("zone-a"),
      zlink::framework::timer_failure_event_t{
        "zone-tick", std::type_index (typeid (int)), 7, true,
        "boom"});
    return observed.size () == 1
           && observed.front ().source_name == "zone-node-1"
           && observed.front ().event
                == zlink::framework::spot_event_kind_t::
                     timer_stopped_after_unhandled_exception
           && observed.front ().diagnostic.spot_id
                == zlink::framework::spot_id_t ("zone-a")
           && observed.front ().diagnostic.timer_name == "zone-tick"
           && observed.front ().diagnostic.delivery_index == 7
           && observed.front ().diagnostic.message == "boom";
}

} // namespace

int main ()
{
    if (!unsubscribed_metrics_remain_disabled ()) {
        std::cerr << "unsubscribed metric emission unexpectedly became enabled\n";
        return 1;
    }
    if (!unsubscribed_metric_storage_unchanged ()) {
        return 1;
    }
    if (!public_spot_event_surface_reports_internal_timer_failure ()) {
        std::cerr << "public Spot monitoring event was not delivered\n";
        return 1;
    }
    return 0;
}

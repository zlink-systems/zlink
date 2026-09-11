/* SPDX-License-Identifier: MPL-2.0 */

#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/diagnostics/runtime_metrics.hpp"
#include "runtime/dispatch/host_capacity_runtime.hpp"
#include "metric_test_reader.hpp"

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

    // The standard no-op MeterProvider never retains instrument or sample state.
    return !metrics.enabled () && state->metric_instruments.empty ();
}

// OBS-B4: a no-op standard provider must not retain instrumentation storage.
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

    if (state.use_count () != baseline_owners || !state->metric_instruments.empty ()) {
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

double metric_value (const std::vector<metric_test::sdk::MetricData> &data,
                     const std::string &name,
                     const std::string &unit,
                     const std::string &state = "")
{
    for (const auto &metric : data) {
        if (metric.instrument_descriptor.name_ != name)
            continue;
        if (metric.instrument_descriptor.unit_ != unit)
            throw std::runtime_error ("unexpected metric unit: " + name);
        for (const auto &point : metric.point_data_attr_) {
            const auto label = point.attributes.find ("state");
            if (!state.empty ()
                && (label == point.attributes.end ()
                    || opentelemetry::nostd::get<std::string> (label->second) != state))
                continue;
            if (const auto *sum =
                  opentelemetry::nostd::get_if<metric_test::sdk::SumPointData> (&point.point_data))
                return opentelemetry::nostd::get<double> (sum->value_);
            if (const auto *gauge =
                  opentelemetry::nostd::get_if<metric_test::sdk::LastValuePointData> (
                    &point.point_data))
                return opentelemetry::nostd::get<double> (gauge->value_);
            if (const auto *histogram =
                  opentelemetry::nostd::get_if<metric_test::sdk::HistogramPointData> (
                    &point.point_data))
                return static_cast<double> (histogram->count_);
        }
    }
    throw std::runtime_error ("missing metric: " + name + " state=" + state);
}

bool standard_provider_records_without_logging ()
{
    namespace fw = zlink::framework;
    metric_test::provider_t provider;
    auto state = std::make_shared<fw::detail::monitoring_runtime_state_t> ();
    fw::logging_builder_t logging;
    std::size_t logs = 0;
    logging.use_provider ("metrics-test", [&] (const fw::log_record_t &) { ++logs; });
    state->diagnostics_logger = logging.create_logger ("metrics-test");
    const fw::runtime::runtime_metrics_t metrics (state);
    if (!metrics.enabled () || state->diagnostics_logger.is_enabled (fw::log_level_t::debug))
        return false;
    double count = 0;
    for (const auto level : {fw::log_level_t::info, fw::log_level_t::warn, fw::log_level_t::error,
                             fw::log_level_t::critical, fw::log_level_t::off}) {
        logging.set_min_level (level);
        ++count;
        metrics.counter ("zlink.test.events", "{event}", 1, {{"operation", "request"}});
        metrics.updown ("zlink.test.active", "{operation}", 2);
        metrics.updown ("zlink.test.active", "{operation}", -1);
        metrics.histogram ("zlink.test.duration", "s", 0.25);
        metrics.observable ("zlink.test.current", "{operation}", count);
        const auto data = provider.collect ();
        if (metric_value (data, "zlink.test.events", "{event}") != count
            || metric_value (data, "zlink.test.active", "{operation}") != count
            || metric_value (data, "zlink.test.duration", "s") != count
            || metric_value (data, "zlink.test.current", "{operation}") != count)
            return false;
    }
    return logs == 0;
}

bool capacity_collection_preserves_epoch_without_status_queries ()
{
    namespace fw = zlink::framework;
    metric_test::provider_t provider;
    fw::logging_builder_t logging;
    auto monitoring = std::make_shared<fw::detail::monitoring_runtime_state_t> ();
    monitoring->diagnostics_logger = logging.create_logger ("capacity-test");
    auto context = std::make_shared<zlink::context_t> ();
    auto jobs = std::make_shared<fw::runtime::application_job_queue_t> (
      fw::runtime::application_job_queue_configuration_t{
        fw::application_job_queue_profile_t::balanced, std::uint32_t{1}, 1, 1});
    fw::runtime::host_capacity_runtime_t capacity (context, jobs, std::nullopt, std::nullopt,
                                                   fw::core_hwm_profile_t::balanced, monitoring);

    auto permit = jobs->try_reserve_supply ();
    if (!permit)
        return false;
    permit->mark_queued ();
    auto waiter = jobs->wait_for_supply ([] (auto) {});
    if (!waiter.cancel ())
        return false;
    // No status query and no collection occurred while the queue accumulated.
    // Lowering logging and repeating collection must not reset or double count.
    for (const auto level : {fw::log_level_t::info, fw::log_level_t::off}) {
        logging.set_min_level (level);
        const auto data = provider.collect ();
        if (metric_value (data, "zlink.host.application_job_queue.capacity_waits", "{wait}") != 1
            || metric_value (data, "zlink.host.application_job_queue.jobs", "{job}", "in_use") != 1
            || metric_value (data, "zlink.host.application_job_queue.pressure_transitions",
                             "{transition}", "paused")
                 != 1
            || metric_value (data, "zlink.host.application_job_queue.pressure_state", "{state}",
                             "paused")
                 != 1)
            return false;
    }
    capacity.reset_metrics ();
    const auto reset = provider.collect ();
    if (metric_value (reset, "zlink.host.application_job_queue.capacity_waits", "{wait}") != 0
        || metric_value (reset, "zlink.host.application_job_queue.pressure_transitions",
                         "{transition}", "paused")
             != 0
        || metric_value (reset, "zlink.host.application_job_queue.jobs", "{job}", "in_use") != 1
        || metric_value (reset, "zlink.host.application_job_queue.jobs", "{job}", "peak") != 1
        || metric_value (reset, "zlink.host.application_job_queue.pressure_state", "{state}",
                         "paused")
             != 1)
        return false;
    permit->release_for_handler_entry ();
    return metric_value (provider.collect (),
                         "zlink.host.application_job_queue.pressure_transitions", "{transition}",
                         "running")
           == 1;
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
    if (!standard_provider_records_without_logging ()) {
        std::cerr << "standard metric recording depends on logging\n";
        return 1;
    }
    if (!capacity_collection_preserves_epoch_without_status_queries ()) {
        std::cerr << "capacity collection lost current or epoch values\n";
        return 1;
    }
    if (!public_spot_event_surface_reports_internal_timer_failure ()) {
        std::cerr << "public Spot monitoring event was not delivered\n";
        return 1;
    }
    return 0;
}

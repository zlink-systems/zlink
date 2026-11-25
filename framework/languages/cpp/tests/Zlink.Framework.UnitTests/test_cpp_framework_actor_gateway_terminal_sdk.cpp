/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_gateway_cases.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/runtime_metric_contract.hpp"
#include "runtime/diagnostics/runtime_metrics.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include <opentelemetry/sdk/common/global_log_handler.h>
#include <atomic>
#include <cassert>
#include <memory>
#include <set>
#include <string>
#include <utility>

namespace zlink::framework::test::actor_gateway
{
// The SDK owns the global log handler. This focused test restores both settings
// after capturing actual SDK warnings, including duplicate instrument kinds.
class scoped_sdk_warnings_t
{
    using log = opentelemetry::sdk::common::internal_log::GlobalLogHandler;
    using level_t = opentelemetry::sdk::common::internal_log::LogLevel;
    using handler_t = opentelemetry::sdk::common::internal_log::LogHandler;
    class capture_t final : public handler_t
    {
      public:
        opentelemetry::nostd::shared_ptr<handler_t> previous;
        std::atomic_uint warnings{0};
        void Handle (level_t level,
                     const char *file,
                     int line,
                     const char *message,
                     const opentelemetry::sdk::common::AttributeMap &attributes) noexcept override
        {
            if (level == level_t::Warning)
                ++warnings;
            if (previous)
                previous->Handle (level, file, line, message, attributes);
        }
    };
    opentelemetry::nostd::shared_ptr<handler_t> _previous = log::GetLogHandler ();
    level_t _previous_level = log::GetLogLevel ();
    std::shared_ptr<capture_t> _capture = std::make_shared<capture_t> ();

  public:
    scoped_sdk_warnings_t ()
    {
        _capture->previous = _previous;
        log::SetLogHandler (opentelemetry::nostd::shared_ptr<handler_t> (
          std::static_pointer_cast<handler_t> (_capture)));
        log::SetLogLevel (level_t::Warning);
    }
    ~scoped_sdk_warnings_t ()
    {
        log::SetLogLevel (_previous_level);
        log::SetLogHandler (_previous);
    }
    unsigned count () const { return _capture->warnings.load (); }
};

// Controlled metric inputs test SDK registration and lifetime. In particular,
// backpressure=1 here does not represent a native backpressure drop.
int actor_terminal_sdk_controlled_counter_projection ()
{
    using namespace zlink::framework;
    namespace runtime = zlink::framework::runtime;
    scoped_sdk_warnings_t warnings;
    metric_test::provider_t provider;
    runtime::mesh::raw_mesh_node_options_t options;
    options.descriptor.mesh_name = "receiver-mesh";
    options.descriptor.node_routing_id = {'r', 'e', 'c', 'v'};
    options.descriptor.lifecycle_generation = options.descriptor.descriptor_revision = 1;
    options.descriptor.advertised_endpoint = "tcp://127.0.0.1:0";
    auto owner = std::make_unique<runtime::mesh::raw_mesh_node_owner_t> (
      test::runtime_failure_options (std::move (options)));
    runtime::runtime_metrics_t metrics (test::runtime_failure_monitoring ());
    const std::set<std::string> reasons{"backpressure", "stale_target", "no_handler"};
    for (const auto &reason : reasons)
        metrics.counter ("zlink.mesh_node.messages.dropped", "{message}", 1,
                         {{"mesh_name", "receiver-mesh"},
                          {"surface", "actor"},
                          {"message_kind", "send"},
                          {"reason", reason}});
    const auto verify_cumulative = [&] {
        const auto data = provider.collect ();
        metric_test::validate_metric_contract (data);
        std::set<std::string> observed;
        for (const auto &metric : data) {
            if (metric.instrument_descriptor.name_ != "zlink.mesh_node.messages.dropped")
                continue;
            assert (metric.instrument_descriptor.type_
                    == metric_test::sdk::InstrumentType::kCounter);
            assert (metric.instrument_descriptor.unit_ == "{message}");
            assert (metric.aggregation_temporality
                    == metric_test::sdk::AggregationTemporality::kCumulative);
            for (const auto &point : metric.point_data_attr_) {
                const auto &sum =
                  opentelemetry::nostd::get<metric_test::sdk::SumPointData> (point.point_data);
                assert (sum.is_monotonic_);
                assert (opentelemetry::nostd::get<double> (sum.value_) == 1);
                const auto label = [&] (const char *key) {
                    return opentelemetry::nostd::get<std::string> (point.attributes.at (key));
                };
                assert (point.attributes.size () == 4);
                assert (label ("mesh_name") == "receiver-mesh");
                assert (label ("surface") == "actor");
                assert (label ("message_kind") == "send");
                const auto reason = label ("reason");
                assert (reasons.contains (reason));
                assert (observed.insert (reason).second);
            }
        }
        assert (observed == reasons);
    };
    // Check the warning first so the pre-fix run fails for the SDK collision.
    if (warnings.count () != 0)
        return 1;
    verify_cumulative ();
    verify_cumulative ();
    owner.reset ();
    verify_cumulative ();
    return warnings.count () == 0 ? 0 : 1;
}
} // namespace zlink::framework::test::actor_gateway

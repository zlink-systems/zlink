/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include "bench_metric_reader.hpp"
#include "bench_stats_server.hpp"
#include <opentelemetry/metrics/observer_result.h>
#include <opentelemetry/metrics/provider.h>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace
{
struct observable_drop_t
{
    double value = 0.0;
};

void observe_drop (opentelemetry::metrics::ObserverResult result, void *state)
{
    const auto observer = opentelemetry::nostd::get<
      opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObserverResultT<double>>> (result);
    const auto *drop = static_cast<observable_drop_t *> (state);
    observer->Observe (drop->value, {{"mesh_name", "bench"},
                                     {"surface", "node"},
                                     {"message_kind", "send"},
                                     {"reason", "backpressure"}});
}

} // namespace

int main ()
{
    zlink_cpp_bench::server_metrics_t metrics;
    const auto rejected = [&] {
        return nlohmann::json::parse (metrics.snapshot_json ()).at ("rejected").get<long long> ();
    };
    const auto require = [] (bool condition) {
        if (!condition) throw std::runtime_error ("incorrect benchmark rejection accounting");
    };
    require (rejected () == 0);
    metrics.observe_rejected_total (7);
    require (rejected () == 7);
    metrics.reset (); // Exclude settled warmup rejections from the active window.
    require (rejected () == 0);
    metrics.observe_rejected_total (7); // Repeated cumulative snapshot.
    metrics.observe_rejected_total (5); // A concurrent, older snapshot arrives later.
    require (rejected () == 0);
    metrics.observe_rejected_total (11);
    require (rejected () == 4);
    metrics.observe_rejected_total (11);
    require (rejected () == 4);
    metrics.reset ();
    metrics.observe_rejected_total (12);
    require (rejected () == 1);

    zlink_cpp_bench::server_metrics_t unavailable (std::nullopt);
    const auto snapshot = [&] {
        return nlohmann::json::parse (unavailable.snapshot_json ()).at ("rejected");
    };
    require (snapshot ().is_null ());
    unavailable.reset ();
    require (snapshot ().is_null ());
    unavailable.observe_rejected_total (7);
    require (snapshot ().is_null ()); // An unknown warmup baseline is not zero.
    unavailable.reset ();
    require (snapshot () == 0);
    unavailable.observe_rejected_total (11);
    require (snapshot () == 4);

    zlink_cpp_bench::bench_metric_reader_t reader;
    const auto read_drop = [&] {
        return reader.collect_cumulative_sum ("zlink.mesh_node.messages.dropped",
                                              {{"mesh_name", "bench"},
                                               {"surface", "node"},
                                               {"message_kind", "send"},
                                               {"reason", "backpressure"}});
    };
    require (!read_drop ().has_value ()); // No measurement must stay unavailable, not become zero.

    auto meter = opentelemetry::metrics::Provider::GetMeterProvider ()->GetMeter ("bench-test");
    auto dropped = meter->CreateDoubleObservableCounter ("zlink.mesh_node.messages.dropped",
                                                         "one-way messages dropped", "{message}");
    observable_drop_t observed;
    dropped->AddCallback (observe_drop, &observed);
    require (read_drop () == 0.0); // An emitted zero is distinguishable from no measurement.
    observed.value = 7.0;
    require (read_drop () == 7.0);
    observed.value = 11.0;
    require (read_drop () == 11.0); // The manual reader keeps the SDK cumulative value.
    require (!reader.collect_cumulative_sum ("zlink.mesh_node.messages.dropped",
                                             {{"mesh_name", "bench"},
                                              {"surface", "node"},
                                              {"message_kind", "send"},
                                              {"reason", "shutdown"}}));
    dropped->RemoveCallback (observe_drop, &observed);
}

/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include "bench_stats_server.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

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
}

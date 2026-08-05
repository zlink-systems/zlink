#include "perf_multi_client_helpers.hpp"
#include "perf_multi_weighted_latency.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

namespace
{

bool close_to (double actual, double expected)
{
    return std::fabs (actual - expected) < 0.000001;
}

void test_exact_sampler_statistics ()
{
    bench_latency_sampler_t samples;
    double sum = 0.0;
    for (int value = 1; value <= 100; ++value) {
        samples.add (static_cast<double> (value));
        sum += value;
    }

    bench_latency_stats_t stats;
    perf_multi_client::normalize_latency_stats (sum, 100, &samples, &stats);

    assert (close_to (stats.mean_ns, 50.5));
    assert (close_to (stats.p95_ns, 95.05));
    assert (close_to (stats.p99_ns, 99.01));
}

void test_percentile_is_not_clamped_to_mean ()
{
    bench_latency_sampler_t samples;
    double sum = 0.0;
    for (int i = 0; i < 999; ++i) {
        samples.add (1.0);
        sum += 1.0;
    }
    samples.add (1000000.0);
    sum += 1000000.0;

    bench_latency_stats_t stats;
    perf_multi_client::normalize_latency_stats (sum, 1000, &samples, &stats);

    assert (close_to (stats.mean_ns, 1000.999));
    assert (close_to (stats.p95_ns, 1.0));
    assert (close_to (stats.p99_ns, 1.0));
}

void test_weighted_child_aggregation ()
{
    std::vector<perf_multi_latency::weighted_sample_t> samples;
    samples.push_back ({1.0, 50.0});
    samples.push_back ({2.0, 50.0});
    samples.push_back ({100.0, 1.0});

    const bench_latency_stats_t stats =
      perf_multi_latency::aggregate (101, 250.0, &samples);

    assert (close_to (stats.mean_ns, 250.0 / 101.0));
    assert (close_to (stats.p95_ns, 2.0));
    assert (close_to (stats.p99_ns, 2.0));
}

void test_count_duration_and_bandwidth ()
{
    const double one_way = throughput_per_second (250, 2.5);
    assert (close_to (one_way, 100.0));
    assert (close_to (
      bandwidth_mb_per_second ("MULTI_PUBSUB", 1000, one_way), 0.1));
    assert (close_to (throughput_per_second (250, 0.0), 0.0));
}

void test_echo_latency_uses_one_way_estimate ()
{
    assert (close_to (
      latency_sample_ns ("MULTI_ROUTER_ROUTER_REQREP", 200), 100.0));
    assert (close_to (latency_sample_ns ("MULTI_PUBSUB", 200), 200.0));
}

void set_multi_hwm_env (const char *value)
{
#if defined(_WIN32)
    _putenv_s ("PERF_MULTI_HWM", value ? value : "");
#else
    if (value)
        setenv ("PERF_MULTI_HWM", value, 1);
    else
        unsetenv ("PERF_MULTI_HWM");
#endif
}

void test_hwm_parser_preserves_uint64_range ()
{
    set_multi_hwm_env ("4294967296");
    assert (resolve_bench_settings ().hwm == UINT64_C (4294967296));
    set_multi_hwm_env ("18446744073709551615");
    assert (resolve_bench_settings ().hwm == std::numeric_limits<uint64_t>::max ());
    set_multi_hwm_env (NULL);
}

}

int main ()
{
    test_exact_sampler_statistics ();
    test_percentile_is_not_clamped_to_mean ();
    test_weighted_child_aggregation ();
    test_count_duration_and_bandwidth ();
    test_echo_latency_uses_one_way_estimate ();
    test_hwm_parser_preserves_uint64_range ();
    return 0;
}

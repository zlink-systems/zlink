#include "perf_multi_client_helpers.hpp"
#include "perf_multi_relay_server.hpp"
#include "perf_multi_weighted_latency.hpp"
#include "../../common/streamclient/perf_stream_common.hpp"

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

void set_latency_sample_cap_env (const char *value)
{
#if defined(_WIN32)
    _putenv_s ("PERF_MULTI_LATENCY_SAMPLE_CAP", value ? value : "");
#else
    if (value)
        setenv ("PERF_MULTI_LATENCY_SAMPLE_CAP", value, 1);
    else
        unsetenv ("PERF_MULTI_LATENCY_SAMPLE_CAP");
#endif
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

void test_snapshot_does_not_reorder_reservoir ()
{
    bench_latency_sampler_t samples;
    samples.add (30.0);
    samples.add (10.0);
    samples.add (20.0);

    std::vector<double> before;
    std::vector<double> after;
    samples.append_samples (&before);
    const bench_latency_stats_t stats = samples.snapshot ();
    samples.append_samples (&after);

    assert (before == after);
    assert (close_to (stats.mean_ns, 20.0));
}

void test_sampler_honors_child_and_environment_caps ()
{
    set_latency_sample_cap_env ("65536");
    bench_latency_sampler_t child_samples (1024);
    for (int value = 1; value <= 4096; ++value)
        child_samples.add (static_cast<double> (value));

    std::vector<double> before;
    std::vector<double> after;
    child_samples.append_samples (&before);
    assert (child_samples.count () == 4096);
    assert (close_to (child_samples.sum_ns (), 8390656.0));
    assert (before.size () == 1024);
    bool contains_late_sample = false;
    for (size_t i = 0; i < before.size (); ++i) {
        if (before[i] > 1024.0) {
            contains_late_sample = true;
            break;
        }
    }
    assert (contains_late_sample);
    (void) child_samples.snapshot ();
    child_samples.append_samples (&after);
    assert (before == after);

    set_latency_sample_cap_env ("512");
    bench_latency_sampler_t environment_limited (1024);
    for (int value = 1; value <= 4096; ++value)
        environment_limited.add (static_cast<double> (value));
    std::vector<double> limited;
    environment_limited.append_samples (&limited);
    assert (limited.size () == 512);

    set_latency_sample_cap_env (NULL);
}

void test_zero_sample_cap_preserves_exact_mean ()
{
    set_latency_sample_cap_env ("0");
    bench_latency_sampler_t samples;
    samples.add (10.0);
    samples.add (20.0);
    const bench_latency_stats_t stats = samples.snapshot ();
    assert (samples.count () == 2);
    assert (close_to (stats.mean_ns, 15.0));
    assert (close_to (stats.p95_ns, 15.0));
    assert (close_to (stats.p99_ns, 15.0));
    set_latency_sample_cap_env (NULL);
}

void test_relay_submit_error_classification ()
{
    using namespace perf_multi_relay_server;
    assert (classify_reply_send_result (ZLINK_SUBMIT_BACKPRESSURED, 0)
            == reply_send_backpressured);
    assert (classify_reply_send_result (ZLINK_SUBMIT_INTERNAL_ERROR, EAGAIN)
            == reply_send_backpressured);
    assert (classify_reply_send_result (ZLINK_SUBMIT_NOT_CONNECTED, 0)
            == reply_send_stale_route);
    assert (classify_reply_send_result (ZLINK_SUBMIT_NOT_FOUND, 0)
            == reply_send_stale_route);
    assert (classify_reply_send_result (ZLINK_SUBMIT_NOT_FOUND, EAGAIN)
            == reply_send_stale_route);
    assert (classify_reply_send_result (ZLINK_SUBMIT_BACKPRESSURED, EHOSTUNREACH)
            == reply_send_backpressured);
    assert (classify_reply_send_result (ZLINK_SUBMIT_INTERNAL_ERROR, EHOSTUNREACH)
            == reply_send_stale_route);
    assert (classify_reply_send_result (ZLINK_SUBMIT_INTERNAL_ERROR, ENOTCONN)
            == reply_send_stale_route);
    assert (classify_reply_send_result (ZLINK_SUBMIT_INVALID_ARGUMENT, EINVAL)
            == reply_send_failed);
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

void test_weighted_sample_population_uses_latency_count ()
{
    const double child_values[] = {10.0, 20.0};
    std::vector<perf_multi_latency::weighted_sample_t> samples;
    assert (perf_multi_latency::append_weighted_samples (
      6, child_values, 2, &samples));
    assert (samples.size () == 2);
    assert (close_to (samples[0].weight, 3.0));
    assert (close_to (samples[1].weight, 3.0));
    assert (!perf_multi_latency::append_weighted_samples (
      1, child_values, 2, &samples));

    const bench_latency_stats_t stats =
      perf_multi_latency::aggregate (6, 90.0, &samples);
    assert (close_to (stats.mean_ns, 15.0));

    std::vector<perf_multi_latency::weighted_sample_t> no_percentiles;
    assert (perf_multi_latency::append_weighted_samples (
      6, NULL, 0, &no_percentiles));
    assert (no_percentiles.empty ());
    const bench_latency_stats_t fallback =
      perf_multi_latency::aggregate (6, 90.0, &no_percentiles);
    assert (close_to (fallback.mean_ns, 15.0));
    assert (close_to (fallback.p95_ns, 15.0));
    assert (close_to (fallback.p99_ns, 15.0));
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

void test_stream_active_completion_cutoff_and_echo_latency ()
{
    assert (perf_stream_common::perf_stream_is_active_completion (
      true, true, UINT64_C (999), UINT64_C (1000)));
    assert (!perf_stream_common::perf_stream_is_active_completion (
      true, true, UINT64_C (1000), UINT64_C (1000)));
    assert (!perf_stream_common::perf_stream_is_active_completion (
      true, false, UINT64_C (999), UINT64_C (1000)));
    assert (!perf_stream_common::perf_stream_is_active_completion (
      false, true, UINT64_C (999), UINT64_C (1000)));
    assert (close_to (perf_stream_common::perf_stream_echo_latency_ns (200), 100.0));
    assert (close_to (perf_stream_common::perf_stream_echo_mean_ns (600, 3), 100.0));
    assert (close_to (perf_stream_common::perf_stream_echo_mean_ns (600, 0), 0.0));
    const uint64_t stream_stamp_ns = perf_stream_common::perf_stream_now_ns ();
    const uint64_t stream_completion_ns = perf_stream_common::perf_stream_now_ns ();
    assert (stream_completion_ns >= stream_stamp_ns);
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
    set_latency_sample_cap_env (NULL);
    test_exact_sampler_statistics ();
    test_percentile_is_not_clamped_to_mean ();
    test_snapshot_does_not_reorder_reservoir ();
    test_sampler_honors_child_and_environment_caps ();
    test_zero_sample_cap_preserves_exact_mean ();
    test_relay_submit_error_classification ();
    test_weighted_child_aggregation ();
    test_weighted_sample_population_uses_latency_count ();
    test_count_duration_and_bandwidth ();
    test_echo_latency_uses_one_way_estimate ();
    test_stream_active_completion_cutoff_and_echo_latency ();
    test_hwm_parser_preserves_uint64_range ();
    return 0;
}

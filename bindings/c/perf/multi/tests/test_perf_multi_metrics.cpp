#include "perf_multi_client_helpers.hpp"
#include "perf_multi_relay_server.hpp"
#include "perf_multi_socket_reqrep.hpp"
#include "perf_multi_stream_session.hpp"
#include "perf_multi_weighted_latency.hpp"
#include "../../common/streamclient/perf_stream_common.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
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
    assert (classify_reply_send_result (ZLINK_SUBMIT_BACKPRESSURED, EAGAIN)
            == reply_send_backpressured);
    assert (classify_reply_send_result (ZLINK_SUBMIT_BACKPRESSURED, 0)
            == reply_send_failed);
    assert (classify_reply_send_result (ZLINK_SUBMIT_INTERNAL_ERROR, EAGAIN)
            == reply_send_failed);
    assert (classify_reply_send_result (ZLINK_SUBMIT_NOT_CONNECTED, 0)
            == reply_send_stale_route);
    assert (classify_reply_send_result (ZLINK_SUBMIT_NOT_FOUND, 0)
            == reply_send_stale_route);
    assert (classify_reply_send_result (ZLINK_SUBMIT_NOT_FOUND, EAGAIN)
            == reply_send_stale_route);
    assert (classify_reply_send_result (ZLINK_SUBMIT_BACKPRESSURED, EHOSTUNREACH)
            == reply_send_failed);
    assert (classify_reply_send_result (ZLINK_SUBMIT_INTERNAL_ERROR, EHOSTUNREACH)
            == reply_send_failed);
    assert (classify_reply_send_result (ZLINK_SUBMIT_INTERNAL_ERROR, ENOTCONN)
            == reply_send_failed);
    assert (classify_reply_send_result (ZLINK_SUBMIT_INVALID_ARGUMENT, EINVAL)
            == reply_send_failed);
}

void test_one_way_writable_retry_state ()
{
    using namespace perf_multi_client;
    assert (ZLINK_COMPLETION_WRITABLE == 3);
    assert (classify_send_result (ZLINK_SUBMIT_OK, 0, 0) == send_ok);
    assert (classify_send_result (ZLINK_SUBMIT_BACKPRESSURED, EAGAIN, 7)
            == send_blocked);
    assert (classify_send_result (ZLINK_SUBMIT_BACKPRESSURED, EAGAIN, 0)
            == send_error);
    assert (classify_send_result (ZLINK_SUBMIT_OK, 0, 7) == send_error);

    send_wait_slot_t slot;
    slot.socket = reinterpret_cast<void *> (1);
    slot.retained = true;
    slot.retained_payload.push_back ('x');
    slot.retained_payload_size = 1;
    slot.wait_token = 7;

    zlink_completion_t admitted;
    std::memset (&admitted, 0, sizeof (admitted));
    admitted.struct_size = sizeof (admitted);
    admitted.kind = ZLINK_COMPLETION_WRITABLE;
    admitted.completion_id = 7;
    admitted.user_context = slot.socket;
    admitted.send_result = ZLINK_SEND_ADMITTED;
    assert (record_writable_completion (&slot, admitted));
    assert (slot.retained);
    assert (slot.retry_ready);
    assert (slot.wait_token == 0);

    slot.retry_ready = false;
    slot.wait_token = 8;
    zlink_completion_t wrong_id = admitted;
    wrong_id.completion_id = 9;
    assert (!record_writable_completion (&slot, wrong_id));
    assert (slot.retained);
    assert (slot.wait_token == 8);
    assert (errno == EPROTO);

    zlink_completion_t terminal = admitted;
    terminal.completion_id = 8;
    terminal.send_result = ZLINK_SEND_TERMINAL;
    terminal.send_terminal_errno = EHOSTUNREACH;
    assert (!record_writable_completion (&slot, terminal));
    assert (!slot.retained);
    assert (slot.wait_token == 0);
    assert (errno == EHOSTUNREACH);
}

void test_reqrep_submit_progress_quantum_is_byte_bounded ()
{
    using perf_multi_socket_reqrep::request_submit_progress_quantum;

    assert (request_submit_progress_quantum (1024, 100, true) == 32);
    assert (request_submit_progress_quantum (4096, 100, true) == 8);
    assert (request_submit_progress_quantum (8192, 100, true) == 4);
    assert (request_submit_progress_quantum (65536, 100, true) == 1);
    assert (request_submit_progress_quantum (4096, 4, true) == 4);
    assert (request_submit_progress_quantum (4096, 0, true) == 0);
    assert (request_submit_progress_quantum (4096, 100, false) == 100);
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

void require_stream_test (bool condition);

void test_stream_active_completion_cutoff_and_echo_latency ()
{
    require_stream_test (perf_stream_common::perf_stream_is_active_completion (
      true, true, UINT64_C (999), UINT64_C (1000)));
    require_stream_test (!perf_stream_common::perf_stream_is_active_completion (
      true, true, UINT64_C (1000), UINT64_C (1000)));
    require_stream_test (!perf_stream_common::perf_stream_is_active_completion (
      true, false, UINT64_C (999), UINT64_C (1000)));
    require_stream_test (!perf_stream_common::perf_stream_is_active_completion (
      false, true, UINT64_C (999), UINT64_C (1000)));
    require_stream_test (
      close_to (perf_stream_common::perf_stream_echo_latency_ns (200), 100.0));
    require_stream_test (
      close_to (perf_stream_common::perf_stream_echo_mean_ns (600, 3), 100.0));
    require_stream_test (
      close_to (perf_stream_common::perf_stream_echo_mean_ns (600, 0), 0.0));
    const uint64_t stream_stamp_ns = perf_stream_common::perf_stream_now_ns ();
    const uint64_t stream_completion_ns = perf_stream_common::perf_stream_now_ns ();
    require_stream_test (stream_completion_ns >= stream_stamp_ns);
}

void test_stream_raw_peer_allows_one_unresolved_echo ()
{
    require_stream_test (perf_stream_common::perf_stream_can_submit_echo (0));
    require_stream_test (!perf_stream_common::perf_stream_can_submit_echo (1));
    require_stream_test (!perf_stream_common::perf_stream_can_submit_echo (2));
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

void require_stream_test (bool condition)
{
    if (!condition)
        std::abort ();
}

void reset_stream_session_for_test (perf_multi_stream::session_t *session)
{
    perf_stop_requested ().store (false, std::memory_order_release);
    perf_multi_stream::reset_session (session, reinterpret_cast<void *> (1));
}

void test_stream_writable_retry_accounting ()
{
    perf_multi_stream::session_t session;
    reset_stream_session_for_test (&session);

    session.outstanding_count.store (1, std::memory_order_release);
    session.wait_token = 7;
    session.retained_packet.push_back (0x41);
    session.retained_rid.size = 1;
    session.retained_rid.data[0] = 0x22;
    zlink_completion_t admitted;
    std::memset (&admitted, 0, sizeof (admitted));
    admitted.struct_size = sizeof (admitted);
    admitted.kind = ZLINK_COMPLETION_WRITABLE;
    admitted.completion_id = 7;
    admitted.user_context = session.send_socket;
    admitted.peer_rid = session.retained_rid;
    admitted.send_result = ZLINK_SEND_ADMITTED;
    require_stream_test (
      perf_multi_stream::record_writable_completion (&session, &admitted));
    require_stream_test (session.retry_ready);
    require_stream_test (session.wait_token == 0);
    require_stream_test (perf_multi_stream::outstanding_size (&session) == 1);
    require_stream_test (session.send_count.load (std::memory_order_acquire) == 0);

    perf_multi_stream::release_retained_packet (&session);
    perf_multi_stream::finish_pending_submission (&session);
    perf_multi_stream::record_immediate_admission (&session);
    require_stream_test (perf_multi_stream::outstanding_size (&session) == 0);
    require_stream_test (session.send_count.load (std::memory_order_acquire) == 1);
    require_stream_test (!session.failed.load (std::memory_order_acquire));

    perf_multi_stream::record_immediate_admission (&session);
    require_stream_test (perf_multi_stream::outstanding_size (&session) == 0);
    require_stream_test (session.send_count.load (std::memory_order_acquire) == 2);

    session.outstanding_count.store (1, std::memory_order_release);
    session.wait_token = 8;
    session.retained_packet.push_back (0x42);
    session.retained_rid.size = 1;
    session.retained_rid.data[0] = 0x23;
    zlink_completion_t terminal;
    std::memset (&terminal, 0, sizeof (terminal));
    terminal.struct_size = sizeof (terminal);
    terminal.kind = ZLINK_COMPLETION_WRITABLE;
    terminal.completion_id = 8;
    terminal.user_context = session.send_socket;
    terminal.peer_rid = session.retained_rid;
    terminal.send_result = ZLINK_SEND_TERMINAL;
    terminal.send_terminal_errno = EHOSTUNREACH;
    require_stream_test (
      perf_multi_stream::record_writable_completion (&session, &terminal));
    require_stream_test (perf_multi_stream::outstanding_size (&session) == 0);
    require_stream_test (session.failure_count.load (std::memory_order_acquire) == 1);
    require_stream_test (
      session.first_failure_errno.load (std::memory_order_acquire) == EHOSTUNREACH);
    require_stream_test (session.failed.load (std::memory_order_acquire));
    perf_stop_requested ().store (false, std::memory_order_release);
}

std::string read_stream_session_source ()
{
    std::string test_path (__FILE__);
    const std::string::size_type slash = test_path.find_last_of ("/\\");
    require_stream_test (slash != std::string::npos);
    const std::string header_path =
      test_path.substr (0, slash) + "/../common/perf_multi_stream_session.hpp";
    std::ifstream input (header_path.c_str (), std::ios::in | std::ios::binary);
    require_stream_test (input.good ());
    std::ostringstream text;
    text << input.rdbuf ();
    return text.str ();
}

void test_stream_async_send_uses_writable_retry_loop ()
{
    const std::string source = read_stream_session_source ();
    require_stream_test (source.find ("zlink_send_part_rid (") != std::string::npos);
    require_stream_test (source.find ("zlink_completion_recv (") != std::string::npos);
    require_stream_test (source.find ("ZLINK_SEND_FLAGS_DONTWAIT") != std::string::npos);
    require_stream_test (source.find ("ZLINK_COMPLETION_WRITABLE")
                         != std::string::npos);
    require_stream_test (source.find ("retry_retained_packet") != std::string::npos);
    require_stream_test (source.find ("retained_packet") != std::string::npos);
    require_stream_test (source.find ("perf_zlink_send_rid_parts (")
                         == std::string::npos);
    require_stream_test (source.find ("EAGAIN") != std::string::npos);
    require_stream_test (source.find ("ZLINK_POLLOUT") != std::string::npos);
    require_stream_test (source.find ("perf_socket_poll (") == std::string::npos);
    require_stream_test (source.find ("pending_by_route") == std::string::npos);
    require_stream_test (source.find ("drain_pending") == std::string::npos);
    require_stream_test (source.find ("std::chrono::milliseconds (1)")
                         == std::string::npos);
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
    test_one_way_writable_retry_state ();
    test_reqrep_submit_progress_quantum_is_byte_bounded ();
    test_weighted_child_aggregation ();
    test_weighted_sample_population_uses_latency_count ();
    test_count_duration_and_bandwidth ();
    test_echo_latency_uses_one_way_estimate ();
    test_stream_active_completion_cutoff_and_echo_latency ();
    test_stream_raw_peer_allows_one_unresolved_echo ();
    test_hwm_parser_preserves_uint64_range ();
    test_stream_writable_retry_accounting ();
    test_stream_async_send_uses_writable_retry_loop ();
    return 0;
}

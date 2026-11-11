#include "perf_multi_client_helpers.hpp"
#include "perf_multi_relay_server.hpp"
#include "perf_multi_socket_reqrep.hpp"
#include "perf_multi_stream_session.hpp"
#include "perf_multi_measurement.hpp"
#include "perf_multi_handshake.hpp"
#include "../../common/streamclient/perf_stream_common.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
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

void test_phase_gate_isolates_receipts ()
{
    perf_multi_handshake::start_signal_state_t gate;
    std::chrono::steady_clock::time_point receipt;
    assert (
      !perf_multi_handshake::query_start (&gate, 64, perf_multi_metric::phase_active, &receipt));
    const auto before = std::chrono::steady_clock::now ();
    perf_multi_handshake::signal_start (&gate, 64, perf_multi_metric::phase_active);
    const auto after = std::chrono::steady_clock::now ();
    assert (
      perf_multi_handshake::query_start (&gate, 64, perf_multi_metric::phase_active, &receipt));
    assert (receipt >= before && receipt <= after);
    const auto first_receipt = receipt;
    // A duplicate signal cannot move the quiet interval's completion evidence.
    perf_multi_handshake::signal_start (&gate, 64, perf_multi_metric::phase_active);
    assert (
      perf_multi_handshake::query_start (&gate, 64, perf_multi_metric::phase_active, &receipt));
    assert (receipt == first_receipt);
    assert (
      !perf_multi_handshake::query_start (&gate, 64, perf_multi_metric::phase_latency, &receipt));
    assert (
      !perf_multi_handshake::query_start (&gate, 256, perf_multi_metric::phase_active, &receipt));
    perf_multi_handshake::signal_start (&gate, 64);
    perf_multi_handshake::signal_start (&gate, 64, perf_multi_metric::phase_latency);
    assert (perf_multi_handshake::wait_for_start (&gate, 64, 1));
    assert (perf_multi_handshake::wait_for_start (&gate, 64, 1, perf_multi_metric::phase_active));
    assert (
      !perf_multi_handshake::query_start (&gate, 64, perf_multi_metric::phase_active, &receipt));
    assert (
      perf_multi_handshake::query_start (&gate, 64, perf_multi_metric::phase_latency, &receipt));
    assert (perf_multi_handshake::wait_for_start (&gate, 64, 1, perf_multi_metric::phase_latency));
    perf_multi_handshake::signal_stop (&gate);
    assert (!perf_multi_handshake::wait_for_start (&gate, 64, 1));
    assert (errno == ECANCELED);
}

void test_phase_end_marker_and_done_parser ()
{
    perf_multi_metric::header_t header;
    perf_multi_metric::init_header (&header, 7, perf_multi_metric::phase_active, 64, 0, 0);
    assert (
      perf_multi_handshake::is_phase_end_marker (header, 7, perf_multi_metric::phase_active, 64));
    assert (
      !perf_multi_handshake::is_phase_end_marker (header, 7, perf_multi_metric::phase_latency, 64));
    assert (
      !perf_multi_handshake::is_phase_end_marker (header, 8, perf_multi_metric::phase_active, 64));
    assert (
      !perf_multi_handshake::is_phase_end_marker (header, 7, perf_multi_metric::phase_active, 256));
    header.seq = 1;
    assert (
      !perf_multi_handshake::is_phase_end_marker (header, 7, perf_multi_metric::phase_active, 64));
    header.seq = 0;
    header.sent_ts_ns = 1;
    assert (
      !perf_multi_handshake::is_phase_end_marker (header, 7, perf_multi_metric::phase_active, 64));
    perf_multi_metric::init_header (&header, 7, perf_multi_metric::phase_latency, 64, 0, 0);
    assert (
      perf_multi_handshake::is_phase_end_marker (header, 7, perf_multi_metric::phase_latency, 64));
    perf_multi_metric::init_header (&header, 7, perf_multi_metric::phase_warmup, 64, 0, 0);
    assert (
      !perf_multi_handshake::is_phase_end_marker (header, 7, perf_multi_metric::phase_warmup, 64));
    size_t size = 0;
    perf_multi_metric::phase_t phase = perf_multi_metric::phase_unknown;
    assert (perf_multi_handshake::parse_phase_done_line ("PHASE_DONE,64,1", &size, &phase));
    assert (size == 64 && phase == perf_multi_metric::phase_active);
    assert (perf_multi_handshake::parse_phase_done_line ("PHASE_DONE,64,3", &size, &phase));
    assert (phase == perf_multi_metric::phase_latency);
    assert (!perf_multi_handshake::parse_phase_done_line ("PHASE_DONE,64,2", &size, &phase));
    assert (!perf_multi_handshake::parse_phase_done_line ("PHASE_DONE,0,1", &size, &phase));
    assert (!perf_multi_handshake::parse_phase_done_line ("PHASE_DONE,64,3,1", &size, &phase));
}

void test_absolute_latency_schedule ()
{
    const uint64_t epoch = 1000000000ULL;
    perf_multi_measurement::schedule_t schedule (3.0, epoch);
    assert (!schedule.due (epoch - 1));
    assert (schedule.due (epoch));
    assert (schedule.accepted_count == 0);
    // Readiness checks and rejected attempts leave the scheduled slot intact.
    assert (schedule.due (epoch + 100));
    assert (schedule.next_ns () == epoch);
    schedule.accepted (epoch + 1000000ULL);
    assert (schedule.accepted_count == 1 && schedule.late_count == 0);
    assert (schedule.next_ns () == epoch + 333333333ULL);
    schedule.accepted (schedule.next_ns () + 1000001ULL);
    assert (schedule.late_count == 1);
    schedule.accepted (schedule.next_ns ());
    assert (schedule.next_ns () == epoch + 1000000000ULL);
    assert (perf_multi_measurement::target_rate (100, 2.0) == 40.0);
    assert (perf_multi_measurement::target_rate (100, 0.0) == 0.0);
    // Admissions can exceed arrivals; only the received count sets the total.
    const uint64_t received = 200, accepted_a = 300, accepted_b = 500;
    const double rate_a = perf_multi_measurement::target_rate (received, 2.0, accepted_a, 800);
    const double rate_b = perf_multi_measurement::target_rate (received, 2.0, accepted_b, 800);
    assert (rate_a == 30.0 && rate_b == 50.0);
    assert (rate_a + rate_b == perf_multi_measurement::target_rate (received, 2.0));
    assert (perf_multi_measurement::target_rate (received, 2.0, 0, 800) == 0.0);
    assert (perf_multi_measurement::target_rate (received, 2.0, 300, 0) == 0.0);
    size_t size = 0, count = 0;
    for (const char *prefix : {"PHASE_PACED,", "PHASE_DRAINED,", "PHASE_COMPLETE,"}) {
        assert (perf_multi_handshake::parse_size_count_command_line (
          std::string (prefix) + "64,12345", prefix, &size, &count));
        assert (size == 64 && count == 12345);
        for (const char *suffix : {"64", "64,", "64,0", "64,-1", "64,+1", "64, 1", "64,1.5",
                                   "64,18446744073709551616", "64,1,2"})
            assert (!perf_multi_handshake::parse_size_count_command_line (
              std::string (prefix) + suffix, prefix, &size, &count));
    }
    assert (perf_multi_handshake::parse_size_count_command_line (
      "PHASE_COMPLETE,64,0", "PHASE_COMPLETE,", &size, &count, true));
    assert (count == 0);
    perf_multi_handshake::start_signal_state_t gate;
    perf_multi_handshake::signal_start (&gate, 64, perf_multi_metric::phase_active, 12345);
    assert (
      perf_multi_handshake::wait_for_start (&gate, 64, 1, perf_multi_metric::phase_active, &count));
    assert (count == 12345);
    std::ostringstream diagnostic;
    std::streambuf *saved = std::cout.rdbuf (diagnostic.rdbuf ());
    perf_multi_measurement::print_diagnostic ("TEST", 64, 800, 2.0, schedule, 200, 79.0);
    std::cout.rdbuf (saved);
    assert (diagnostic.str ().find (",active_sender_rate,400,") != std::string::npos);
    assert (diagnostic.str ().find (",received_rate,100,paced_received_rate,79")
            != std::string::npos);
    perf_multi_measurement::schedule_t idle (0.0, epoch);
    assert (!idle.due (epoch + 1000000000ULL));
}

void test_latency_send_timestamp_window ()
{
    const uint64_t epoch = 1000000000ULL;
    assert (!perf_multi_measurement::latency_sample (epoch - 1, epoch, 2.0));
    assert (!perf_multi_measurement::latency_sample (epoch + 499999999ULL, epoch, 2.0));
    assert (perf_multi_measurement::latency_sample (epoch + 500000000ULL, epoch, 2.0));
    assert (perf_multi_measurement::latency_sample (epoch + 2499999999ULL, epoch, 2.0));
    assert (!perf_multi_measurement::latency_sample (epoch + 2500000000ULL, epoch, 2.0));
    perf_multi_metric::header_t header;
    perf_multi_metric::init_header (&header, 1, perf_multi_metric::phase_latency, 64, 1, epoch);
    assert (perf_multi_metric::is_expected (header, 1, perf_multi_metric::phase_latency, 64));
    assert (!perf_multi_metric::is_expected (header, 1, perf_multi_metric::phase_active, 64));
}

void test_request_metrics_separate_active_and_latency ()
{
    using namespace perf_multi_socket_reqrep;
    client_state_t state;
    client_slot_t slot;
    slot.owner = &state;
    const uint64_t now = perf_multi_metric::now_ns ();
    state.active_run_id = 7;
    state.active_msg_size = 64;
    state.active_deadline_ns = now + 1000000000ULL;
    zlink_msg_t parts[2];
    assert (zlink_msg_init_size (&parts[0], 64) == 0);
    assert (zlink_msg_init (&parts[1]) == 0);
    const auto complete = [&] (perf_multi_metric::phase_t phase, uint64_t sent_ns) {
        assert (perf_multi_metric::stamp_payload (zlink_msg_data (&parts[0]), 64, 7, phase, 64, 1,
                                                  sent_ns));
        record_request_completion (&slot, ZLINK_REQUEST_OK, parts, perf_measurement_part_count ());
    };
    complete (perf_multi_metric::phase_active, now);
    assert (state.active_reply_count == 1);
    assert (state.latency.count () == 0);

    perf_multi_measurement::schedule_t schedule (80.0,
                                                 now - perf_multi_measurement::settle_ns - 1000);
    state.schedule = &schedule;
    state.latency_duration_seconds = 1;
    state.active_reply_count = 0;
    complete (perf_multi_metric::phase_active, now);
    complete (perf_multi_metric::phase_warmup, now);
    complete (perf_multi_metric::phase_latency,
              schedule.epoch_ns + perf_multi_measurement::settle_ns - 1);
    assert (state.active_reply_count == 0);
    assert (state.latency.count () == 0);
    // An arrival inside the sample window counts even if its send was settling.
    assert (state.paced_received_count == 1);
    complete (perf_multi_metric::phase_latency, now);
    assert (state.active_reply_count == 1);
    assert (state.latency.count () == 1);
    assert (state.paced_received_count == 2);
    // Tail replies remain latency samples but are outside paced receive rate.
    schedule.epoch_ns = now - perf_multi_measurement::settle_ns - 2000000000ULL;
    complete (perf_multi_metric::phase_latency,
              schedule.epoch_ns + perf_multi_measurement::settle_ns + 1);
    assert (state.latency.count () == 2);
    assert (state.paced_received_count == 2);
    zlink_multipart_close (parts, 2u);
}

void test_latency_duration_settings ()
{
    const char *names[] = {"PERF_MULTI_DURATION_SECONDS", "PERF_MULTI_LATENCY_DURATION_SECONDS"};
    std::string saved[2];
    bool present[2];
    for (int i = 0; i < 2; ++i) {
        const char *value = std::getenv (names[i]);
        present[i] = value != NULL;
        if (value)
            saved[i] = value;
    }
    const auto put = [] (const char *name, const char *value) {
#if defined(_WIN32)
        _putenv_s (name, value ? value : "");
#else
        if (value)
            setenv (name, value, 1);
        else
            unsetenv (name);
#endif
    };
    put (names[0], "7");
    put (names[1], NULL);
    assert (resolve_bench_settings ().latency_duration_seconds == 7);
    put (names[1], "2");
    assert (resolve_bench_settings ().duration_seconds == 7);
    assert (resolve_bench_settings ().latency_duration_seconds == 2);
    put (names[1], "invalid");
    assert (resolve_bench_settings ().latency_duration_seconds == 7);
    put (names[1], "0");
    assert (resolve_bench_settings ().latency_duration_seconds == 1);
    for (int i = 0; i < 2; ++i)
        put (names[i], present[i] ? saved[i].c_str () : NULL);
}

bool close_to (double actual, double expected)
{
    return std::fabs (actual - expected) < 0.000001;
}

void test_histogram_statistics_and_merge ()
{
    perf_latency_histogram_t all;
    perf_latency_histogram_t left;
    perf_latency_histogram_t right;
    std::vector<double> sorted;
    for (int value = 1; value <= 10000; ++value) {
        all.add (static_cast<double> (value));
        (value <= 5000 ? left : right).add (static_cast<double> (value));
        sorted.push_back (static_cast<double> (value));
    }
    perf_latency_histogram_t merged;
    merged.merge (left);
    merged.merge (right);
    const double p50_position = (sorted.size () - 1) * 0.50;
    const double p99_position = (sorted.size () - 1) * 0.99;
    const double exact_p50 = sorted[static_cast<size_t> (p50_position)]
                             + (sorted[static_cast<size_t> (p50_position) + 1]
                                - sorted[static_cast<size_t> (p50_position)])
                                 * (p50_position - std::floor (p50_position));
    const double exact_p99 = sorted[static_cast<size_t> (p99_position)]
                             + (sorted[static_cast<size_t> (p99_position) + 1]
                                - sorted[static_cast<size_t> (p99_position)])
                                 * (p99_position - std::floor (p99_position));
    assert (all.count () == sorted.size ());
    assert (close_to (all.sum_ns () / all.count (), 5000.5));
    assert (all.percentile (0.50) >= exact_p50
            && all.percentile (0.50) <= exact_p50 * (1.0 + 1.0 / 128.0));
    assert (all.percentile (0.99) >= exact_p99
            && all.percentile (0.99) <= exact_p99 * (1.0 + 1.0 / 128.0));
    assert (close_to (merged.percentile (0.50), all.percentile (0.50)));
    assert (close_to (merged.percentile (0.99), all.percentile (0.99)));
    assert (all.min_ns () == 1.0 && all.max_ns () == 10000.0);
    perf_latency_histogram_t half_nanosecond;
    half_nanosecond.add (0.5);
    assert (half_nanosecond.percentile (0.50) >= 0.5);
    assert (half_nanosecond.percentile (0.50) <= 0.5 * (1.0 + 1.0 / 128.0));
}

void test_echo_failure_reports_stderr ()
{
    const multi_bench_settings_t settings = {};
    std::vector<void *> sockets;
    std::vector<char> payload;
    double throughput = 0.0;
    bench_latency_stats_t latency;
    std::ostringstream captured;
    std::streambuf *const old_stderr = std::cerr.rdbuf (captured.rdbuf ());
    const bool result = perf_multi_client::run_echo_duration (
      sockets, settings, payload, 4096, 4096, perf_multi_metric::header_size (), "SERVER", true,
      false, 1, &throughput, &latency);
    std::cerr.rdbuf (old_stderr);
    if (result
        || captured.str ().find ("[perf-multi-echo] no sockets size=4096") == std::string::npos)
        std::abort ();
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
    zlink_completion_t timed_out = admitted;
    timed_out.completion_id = 8;
    timed_out.send_result = ZLINK_SEND_TIMED_OUT;
    timed_out.send_terminal_errno = EAGAIN;
    assert (record_writable_completion (&slot, timed_out));
    assert (slot.retained);
    assert (slot.retry_ready);
    assert (slot.wait_token == 0);
    assert ((tracker_slot_events (send_wait_tracker_t (), slot) & ZLINK_POLLOUT) == 0);

    slot.retry_ready = false;
    slot.wait_token = 9;
    zlink_completion_t wrong_id = admitted;
    wrong_id.completion_id = 10;
    assert (!record_writable_completion (&slot, wrong_id));
    assert (slot.retained);
    assert (slot.wait_token == 9);
    assert (errno == EPROTO);

    zlink_completion_t terminal = admitted;
    terminal.completion_id = 9;
    terminal.send_result = ZLINK_SEND_NOT_FOUND;
    terminal.send_terminal_errno = ENOENT;
    assert (!record_writable_completion (&slot, terminal));
    assert (!slot.retained);
    assert (slot.wait_token == 0);
    assert (errno == ENOENT);
}

void test_paced_retained_retries_share_one_schedule ()
{
    using namespace perf_multi_client;
    ctx_guard_t context;
    assert (context.valid ());
    socket_guard_t server (context.get (), ZLINK_SOCKET_ROUTER);
    socket_guard_t first (context.get (), ZLINK_SOCKET_DEALER);
    socket_guard_t second (context.get (), ZLINK_SOCKET_DEALER);
    assert (server.valid () && first.valid () && second.valid ());
    const int zero_linger = 0;
    void *all[] = {server.get (), first.get (), second.get ()};
    for (size_t i = 0; i < 3; ++i)
        assert (zlink_set_option (all[i], ZLINK_OPT_LINGER, &zero_linger, sizeof (zero_linger))
                == ZLINK_CONFIG_OK);
    const char *endpoint = "inproc://perf-paced-retained-retries";
    assert (zlink_bind (server.get (), endpoint) == ZLINK_BIND_OK);
    assert (zlink_connect (first.get (), endpoint) == ZLINK_CONNECT_OK);
    assert (zlink_connect (second.get (), endpoint) == ZLINK_CONNECT_OK);
    std::vector<void *> sockets;
    sockets.push_back (first.get ());
    sockets.push_back (second.get ());
    send_wait_tracker_t tracker;
    assert (create_send_wait_tracker (sockets, ZLINK_POLLIN, &tracker));
    for (size_t i = 0; i < tracker.slots.size (); ++i) {
        send_wait_slot_t &slot = tracker.slots[i];
        slot.retained = true;
        slot.retry_ready = true;
        slot.retained_payload.assign (64, 'c');
        slot.retained_payload_size = slot.retained_payload.size ();
    }
    // Both WRITABLE events have already been consumed. A future deadline
    // defers both records without clearing readiness or admitting either one.
    perf_multi_measurement::schedule_t schedule (1.0,
                                                 perf_multi_metric::now_ns () + 3600000000000ULL);
    uint64_t accepted = 0;
    for (size_t i = 0; i < tracker.slots.size (); ++i) {
        assert (retry_retained_send (&tracker, &tracker.slots[i], &accepted, 0, &schedule));
        assert (tracker.slots[i].retained && tracker.slots[i].retry_ready);
        assert (tracker.slots[i].replies == 0);
    }
    assert (accepted == 0 && schedule.accepted_count == 0);

    // One admitted retry advances the shared deadline before the other slot
    // is considered. Moving the epoch models the next timer tick without sleep.
    schedule.epoch_ns = perf_multi_metric::now_ns ();
    assert (retry_retained_send (&tracker, &tracker.slots[0], &accepted, 0, &schedule));
    assert (!tracker.slots[0].retained && tracker.slots[0].replies == 1);
    assert (accepted == 1 && schedule.accepted_count == 1);
    assert (retry_retained_send (&tracker, &tracker.slots[1], &accepted, 0, &schedule));
    assert (tracker.slots[1].retained && tracker.slots[1].retry_ready);
    assert (accepted == 1 && schedule.accepted_count == 1);
    schedule.epoch_ns -= 2000000000ULL;
    assert (retry_retained_send (&tracker, &tracker.slots[1], &accepted, 0, &schedule));
    assert (!tracker.slots[1].retained && tracker.slots[1].replies == 1);
    assert (accepted == 2 && schedule.accepted_count == 2);

    // Tail drain keeps the existing ungated policy for retained records, while
    // counting paced admissions and excluding them from ACTIVE calibration.
    send_wait_slot_t &tail = tracker.slots[0];
    tail.retained = true;
    tail.retry_ready = true;
    tail.retained_payload.assign (64, 'c');
    tail.retained_payload_size = tail.retained_payload.size ();
    schedule.epoch_ns = perf_multi_metric::now_ns () + 3600000000000ULL;
    assert (retry_retained_send (&tracker, &tail, &accepted, 1, &schedule, false));
    assert (!tail.retained && tail.replies == 2);
    assert (accepted == 2 && schedule.accepted_count == 3);
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

void test_count_duration_and_bandwidth ()
{
    const double one_way = throughput_per_second (250, 2.5);
    assert (close_to (one_way, 100.0));
    assert (close_to (
      bandwidth_mb_per_second ("PUBSUB", 1000, one_way), 0.1));
    assert (close_to (throughput_per_second (250, 0.0), 0.0));
}

void test_echo_latency_uses_one_way_estimate ()
{
    assert (close_to (
      latency_sample_ns ("ROUTER_ROUTER_REQREP", 200), 100.0));
    assert (close_to (latency_sample_ns ("PUBSUB", 200), 200.0));
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
    zlink_completion_t timed_out;
    std::memset (&timed_out, 0, sizeof (timed_out));
    timed_out.struct_size = sizeof (timed_out);
    timed_out.kind = ZLINK_COMPLETION_WRITABLE;
    timed_out.completion_id = 8;
    timed_out.user_context = session.send_socket;
    timed_out.peer_rid = session.retained_rid;
    timed_out.send_result = ZLINK_SEND_TIMED_OUT;
    timed_out.send_terminal_errno = EAGAIN;
    require_stream_test (perf_multi_stream::record_writable_completion (&session, &timed_out));
    require_stream_test (perf_multi_stream::outstanding_size (&session) == 1);
    require_stream_test (!session.retained_packet.empty ());
    require_stream_test (session.retry_ready);
    require_stream_test (session.wait_token == 0);
    require_stream_test ((perf_multi_stream::session_poll_events (&session, true) & ZLINK_POLLOUT)
                         == 0);

    perf_multi_stream::release_retained_packet (&session);
    perf_multi_stream::finish_pending_submission (&session);
    session.outstanding_count.store (1, std::memory_order_release);
    session.wait_token = 9;
    session.retained_packet.push_back (0x42);
    session.retained_rid.size = 1;
    session.retained_rid.data[0] = 0x23;
    zlink_completion_t terminal;
    std::memset (&terminal, 0, sizeof (terminal));
    terminal.struct_size = sizeof (terminal);
    terminal.kind = ZLINK_COMPLETION_WRITABLE;
    terminal.completion_id = 9;
    terminal.user_context = session.send_socket;
    terminal.peer_rid = session.retained_rid;
    terminal.send_result = ZLINK_SEND_NOT_CONNECTED;
    terminal.send_terminal_errno = ENOTCONN;
    require_stream_test (
      perf_multi_stream::record_writable_completion (&session, &terminal));
    require_stream_test (perf_multi_stream::outstanding_size (&session) == 0);
    require_stream_test (session.failure_count.load (std::memory_order_acquire) == 1);
    require_stream_test (
      session.first_failure_errno.load (std::memory_order_acquire) == ENOTCONN);
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
    require_stream_test (source.find ("zlink_send_rid (") != std::string::npos);
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
    test_phase_gate_isolates_receipts ();
    test_phase_end_marker_and_done_parser ();
    test_latency_duration_settings ();
    test_absolute_latency_schedule ();
    test_latency_send_timestamp_window ();
    test_request_metrics_separate_active_and_latency ();
    connect_monitor_state_t ready_state;
    zlink_monitor_event_t ready_event = {};
    ready_event.event = ZLINK_EVENT_CONNECTION_READY;
    const uint64_t ready_values[] = {2, 2, 1, 0, 3};
    for (size_t i = 0; i != sizeof (ready_values) / sizeof (ready_values[0]); ++i) {
        ready_event.value = ready_values[i];
        connect_monitor_handler (&ready_event, &ready_state);
        assert (connect_ready_count (&ready_state) == ready_values[i]);
    }
    test_echo_failure_reports_stderr ();
    test_histogram_statistics_and_merge ();
    test_relay_submit_error_classification ();
    test_one_way_writable_retry_state ();
    test_paced_retained_retries_share_one_schedule ();
    test_reqrep_submit_progress_quantum_is_byte_bounded ();
    test_count_duration_and_bandwidth ();
    test_echo_latency_uses_one_way_estimate ();
    test_stream_active_completion_cutoff_and_echo_latency ();
    test_stream_raw_peer_allows_one_unresolved_echo ();
    test_hwm_parser_preserves_uint64_range ();
    test_stream_writable_retry_accounting ();
    test_stream_async_send_uses_writable_retry_loop ();
    return 0;
}

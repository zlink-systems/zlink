/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include "../subscriber_server/fanout_receipts.hpp"
#include <perf/server/fanout_support.hpp>
#include <perf/send_send_correlation.hpp>
#include "../channel_server/s2s_return_to_spot_handler.hpp"

#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>
#include <thread>

namespace
{
using namespace perf;
namespace fw = zlink::framework;

void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}

role_config_t config ()
{
    role_config_t out;
    out.run_id = "test-run";
    out.cell_id = "cpp-perf-contract-test";
    out.role = "client";
    out.source = true;
    out.workload.payload_size = 4;
    out.workload.warmup_seconds = 0.001;
    out.workload.duration_seconds = 0.040;
    return out;
}

trigger_request_t trigger (const std::string &phase, const std::string &reset_seq)
{
    trigger_request_t out;
    out.run_id = "test-run";
    out.cell_id = "cpp-perf-contract-test";
    out.phase = phase;
    out.reset_seq = reset_seq;
    return out;
}

json measured_snapshot (measurement_t &measurement)
{
    return measurement.snapshot (json::object ());
}

void test_terminal_window_and_inflight_accounting ()
{
    measurement_t measurement (config (), true);
    require (measured_snapshot (measurement).at ("metrics").at ("messages.inflightAtEnd") == "0",
             "inflightAtEnd must be numeric before the measured window");
    require (measurement.start (trigger ("warmup", "0"), {}).accepted, "warmup did not start");
    measurement.wait_phase ();
    const auto [reset, status] = measurement.reset (reset_request_t{"test-run", "cpp-perf-contract-test", "1"}, {});
    require (status == 200 && reset.at ("ok").get<bool> (), "reset did not advance to measured");
    require (measurement.start (trigger ("measured", "1"), {}).accepted, "measured phase did not start");

    std::int64_t inside = 0, after = 0;
    require (measurement.begin_operation (inside), "inside-window operation was rejected");
    require (measurement.complete_operation (inside, nullptr, inside + 1),
             "an unsealed success before endTicks must return true");
    require (measurement.begin_operation (after), "late-terminal operation was rejected before the end");
    require (!measurement.complete_operation (after, nullptr, measurement.end_ticks () + 1),
             "a terminal at or after endTicks must return false");
    measurement.wait_phase ();

    const auto snapshot = measured_snapshot (measurement);
    const auto &metrics = snapshot.at ("metrics");
    require (metrics.at ("messages.sent") == "2", "both started operations must be sent");
    require (metrics.at ("messages.completed") == "1", "only terminalTicks < endTicks may complete");
    require (metrics.contains ("messages.inflightAtEnd") && metrics.at ("messages.inflightAtEnd") == "1",
             "a terminal after endTicks belongs in inflightAtEnd");
    require (snapshot.at ("histograms").at ("latencyMs").at ("count") == "1",
             "late terminal must not enter the measured latency histogram");
    require (!measurement.complete_operation (after, nullptr, after + 1),
             "a sealed measurement must reject later success");
    require (measured_snapshot (measurement).at ("metrics").at ("messages.inflightAtEnd") == "1",
             "a commit after sealing must leave the end-of-window balance unchanged");
}

std::exception_ptr system_error (std::errc code)
{
    try {
        throw std::system_error (std::make_error_code (code));
    }
    catch (...) {
        return std::current_exception ();
    }
}

void test_driver_latency_uses_result_window ()
{
    measurement_t measurement (config (), true);
    scenario_metrics_t metrics (measurement);
    metrics.latency ("driverLatencyMs", "driver.latency");
    require (measurement.start (trigger ("warmup", "0"), {}).accepted, "warmup did not start");
    measurement.wait_phase ();
    const auto [reset, status] = measurement.reset (reset_request_t{"test-run", "cpp-perf-contract-test", "1"}, {});
    require (status == 200, "driver reset failed");
    require (measurement.start (trigger ("measured", "1"), {}).accepted, "measured did not start");
    const auto began = measurement.start_ticks ();
    const auto ended = measurement.end_ticks ();
    metrics.record ("driverLatencyMs", began, began + 1, began - 1);
    metrics.record ("driverLatencyMs", began, began + 1, ended - 1);
    metrics.record ("driverLatencyMs", began, began + 2, ended);
    const auto snapshot = measured_snapshot (measurement);
    require (snapshot.at ("histograms").at ("driverLatencyMs").at ("count") == "1",
             "driver latency is admitted only by the result time window");
}

void test_public_error_classification ()
{
    measurement_t measurement (config (), true);
    require (measurement.start (trigger ("warmup", "0"), {}).accepted, "warmup did not start");
    measurement.wait_phase ();
    const auto [reset, status] = measurement.reset (reset_request_t{"test-run", "cpp-perf-contract-test", "1"}, {});
    require (status == 200 && reset.at ("ok").get<bool> (), "reset did not advance to measured");
    require (measurement.start (trigger ("measured", "1"), {}).accepted, "measured phase did not start");

    std::int64_t timeout = 0, cancelled = 0, invalid = 0, unrelated = 0;
    require (measurement.begin_operation (timeout), "timeout operation was rejected");
    measurement.complete_operation (timeout, system_error (std::errc::timed_out), timeout + 1);
    require (measurement.begin_operation (cancelled), "cancelled operation was rejected");
    measurement.complete_operation (cancelled, system_error (std::errc::operation_canceled), cancelled + 1);
    require (measurement.begin_operation (invalid), "invalid-operation case was rejected");
    measurement.complete_operation (
      invalid,
      std::make_exception_ptr (fw::framework_exception_t (fw::framework_error_kind_t::invalid_operation, "public invalid operation")),
      invalid + 1);
    require (measurement.begin_operation (unrelated), "unrelated system-error case was rejected");
    measurement.complete_operation (unrelated, system_error (std::errc::operation_not_permitted), unrelated + 1);
    measurement.wait_phase ();

    const auto snapshot = measured_snapshot (measurement);
    const auto &metrics = snapshot.at ("metrics");
    require (metrics.at ("messages.timeout") == "1", "timed_out must be timeout");
    require (metrics.at ("messages.cancelled") == "1", "operation_canceled must be cancelled");
    require (metrics.at ("messages.failed") == "2", "Framework InvalidOperation and unrelated errors must stay failed");
}

void test_histogram_percentile_cap ()
{
    histogram_t histogram;
    histogram.record (2'250'000);
    json metrics = json::object (), histograms = json::object (), reasons = json::object ();
    histogram.export_to ("latencyMs", "latency", metrics, histograms, reasons);
    require (metrics.at ("latency.p50Ms") == 2.25, "nearest-rank percentile must not exceed the exact observed maximum");
    require (histograms.at ("latencyMs").value ("percentileMethod", std::string ())
               == "nearest-rank-bucket-upper-bound-capped-by-max",
             "histogram metadata must describe the capped percentile");
}

void test_removed_unique_delivery_metric ()
{
    measurement_t measurement (config (), true);
    require (!measured_snapshot (measurement).at ("metrics").contains ("fanout.uniqueDelivered"),
             "the removed uniqueDelivered metric must not appear in a baseline snapshot");

    json snapshot{{"metrics", json::object ()}, {"histograms", json::object ()}, {"nullReasons", json::object ()}};
    fanout_metrics::apply_common (snapshot, true);
    require (!snapshot.at ("metrics").contains ("fanout.uniqueDelivered"),
             "the removed uniqueDelivered metric must not appear in a fanout snapshot");
}

void test_return_spot_address ()
{
    auto cfg = config ();
    cfg.spot_ids = {"spot-0", "spot-1"};
    measurement_t measurement (cfg, false);
    auto request = measurement.request (1, 1, true);
    request.return_spot_id = "spot-0";
    bool rejected = false;
    try {
        measurement.validate_request (request, std::nullopt, cfg.spot_ids[request.client_id % cfg.spot_ids.size ()]);
    }
    catch (const validation_error_t &) { rejected = true; }
    require (rejected, "a present but wrong return SpotId must be rejected");
    request.return_spot_id = "spot-1";
    measurement.validate_request (request, std::nullopt, cfg.spot_ids[request.client_id % cfg.spot_ids.size ()]);

    role_t role (cfg, false);
    fw::route_client_t route;
    s2s_return_to_spot_handler_t handler (role, route);
    request.return_spot_id = "spot-0";
    const auto handled = handler.handle (request).result ();
    require (!handled.has_value (), "Channel target must reject a wrong return SpotId before sending");
    const auto errors = role.measurement.error_evidence ();
    require (!errors.empty () && errors.back ().at ("harnessKind") == "IdentityMismatch",
             "Channel target must classify the wrong return SpotId as IdentityMismatch");
}

void test_subscriber_entry_time_survives_snapshot ()
{
    auto cfg = config ();
    cfg.role = "subscriber-server";
    cfg.role_instance = 0;
    cfg.source = false;
    role_t role (cfg, false);
    const auto directory = std::filesystem::temp_directory_path () /
                           ("cpp-perf-receipts-" + std::to_string (now_ticks ()));
    std::filesystem::create_directory (directory);
    fanout_receipts_t receipts (role, directory.string ());
    require (role.measurement.start (trigger ("warmup", "0"), {}).accepted, "warmup did not start");
    role.measurement.wait_phase ();
    const auto [reset, status] = role.measurement.reset (reset_request_t{"test-run", "cpp-perf-contract-test", "1"}, {});
    require (status == 200, "subscriber reset failed");
    require (role.measurement.start (trigger ("measured", "1"), {}).accepted, "measured did not start");
    const auto entry = now_ticks ();
    publish_event_t event;
    event.run_id = cfg.run_id;
    event.cell_id = cfg.cell_id;
    event.phase = "measured";
    event.reset_seq = "1";
    event.sequence = "1";
    event.topic = fanout_topic;
    event.clock_domain_id = clock_domain ();
    event.sent_ticks = dec (entry);
    event.payload = role.measurement.pattern ().base64 ();
    role.measurement.wait_phase ();
    role.measurement.set_final_snapshot (true);
    (void) role.measurement.snapshot (json::object ());
    receipts.record (event, entry);
    receipts.record (event, entry);
    receipts.record (event, role.measurement.end_ticks () + 1);
    event.sequence = "2";
    receipts.record (event, role.measurement.end_ticks () + 1);
    role.measurement.set_final_snapshot (false);
    const auto snapshot = role.measurement.snapshot (json::object ());
    require (snapshot.at ("runtimeMetrics").at ("fanoutReceipts").at ("value").at ("uniqueInWindow") == "1",
             "a handler entry inside the window must count after snapshot");
    require (snapshot.at ("metrics").at ("fanout.duplicateEvents") == "2",
             "repeated measured events are duplicates even outside the window");
    require (snapshot.at ("runtimeMetrics").at ("fanoutReceipts").at ("value").at ("measuredEventsSeen") == "2",
             "seen includes first receipt inside and first receipt outside the window");
    require (snapshot.at ("runtimeMetrics").at ("fanoutReceipts").at ("value").at ("measuredOutsideWindow") == "1",
             "a first-seen event after endTicks is outside the window");
    std::filesystem::remove_all (directory);
}

void test_correlation_owner_completes_once ()
{
    auto cfg = config ();
    cfg.workload.correlation_expiry_ms = 100;
    cfg.workload.duration_seconds = 0.25;
    measurement_t measurement (cfg, true);
    scenario_metrics_t metrics (measurement);
    send_send_correlation_t correlations (measurement, metrics);
    require (measurement.start (trigger ("warmup", "0"), {}).accepted, "warmup did not start");
    measurement.wait_phase ();
    const auto [reset, status] = measurement.reset (reset_request_t{"test-run", "cpp-perf-contract-test", "1"}, {});
    require (status == 200, "correlation reset failed");
    require (measurement.start (trigger ("measured", "1"), {}).accepted, "measured did not start");

    std::int64_t started = 0;
    require (measurement.begin_operation (started, "send"), "correlation did not start");
    auto request = measurement.request (0, 1);
    auto entry = correlations.register_request (request);
    correlations.reply (payload_pattern_t::reply (request, now_ticks ()));
    require (measured_snapshot (measurement).at ("metrics").at ("messages.completed") == "0",
             "correlation does not own operation accounting");
    correlations.first_send_ended (entry, nullptr);
    const auto [error, completed] = correlations.complete (entry).result ().value ();
    require (!error && completed == entry->closed_ticks, "first valid reply must close with its own time");
    require (measurement.complete_operation (started, error, completed), "owner must count the first success");
    correlations.reply (payload_pattern_t::reply (request, now_ticks ()));
    require (measured_snapshot (measurement).at ("metrics").at ("messages.completed") == "1",
             "duplicate reply must not account a second operation");
    require (measured_snapshot (measurement).at ("metrics").at ("messages.duplicateReply") == "1", "duplicate family counter must count");

    std::int64_t raced_started = 0;
    require (measurement.begin_operation (raced_started, "send"), "raced correlation did not start");
    auto raced = measurement.request (0, 3);
    auto raced_entry = correlations.register_request (raced);
    const auto raced_reply = payload_pattern_t::reply (raced, now_ticks ());
    std::thread first_reply ([&] { correlations.reply (raced_reply); });
    std::thread second_reply ([&] { correlations.reply (raced_reply); });
    first_reply.join ();
    second_reply.join ();
    const auto [raced_error, raced_completed] = correlations.complete (raced_entry).result ().value ();
    require (!raced_error && measurement.complete_operation (raced_started, raced_error, raced_completed),
             "one raced reply must complete the owner operation");
    require (measured_snapshot (measurement).at ("metrics").at ("messages.completed") == "2",
             "two simultaneous replies must complete one operation once");
    require (measured_snapshot (measurement).at ("metrics").at ("messages.duplicateReply") == "2",
             "the losing simultaneous reply must count as duplicate");

    std::int64_t late_started = 0;
    require (measurement.begin_operation (late_started, "send"), "second correlation did not start");
    auto late = measurement.request (0, 2);
    auto late_entry = correlations.register_request (late);
    std::this_thread::sleep_for (std::chrono::milliseconds (110));
    correlations.reply (payload_pattern_t::reply (late, now_ticks ()));
    const auto [late_error, late_completed] = correlations.complete (late_entry).result ().value ();
    require (late_error && late_entry->state == send_send_correlation_t::expired,
             "reply at or after deadline must expire");
    require (!measurement.complete_operation (late_started, late_error, late_completed), "expiry must not count success");
    correlations.reply (payload_pattern_t::reply (late, now_ticks ()));
    require (measured_snapshot (measurement).at ("metrics").at ("messages.expired") == "1", "expiry counter must count");
    require (measured_snapshot (measurement).at ("metrics").at ("messages.lateReply") == "2", "both replies after expiry must count as late");
    auto unknown = payload_pattern_t::reply (late, now_ticks ());
    unknown.correlation_id = "unknown";
    correlations.reply (unknown);
    require (measured_snapshot (measurement).at ("metrics").at ("messages.unknownCorrelation") == "1", "unknown reply counter must count");
    measurement.wait_phase ();
    correlations.reply (payload_pattern_t::reply (request, now_ticks ()));
    correlations.reply (payload_pattern_t::reply (late, now_ticks ()));
    correlations.reply (unknown);
    const auto after_window = measured_snapshot (measurement).at ("metrics");
    require (after_window.at ("messages.duplicateReply") == "3", "duplicate counter must continue after window");
    require (after_window.at ("messages.lateReply") == "3", "late counter must continue after window");
    require (after_window.at ("messages.unknownCorrelation") == "2", "unknown counter must continue after window");
}

void test_readiness_uses_probe_evidence_not_setup_evidence ()
{
    role_t role (config (), true);
    role.objects->set (true, "", json::array ({{{"kind", "actorCreateAndBind"}}}));
    const auto setup_only = role.ready ();
    require (!setup_only.at ("consumersReady").get<bool> (),
             "session actor create/bind evidence alone must not mark consumers ready");
    const bool actor_setup_is_visible = std::any_of (setup_only.at ("evidence").begin (), setup_only.at ("evidence").end (),
                                                     [] (const json &item) { return item.value ("kind", std::string ()) == "actorCreateAndBind"; });
    require (actor_setup_is_visible, "session actor create/bind evidence must remain visible before the relay probe succeeds");
    role.measurement.set_setup_evidence (
      json::array ({{{"kind", "typedProbeRelay"}, {"observedValue", {{"completed", 1}}}}}));
    require (role.ready ().at ("consumersReady").get<bool> (),
             "a successfully completed session relay probe must mark consumers ready");
    role.measurement.set_setup_evidence (json::array ({{{"kind", "warmupMarker"}}}));
    require (role.ready ().at ("consumersReady").get<bool> (),
             "a received subscriber warmup marker must mark consumers ready");
    role.measurement.set_setup_evidence (json::array ({{{"kind", "warmupMarkerPublished"}}}));
    require (role.ready ().at ("consumersReady").get<bool> (),
             "a published fanout warmup marker must preserve publisher readiness");
}

void test_sequence_original_collision ()
{
    const auto path = std::filesystem::temp_directory_path ()
                      / ("cpp-perf-sequences-"
                         + std::to_string (std::chrono::steady_clock::now ().time_since_epoch ().count ()) + ".json");
    std::error_code ignored;
    std::filesystem::remove (path, ignored);
    write_once (path.string (), json{{"original", 1}});
    bool rejected = false;
    try {
        write_once (path.string (), json{{"original", 2}});
    }
    catch (const std::system_error &) {
        rejected = true;
    }
    std::ifstream file (path);
    const auto original = json::parse (file);
    std::filesystem::remove (path, ignored);
    require (rejected, "a second sequence original must fail instead of replacing the first");
    require (original.at ("original") == 1, "a collision must preserve the first sequence original");
}
} // namespace

int main ()
{
    try {
        test_terminal_window_and_inflight_accounting ();
        test_driver_latency_uses_result_window ();
        test_public_error_classification ();
        test_histogram_percentile_cap ();
        test_removed_unique_delivery_metric ();
        test_return_spot_address ();
        test_subscriber_entry_time_survives_snapshot ();
        test_correlation_owner_completes_once ();
        test_readiness_uses_probe_evidence_not_setup_evidence ();
        test_sequence_original_collision ();
        std::cout << "cpp perf contract tests passed\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception &error) {
        std::cerr << "cpp perf contract tests failed: " << error.what () << '\n';
        return EXIT_FAILURE;
    }
}

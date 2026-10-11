#include "send_metrics.hpp"
#include "../common/bench_common.hpp"

#include <cassert>

int main ()
{
    zlink_c_bench::send_metrics_t metrics;
    metrics.start (100);
    char body[zlink_c_bench::k_header_size];
    zlink_c_bench::stamp_payload (body, sizeof body, 1, zlink_c_bench::phase_active, 0);
    zlink_c_bench::write_u64_le (reinterpret_cast<unsigned char *> (body) + 21, 50);
    metrics.record (body, sizeof body, 75);
    metrics.close (100);
    metrics.record (body, sizeof body, 125);
    auto result = metrics.snapshot (125);
    assert (result.completed == 1);
    assert (result.server_received == 2);
    assert (result.mean_us == 0.025);
    zlink_c_bench::send_metrics_t warmup;
    warmup.start (100, zlink_c_bench::phase_warmup);
    zlink_c_bench::stamp_payload (body, sizeof body, 1, zlink_c_bench::phase_warmup, 0);
    warmup.record (body, sizeof body, 75);
    warmup.close (100);
    assert (warmup.snapshot (100).completed == 1);
    metrics.start (200);
    metrics.record (body, sizeof body, 125);
    assert (metrics.snapshot (200).completed == 0);
    const auto run_dir = std::filesystem::temp_directory_path ()
                         / ("bench-c-send-metrics-" + std::to_string (getpid ()));
    setenv ("BENCH_RUN_DIR", run_dir.c_str (), 1);
    setenv ("DURATION_SECONDS", "5", 1);
    result.implementation = "zlink-c";
    result.pattern = "send-saturation";
    result.size = 1024;
    result.completed = 7;
    result.submitted = 11;
    result.server_received = 11;
    result.elapsed_s = 5.5;
    result.warmup_abandoned = 3;
    assert (zlink_c_bench::write_cell_json (result, ""));
    const auto path = run_dir / "zlink-c-send-saturation-1024/results.json";
    std::ifstream file (path);
    const std::string json ((std::istreambuf_iterator<char> (file)), {});
    assert (json.find ("\"throughput_per_second\": 1.400000") != std::string::npos);
    assert (json.find ("\"warmupSeconds\": 2") != std::string::npos);
    assert (json.find ("\"warmup_abandoned\": 3") != std::string::npos);
    assert (json.find ("\"active_elapsed_ms\": 5000.000000") != std::string::npos);
    result.pattern = "request-serial";
    result.server_received.reset ();
    assert (zlink_c_bench::write_cell_json (result, ""));
    std::ifstream request_file (run_dir / "zlink-c-request-serial-1024/results.json");
    const std::string request_json ((std::istreambuf_iterator<char> (request_file)), {});
    assert (request_json.find ("\"throughput_per_second\": 1.400000") != std::string::npos);
    result.pattern = "send-saturation";
    result.server_received = 11;
    result.completed = 12;
    assert (!zlink_c_bench::write_cell_json (result, ""));
    result.completed = 7;
    result.server_received = 12;
    assert (!zlink_c_bench::write_cell_json (result, ""));
    std::filesystem::remove_all (run_dir);
    body[0] = 0;
    metrics.record (body, sizeof body, 130);
    assert (metrics.snapshot (130).server_errors == 1);
}

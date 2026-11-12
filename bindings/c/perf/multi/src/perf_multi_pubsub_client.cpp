#include "../common/perf_common.hpp"
#include "../common/perf_common_multi.hpp"
#include "../common/perf_multi_client_helpers.hpp"
#include "../common/perf_multi_handshake.hpp"
#include <set>
#include <memory>
#include <thread>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{

static const char *k_pattern = "PUBSUB";
static const zlink_socket_type_t k_client_socket_type = ZLINK_SOCKET_SUB;
static const char *k_pubsub_topic = "bench";

enum pubsub_recv_result_t
{
    pubsub_recv_error = -1,
    pubsub_recv_empty = 0,
    pubsub_recv_payload = 1
};

using perf_multi_client::close_client_monitors;
using perf_multi_client::close_client_sockets;
using perf_multi_client::create_client_sockets;
using perf_multi_client::is_supported_transport;
using perf_multi_client::parse_endpoint_arg;
using perf_multi_client::print_client_result_lines;
using perf_multi_client::refresh_connected_client_auto_hwm;
using perf_multi_client::resolve_case_msg_sizes;
using perf_multi_client::wait_client_connect_ready_all;

pubsub_recv_result_t recv_one_pubsub_message (void *socket,
                                              size_t expected_msg_size,
                                              uint32_t expected_run_id,
                                              perf_multi_metric::header_t *header_out,
                                              double *sample_ns_out,
                                              bool *have_sample_out)
{
    const zlink_routing_id_t *source_rid = NULL;
    zlink_msg_t parts[2];
    size_t part_count = 0;
    char topic[256];
    size_t topic_len = sizeof (topic);
    const int rc = zlink_subscribe (socket, &source_rid, topic, sizeof (topic), &topic_len,
                                    parts, 2u, &part_count, ZLINK_RECV_FLAGS_DONTWAIT);
    if (rc != 0) {
        const int err = zlink_errno ();
        if (err == EAGAIN || err == EINTR)
            return pubsub_recv_empty;
        return pubsub_recv_error;
    }
    zlink_msg_t &part = parts[0];

    if (topic_len != std::strlen (k_pubsub_topic) || source_rid) {
        if (bench_debug_enabled ()) {
            std::cerr << "[multi-pubsub-client] recv shape mismatch topic_len=" << topic_len
                      << " part_count=" << part_count << std::endl;
        }
        zlink_multipart_close (parts, part_count);
        return pubsub_recv_payload;
    }

    if (!perf_zlink_measurement_parts_valid (parts, part_count)) {
        zlink_multipart_close (parts, part_count);
        return pubsub_recv_error;
    }

    perf_multi_metric::header_t header;
    std::memset (&header, 0, sizeof (header));
    const bool decoded = perf_multi_metric::decode_payload_header (zlink_msg_data (&part),
                                                                   zlink_msg_size (&part), &header);
    zlink_multipart_close (parts, part_count);

    if (!decoded || header.magic != perf_multi_metric::k_magic || header.run_id != expected_run_id
        || header.msg_size != static_cast<uint32_t> (expected_msg_size)) {
        if (bench_debug_enabled ()) {
            std::cerr << "[multi-pubsub-client] header mismatch decoded=" << decoded
                      << " magic=" << header.magic << " run=" << header.run_id
                      << " phase=" << static_cast<unsigned int> (header.phase)
                      << " size=" << header.msg_size << " expected_size=" << expected_msg_size
                      << std::endl;
        }
        return pubsub_recv_payload;
    }

    if (header_out)
        *header_out = header;
    if (have_sample_out)
        *have_sample_out = false;
    if (sample_ns_out)
        *sample_ns_out = 0.0;
    const uint64_t now_ns = perf_multi_metric::now_ns ();
    if (header.sent_ts_ns > 0 && now_ns >= header.sent_ts_ns) {
        if (sample_ns_out)
            *sample_ns_out = static_cast<double> (now_ns - header.sent_ts_ns);
        if (have_sample_out)
            *have_sample_out = true;
    }
    return pubsub_recv_payload;
}

bool create_pubsub_poller (const std::vector<void *> &sockets, void **poller_out)
{
    if (!poller_out)
        return false;

    *poller_out = NULL;
    void *poller = zlink_poller_new ();
    if (!poller)
        return false;

    for (size_t i = 0; i < sockets.size (); ++i) {
        if (!sockets[i])
            continue;
        if (zlink_poller_add (poller, sockets[i], sockets[i], ZLINK_POLLIN) != 0) {
            zlink_poller_destroy (&poller);
            return false;
        }
    }

    *poller_out = poller;
    return true;
}

bool run_recv_duration (const std::vector<void *> &sockets,
                        void *poller,
                        const multi_bench_settings_t &settings,
                        size_t msg_size,
                        uint32_t run_id,
                        double *throughput_out,
                        bench_latency_stats_t *latency_out,
                        bench_latency_stats_t *latency_reference_out,
                        uint64_t *inflight_floor_out,
                        double *target_rate_out,
                        perf_multi_handshake::start_signal_state_t *phase_gate,
                        bool *latency_valid_out)
{
    if (!throughput_out || !latency_out || !latency_reference_out || !inflight_floor_out
        || !target_rate_out || !latency_valid_out || sockets.empty () || !poller)
        return false;

    const double active_seconds = static_cast<double> (std::max (1, settings.duration_seconds));
    long recv_count = 0;
    std::map<void *, uint64_t> active_received;
    std::map<void *, uint64_t> paced_received;
    std::map<void *, uint64_t> latency_received;
    for (size_t i = 0; i < sockets.size (); ++i) {
        active_received[sockets[i]] = 0;
        paced_received[sockets[i]] = 0;
        latency_received[sockets[i]] = 0;
    }
    double lat_sum = 0.0;
    long lat_count = 0;
    bench_latency_sampler_t lat_samples;
    bench_latency_sampler_t latency_reference_samples;
    std::vector<zlink_poller_event_t> events (sockets.size ());
    perf_multi_measurement::catch_up_tracker_t *catch_up = NULL;
    uint64_t paced_epoch_ns = 0;
    uint64_t next_catch_up_progress_ns = 0;
    double paced_target_rate = 0.0;
    bool catch_up_reported = false;
    uint64_t inflight_floor = std::numeric_limits<uint64_t>::max ();
    const auto smallest_received = [&] (const std::map<void *, uint64_t> &counts) {
        uint64_t count = std::numeric_limits<uint64_t>::max ();
        for (const auto &entry : counts)
            count = std::min (count, entry.second);
        return count;
    };
    // The publisher waits for PHASE_DRAINED before pacing. Drain every
    // subscriber, with a bounded quiet interval because PUB may drop markers.
    const auto receive_phase = [&] (perf_multi_metric::phase_t phase, double seconds) -> bool {
        const auto deadline = std::chrono::steady_clock::now ()
                              + std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                                std::chrono::duration<double> (seconds));
        const auto drain_deadline = deadline + std::chrono::seconds (10);
        auto last_payload = std::chrono::steady_clock::now ();
        std::set<void *> stopped;
        while (!perf_stop_requested ().load (std::memory_order_acquire)
               && std::chrono::steady_clock::now () < drain_deadline) {
            const auto now = std::chrono::steady_clock::now ();
            std::chrono::steady_clock::time_point done_receipt;
            const bool producer_done =
              perf_multi_handshake::query_start (phase_gate, msg_size, phase, &done_receipt);
            if (producer_done
                && (stopped.size () == sockets.size ()
                    || (now >= deadline
                        && now - std::max (last_payload, done_receipt)
                             >= std::chrono::milliseconds (50)))) {
                return perf_multi_handshake::wait_for_start (phase_gate, msg_size, 1, phase);
            }
            const int poll_rc = zlink_poller_wait (poller, events.empty () ? NULL : &events[0],
                                                   static_cast<int> (events.size ()), 10, NULL);
            if (poll_rc < 0) {
                if (zlink_errno () == EINTR || zlink_errno () == EAGAIN)
                    continue;
                return false;
            }
            for (int i = 0; i < poll_rc; ++i) {
                if ((events[i].events & ZLINK_POLLIN) == 0)
                    continue;
                void *socket = events[i].socket;
                // Bound each subscriber's turn before serving the next ready socket.
                for (size_t received = 0; received < 64; ++received) {
                    perf_multi_metric::header_t header = {};
                    header.phase = perf_multi_metric::phase_unknown;
                    double sample_ns = 0.0;
                    bool have_sample = false;
                    const pubsub_recv_result_t status = recv_one_pubsub_message (
                      socket, msg_size, run_id, &header, &sample_ns, &have_sample);
                    if (status == pubsub_recv_error)
                        return false;
                    if (status == pubsub_recv_empty)
                        break;
                    last_payload = std::chrono::steady_clock::now ();
                    if (last_payload >= drain_deadline) {
                        errno = ETIMEDOUT;
                        return false;
                    }
                    if (phase == perf_multi_metric::phase_latency
                        && perf_multi_handshake::is_phase_end_marker (header, run_id, phase,
                                                                      msg_size)) {
                        stopped.insert (socket);
                        continue;
                    }
                    if (phase == perf_multi_metric::phase_latency
                        && perf_multi_measurement::is_paced_receive_phase (header.phase)) {
                        ++paced_received[socket];
                        if (paced_epoch_ns == 0 && header.sent_ts_ns > 0)
                            paced_epoch_ns = static_cast<uint64_t> (header.sent_ts_ns);
                        if (have_sample)
                            latency_reference_samples.add (sample_ns);
                    }
                    if (header.phase != static_cast<uint8_t> (phase))
                        continue;
                    if (perf_multi_handshake::is_phase_end_marker (header, run_id, phase,
                                                                   msg_size)) {
                        stopped.insert (socket);
                        continue;
                    }
                    if (phase == perf_multi_metric::phase_active) {
                        if (std::chrono::steady_clock::now () < deadline) {
                            ++recv_count;
                            ++active_received[socket];
                        }
                    } else {
                        if (have_sample && catch_up && catch_up->caught_up ()) {
                            ++latency_received[socket];
                            lat_sum += sample_ns;
                            ++lat_count;
                            lat_samples.add (sample_ns);
                        }
                    }
                }
            }
            if (phase == perf_multi_metric::phase_latency && catch_up && paced_epoch_ns != 0) {
                const uint64_t now_ns = perf_multi_metric::now_ns ();
                const double elapsed = (now_ns - paced_epoch_ns) / 1e9;
                const uint64_t scheduled = perf_multi_measurement::scheduled_count (
                  paced_target_rate, paced_epoch_ns, now_ns);
                const uint64_t received = smallest_received (paced_received);
                if (!catch_up->caught_up () && now_ns >= next_catch_up_progress_ns) {
                    const uint64_t in_flight = scheduled > received ? scheduled - received : 0;
                    inflight_floor = std::min (inflight_floor, in_flight);
                    std::cout << "PUBSUB_CATCHUP_PROGRESS," << msg_size << ',' << elapsed << ','
                              << in_flight << ',' << received << ',' << scheduled << ','
                              << paced_target_rate << std::endl;
                    next_catch_up_progress_ns = now_ns + 1000000000ULL;
                }
                if (!catch_up->caught_up () && catch_up->observe (now_ns, received, scheduled)) {
                    std::cout << "PHASE_CATCHUP," << msg_size << std::endl;
                    std::cout << "PUBSUB_CATCHUP_DIAG," << msg_size << ','
                              << (now_ns - paced_epoch_ns) / 1e9 << ','
                              << catch_up->in_flight_at_catch_up () << std::endl;
                    catch_up_reported = true;
                }
            }
        }
        errno = ETIMEDOUT;
        return false;
    };
    if (!receive_phase (perf_multi_metric::phase_active, active_seconds))
        return false;
    const uint64_t min_received = smallest_received (active_received);
    if (min_received == 0)
        return false;
    std::cout << "PHASE_DRAINED," << msg_size << ',' << min_received << std::endl;
    paced_target_rate = perf_multi_measurement::target_rate (min_received, active_seconds);
    perf_multi_measurement::catch_up_tracker_t tracker (paced_target_rate);
    catch_up = &tracker;
    if (!receive_phase (perf_multi_metric::phase_latency, 2.0 * settings.latency_duration_seconds))
        return false;
    *latency_valid_out = catch_up_reported && lat_count > 0;
    if (recv_count <= 0)
        return false;
    std::cout << "PHASE_COMPLETE," << msg_size << ',' << smallest_received (latency_received)
              << std::endl;
    *throughput_out = throughput_per_second (static_cast<uint64_t> (recv_count), active_seconds);
    perf_multi_client::normalize_latency_stats (lat_sum, lat_count, &lat_samples, latency_out);
    *latency_reference_out = latency_reference_samples.snapshot ();
    *inflight_floor_out =
      inflight_floor == std::numeric_limits<uint64_t>::max () ? 0 : inflight_floor;
    *target_rate_out = paced_target_rate;
    return true;
}

inline bool run_single_size_case (const std::vector<void *> &sockets,
                                  void *poller,
                                  const multi_bench_settings_t &base_settings,
                                  size_t scratch_capacity,
                                  const std::string &lib_name,
                                  const std::string &transport,
                                  size_t msg_size,
                                  uint32_t run_id,
                                  perf_multi_handshake::start_signal_state_t *phase_gate)
{
    (void) scratch_capacity;
    if (!perf_multi_handshake::wait_for_start (phase_gate, msg_size,
                                               base_settings.connect_ready_timeout_ms))
        return false;

    double throughput = 0.0;
    bench_latency_stats_t latency;
    bench_latency_stats_t latency_reference;
    uint64_t inflight_floor = 0;
    double target_rate = 0.0;
    bool latency_valid = false;
    const bool ok = run_recv_duration (sockets, poller, base_settings, msg_size, run_id,
                                       &throughput, &latency, &latency_reference, &inflight_floor,
                                       &target_rate, phase_gate, &latency_valid);
    if (!ok) {
        return false;
    }

    if (latency_valid)
        print_client_result_lines (k_pattern, lib_name, transport, msg_size, throughput, latency);
    else {
        print_invalid_latency_result (lib_name, k_pattern, transport, msg_size, throughput,
                                      latency_reference, inflight_floor, target_rate,
                                      "receiver_not_caught_up_by_deadline");
    }

    return true;
}

inline int run_client_benchmark (const std::string &lib_name,
                                 const std::string &transport,
                                 const std::string &endpoint,
                                 size_t fallback_size)
{
    set_perf_multi_pattern_env (k_pattern);

    if (!is_supported_transport (transport)) {
        std::cout << "UNSUPPORTED," << lib_name << "," << k_pattern << "," << transport
                  << std::endl;
        return 0;
    }

    if (!transport_available (transport)) {
        std::cerr << "transport unavailable: " << transport << std::endl;
        return 1;
    }

    const multi_bench_settings_t base_settings = resolve_multi_bench_settings ();
    const std::vector<size_t> msg_sizes = resolve_case_msg_sizes (fallback_size);
    size_t max_msg_size = fallback_size > 0 ? fallback_size : 64;
    for (size_t i = 0; i < msg_sizes.size (); ++i) {
        if (msg_sizes[i] > max_msg_size)
            max_msg_size = msg_sizes[i];
    }

    ctx_guard_t ctx;
    if (!ctx.valid ())
        return 1;

    std::vector<void *> sockets;
    std::vector<ready_monitor_t> monitors;
    void *poller = NULL;
    if (!create_client_sockets (ctx, transport, endpoint, base_settings, k_client_socket_type,
                                max_msg_size, &sockets, &monitors, false)) {
        close_client_monitors (&monitors);
        close_client_sockets (&sockets);
        return 1;
    }
    if (!wait_client_connect_ready_all (monitors, base_settings.connect_ready_timeout_ms)) {
        close_client_monitors (&monitors);
        close_client_sockets (&sockets);
        return 1;
    }
    close_client_monitors (&monitors);

    if (!create_pubsub_poller (sockets, &poller)) {
        if (bench_debug_enabled ()) {
            std::cerr << "[multi-pubsub-client] poller create failed" << std::endl;
        }
        close_client_sockets (&sockets);
        return 1;
    }

    const size_t scratch_capacity = static_cast<size_t> (64);
    const std::shared_ptr<perf_multi_handshake::start_signal_state_t> phase_gate (
      new perf_multi_handshake::start_signal_state_t ());
    perf_stop_requested ().store (false, std::memory_order_release);
    std::thread stdin_watcher ([phase_gate] () {
        std::string line;
        while (std::getline (std::cin, line)) {
            size_t msg_size = 0;
            perf_multi_metric::phase_t phase = perf_multi_metric::phase_unknown;
            if (perf_multi_handshake::parse_size_command_line (line, "START,", &msg_size))
                perf_multi_handshake::signal_start (phase_gate.get (), msg_size);
            else if (perf_multi_handshake::parse_phase_done_line (line, &msg_size, &phase))
                perf_multi_handshake::signal_start (phase_gate.get (), msg_size, phase);
            else if (line == "STOP" || line == "QUIT")
                break;
        }
        perf_stop_requested ().store (true, std::memory_order_release);
        perf_multi_handshake::signal_stop (phase_gate.get ());
    });
    bool ok = true;
    for (size_t si = 0; si < msg_sizes.size (); ++si) {
        const size_t msg_size = msg_sizes[si];
        const uint32_t run_id = static_cast<uint32_t> (si + 1);
        refresh_connected_client_auto_hwm (sockets, k_client_socket_type,
                                           base_settings.hwm, transport, msg_size);
        std::cout << "CLIENT_READY," << msg_size << std::endl;
        if (!run_single_size_case (sockets, poller, base_settings, scratch_capacity, lib_name,
                                   transport, msg_size, run_id, phase_gate.get ())) {
            if (bench_debug_enabled ()) {
                std::cerr << "[multi-pubsub-client] size case failed size=" << msg_size
                          << " run_id=" << run_id << std::endl;
            }
            ok = false;
            break;
        }
        std::cout << "CLIENT_DONE," << msg_size << std::endl;
    }
    zlink_poller_destroy (&poller);
    close_client_sockets (&sockets);
    bool reader_done = false;
    if (ok) {
        const int wait_ms = perf_stop_send_timeout_ms () * perf_stop_send_max_attempts ();
        std::unique_lock<std::mutex> lock (phase_gate->mutex);
        reader_done = phase_gate->cv.wait_for (lock, std::chrono::milliseconds (wait_ms),
                                               [phase_gate] () { return phase_gate->stopped; });
    }
    if (reader_done)
        stdin_watcher.join ();
    else
        stdin_watcher.detach ();
    return ok ? 0 : 1;
}

} // namespace

int main (int argc, char **argv)
{
    if (argc < 4)
        return 1;
    if (!multi_perf_validate_recv_mode_for_pattern (k_pattern))
        return 1;

    const std::string lib_name = argv[1];
    const std::string transport = argv[2];
    const size_t fallback_size = static_cast<size_t> (std::strtoull (argv[3], NULL, 10));

    std::string endpoint;
    if (!parse_endpoint_arg (argc, argv, &endpoint)) {
        std::cerr << "missing --endpoint" << std::endl;
        return 1;
    }

    return run_client_benchmark (lib_name, transport, endpoint, fallback_size);
}

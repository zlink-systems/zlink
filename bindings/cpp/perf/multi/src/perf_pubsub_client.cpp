// PUBSUB multi client benchmark: one-way subscriber receive workload.
// Topology: server PUB(bind, 1) -> client SUB(connect, N)
// Measurement: active-phase receive throughput + header-based latency sample.

#include "../common/perf_common.hpp"
#include "../common/perf_common_multi.hpp"
#include "../common/perf_entry.hpp"
#include "../common/perf_client_helpers.hpp"
#include "../common/perf_metric_header.hpp"

#include "../common/perf_multi_measurement.hpp"

#include <algorithm>
#include <any>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <limits>
#include <vector>

namespace
{

static const char *k_pattern_env = "PUBSUB";
static const char *k_pattern_result = "PUBSUB";
static const char *k_topic = "bench";
static const uint32_t k_run_id = 1U;
static constexpr auto k_receive_poll = std::chrono::milliseconds (10);
static constexpr auto k_drain_quiet = std::chrono::milliseconds (50);
static constexpr auto k_drain_duration = std::chrono::seconds (10);
struct phase_config_t
{
    int active_seconds;
};

struct bench_result_t
{
    unsigned long long active_count;
    perf::multi::bench_latency_stats_t latency;

    bench_result_t () : active_count (0), latency () {}
};

enum pubsub_recv_result_t
{
    pubsub_recv_error = -1,
    pubsub_recv_empty = 0,
    pubsub_recv_payload = 1,
    pubsub_recv_stop = 2
};

pubsub_recv_result_t recv_one_pubsub_message (zlink::sub_socket_t &sock,
                                              size_t expected_msg_size,
                                              perf_metric::header_t *header_out)
{
    zlink::topic_message_t message;
    const int rc = sock.subscribe (message, zlink::recv_flags_t::dontwait);
    if (rc != 0) {
        const int err = errno;
        if (rc == static_cast<int> (zlink::recv_result_t::no_data) || err == EAGAIN || err == EINTR)
            return pubsub_recv_empty;
        return pubsub_recv_error;
    }

    if (message.routing_id () || message.topic ().size () != std::strlen (k_topic))
        return pubsub_recv_payload;

    const bool single_part = message.is_single_part ();
    const auto &part = single_part ? message.first_part () : message.parts ().front ();
    const size_t recv_size = part.size ();
    const void *recv_data = part.data ();
    if (perf::multi::is_stop_token (recv_data, recv_size))
        return single_part ? pubsub_recv_stop : pubsub_recv_error;
    if (recv_size != std::max<size_t> (expected_msg_size, perf_metric::header_size ()))
        return pubsub_recv_payload;

    const bool valid_parts = perf::multi::measurement_part_count () == 1
                               ? single_part
                               : perf::multi::measurement_parts_valid (message.parts ());
    if (!valid_parts)
        return pubsub_recv_error;

    perf_metric::header_t header;
    const bool decoded = perf_metric::decode_payload_header (recv_data, recv_size, &header);
    if (!decoded || header.magic != perf_metric::k_magic || header.run_id != k_run_id
        || header.msg_size != static_cast<uint32_t> (expected_msg_size)) {
        return pubsub_recv_payload;
    }

    if (header_out)
        *header_out = header;
    return pubsub_recv_payload;
}

class pubsub_client_bench_t
{
  public:
    pubsub_client_bench_t (const std::string &transport,
                           const std::string &lib_name,
                           size_t msg_size,
                           const std::string &endpoint,
                           const perf::multi::multi_bench_settings_t &settings) :
        _transport (transport),
        _lib_name (lib_name),
        _msg_size (msg_size),
        _endpoint (endpoint),
        _settings (settings),
        _ctx (),
        _holders (),
        _monitors (),
        _sockets (),
        _poller (),
        _poll_events (settings.clients),
        _phase_cfg (),
        _result (),
        _latency_reference (),
        _inflight_floor ((std::numeric_limits<uint64_t>::max) ()),
        _paced_target_rate (0.0),
        _failure_stage ("setup_sockets")
    {
        _holders.reserve (_settings.clients);
        _monitors.reserve (_settings.clients);
        _sockets.reserve (_settings.clients);

        _phase_cfg.active_seconds = std::max (1, _settings.duration_seconds);
    }

    bool run ()
    {
        if (!setup_sockets ())
            return false;

        std::cout << "CLIENT_READY," << _msg_size << std::endl;
        if (!perf::multi::wait_for_start_from_stdin (_msg_size)) {
            _failure_stage = "start_signal";
            return false;
        }

        std::vector<uint64_t> active_received (_sockets.size (), 0);
        std::vector<bool> active_stop_seen (_sockets.size (), false);
        _failure_stage = "active_receive";
        if (!run_active (active_received, active_stop_seen))
            return false;

        if (_result.active_count == 0) {
            _failure_stage = "no_active_data";
            return false;
        }

        _failure_stage = "active_phase_done";
        if (!perf::multi::wait_for_phase_done_from_stdin (
              _msg_size, static_cast<int> (perf_metric::phase_active)))
            return false;
        _failure_stage = "active_drain";
        if (!drain_active (active_stop_seen))
            return false;
        const uint64_t minimum =
          *std::min_element (active_received.begin (), active_received.end ());
        if (minimum == 0) {
            _failure_stage = "no_active_data_on_subscriber";
            return false;
        }
        std::cout << "PHASE_DRAINED," << _msg_size << ',' << minimum << std::endl;
        const double target_rate = perf_multi_measurement::target_rate (
          minimum, static_cast<double> (_phase_cfg.active_seconds));
        bool caught_up = false;
        uint64_t paced_received = 0;
        _failure_stage = "paced_receive";
        if (!run_paced (target_rate, &caught_up, &paced_received))
            return false;
        if (caught_up) {
            print_result ();
            std::cout << "PUBSUB_LATENCY_DIAG," << _msg_size << ",latency_p50_ms," << std::fixed
                      << std::setprecision (6) << _result.latency.p50_ns / 1000000.0 << std::endl;
        } else {
            print_invalid_latency ();
        }
        std::cout << "PHASE_COMPLETE," << _msg_size << ',' << paced_received << std::endl;
        std::cout << "CLIENT_DONE," << _msg_size << std::endl;
        return true;
    }

    const char *failure_stage () const { return _failure_stage; }

    unsigned long long active_count () const { return _result.active_count; }

  private:
    void close_monitors ()
    {
        for (size_t i = 0; i < _monitors.size (); ++i)
            perf::multi::close_connect_monitor (_monitors[i]);
    }

    bool setup_sockets ()
    {
        for (size_t i = 0; i < _settings.clients; ++i) {
            _holders.emplace_back (new zlink::sub_socket_t (_ctx.ctx ()));
            zlink::sub_socket_t &sock = *_holders.back ();

            (void) sock.set_subscription (std::string ());
            perf::multi::apply_benchmark_socket_options (sock, _settings, _transport);
            if (!perf::multi::setup_tls_client (sock, _transport))
                return false;
            _monitors.push_back (perf::multi::connect_monitor_t ());
            if (!perf::multi::open_connect_monitor (
                  sock, _settings.monitor_hwm, _monitors.back ())) {
                close_monitors ();
                return false;
            }
            try {
                sock.connect (_endpoint);
            }
            catch (const zlink::binding_error_t &) {
                close_monitors ();
                return false;
            }

            _sockets.push_back (&sock);
            _poller.add (sock, zlink::poll_event_flag_t::pollin, _sockets.size () - 1);
        }
        if (!perf::multi::recalculate_auto_hwm (_ctx))
            return false;
        const bool ready =
          perf::multi::wait_connect_ready_all (_monitors, _settings.connect_ready_timeout_ms);
        close_monitors ();
        if (!ready) {
            _failure_stage = "connect_ready";
            return false;
        }
        if (!_holders.empty () && _holders[0].get () && _holders[0]->valid ()) {
            perf::multi::emit_auto_hwm_detail (*_holders[0], "client", "endpoint", _transport,
                                               _msg_size, "sub");
        }
        return !_sockets.empty ();
    }

    bool run_active (std::vector<uint64_t> &received_, std::vector<bool> &stop_seen)
    {
        const auto deadline =
          std::chrono::steady_clock::now () + std::chrono::seconds (_phase_cfg.active_seconds);
        while (std::chrono::steady_clock::now () < deadline) {
            const auto poll_wait =
              std::min (perf::multi::remaining_bounded_timeout (deadline), k_receive_poll);
            const size_t ready =
              _poller.wait (_poll_events.data (), _poll_events.size (), poll_wait);
            for (size_t i = 0; i < ready; ++i) {
                const size_t slot = _poll_events[i].slot;
                if (slot >= _sockets.size () || stop_seen[slot])
                    continue;
                for (size_t n = 0; n < perf_multi_measurement::max_burst_per_sender; ++n) {
                    if (std::chrono::steady_clock::now () >= deadline)
                        return true;
                    perf_metric::header_t header = {};
                    const pubsub_recv_result_t rc =
                      recv_one_pubsub_message (*_sockets[slot], _msg_size, &header);
                    if (rc == pubsub_recv_error)
                        return false;
                    if (rc == pubsub_recv_empty)
                        break;
                    if (rc == pubsub_recv_stop) {
                        stop_seen[slot] = true;
                        if (std::all_of (stop_seen.begin (), stop_seen.end (),
                                         [] (bool stopped) { return stopped; }))
                            return true;
                        break;
                    }
                    if (header.phase == perf_metric::phase_active
                        && std::chrono::steady_clock::now () < deadline) {
                        ++_result.active_count;
                        ++received_[slot];
                    }
                }
            }
        }
        return true;
    }

    bool drain_active (std::vector<bool> &stop_seen)
    {
        const auto deadline = std::chrono::steady_clock::now () + k_drain_duration;
        auto last_payload = std::chrono::steady_clock::now ();
        while (std::chrono::steady_clock::now () < deadline) {
            if (std::all_of (stop_seen.begin (), stop_seen.end (),
                             [] (bool stopped) { return stopped; })
                || std::chrono::steady_clock::now () - last_payload >= k_drain_quiet)
                return true;

            const size_t ready =
              _poller.wait (_poll_events.data (), _poll_events.size (), k_receive_poll);
            for (size_t i = 0; i < ready; ++i) {
                const size_t slot = _poll_events[i].slot;
                if (slot >= _sockets.size () || stop_seen[slot])
                    continue;
                for (size_t n = 0; n < perf_multi_measurement::max_burst_per_sender; ++n) {
                    perf_metric::header_t header = {};
                    const pubsub_recv_result_t rc =
                      recv_one_pubsub_message (*_sockets[slot], _msg_size, &header);
                    if (rc == pubsub_recv_error)
                        return false;
                    if (rc == pubsub_recv_empty)
                        break;
                    last_payload = std::chrono::steady_clock::now ();
                    if (rc == pubsub_recv_stop) {
                        stop_seen[slot] = true;
                        break;
                    }
                }
            }
        }
        errno = ETIMEDOUT;
        return false;
    }

    bool run_paced (double target_rate, bool *caught_up_out, uint64_t *received_out)
    {
        perf_multi_measurement::catch_up_tracker_t tracker (target_rate);
        perf::multi::bench_latency_sampler_t latency;
        perf::multi::bench_latency_sampler_t reference;
        _paced_target_rate = target_rate;
        std::vector<uint64_t> paced_received (_sockets.size (), 0);
        std::vector<uint64_t> latency_received (_sockets.size (), 0);
        std::vector<bool> stop_seen (_sockets.size (), false);
        uint64_t epoch = 0;
        uint64_t catch_up_at = 0;
        uint64_t next_progress_ns = 0;
        const uint64_t duration_ns =
          static_cast<uint64_t> (_settings.latency_duration_seconds) * 1000000000ULL;
        const uint64_t outer_deadline =
          static_cast<uint64_t> (perf_metric::now_ns ()) + 2 * duration_ns;
        uint64_t last_payload_ns = static_cast<uint64_t> (perf_metric::now_ns ());
        const auto drain_deadline =
          outer_deadline
          + std::chrono::duration_cast<std::chrono::nanoseconds> (k_drain_duration).count ();
        while (static_cast<uint64_t> (perf_metric::now_ns ()) < drain_deadline) {
            const uint64_t poll_start = static_cast<uint64_t> (perf_metric::now_ns ());
            const uint64_t sample_end =
              catch_up_at != 0 ? catch_up_at + duration_ns : outer_deadline;
            if (poll_start >= sample_end
                && (std::all_of (stop_seen.begin (), stop_seen.end (),
                                 [] (bool stopped) { return stopped; })
                    || poll_start - last_payload_ns >= static_cast<uint64_t> (
                         std::chrono::duration_cast<std::chrono::nanoseconds> (k_drain_quiet)
                           .count ())))
                break;
            const size_t ready =
              _poller.wait (_poll_events.data (), _poll_events.size (), k_receive_poll);
            for (size_t i = 0; i < ready; ++i) {
                const size_t slot = _poll_events[i].slot;
                if (slot >= _sockets.size ())
                    continue;
                for (size_t n = 0; n < perf_multi_measurement::max_burst_per_sender; ++n) {
                    perf_metric::header_t header = {};
                    const pubsub_recv_result_t rc =
                      recv_one_pubsub_message (*_sockets[slot], _msg_size, &header);
                    if (rc == pubsub_recv_error)
                        return false;
                    if (rc == pubsub_recv_empty)
                        break;
                    last_payload_ns = static_cast<uint64_t> (perf_metric::now_ns ());
                    if (rc == pubsub_recv_stop) {
                        stop_seen[slot] = true;
                        break;
                    }

                    if (!perf_multi_measurement::is_paced_receive_phase (header.phase))
                        continue;
                    ++paced_received[slot];
                    if (epoch == 0 && header.sent_ts_ns > 0)
                        epoch = static_cast<uint64_t> (header.sent_ts_ns);
                    reference.add (
                      perf_metric::elapsed_latency_ns (perf_metric::now_ns (), header.sent_ts_ns));
                    if (catch_up_at != 0 && header.phase == perf_metric::phase_latency
                        && header.sent_ts_ns > 0
                        && static_cast<uint64_t> (header.sent_ts_ns) < catch_up_at + duration_ns) {
                        ++latency_received[slot];
                        latency.add (perf_metric::elapsed_latency_ns (perf_metric::now_ns (),
                                                                      header.sent_ts_ns));
                    }
                }
            }
            const uint64_t now = static_cast<uint64_t> (perf_metric::now_ns ());
            if (epoch != 0 && catch_up_at == 0 && now <= epoch + duration_ns) {
                const uint64_t scheduled =
                  perf_multi_measurement::scheduled_count (target_rate, epoch, now);
                const bool caught_up = tracker.observe (now, paced_received, scheduled);
                if (now >= next_progress_ns) {
                    const uint64_t in_flight = tracker.in_flight ();
                    _inflight_floor = std::min (_inflight_floor, in_flight);
                    std::cout << "PUBSUB_CATCHUP_PROGRESS," << _msg_size << ','
                              << (now - epoch) / 1e9 << ',' << in_flight << ','
                              << tracker.min_received () << ',' << scheduled << ',' << target_rate
                              << std::endl;
                    next_progress_ns = now + 1000000000ULL;
                }

                if (caught_up) {
                    catch_up_at = now;
                    std::cout << "PHASE_CATCHUP," << _msg_size << std::endl;
                    std::cout << "PUBSUB_CATCHUP_DIAG," << _msg_size << ',' << (now - epoch) / 1e9
                              << ',' << tracker.in_flight_at_catch_up ()
                              << ",window_start_in_flight," << tracker.window_start_in_flight ()
                              << std::endl;
                }
            }
        }
        *caught_up_out = catch_up_at != 0 && latency.count () > 0;
        *received_out = *std::min_element (latency_received.begin (), latency_received.end ());
        _result.latency = latency.snapshot ();
        _latency_reference = reference.snapshot ();
        if (_inflight_floor == (std::numeric_limits<uint64_t>::max) ())
            _inflight_floor = 0;
        return true;
    }

    void print_invalid_latency () const
    {
        const double throughput =
          static_cast<double> (_result.active_count) / _phase_cfg.active_seconds;
        const std::string prefix = "RESULT," + _lib_name + "," + k_pattern_result + "," + _transport
                                   + "," + std::to_string (_msg_size);
        std::cout << prefix << ",throughput," << throughput << std::endl;
        std::cout << prefix << ",bandwidth," << throughput * _msg_size / 1000000.0 << std::endl;
        for (const char *metric : {"latency", "latency_p95", "latency_p99"})
            std::cout << prefix << ',' << metric << ",invalid:receiver_not_caught_up_by_deadline"
                      << std::endl;
        const std::string reference_prefix = "REFERENCE," + _lib_name + "," + k_pattern_result + ","
                                             + _transport + "," + std::to_string (_msg_size);
        std::cout << reference_prefix << ",latency_ref_p50,"
                  << _latency_reference.p50_ns / 1000000.0 << std::endl;
        std::cout << reference_prefix << ",latency_ref_p99,"
                  << _latency_reference.p99_ns / 1000000.0 << std::endl;
        std::cout << reference_prefix << ",inflight_floor," << _inflight_floor << std::endl;
        std::cout << reference_prefix << ",inflight_floor_ms,"
                  << (_paced_target_rate > 0.0 ? _inflight_floor / _paced_target_rate * 1000.0
                                               : 0.0)
                  << std::endl;
    }

    void print_result () const
    {
        perf::multi::print_client_result_lines (_lib_name, k_pattern_result, _transport, _msg_size,
                                                _result.active_count, _phase_cfg.active_seconds,
                                                1.0, _result.latency);
    }

  private:
    const std::string _transport;
    const std::string _lib_name;
    const size_t _msg_size;
    const std::string _endpoint;
    const perf::multi::multi_bench_settings_t _settings;

    perf::multi::ctx_guard_t _ctx;
    std::vector<std::unique_ptr<zlink::sub_socket_t>> _holders;
    std::vector<perf::multi::connect_monitor_t> _monitors;
    std::vector<zlink::sub_socket_t *> _sockets;
    zlink::poller_t _poller;
    std::vector<zlink::poll_event_t> _poll_events;

    phase_config_t _phase_cfg;
    bench_result_t _result;
    perf::multi::bench_latency_stats_t _latency_reference;
    uint64_t _inflight_floor;
    double _paced_target_rate;
    const char *_failure_stage;
};

} // namespace

bool perf_pubsub_client (const std::string &lib_name,
                         const std::string &transport,
                         size_t msg_size,
                         const std::string &endpoint)
{
    perf::multi::set_perf_pattern_env (k_pattern_env);

    if (!perf::multi::is_supported_transport (transport)) {
        std::cout << "UNSUPPORTED," << lib_name << "," << k_pattern_result << "," << transport
                  << std::endl;
        return true;
    }

    const perf::multi::multi_bench_settings_t settings =
      perf::multi::resolve_multi_bench_settings ();

    pubsub_client_bench_t bench (transport, lib_name, msg_size, endpoint, settings);
    try {
        if (bench.run ())
            return true;
    }
    catch (const zlink::binding_error_t &err) {
        std::cerr << "PUBSUB_CLIENT_ERROR," << err.what () << std::endl;
    }
        std::cerr << "PUBSUB_CLIENT_FAIL,stage=" << bench.failure_stage ()
                  << ",transport=" << transport << ",size=" << msg_size
                  << ",active=" << bench.active_count () << std::endl;
        return false;
}

int main (int argc, char **argv)
{
    if (argc < 4) {
        std::cerr << "usage: <lib_name> <transport> <size> --endpoint <endpoint>" << std::endl;
        return 1;
    }

    const std::string lib_name = argv[1];
    const std::string transport = argv[2];
    const size_t size = static_cast<size_t> (std::strtoull (argv[3], NULL, 10));
    if (size == 0)
        return 1;

    const std::string endpoint = perf::multi::parse_endpoint_arg (argc, argv);
    if (endpoint.empty ()) {
        std::cerr << "missing --endpoint" << std::endl;
        return 1;
    }

    return perf_pubsub_client (lib_name, transport, size, endpoint) ? 0 : 1;
}

#include "../common/bench_common.hpp"
#include "../common/perf_single_latency.hpp"
#include "../common/perf_single_metric_header.hpp"
#include "../common/perf_single_monitor.hpp"
#include "../common/perf_single_one_way.hpp"
#include "../common/perf_single_phase.hpp"
#include <zlink.h>

#include <algorithm>
#include <vector>

namespace
{

typedef perf_single_one_way::state_t dealer_recv_state_t;

} // namespace

void run_dealer_dealer (const std::string &transport, size_t msg_size, const std::string &lib_name)
{
    if (!transport_available (transport))
        return;

    auto print_fail = [&] () {
        print_fail_result (lib_name, "DEALER_DEALER", transport, msg_size);
    };

    const size_t payload_size = std::max<size_t> (msg_size, perf_single_metric::header_size ());
    std::vector<char> payload (payload_size, 'a');
    dealer_recv_state_t state;
    state.run_id = next_single_metric_run_id ();
    state.msg_size = msg_size;
    state.payload_size = payload_size;

    ctx_guard_t ctx;
    if (!ctx.valid ()) {
        print_fail ();
        return;
    }
    if (!apply_single_auto_hwm_msg_unit (ctx.get (), msg_size)) {
        print_fail ();
        return;
    }

    socket_guard_t receiver (ctx.get (), ZLINK_SOCKET_DEALER);
    socket_guard_t sender (ctx.get (), ZLINK_SOCKET_DEALER);
    if (!receiver.valid () || !sender.valid ()) {
        print_fail ();
        return;
    }

    if (!setup_connected_pair (receiver.get (), sender.get (), transport,
                               lib_name + "_dealer_dealer")) {
        print_fail ();
        return;
    }

    const int duration_s = std::max (1, resolve_single_duration_seconds ());
    const int recv_timeout_ms = resolve_single_recv_timeout_ms ();
    unsigned long long received = 0;
    latency_stats_t latency;
    if (!perf_single_one_way::run_active_phase (
          sender.get (), receiver.get (), &payload, &state, duration_s, recv_timeout_ms,
          "perf-dealer-dealer",
          [] (void *sender, std::vector<char> *active_payload,
              const dealer_recv_state_t &active_state, uint64_t seq) {
              zlink_msg_t part;
              if (!perf_single_one_way::init_active_payload_part (&part, active_payload,
                                                                  active_state, seq)) {
                  return perf_single_one_way::send_step_fatal;
              }
              return perf_single_one_way::send_socket_active_message (sender, &part, ZLINK_DONTWAIT,
                                                                      true);
          },
          [] (void *socket, size_t expected_size, int flags,
              perf_single_metric::header_t *header_out, bool *header_ok_out) {
              return perf_single_one_way::recv_single_part_header_flags (
                socket, expected_size, flags, "perf-dealer-dealer", header_out, header_ok_out);
          },
          [] (void *sender) { return perf_single_one_way::send_stop_token_socket (sender); },
          &received, &latency)) {
        print_fail ();
        return;
    }

    emit_single_socket_hwm_detail (receiver.get (), "DEALER_DEALER", transport, "receiver",
                                   ZLINK_SOCKET_DEALER, msg_size);
    emit_single_socket_hwm_detail (sender.get (), "DEALER_DEALER", transport, "sender",
                                   ZLINK_SOCKET_DEALER, msg_size);
    print_result (lib_name, "DEALER_DEALER", transport, msg_size,
                  static_cast<double> (received) / static_cast<double> (duration_s),
                  latency.mean_ns, latency.p95_ns, latency.p99_ns);
}

int main (int argc, char **argv)
{
    return run_standard_bench_main (argc, argv, "DEALER_DEALER", run_dealer_dealer);
}

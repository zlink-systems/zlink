#include "perf_common.hpp"
#include "perf_multi_measurement.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <deque>

namespace
{
using perf_multi_measurement::max_burst_per_sender;

struct echo_fixture_t
{
    perf_multi_measurement::schedule_t schedule {1e9, 0};
    std::deque<std::vector<char>> replies;
    zlink_poller_event_t ready {};
    size_t received = 0;
    std::vector<size_t> send_positions;
};

echo_fixture_t fixture;

zlink_config_result_t add_echo_socket (void *, void *socket, void *user_data, short)
{
    fixture.ready.socket = socket;
    fixture.ready.user_data = user_data;
    fixture.ready.events = ZLINK_POLLIN;
    return ZLINK_CONFIG_OK;
}

int poll_echo_socket (void *, zlink_poller_event_t *events, int, long timeout,
                      zlink_config_result_t *)
{
    // Accumulate three bursts before exposing the first readable event.
    if (!fixture.replies.empty ()
        && (fixture.received != 0 || fixture.replies.size () >= 3 * max_burst_per_sender)) {
        events[0] = fixture.ready;
        return 1;
    }
    if (timeout > 0)
        std::this_thread::sleep_for (std::chrono::milliseconds (timeout));
    return 0;
}

zlink_submit_result_t send_echo_request (void *, zlink_msg_t *parts, size_t part_count,
                                        zlink_send_flags_t, void *, zlink_completion_id_t *)
{
    const char *data = static_cast<const char *> (zlink_msg_data (&parts[0]));
    fixture.replies.emplace_back (data, data + zlink_msg_size (&parts[0]));
    zlink_multipart_close (parts, part_count);
    if (fixture.received != 0) {
        fixture.send_positions.push_back (fixture.received);
        // Give each receive interval one due send without depending on wall-clock timing.
        fixture.schedule.epoch_ns = perf_multi_metric::now_ns () + 10000000000ULL;
    }
    return ZLINK_SUBMIT_OK;
}

int receive_echo_reply (void *, const zlink_routing_id_t **source, zlink_msg_t *parts,
                        size_t, size_t *part_count, zlink_recv_flags_t)
{
    if (fixture.replies.empty ()) {
        errno = EAGAIN;
        return -1;
    }

    const std::vector<char> &reply = fixture.replies.front ();
    assert (zlink_msg_init_size (&parts[0], reply.size ()) == 0);
    std::memcpy (zlink_msg_data (&parts[0]), reply.data (), reply.size ());
    fixture.replies.pop_front ();
    *source = NULL;
    *part_count = perf_measurement_part_count ();
    if (*part_count == 2)
        assert (zlink_msg_init (&parts[1]) == 0);

    ++fixture.received;
    if (fixture.received == 1 || fixture.received == max_burst_per_sender + 1)
        fixture.schedule.epoch_ns = 0;
    return 0;
}
}

// Exercise the production loop with a scripted readable socket and real message records.
#define zlink_poller_add add_echo_socket
#define zlink_poller_wait poll_echo_socket
#define zlink_send send_echo_request
#define zlink_recv receive_echo_reply
#include "perf_multi_client_helpers.hpp"
#undef zlink_recv
#undef zlink_send
#undef zlink_poller_wait
#undef zlink_poller_add

int main ()
{
    const std::vector<void *> sockets (1, &fixture);
    multi_bench_settings_t settings {};
    settings.duration_seconds = 1;
    settings.latency_duration_seconds = 1;
    std::vector<char> payload (64);
    assert (perf_multi_client::run_echo_window_round_robin (
      sockets, settings, payload, payload.size (), payload.size (), "", false, 1,
      perf_multi_metric::phase_latency, payload.size (), 0.25, true, false, false,
      NULL, NULL, NULL, NULL, NULL, NULL, &fixture.schedule));

    const std::vector<size_t> expected {max_burst_per_sender, 2 * max_burst_per_sender};
    std::cout << "Expected send positions " << expected[0] << ' ' << expected[1]
              << ". Observed";
    for (size_t position : fixture.send_positions)
        std::cout << ' ' << position;
    std::cout << std::endl;
    assert (fixture.send_positions == expected);
    assert (fixture.received == 3 * max_burst_per_sender + expected.size ());
}

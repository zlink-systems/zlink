#include "../common/perf_common.hpp"
#include "../common/perf_common_multi.hpp"
#include "../common/perf_multi_client_helpers.hpp"
#include "../common/perf_multi_handshake.hpp"
#include "../common/perf_multi_metric_header.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace
{

static const char *k_pattern = "MULTI_DEALER_DEALER";
static const char *k_token = "dealer_dealer";
static const zlink_socket_type_t k_server_socket_type = ZLINK_SOCKET_DEALER;

using perf_multi_client::normalize_latency_stats;

enum recv_result_t
{
    recv_ok = 0,
    recv_none = 1,
    recv_fatal = 2,
    recv_stop = 3
};

struct server_send_state_t
{
    server_send_state_t () :
        socket (NULL),
        poller (NULL),
        wait_token (0),
        retry_ready (false),
        retained_latency_ack_payload (),
        event ()
    {
        std::memset (&event, 0, sizeof (event));
    }

    void *socket;
    void *poller;
    zlink_completion_id_t wait_token;
    bool retry_ready;
    std::vector<char> retained_latency_ack_payload;
    zlink_poller_event_t event;
};

inline short server_send_events (const server_send_state_t &state)
{
    short events = ZLINK_POLLCOMPLETION;
    if (state.wait_token == 0)
        events = static_cast<short> (events | ZLINK_POLLIN);
    else
        events = static_cast<short> (events | ZLINK_POLLOUT);
    return events;
}

inline bool update_server_send_events (server_send_state_t *state)
{
    return state && state->poller && state->socket
           && zlink_poller_modify (state->poller, state->socket,
                                   server_send_events (*state))
                == ZLINK_CONFIG_OK;
}

inline void close_server_send_state (server_send_state_t *state)
{
    if (!state)
        return;
    if (state->poller)
        zlink_poller_destroy (&state->poller);
    state->retained_latency_ack_payload.clear ();
    state->socket = NULL;
    state->wait_token = 0;
    state->retry_ready = false;
}

inline bool init_server_send_state (void *socket, server_send_state_t *state)
{
    if (!socket || !state)
        return false;
    close_server_send_state (state);
    state->socket = socket;
    state->poller = zlink_poller_new ();
    if (!state->poller)
        return false;
    if (zlink_poller_add (state->poller, socket, state,
                          server_send_events (*state))
        != ZLINK_CONFIG_OK) {
        close_server_send_state (state);
        return false;
    }
    return true;
}

inline void clear_retained_latency_ack (server_send_state_t *state)
{
    if (!state)
        return;
    state->wait_token = 0;
    state->retry_ready = false;
    state->retained_latency_ack_payload.clear ();
}

inline bool submit_retained_latency_ack (server_send_state_t *state)
{
    if (!state || !state->socket || state->wait_token != 0
        || state->retained_latency_ack_payload.empty ()) {
        errno = EBUSY;
        return false;
    }

    zlink_msg_t payload;
    if (zlink_msg_init_size (&payload,
                             state->retained_latency_ack_payload.size ()) != 0)
        return false;
    std::memcpy (zlink_msg_data (&payload),
                 state->retained_latency_ack_payload.data (),
                 state->retained_latency_ack_payload.size ());

    zlink_completion_id_t wait_token = 0;
    const zlink_submit_result_t rc = zlink_send_part (
      state->socket, &payload, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
      state->socket, &wait_token);
    const int submit_errno = rc == ZLINK_SUBMIT_OK ? 0 : zlink_errno ();
    zlink_msg_close (&payload);

    if (rc == ZLINK_SUBMIT_OK && wait_token == 0) {
        clear_retained_latency_ack (state);
        return update_server_send_events (state);
    }
    if (rc == ZLINK_SUBMIT_BACKPRESSURED
        && (submit_errno == EAGAIN || submit_errno == EWOULDBLOCK)
        && wait_token != 0) {
        state->wait_token = wait_token;
        state->retry_ready = false;
        if (!update_server_send_events (state))
            return false;
        errno = submit_errno;
        return true;
    }

    errno = rc == ZLINK_SUBMIT_OK || rc == ZLINK_SUBMIT_BACKPRESSURED
              ? EPROTO
              : (submit_errno != 0 ? submit_errno : EIO);
    return false;
}

inline bool send_latency_ack (server_send_state_t *state,
                              const zlink_msg_t *payload)
{
    if (!state || !state->socket || !payload
        || !state->retained_latency_ack_payload.empty ()) {
        errno = EBUSY;
        return false;
    }

    const size_t payload_size =
      zlink_msg_size (const_cast<zlink_msg_t *> (payload));
    if (payload_size == 0) {
        errno = EPROTO;
        return false;
    }
    state->retained_latency_ack_payload.resize (payload_size);
    std::memcpy (state->retained_latency_ack_payload.data (),
                 zlink_msg_data (const_cast<zlink_msg_t *> (payload)),
                 payload_size);
    return submit_retained_latency_ack (state);
}

inline bool defer_latency_ack (server_send_state_t *state, const zlink_msg_t *payload)
{
    if (!state || !payload || !state->retained_latency_ack_payload.empty ()) {
        errno = EPROTO;
        return false;
    }

    const size_t payload_size = zlink_msg_size (payload);
    if (payload_size == 0) {
        errno = EPROTO;
        return false;
    }
    state->retained_latency_ack_payload.resize (payload_size);
    std::memcpy (state->retained_latency_ack_payload.data (),
                 zlink_msg_data (const_cast<zlink_msg_t *> (payload)), payload_size);
    return true;
}

inline bool send_deferred_latency_ack (server_send_state_t *state)
{
    if (!state || state->retained_latency_ack_payload.empty ()) {
        errno = EPROTO;
        return false;
    }
    return submit_retained_latency_ack (state);
}

inline bool record_server_writable (server_send_state_t *state,
                                    const zlink_completion_t &completion)
{
    if (!state || completion.kind != ZLINK_COMPLETION_WRITABLE
        || completion.completion_id == 0
        || completion.user_context != state->socket
        || completion.peer_rid.size != 0
        || state->retained_latency_ack_payload.empty ()
        || state->wait_token == 0
        || completion.completion_id != state->wait_token) {
        errno = EPROTO;
        return false;
    }

    state->wait_token = 0;
    if (completion.send_result == ZLINK_SEND_ADMITTED
        && completion.send_terminal_errno == 0) {
        state->retry_ready = true;
        return true;
    }

    const int terminal_errno = completion.send_terminal_errno;
    clear_retained_latency_ack (state);
    errno = terminal_errno != 0 ? terminal_errno : EIO;
    return false;
}

inline bool drain_server_writable (server_send_state_t *state)
{
    if (!state || !state->socket)
        return false;
    for (;;) {
        zlink_completion_t completion;
        std::memset (&completion, 0, sizeof (completion));
        completion.struct_size = sizeof (completion);
        const zlink_recv_result_t rc = zlink_completion_recv (
          state->socket, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA)
            break;
        if (rc != ZLINK_RECV_OK)
            return false;

        const bool valid = record_server_writable (state, completion);
        const int completion_errno = valid ? 0 : errno;
        zlink_completion_close (&completion);
        if (!valid) {
            errno = completion_errno;
            return false;
        }
    }

    // WRITABLE only grants another admission attempt. Retry after the queue
    // reaches NO_DATA, retaining these exact bytes again if credit was raced.
    if (state->retry_ready && !submit_retained_latency_ack (state))
        return false;
    return true;
}

inline bool decode_and_match_header (const zlink_msg_t *msg,
                                     size_t expected_msg_size,
                                     uint32_t expected_run_id,
                                     perf_multi_metric::phase_t expected_phase,
                                     perf_multi_metric::header_t *header_out)
{
    if (!msg || !header_out)
        return false;

    if (!perf_multi_metric::decode_payload_header (zlink_msg_data (const_cast<zlink_msg_t *> (msg)),
                                                   zlink_msg_size (const_cast<zlink_msg_t *> (msg)),
                                                   header_out)) {
        return false;
    }

    return header_out->magic == perf_multi_metric::k_magic && header_out->run_id == expected_run_id
           && header_out->phase == static_cast<uint32_t> (expected_phase)
           && header_out->msg_size == static_cast<uint32_t> (expected_msg_size);
}

inline recv_result_t receive_one_message (server_send_state_t *send_state,
                                          int flags,
                                          size_t expected_msg_size,
                                          uint32_t expected_run_id,
                                          perf_multi_metric::phase_t expected_phase,
                                          bool *detail_emitted,
                                          const std::string *transport,
                                          bool count_message,
                                          bool collect_latency,
                                          long *message_count,
                                          double *lat_sum,
                                          long *lat_count,
                                          bench_latency_sampler_t *lat_samples,
                                          bool echo_matched_payload,
                                          bool *record_boundary_out)
{
    void *server = send_state ? send_state->socket : NULL;
    if (!server)
        return recv_fatal;
    if (record_boundary_out)
        *record_boundary_out = false;

    const zlink_routing_id_t *source_rid = NULL;
    zlink_msg_t part;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    if (zlink_msg_init (&part) != 0)
        return recv_fatal;
    const int rc = zlink_recv_part (server, &source_rid, &part, &has_more,
                                    static_cast<zlink_recv_flags_t> (flags));
    if (rc != 0) {
        const int err = zlink_errno ();
        zlink_msg_close (&part);
        if (err == EAGAIN || err == EINTR || err == ETIMEDOUT)
            return recv_none;
        if (bench_transition_debug_enabled ()) {
            std::cerr << "[multi-dealer-dealer-server] recv fatal err=" << err
                      << " size=" << expected_msg_size << " run=" << expected_run_id
                      << " phase=" << static_cast<unsigned int> (expected_phase) << std::endl;
        }
        return recv_fatal;
    }
    if (record_boundary_out)
        *record_boundary_out = has_more == ZLINK_PART_FINAL;

    if (source_rid) {
        if (bench_transition_debug_enabled ()) {
            std::cerr << "[multi-dealer-dealer-server] unexpected recv metadata"
                      << " rid=" << (source_rid ? 1 : 0)
                      << " has_more=" << static_cast<int> (has_more)
                      << " size=" << expected_msg_size << " run=" << expected_run_id
                      << " phase=" << static_cast<unsigned int> (expected_phase) << std::endl;
        }
        zlink_msg_close (&part);
        return recv_fatal;
    }

    if (is_stop_token_message (part)) {
        if (has_more != ZLINK_PART_FINAL) {
            zlink_msg_close (&part);
            return recv_fatal;
        }
        zlink_msg_close (&part);
        return recv_stop;
    }
    // This socket multiplexes parts from all connected DEALER pipes. The
    // next zlink_recv_part() call may select another ready pipe, so it cannot
    // be used to validate the empty measurement tail for the part just read.
    // Drain every part independently and let the metric header identify the
    // one payload frame that contributes to throughput and latency. Empty
    // tails and other non-metric parts are consumed without being counted.

    perf_multi_metric::header_t header;
    std::memset (&header, 0, sizeof (header));
    const bool matched =
      decode_and_match_header (&part, expected_msg_size, expected_run_id, expected_phase, &header);

    if (!matched && bench_debug_enabled ()) {
        std::cerr << "[multi-dealer-dealer-server] header mismatch expected_size="
                  << expected_msg_size << " expected_run=" << expected_run_id
                  << " expected_phase=" << static_cast<unsigned int> (expected_phase)
                  << " got_magic=" << header.magic << " got_run=" << header.run_id
                  << " got_phase=" << static_cast<unsigned int> (header.phase)
                  << " got_size=" << header.msg_size << std::endl;
    }
    if (matched && count_message && message_count)
        (*message_count)++;
    if (matched && detail_emitted && !*detail_emitted && transport) {
        perf_print_auto_hwm_snapshot (server, false, "server", *transport, true, expected_msg_size,
                                      k_server_socket_type);
        *detail_emitted = true;
    }

    if (matched && collect_latency && lat_sum && lat_count) {
        const uint64_t now_ns = perf_multi_metric::now_ns ();
        if (header.sent_ts_ns > 0 && now_ns >= header.sent_ts_ns) {
            const double sample_ns = static_cast<double> (now_ns - header.sent_ts_ns);
            *lat_sum += sample_ns;
            (*lat_count)++;
            if (lat_samples)
                lat_samples->add (sample_ns);
        }
    }

    if (echo_matched_payload) {
        bool ack_ok = true;
        if (matched) {
            ack_ok = has_more == ZLINK_PART_FINAL
                       ? send_latency_ack (send_state, &part)
                       : defer_latency_ack (send_state, &part);
        } else if (has_more == ZLINK_PART_FINAL
                   && !send_state->retained_latency_ack_payload.empty ()) {
            // The measurement record is payload MORE + empty FINAL by
            // default. Delay the ACK until FINAL is consumed so the client
            // cannot submit the next global in-flight record while this
            // DEALER is still between parts.
            ack_ok = zlink_msg_size (&part) == 0
                     && send_deferred_latency_ack (send_state);
        }
        if (!ack_ok) {
            const int ack_errno = zlink_errno ();
            zlink_msg_close (&part);
            if (ack_errno != 0)
                errno = ack_errno;
            return recv_fatal;
        }
    }

    zlink_msg_close (&part);
    return recv_ok;
}

inline bool drain_non_blocking_messages (server_send_state_t *send_state,
                                         size_t expected_msg_size,
                                         uint32_t expected_run_id,
                                         perf_multi_metric::phase_t expected_phase,
                                         bool *detail_emitted,
                                         const std::string *transport,
                                         bool count_message,
                                         bool collect_latency,
                                         long *message_count,
                                         double *lat_sum,
                                         long *lat_count,
                                         bench_latency_sampler_t *lat_samples,
                                         size_t *stop_count,
                                         size_t expected_stop_count,
                                         bool echo_matched_payload,
                                         const std::chrono::steady_clock::time_point
                                           *count_deadline,
                                         const std::chrono::steady_clock::time_point &deadline)
{
    while (!perf_stop_requested ().load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < deadline
           && (!stop_count || *stop_count < expected_stop_count)) {
        const bool count_this_message =
          count_message
          && (!count_deadline || std::chrono::steady_clock::now () < *count_deadline);
        bool record_boundary = false;
        const recv_result_t status =
          receive_one_message (send_state, ZLINK_DONTWAIT, expected_msg_size, expected_run_id,
                               expected_phase, detail_emitted, transport, count_this_message,
                               collect_latency, message_count, lat_sum, lat_count, lat_samples,
                               echo_matched_payload, &record_boundary);
        if (status == recv_none)
            break;
        if (status == recv_fatal)
            return false;
        if (status == recv_stop) {
            if (stop_count)
                ++(*stop_count);
            if (echo_matched_payload && send_state->wait_token != 0)
                break;
            continue;
        }
        if (echo_matched_payload && record_boundary
            && send_state->wait_token != 0) {
            break;
        }
    }
    return true;
}

inline int server_wait_ms (const std::chrono::steady_clock::time_point &deadline,
                           int maximum_ms)
{
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ();
    if (now >= deadline)
        return 0;
    const long long remaining =
      std::chrono::duration_cast<std::chrono::milliseconds> (deadline - now).count ();
    return static_cast<int> (std::max<long long> (1, std::min<long long> (maximum_ms, remaining)));
}

inline bool run_receive_phase (server_send_state_t *send_state,
                               size_t expected_msg_size,
                               uint32_t expected_run_id,
                               perf_multi_metric::phase_t expected_phase,
                               bool *detail_emitted,
                               const std::string *transport,
                               double maximum_wait_seconds,
                               double count_window_seconds,
                               bool count_message,
                               bool collect_latency,
                               bool echo_matched_payload,
                               long *message_count,
                               double *lat_sum,
                               long *lat_count,
                               bench_latency_sampler_t *lat_samples,
                               size_t expected_stop_count)
{
    if (!send_state || !send_state->socket || !send_state->poller || expected_stop_count == 0
        || maximum_wait_seconds <= 0.0) {
        errno = EINVAL;
        return false;
    }

    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::duration_cast<std::chrono::steady_clock::duration> (
        std::chrono::duration<double> (maximum_wait_seconds));
    std::chrono::steady_clock::time_point count_deadline;
    const std::chrono::steady_clock::time_point *count_deadline_ptr = NULL;
    if (count_message && count_window_seconds > 0.0) {
        count_deadline = std::chrono::steady_clock::now ()
                         + std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                           std::chrono::duration<double> (count_window_seconds));
        count_deadline_ptr = &count_deadline;
    }
    size_t stop_count = 0;

    while (!perf_stop_requested ().load (std::memory_order_acquire)
           && stop_count < expected_stop_count
           && std::chrono::steady_clock::now () < deadline) {
        // WRITABLE is level-held through POLLOUT and POLLCOMPLETION. Drain the
        // queue before deciding whether POLLIN stays gated, then resubmit the
        // retained ACK from that same event-loop turn.
        if (send_state->wait_token != 0
            && !drain_server_writable (send_state)) {
            return false;
        }
        std::memset (&send_state->event, 0, sizeof (send_state->event));
        const int event_count = zlink_poller_wait (send_state->poller, &send_state->event, 1,
                                                   server_wait_ms (deadline, 50), NULL);
        if (event_count < 0) {
            if (zlink_errno () == EINTR || zlink_errno () == EAGAIN)
                continue;
            return false;
        }
        if (event_count == 0)
            continue;

        if (send_state->event.socket != send_state->socket
            || send_state->event.user_data != send_state) {
            errno = EPROTO;
            return false;
        }

        if ((send_state->event.events
             & (ZLINK_POLLOUT | ZLINK_POLLCOMPLETION)) != 0
            && !drain_server_writable (send_state)) {
            return false;
        }

        if ((send_state->event.events & ZLINK_POLLIN) != 0
            && (!echo_matched_payload || send_state->wait_token == 0)) {
            if (!drain_non_blocking_messages (
                  send_state, expected_msg_size, expected_run_id, expected_phase, detail_emitted,
                  transport, count_message, collect_latency, message_count, lat_sum, lat_count,
                  lat_samples, &stop_count, expected_stop_count, echo_matched_payload,
                  count_deadline_ptr, deadline)) {
                return false;
            }
        }
    }

    if (stop_count != expected_stop_count) {
        errno = ETIMEDOUT;
        return false;
    }
    if (!send_state->retained_latency_ack_payload.empty ()
        && send_state->wait_token == 0) {
        errno = EPROTO;
        return false;
    }

    // A latency ACK may still be waiting for write credit when the final stop
    // arrives. Admit its retained bytes before the phase state is reused.
    while (send_state->wait_token != 0
           && std::chrono::steady_clock::now () < deadline) {
        if (!drain_server_writable (send_state))
            return false;
        if (send_state->wait_token == 0)
            break;
        std::memset (&send_state->event, 0, sizeof (send_state->event));
        const int event_count = zlink_poller_wait (send_state->poller, &send_state->event, 1,
                                                   server_wait_ms (deadline, 50), NULL);
        if (event_count < 0) {
            if (zlink_errno () == EINTR || zlink_errno () == EAGAIN)
                continue;
            return false;
        }
        if (event_count > 0
            && (send_state->event.events
                & (ZLINK_POLLOUT | ZLINK_POLLCOMPLETION)) != 0
            && !drain_server_writable (send_state)) {
            return false;
        }
    }
    if (send_state->wait_token != 0
        || !send_state->retained_latency_ack_payload.empty ()) {
        errno = ETIMEDOUT;
        return false;
    }
    return true;
}

inline bool run_one_size_benchmark (server_send_state_t *send_state,
                                    const multi_bench_settings_t &settings,
                                    size_t msg_size,
                                    uint32_t run_id,
                                    const std::string &lib_name,
                                    const std::string &transport)
{
    const double active_s = static_cast<double> (std::max (1, settings.duration_seconds));

    long recv_count = 0;
    double lat_sum = 0.0;
    long lat_count = 0;
    bench_latency_sampler_t lat_samples;
    bool detail_emitted = false;
    const bool active_ok = run_receive_phase (
      send_state, msg_size, run_id, perf_multi_metric::phase_active, &detail_emitted, &transport,
      active_s + 10.0, active_s, true, false, false, &recv_count, NULL, NULL, NULL,
      settings.clients);
    if (!active_ok) {
        if (bench_transition_debug_enabled ()) {
            std::cerr << "[multi-dealer-dealer-server] active failed size=" << msg_size
                      << " run=" << run_id << std::endl;
        }
        return false;
    }

    if (recv_count <= 0) {
        if (bench_transition_debug_enabled ()) {
            std::cerr << "[multi-dealer-dealer-server] active empty size=" << msg_size
                      << " run=" << run_id << " recv_count=" << recv_count << std::endl;
        }
        return false;
    }

    // Receiving one ordered stop token from every client pipe proves that the
    // saturated active backlog has been consumed. The runner forwards this
    // token to the client before the one-record-at-a-time latency phase starts.
    std::cout << "PHASE_LATENCY," << msg_size << std::endl;

    const bool latency_ok = run_receive_phase (
      send_state, msg_size, run_id, perf_multi_metric::phase_latency, NULL, NULL, 11.0, 0.0,
      false, true, true, NULL, &lat_sum, &lat_count, &lat_samples, settings.clients);
    if (!latency_ok || lat_count <= 0) {
        if (bench_transition_debug_enabled ()) {
            std::cerr << "[multi-dealer-dealer-server] latency failed size=" << msg_size
                      << " run=" << run_id << " ok=" << (latency_ok ? 1 : 0)
                      << " lat_count=" << lat_count << " err=" << zlink_errno () << std::endl;
        }
        return false;
    }

    // Every client pipe has delivered its final stop token and every latency
    // echo retry has been admitted. Keep the server alive until the runner
    // observes CLIENT_DONE and replies with STOP.
    std::cout << "PHASE_DONE," << msg_size << std::endl;

    bench_latency_stats_t latency;
    normalize_latency_stats (lat_sum, lat_count, &lat_samples, &latency);

    const double throughput = static_cast<double> (recv_count)
                              / static_cast<double> (std::max (1, settings.duration_seconds));

    print_result (lib_name, k_pattern, transport, msg_size, throughput, latency.mean_ns,
                  latency.p95_ns, latency.p99_ns);

    return true;
}

struct runner_stop_wait_state_t
{
    runner_stop_wait_state_t () : done (false) {}

    std::mutex lock;
    std::condition_variable changed;
    bool done;
};

inline void wait_for_runner_stop_or_eof_bounded (server_send_state_t *send_state)
{
    const std::shared_ptr<runner_stop_wait_state_t> state (
      new runner_stop_wait_state_t ());
    std::thread watcher ([state] () {
        std::string line;
        while (std::getline (std::cin, line)) {
            if (line == "STOP" || line == "QUIT")
                break;
        }
        {
            std::lock_guard<std::mutex> guard (state->lock);
            state->done = true;
        }
        state->changed.notify_one ();
    });

    const int wait_ms = perf_stop_send_timeout_ms () * perf_stop_send_max_attempts ();
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (wait_ms);
    bool done = false;
    while (!done && std::chrono::steady_clock::now () < deadline) {
        std::unique_lock<std::mutex> guard (state->lock);
        done = state->changed.wait_for (guard, std::chrono::milliseconds (10),
                                        [state] () { return state->done; });
        guard.unlock ();

        // Keep servicing the socket while waiting for CLIENT_DONE -> STOP.
        if (!done && send_state && send_state->poller) {
            std::memset (&send_state->event, 0, sizeof (send_state->event));
            const int event_count = zlink_poller_wait (
              send_state->poller, &send_state->event, 1, 0, NULL);
            if (event_count > 0
                && (send_state->event.events
                    & (ZLINK_POLLOUT | ZLINK_POLLCOMPLETION)) != 0) {
                (void) drain_server_writable (send_state);
            }
        }
    }
    if (done)
        watcher.join ();
    else
        watcher.detach ();
}

inline int run_server_benchmark (const std::string &lib_name, const std::string &transport)
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

    ctx_guard_t ctx;
    if (!ctx.valid ())
        return 1;

    void *server = zlink_socket (ctx.get (), k_server_socket_type);
    if (!server)
        return 1;

    const multi_bench_settings_t settings = resolve_multi_bench_settings ();
    apply_benchmark_socket_options (server, settings.hwm, transport, k_server_socket_type, 0,
                                    false);

    if (!setup_tls_server (server, transport)) {
        zlink_close (server);
        return 1;
    }

    const std::string endpoint =
      bind_server_endpoint (server, transport, lib_name + std::string ("_") + k_token + "_server");
    if (endpoint.empty ()) {
        zlink_close (server);
        return 1;
    }

    server_send_state_t send_state;
    if (!init_server_send_state (server, &send_state)) {
        zlink_close (server);
        return 1;
    }

    perf_stop_requested ().store (false, std::memory_order_release);
    install_perf_signal_handlers ();

    std::vector<size_t> sizes = resolve_bench_msg_sizes (64);
    if (sizes.empty ())
        sizes.push_back (64);
    std::cout << "READY," << endpoint << std::endl;

    bool ok = true;
    for (size_t si = 0; si < sizes.size (); ++si) {
        if (perf_stop_requested ().load (std::memory_order_acquire)) {
            ok = false;
            break;
        }

        if (bench_transition_debug_enabled ()) {
            std::cerr << "[multi-dealer-dealer-server] wait start size=" << sizes[si] << std::endl;
        }
        apply_benchmark_hwm (server, settings.hwm);
        if (!perf_multi_handshake::wait_for_start_from_stdin (sizes[si])) {
            if (bench_transition_debug_enabled ()) {
                std::cerr << "[multi-dealer-dealer-server] start gate failed size=" << sizes[si]
                          << std::endl;
            }
            ok = false;
            break;
        }
        if (zlink_ctx_auto_hwm_recalculate (ctx.get ()) != ZLINK_CONFIG_OK) {
            if (bench_debug_enabled ()) {
                std::cerr << "[multi-dealer-dealer-server] ctx auto-hwm recalc failed err="
                          << zlink_errno () << std::endl;
            }
            ok = false;
            break;
        }
        if (bench_transition_debug_enabled ()) {
            std::cerr << "[multi-dealer-dealer-server] start size=" << sizes[si] << std::endl;
        }

        const uint32_t run_id = static_cast<uint32_t> (si + 1);
        if (!run_one_size_benchmark (
              &send_state, settings, sizes[si], run_id, lib_name, transport)) {
            ok = false;
            break;
        }
    }

    // PHASE_DONE lets the client publish CLIENT_DONE only after every final
    // stop has arrived. The runner then writes STOP; this wait is bounded so
    // a broken control path cannot hold server shutdown indefinitely.
    if (ok)
        wait_for_runner_stop_or_eof_bounded (&send_state);

    perf_stop_requested ().store (true, std::memory_order_release);
    close_server_send_state (&send_state);
    zlink_close (server);

    return ok ? 0 : 1;
}

} // namespace

int main (int argc, char **argv)
{
    if (argc < 3)
        return 1;
    if (!multi_perf_validate_recv_mode_for_pattern (k_pattern))
        return 1;

    const std::string lib_name = argv[1];
    const std::string transport = argv[2];
    return run_server_benchmark (lib_name, transport);
}

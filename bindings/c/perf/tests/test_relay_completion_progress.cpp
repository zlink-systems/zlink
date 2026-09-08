/* SPDX-License-Identifier: MPL-2.0 */

#include "../multi/common/perf_multi_relay_server.hpp"

#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>

namespace
{

const size_t payload_size = 64u * 1024u;
const size_t request_count = 16u;
const int event_wait_ms = 1000;

#define REQUIRE_TRUE(expression)                                                \
    do {                                                                        \
        if (!(expression)) {                                                    \
            std::cerr << __FILE__ << ':' << __LINE__ << ": " #expression      \
                      << " failed (errno=" << zlink_errno () << ")"           \
                      << std::endl;                                             \
            return false;                                                       \
        }                                                                       \
    } while (false)

bool set_hwm (void *socket, zlink_option_t option, uint64_t value)
{
    return zlink_set_option (socket, option, &value, sizeof (value))
           == ZLINK_CONFIG_OK;
}

class poller_guard_t
{
  public:
    poller_guard_t () : poller (zlink_poller_new ()) {}
    ~poller_guard_t ()
    {
        if (poller)
            zlink_poller_destroy (&poller);
    }

    void *get () const { return poller; }

  private:
    poller_guard_t (const poller_guard_t &);
    poller_guard_t &operator= (const poller_guard_t &);
    void *poller;
};

bool wait_for_event (void *poller, short expected)
{
    zlink_poller_event_t event;
    std::memset (&event, 0, sizeof (event));
    REQUIRE_TRUE (
      zlink_poller_wait (poller, &event, 1, event_wait_ms, NULL) == 1);
    REQUIRE_TRUE ((event.events & expected) != 0);
    return true;
}

bool send_request (void *client, uint64_t sequence)
{
    zlink_msg_t part;
    REQUIRE_TRUE (zlink_msg_init_size (&part, payload_size) == ZLINK_CONFIG_OK);
    std::memset (zlink_msg_data (&part), static_cast<int> (sequence),
                 payload_size);
    std::memcpy (zlink_msg_data (&part), &sequence, sizeof (sequence));

    const zlink_submit_result_t result = zlink_send_part (
      client, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, NULL, NULL);
    const int submit_errno = zlink_errno ();
    const bool consumed = zlink_msg_size (&part) == 0;
    const bool closed = zlink_msg_close (&part) == ZLINK_CONFIG_OK;
    if (result != ZLINK_SUBMIT_OK)
        errno = submit_errno;
    REQUIRE_TRUE (result == ZLINK_SUBMIT_OK);
    REQUIRE_TRUE (consumed);
    REQUIRE_TRUE (closed);
    return true;
}

bool drain_echoes (void *client, size_t *next_sequence)
{
    REQUIRE_TRUE (next_sequence != NULL);
    for (;;) {
        zlink_msg_t part;
        REQUIRE_TRUE (zlink_msg_init (&part) == ZLINK_CONFIG_OK);
        const zlink_routing_id_t *source_rid = NULL;
        zlink_part_flag_t part_flag = ZLINK_PART_MORE;
        const zlink_recv_result_t result = zlink_recv_part (
          client, &source_rid, &part, &part_flag, ZLINK_RECV_FLAGS_DONTWAIT);
        const int recv_errno = zlink_errno ();
        if (result == ZLINK_RECV_NO_DATA) {
            REQUIRE_TRUE (zlink_msg_close (&part) == ZLINK_CONFIG_OK);
            REQUIRE_TRUE (recv_errno == EAGAIN || recv_errno == EWOULDBLOCK);
            return true;
        }
        REQUIRE_TRUE (result == ZLINK_RECV_OK);
        REQUIRE_TRUE (part_flag == ZLINK_PART_FINAL);
        REQUIRE_TRUE (zlink_msg_size (&part) == payload_size);
        REQUIRE_TRUE (*next_sequence < request_count);

        uint64_t sequence = UINT64_MAX;
        std::memcpy (&sequence, zlink_msg_data (&part), sizeof (sequence));
        REQUIRE_TRUE (sequence == *next_sequence);
        ++*next_sequence;
        REQUIRE_TRUE (zlink_msg_close (&part) == ZLINK_CONFIG_OK);
    }
}

bool run_test ()
{
    using namespace perf_multi_relay_server;

    ctx_guard_t context;
    REQUIRE_TRUE (context.valid ());
    REQUIRE_TRUE (zlink_ctx_set (context.get (), ZLINK_CTX_OPT_AUTO_HWM_ENABLE, 0)
                  == ZLINK_CONFIG_OK);

    socket_guard_t server (context.get (), ZLINK_SOCKET_ROUTER);
    socket_guard_t client (context.get (), ZLINK_SOCKET_DEALER);
    REQUIRE_TRUE (server.valid ());
    REQUIRE_TRUE (client.valid ());

    const int zero_linger = 0;
    const int mandatory = 1;
    const uint64_t response_hwm = 1;
    const uint64_t request_hwm = 4u * 1024u * 1024u;
    REQUIRE_TRUE (zlink_set_option (server.get (), ZLINK_OPT_LINGER, &zero_linger,
                                    sizeof (zero_linger)) == ZLINK_CONFIG_OK);
    REQUIRE_TRUE (zlink_set_option (client.get (), ZLINK_OPT_LINGER, &zero_linger,
                                    sizeof (zero_linger)) == ZLINK_CONFIG_OK);
    REQUIRE_TRUE (zlink_set_router_option (server.get (), ZLINK_ROUTER_OPT_MANDATORY,
                                           &mandatory, sizeof (mandatory))
                  == ZLINK_CONFIG_OK);
    REQUIRE_TRUE (set_hwm (server.get (), ZLINK_OPT_SNDHWM, response_hwm));
    REQUIRE_TRUE (set_hwm (client.get (), ZLINK_OPT_RCVHWM, response_hwm));
    REQUIRE_TRUE (set_hwm (client.get (), ZLINK_OPT_SNDHWM, request_hwm));
    REQUIRE_TRUE (set_hwm (server.get (), ZLINK_OPT_RCVHWM, request_hwm));
    REQUIRE_TRUE (zlink_set_routing_id (client.get (), "relay-client", 12)
                  == ZLINK_CONFIG_OK);

    const char *endpoint = "inproc://perf-relay-completion-progress";
    REQUIRE_TRUE (zlink_bind (server.get (), endpoint) == ZLINK_BIND_OK);
    REQUIRE_TRUE (zlink_connect (client.get (), endpoint) == ZLINK_CONNECT_OK);

    for (size_t i = 0; i < request_count; ++i)
        REQUIRE_TRUE (send_request (client.get (), static_cast<uint64_t> (i)));

    std::deque<pending_reply_t> pending;
    reply_wait_state_t wait_state;
    wait_state.socket = server.get ();
    poller_guard_t completion_poller;
    REQUIRE_TRUE (completion_poller.get () != NULL);
    REQUIRE_TRUE (zlink_poller_add (completion_poller.get (), server.get (),
                                    &wait_state, ZLINK_POLLIN)
                  == ZLINK_CONFIG_OK);
    REQUIRE_TRUE (wait_for_event (completion_poller.get (), ZLINK_POLLIN));

    bool recv_drained = false;
    perf_stop_requested ().store (false, std::memory_order_release);
    REQUIRE_TRUE (drain_recv_and_relay (server.get (), &pending, &wait_state,
                                        &recv_drained));

    // Once the first refused echo owns a wait token, receive must yield to the
    // completion owner instead of moving the rest of the 64 KiB input queue
    // into this application-owned deque.
    REQUIRE_TRUE (wait_state.wait_token != 0);
    REQUIRE_TRUE (!recv_drained);
    REQUIRE_TRUE (pending.size () == 1);

    poller_guard_t echo_poller;
    REQUIRE_TRUE (echo_poller.get () != NULL);
    REQUIRE_TRUE (zlink_poller_modify (completion_poller.get (), server.get (),
                                       ZLINK_POLLCOMPLETION)
                  == ZLINK_CONFIG_OK);
    REQUIRE_TRUE (zlink_poller_add (echo_poller.get (), client.get (), client.get (),
                                    ZLINK_POLLIN) == ZLINK_CONFIG_OK);

    size_t next_echo = 0;
    bool all_input_received = false;
    for (size_t turn = 0; turn < request_count + 1 && next_echo < request_count;
         ++turn) {
        REQUIRE_TRUE (wait_for_event (echo_poller.get (), ZLINK_POLLIN));
        REQUIRE_TRUE (drain_echoes (client.get (), &next_echo));

        if (wait_state.wait_token != 0) {
            REQUIRE_TRUE (wait_for_event (completion_poller.get (),
                                          ZLINK_POLLCOMPLETION));
            REQUIRE_TRUE (drain_reply_writable (server.get (), &wait_state, true));
            REQUIRE_TRUE (wait_state.wait_token == 0);
            REQUIRE_TRUE (flush_pending_replies (server.get (), &pending,
                                                 &wait_state));
            REQUIRE_TRUE (pending.empty ());
        }

        if (!all_input_received) {
            recv_drained = false;
            REQUIRE_TRUE (drain_recv_and_relay (server.get (), &pending,
                                                &wait_state, &recv_drained));
            all_input_received = recv_drained;
            if (!recv_drained) {
                REQUIRE_TRUE (wait_state.wait_token != 0);
                REQUIRE_TRUE (pending.size () == 1);
            }
        }
    }

    REQUIRE_TRUE (drain_echoes (client.get (), &next_echo));
    REQUIRE_TRUE (next_echo == request_count);
    REQUIRE_TRUE (all_input_received);
    REQUIRE_TRUE (pending.empty ());
    REQUIRE_TRUE (wait_state.wait_token == 0);
    return true;
}

} // namespace

int main ()
{
    return run_test () ? 0 : 1;
}

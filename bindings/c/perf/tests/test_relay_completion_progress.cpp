/* SPDX-License-Identifier: MPL-2.0 */

#include "../multi/common/perf_multi_relay_server.hpp"

#include <array>
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

    const zlink_submit_result_t result = zlink_send (
      client, &part, 1, ZLINK_SEND_FLAGS_NONE, NULL, NULL);
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

bool drain_echoes (void *client, std::array<bool, request_count> *seen, size_t *echo_count)
{
    REQUIRE_TRUE (seen != NULL && echo_count != NULL);
    for (;;) {
        zlink_msg_t part;
        const zlink_routing_id_t *source_rid = NULL;
        size_t part_count = 0;
        const zlink_recv_result_t result = zlink_recv (
          client, &source_rid, &part, 1, &part_count, ZLINK_RECV_FLAGS_DONTWAIT);
        const int recv_errno = zlink_errno ();
        if (result == ZLINK_RECV_NO_DATA) {
            REQUIRE_TRUE (recv_errno == EAGAIN || recv_errno == EWOULDBLOCK);
            return true;
        }
        REQUIRE_TRUE (result == ZLINK_RECV_OK);
        REQUIRE_TRUE (part_count == 1);
        REQUIRE_TRUE (zlink_msg_size (&part) == payload_size);
        uint64_t sequence = UINT64_MAX;
        std::memcpy (&sequence, zlink_msg_data (&part), sizeof (sequence));
        REQUIRE_TRUE (sequence < request_count);
        REQUIRE_TRUE (!(*seen)[static_cast<size_t> (sequence)]);
        (*seen)[static_cast<size_t> (sequence)] = true;
        ++*echo_count;
        zlink_multipart_close (&part, part_count);
    }
}

bool run_test ()
{
    using namespace perf_multi_relay_server;

    pending_replies_t duplicate_pending;
    std::unique_ptr<pending_reply_t> occupied_reservation (new pending_reply_t ());
    pending_reply_t duplicate_reply;
    errno = 0;
    REQUIRE_TRUE (!retain_or_send_reply (&duplicate_pending, &occupied_reservation,
                                         std::move (duplicate_reply), reply_send_reservation_full));
    REQUIRE_TRUE (errno == EPROTO);
    REQUIRE_TRUE (occupied_reservation.get () != NULL);
    REQUIRE_TRUE (duplicate_pending.empty ());

    ctx_guard_t context;
    REQUIRE_TRUE (context.valid ());
    REQUIRE_TRUE (zlink_ctx_set (context.get (), ZLINK_CTX_OPT_AUTO_HWM_ENABLE, 0)
                  == ZLINK_CONFIG_OK);

    socket_guard_t server (context.get (), ZLINK_SOCKET_ROUTER);
    socket_guard_t client (context.get (), ZLINK_SOCKET_DEALER);
    socket_guard_t second_client (context.get (), ZLINK_SOCKET_DEALER);
    REQUIRE_TRUE (server.valid ());
    REQUIRE_TRUE (client.valid ());
    REQUIRE_TRUE (second_client.valid ());

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
    REQUIRE_TRUE (zlink_set_routing_id (second_client.get (), "relay-second", 12)
                  == ZLINK_CONFIG_OK);

    const char *endpoint = "inproc://perf-relay-completion-progress";
    REQUIRE_TRUE (zlink_bind (server.get (), endpoint) == ZLINK_BIND_OK);
    REQUIRE_TRUE (zlink_connect (client.get (), endpoint) == ZLINK_CONNECT_OK);
    REQUIRE_TRUE (zlink_connect (second_client.get (), endpoint) == ZLINK_CONNECT_OK);

    for (size_t i = 0; i < request_count; ++i)
        REQUIRE_TRUE (send_request (client.get (), static_cast<uint64_t> (i)));
    REQUIRE_TRUE (send_request (second_client.get (), 100));

    pending_replies_t pending;
    std::unique_ptr<pending_reply_t> reservation_full;
    std::vector<pending_reply_t> ready;
    poller_guard_t completion_poller;
    REQUIRE_TRUE (completion_poller.get () != NULL);
    REQUIRE_TRUE (zlink_poller_add (completion_poller.get (), server.get (), &pending,
                                    ZLINK_POLLIN | ZLINK_POLLCOMPLETION)
                  == ZLINK_CONFIG_OK);
    REQUIRE_TRUE (wait_for_event (completion_poller.get (), ZLINK_POLLIN));

    bool recv_drained = false;
    perf_stop_requested ().store (false, std::memory_order_release);
    REQUIRE_TRUE (drain_recv_and_relay (server.get (), &pending, &reservation_full, &recv_drained));
    REQUIRE_TRUE (recv_drained);
    REQUIRE_TRUE (!reservation_full);
    bool retained_with_token = false;
    for (pending_replies_t::const_iterator it = pending.begin (); it != pending.end (); ++it)
        retained_with_token |= it->second.wait_token != 0;
    REQUIRE_TRUE (retained_with_token);
    bool received_second = false;
    for (pending_replies_t::const_iterator it = pending.begin (); it != pending.end (); ++it)
        received_second |=
          it->second.rid.size == 12 && std::memcmp (it->second.rid.data, "relay-second", 12) == 0;

    poller_guard_t echo_poller;
    REQUIRE_TRUE (echo_poller.get () != NULL);
    REQUIRE_TRUE (zlink_poller_add (echo_poller.get (), second_client.get (), second_client.get (),
                                    ZLINK_POLLIN)
                  == ZLINK_CONFIG_OK);

    zlink_poller_event_t second_event;
    std::memset (&second_event, 0, sizeof (second_event));
    REQUIRE_TRUE (zlink_poller_wait (echo_poller.get (), &second_event, 1, event_wait_ms, NULL)
                  == 1);
    REQUIRE_TRUE (second_event.socket == second_client.get ());
    zlink_msg_t second_echo;
    const zlink_routing_id_t *source_rid = NULL;
    size_t second_part_count = 0;
    REQUIRE_TRUE (zlink_recv (second_client.get (), &source_rid, &second_echo, 1,
                              &second_part_count, ZLINK_RECV_FLAGS_DONTWAIT)
                  == ZLINK_RECV_OK);
    uint64_t second_sequence = 0;
    std::memcpy (&second_sequence, zlink_msg_data (&second_echo), sizeof (second_sequence));
    REQUIRE_TRUE (second_sequence == 100);
    zlink_multipart_close (&second_echo, second_part_count);
    REQUIRE_TRUE (!received_second);

    std::array<bool, request_count> seen = {};
    size_t echo_count = 0;
    for (size_t turn = 0; turn < request_count * 4 && echo_count < request_count; ++turn) {
        REQUIRE_TRUE (drain_echoes (client.get (), &seen, &echo_count));
        if (echo_count == request_count)
            break;
        REQUIRE_TRUE (wait_for_event (completion_poller.get (), ZLINK_POLLCOMPLETION));
        REQUIRE_TRUE (drain_reply_writable (server.get (), &pending, &ready));
        REQUIRE_TRUE (!ready.empty ());
        REQUIRE_TRUE (flush_pending_replies (server.get (), &pending, &reservation_full, &ready));
    }

    REQUIRE_TRUE (drain_echoes (client.get (), &seen, &echo_count));
    REQUIRE_TRUE (echo_count == request_count);
    REQUIRE_TRUE (pending.empty ());
    return true;
}

} // namespace

int main ()
{
    return run_test () ? 0 : 1;
}

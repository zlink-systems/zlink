/* SPDX-License-Identifier: MPL-2.0 */

#include "../multi/common/perf_multi_socket_reqrep.hpp"

#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>

namespace
{

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::cerr << __FILE__ << ':' << __LINE__ << ": " #expression                           \
                      << " failed (errno=" << zlink_errno () << ")" << std::endl;                  \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

bool set_hwm (void *socket, zlink_option_t option, uint64_t value)
{
    return zlink_set_option (socket, option, &value, sizeof (value)) == ZLINK_CONFIG_OK;
}

bool send_request (void *client, uint64_t sequence)
{
    zlink_msg_t payload;
    CHECK (zlink_msg_init_size (&payload, 4096) == ZLINK_CONFIG_OK);
    std::memcpy (zlink_msg_data (&payload), &sequence, sizeof (sequence));
    zlink_completion_id_t completion_id = 0;
    const zlink_submit_result_t result = perf_zlink_dealer_request_measurement_part (
      client, &payload, ZLINK_SEND_FLAGS_NONE, 10000, client, &completion_id);
    zlink_msg_close (&payload);
    CHECK (result == ZLINK_SUBMIT_OK);
    CHECK (completion_id != 0);
    return true;
}

bool wait_for_request (void *server)
{
    zlink_pollitem_t item = {server, 0, ZLINK_POLLIN, 0};
    CHECK (perf_socket_poll (&item, 1, 1000) == 1);
    CHECK ((item.revents & ZLINK_POLLIN) != 0);
    return true;
}

bool run_test ()
{
    using namespace perf_multi_socket_reqrep;
    CHECK (classify_server_reply_submit (ZLINK_SUBMIT_OK, 0, false)
           == server_reply_submit_admitted);
    CHECK (classify_server_reply_submit (ZLINK_SUBMIT_BACKPRESSURED, EAGAIN, false)
           == server_reply_submit_wait);
    CHECK (classify_server_reply_submit (ZLINK_SUBMIT_INTERNAL_ERROR, ENOTCONN, false)
           == server_reply_submit_error);
    CHECK (classify_server_reply_submit (ZLINK_SUBMIT_TERMINATED, 0, false)
           == server_reply_submit_error);
    CHECK (classify_server_reply_submit (ZLINK_SUBMIT_INTERNAL_ERROR, ENOTCONN, true)
           == server_reply_submit_teardown);
    CHECK (classify_server_reply_submit (ZLINK_SUBMIT_TERMINATED, 0, true)
           == server_reply_submit_teardown);
    ctx_guard_t context;
    CHECK (context.valid ());
    CHECK (zlink_ctx_set (context.get (), ZLINK_CTX_OPT_AUTO_HWM_ENABLE, 0) == ZLINK_CONFIG_OK);
    socket_guard_t server (context.get (), ZLINK_SOCKET_ROUTER);
    socket_guard_t first (context.get (), ZLINK_SOCKET_DEALER);
    socket_guard_t second (context.get (), ZLINK_SOCKET_DEALER);
    CHECK (server.valid () && first.valid () && second.valid ());
    const int zero_linger = 0;
    const int send_timeout_ms = 200;
    CHECK (zlink_set_option (server.get (), ZLINK_OPT_LINGER, &zero_linger, sizeof (zero_linger))
           == ZLINK_CONFIG_OK);
    CHECK (zlink_set_option (server.get (), ZLINK_OPT_SNDTIMEO, &send_timeout_ms,
                             sizeof (send_timeout_ms))
           == ZLINK_CONFIG_OK);
    CHECK (zlink_set_option (first.get (), ZLINK_OPT_LINGER, &zero_linger, sizeof (zero_linger))
           == ZLINK_CONFIG_OK);
    CHECK (zlink_set_option (second.get (), ZLINK_OPT_LINGER, &zero_linger, sizeof (zero_linger))
           == ZLINK_CONFIG_OK);
    CHECK (set_hwm (server.get (), ZLINK_OPT_SNDHWM, 65536));
    CHECK (set_hwm (server.get (), ZLINK_OPT_RCVHWM, 4u * 1024u * 1024u));
    CHECK (set_hwm (first.get (), ZLINK_OPT_RCVHWM, 1));
    CHECK (set_hwm (first.get (), ZLINK_OPT_SNDHWM, 4u * 1024u * 1024u));
    CHECK (set_hwm (second.get (), ZLINK_OPT_RCVHWM, 4u * 1024u * 1024u));
    CHECK (set_hwm (second.get (), ZLINK_OPT_SNDHWM, 4u * 1024u * 1024u));
    CHECK (zlink_set_routing_id (first.get (), "reqrep-first", 12) == ZLINK_CONFIG_OK);
    CHECK (zlink_set_routing_id (second.get (), "reqrep-next", 11) == ZLINK_CONFIG_OK);
    const char *endpoint = "inproc://perf-reqrep-reply-progress";
    CHECK (zlink_bind (server.get (), endpoint) == ZLINK_BIND_OK);
    CHECK (zlink_connect (first.get (), endpoint) == ZLINK_CONNECT_OK);
    CHECK (zlink_connect (second.get (), endpoint) == ZLINK_CONNECT_OK);

    size_t active_msg_size = 0;
    std::deque<pending_server_reply_t> pending;
    pending_reply_rid_counts_t pending_by_rid;
    CHECK (send_request (second.get (), 0));
    CHECK (wait_for_request (server.get ()));
    CHECK (reply_one_request (server.get (), context.get (), 65536, "tcp", ZLINK_SOCKET_ROUTER,
                              &active_msg_size, &pending, &pending_by_rid)
           == server_recv_step_replied);
    CHECK (pending.empty ());
    CHECK (pending_by_rid.empty ());

    CHECK (send_request (first.get (), 1));
    CHECK (wait_for_request (server.get ()));
    CHECK (reply_one_request (server.get (), context.get (), 65536, "tcp", ZLINK_SOCKET_ROUTER,
                              &active_msg_size, &pending, &pending_by_rid)
           == server_recv_step_replied);
    CHECK (pending.size () == 1);
    CHECK (pending.front ().token != 0);
    CHECK (pending.front ().parts.size () == 2);
    CHECK (zlink_msg_size (&pending.front ().parts[0]) == 4096);
    CHECK (pending_by_rid.size () == 1);

    CHECK (send_request (second.get (), 2));
    CHECK (wait_for_request (server.get ()));
    CHECK (reply_one_request (server.get (), context.get (), 65536, "tcp", ZLINK_SOCKET_ROUTER,
                              &active_msg_size, &pending, &pending_by_rid)
           == server_recv_step_replied);
    CHECK (pending.size () == 1);
    CHECK (pending.front ().token != 0);
    CHECK (zlink_msg_size (&pending.front ().parts[0]) == 4096);
    CHECK (pending_by_rid.size () == 1);
    return true;
}

} // namespace

int main ()
{
    return run_test () ? 0 : 1;
}

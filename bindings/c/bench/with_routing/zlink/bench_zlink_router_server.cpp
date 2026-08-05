#include "../common/bench_router_compare_common.hpp"

#include <zlink.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{

using namespace bench_rc;

static std::atomic<bool> g_stop (false);

void on_signal (int)
{
    g_stop.store (true, std::memory_order_release);
}

void apply_socket_options (void *socket)
{
    const int linger = 0;
    const int rcvtimeo = 100;
    const int sndtimeo = 100;
    const uint64_t hwm =
      static_cast<uint64_t> (parse_long_env ("BENCH_HWM", 1000, 1));
    (void) zlink_set_option (socket, ZLINK_OPT_LINGER, &linger, sizeof (linger));
    (void) zlink_set_option (socket, ZLINK_OPT_RCVTIMEO, &rcvtimeo, sizeof (rcvtimeo));
    (void) zlink_set_option (socket, ZLINK_OPT_SNDTIMEO, &sndtimeo, sizeof (sndtimeo));
    if (zlink_set_option (socket, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)) != 0
        || zlink_set_option (socket, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)) != 0)
        std::abort ();
    const int nodelay = 1;
    (void) zlink_set_option (socket, ZLINK_OPT_TCP_NODELAY, &nodelay, sizeof (nodelay));
    const int backlog = 512;
    (void) zlink_set_option (socket, ZLINK_OPT_BACKLOG, &backlog, sizeof (backlog));
}

bool handle_router_once (
  void *server, char *id_buf, size_t id_cap, char *payload_buf, size_t payload_cap)
{
    const zlink_routing_id_t *source_rid = NULL;
    zlink_msg_t part;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    if (zlink_msg_init (&part) != 0)
        return false;
    const zlink_recv_result_t rc =
      ::zlink_recv_part (server, &source_rid, &part, &has_more, ZLINK_RECV_FLAGS_DONTWAIT);
    if (rc != ZLINK_RECV_OK) {
        zlink_msg_close (&part);
        return false;
    }

    if (!source_rid || source_rid->size == 0) {
        zlink_msg_close (&part);
        return false;
    }

    const size_t id_len = std::min (id_cap, static_cast<size_t> (source_rid->size));
    std::memcpy (id_buf, source_rid->data, id_len);

    const size_t payload_size = zlink_msg_size (&part);
    const size_t payload_len = std::min (payload_cap, payload_size);
    if (payload_len > 0) {
        std::memcpy (payload_buf, zlink_msg_data (&part), payload_len);
    }

    while (has_more == ZLINK_PART_MORE) {
        zlink_msg_t next;
        if (zlink_msg_init (&next) != 0) {
            zlink_msg_close (&part);
            return false;
        }
        const zlink_recv_result_t next_rc =
          ::zlink_recv_part (server, &source_rid, &next, &has_more, ZLINK_RECV_FLAGS_DONTWAIT);
        zlink_msg_close (&next);
        if (next_rc != ZLINK_RECV_OK) {
            zlink_msg_close (&part);
            return false;
        }
    }
    zlink_msg_close (&part);

    zlink_routing_id_t target_rid;
    target_rid.size = static_cast<uint8_t> (id_len);
    if (id_len > 0)
        std::memcpy (target_rid.data, id_buf, id_len);

    zlink_msg_t reply_part;
    if (zlink_msg_init_size (&reply_part, payload_len) != 0)
        return false;
    if (payload_len > 0)
        std::memcpy (zlink_msg_data (&reply_part), payload_buf, payload_len);
    if (::zlink_send_part_rid (server, &target_rid, &reply_part, ZLINK_SEND_FLAGS_NONE,
                               ZLINK_PART_FINAL)
        != ZLINK_SUBMIT_OK) {
        zlink_msg_close (&reply_part);
        return false;
    }
    return true;
}

static const long k_poll_timeout_ms = 1000;

int run_echo_server (void *server)
{
    zlink_pollitem_t item[] = {{server, 0, ZLINK_POLLIN, 0}};

    std::vector<char> id_buf (512);
    std::vector<char> payload_buf (1024 * 1024);

    while (!g_stop.load (std::memory_order_acquire)) {
        const int prc = zlink_poll (item, 1, k_poll_timeout_ms, NULL);
        if (prc < 0) {
            if (zlink_errno () == EINTR)
                continue;
            break;
        }
        if (prc == 0 || (item[0].revents & ZLINK_POLLIN) == 0)
            continue;

        for (;;) {
            if (!handle_router_once (server, id_buf.data (), id_buf.size (), payload_buf.data (),
                                     payload_buf.size ()))
                break;
        }
    }

    return 0;
}

} // namespace

int main (int /*argc*/, char ** /*argv*/)
{
    std::signal (SIGINT, on_signal);
    std::signal (SIGTERM, on_signal);

    const int port = static_cast<int> (parse_long_env ("BENCH_PORT", 29200, 1));

    void *ctx = zlink_ctx_new ();
    if (!ctx)
        return 2;

    const int io_threads = static_cast<int> (parse_long_env ("BENCH_IO_THREADS", 4, 1));
    (void) zlink_ctx_set (ctx, ZLINK_IO_THREADS, io_threads);

    void *server = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    if (!server) {
        zlink_ctx_term (ctx);
        return 2;
    }

    apply_socket_options (server);

    const char *server_id = "RC_SRV";
    (void) zlink_set_routing_id (server, server_id, std::strlen (server_id));

    const std::string endpoint = endpoint_from_port (port);
    if (zlink_bind (server, endpoint.c_str ()) != ZLINK_BIND_OK) {
        zlink_close (server);
        zlink_ctx_term (ctx);
        return 2;
    }

    (void) run_echo_server (server);

    zlink_close (server);
    zlink_ctx_term (ctx);
    return 0;
}

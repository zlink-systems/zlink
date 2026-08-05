#include "../common/bench_router_compare_common.hpp"

#include <zmq.h>

#include <atomic>
#include <csignal>
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
    const int hwm = static_cast<int> (parse_long_env ("BENCH_HWM", 1000, 1));
    (void) zmq_setsockopt (socket, ZMQ_LINGER, &linger, sizeof (linger));
    (void) zmq_setsockopt (socket, ZMQ_RCVTIMEO, &rcvtimeo, sizeof (rcvtimeo));
    (void) zmq_setsockopt (socket, ZMQ_SNDTIMEO, &sndtimeo, sizeof (sndtimeo));
    (void) zmq_setsockopt (socket, ZMQ_RCVHWM, &hwm, sizeof (hwm));
    (void) zmq_setsockopt (socket, ZMQ_SNDHWM, &hwm, sizeof (hwm));
#ifdef ZMQ_TCP_NODELAY
    const int nodelay = 1;
    (void) zmq_setsockopt (socket, ZMQ_TCP_NODELAY, &nodelay, sizeof (nodelay));
#endif
    const int backlog = 512;
    (void) zmq_setsockopt (socket, ZMQ_BACKLOG, &backlog, sizeof (backlog));
}

bool handle_router_once (
  void *server, char *id_buf, size_t id_cap, char *payload_buf, size_t payload_cap)
{
    const int id_len = zmq_recv (server, id_buf, id_cap, ZMQ_DONTWAIT);
    if (id_len < 0)
        return false;
    const int rc = zmq_recv (server, payload_buf, payload_cap, 0);
    if (rc < 0)
        return false;

    if (zmq_send (server, id_buf, id_len, ZMQ_SNDMORE) < 0)
        return false;
    return zmq_send (server, payload_buf, rc, 0) >= 0;
}

static const long k_poll_timeout_ms = 1000;

int run_echo_server (void *server)
{
    zmq_pollitem_t item[] = {{server, 0, ZMQ_POLLIN, 0}};

    std::vector<char> id_buf (512);
    std::vector<char> payload_buf (1024 * 1024);

    while (!g_stop.load (std::memory_order_acquire)) {
        const int prc = zmq_poll (item, 1, k_poll_timeout_ms);
        if (prc < 0) {
            if (zmq_errno () == EINTR)
                continue;
            break;
        }
        if (prc == 0 || (item[0].revents & ZMQ_POLLIN) == 0)
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

    void *ctx = zmq_ctx_new ();
    if (!ctx)
        return 2;

    const int io_threads = static_cast<int> (parse_long_env ("BENCH_IO_THREADS", 4, 1));
    (void) zmq_ctx_set (ctx, ZMQ_IO_THREADS, io_threads);

    void *server = zmq_socket (ctx, ZMQ_ROUTER);
    if (!server) {
        zmq_ctx_term (ctx);
        return 2;
    }

    apply_socket_options (server);

    const char *server_id = "RC_SRV";
    (void) zmq_setsockopt (server, ZMQ_ROUTING_ID, server_id, std::strlen (server_id));

    const std::string endpoint = endpoint_from_port (port);
    if (zmq_bind (server, endpoint.c_str ()) != 0) {
        zmq_close (server);
        zmq_ctx_term (ctx);
        return 2;
    }

    (void) run_echo_server (server);

    zmq_close (server);
    zmq_ctx_term (ctx);
    return 0;
}

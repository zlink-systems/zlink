#include "../common/bench_common.hpp"

#include <zmq.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>

namespace
{
std::atomic<bool> g_stop {false};

void on_signal (int) { g_stop.store (true); }

bool recv_msg (void *socket, zmq_msg_t *msg)
{
    if (zmq_msg_init (msg) != 0)
        return false;
    if (zmq_msg_recv (msg, socket, 0) < 0) {
        zmq_msg_close (msg);
        return false;
    }
    return true;
}
}

int main ()
{
    std::signal (SIGINT, on_signal);
    std::signal (SIGTERM, on_signal);

    const std::string endpoint =
      zlink_c_bench::env_string ("ZMQ_SEND_ENDPOINT", "tcp://127.0.0.1:6079");

    void *ctx = zmq_ctx_new ();
    void *router = ctx ? zmq_socket (ctx, ZMQ_ROUTER) : nullptr;
    if (!ctx || !router) {
        std::fprintf (stderr, "zmq server: failed to create context/socket\n");
        return 2;
    }

    const int linger = 0;
    (void) zmq_setsockopt (router, ZMQ_LINGER, &linger, sizeof (linger));
    if (zmq_bind (router, endpoint.c_str ()) != 0) {
        std::fprintf (stderr, "zmq server: bind failed errno=%d\n", zmq_errno ());
        return 2;
    }

    while (!g_stop.load ()) {
        zmq_msg_t rid;
        zmq_msg_t payload;
        if (!recv_msg (router, &rid))
            continue;
        if (!zmq_msg_more (&rid) || !recv_msg (router, &payload)) {
            zmq_msg_close (&rid);
            continue;
        }

        if (zmq_msg_send (&rid, router, ZMQ_SNDMORE) < 0) {
            zmq_msg_close (&rid);
            zmq_msg_close (&payload);
            continue;
        }
        if (zmq_msg_send (&payload, router, 0) < 0)
            zmq_msg_close (&payload);
    }

    zmq_close (router);
    zmq_ctx_term (ctx);
    return 0;
}

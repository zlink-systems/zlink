#include "../common/bench_common.hpp"

#include "raw_wire.hpp"
#include "send_metrics.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <thread>

namespace
{
std::atomic<bool> g_stop{false};
constexpr const char *k_response_envelope =
  "{\"kind\":2,\"channelName\":\"bench\",\"messageName\":\"BenchPayload\",\"contentType\":"
  "\"application/"
  "x-protobuf\",\"correlationId\":null,\"deadline\":null,\"topic\":null,\"errorCode\":null,"
  "\"errorMessage\":null,\"source\":null}";

void on_signal (int)
{
    g_stop.store (true);
}

bool make_response_header (zlink_msg_t *msg)
{
    const size_t size = std::strlen (k_response_envelope);
    if (zlink_msg_init_size (msg, size) != ZLINK_CONFIG_OK)
        return false;
    std::memcpy (zlink_msg_data (msg), k_response_envelope, size);
    return true;
}

bool make_response_body (const zlink_msg_t *request_body, zlink_msg_t *reply_body)
{
    zlink::framework::bench::withgrpc::BenchPayload request;
    if (!request.ParseFromArray (zlink_msg_data (const_cast<zlink_msg_t *> (request_body)),
                                 static_cast<int> (zlink_msg_size (request_body))))
        return false;
    zlink::framework::bench::withgrpc::BenchPayload reply;
    reply.set_body (request.body ());
    return serialize_bench_payload (reply, reply_body);
}

bool recv_multipart_body (void *router,
                          zlink_routing_id_t *rid_out,
                          zlink_reply_token_t *reply_token_out,
                          zlink_msg_t *body_out,
                          zlink_recv_flags_t flags = ZLINK_RECV_FLAGS_NONE)
{
    const zlink_routing_id_t *rid = nullptr;
    zlink_reply_token_t reply_token = 0;
    zlink_msg_t parts[2];
    size_t part_count = 0;
    const zlink_recv_result_t rc =
      zlink_router_recv (router, &rid, &reply_token, parts, 2, &part_count, flags);
    if (rc != ZLINK_RECV_OK)
        return false;
    if (!rid || !rid_out || !reply_token_out || part_count != 2) {
        zlink_multipart_close (parts, part_count);
        return false;
    }

    *rid_out = *rid;
    *reply_token_out = reply_token;
    zlink_msg_move (body_out, &parts[1]);
    zlink_multipart_close (parts, part_count);
    return true;
}

bool reply_multipart (void *router,
                      const zlink_routing_id_t *rid,
                      zlink_reply_token_t reply_token,
                      const zlink_msg_t *request_body)
{
    zlink_msg_t parts[2];
    if (!make_response_header (&parts[0]))
        return false;
    if (!make_response_body (request_body, &parts[1])) {
        zlink_msg_close (&parts[0]);
        return false;
    }

    return zlink_reply (router, rid, reply_token, parts, 2) == ZLINK_SUBMIT_OK;
}

void request_loop (void *router)
{
    while (!g_stop.load ()) {
        zlink_routing_id_t rid{};
        zlink_reply_token_t reply_token = 0;
        zlink_msg_t body;
        if (zlink_msg_init (&body) != 0)
            continue;
        if (!recv_multipart_body (router, &rid, &reply_token, &body)) {
            zlink_msg_close (&body);
            continue;
        }
        if (reply_token != 0)
            (void) reply_multipart (router, &rid, reply_token, &body);
        zlink_msg_close (&body);
    }
}

void send_loop (void *router)
{
    zlink_c_bench::send_metrics_t metrics;
    void *poller = zlink_poller_new ();
    if (!poller || zlink_poller_add (poller, router, nullptr, ZLINK_POLLIN) != ZLINK_CONFIG_OK)
        throw std::runtime_error ("send stats poller setup failed");
    while (!g_stop.load ()) {
        const uint64_t now = zlink_c_bench::now_ns ();
        metrics.close (now);
        long wait_ms = 100;
        if (metrics.deadline () > now)
            wait_ms = std::min<long> (
              wait_ms, static_cast<long> ((metrics.deadline () - now + 999999) / 1000000));
        zlink_poller_event_t event{};
        zlink_config_result_t error = ZLINK_CONFIG_OK;
        const int ready = zlink_poller_wait (poller, &event, 1, wait_ms, &error);
        if (ready < 0 && zlink_errno () != EINTR)
            throw std::runtime_error ("send stats poll failed");
        metrics.close (zlink_c_bench::now_ns ());
        if (ready <= 0)
            continue;
        zlink_routing_id_t rid{};
        zlink_reply_token_t reply_token = 0;
        zlink_msg_t body;
        if (zlink_msg_init (&body) != 0)
            continue;
        if (recv_multipart_body (router, &rid, &reply_token, &body, ZLINK_RECV_FLAGS_DONTWAIT)) {
            zlink::framework::bench::withgrpc::BenchPayload payload;
            if (!payload.ParseFromArray (zlink_msg_data (&body),
                                         static_cast<int> (zlink_msg_size (&body)))) {
                metrics.record (nullptr, 0, zlink_c_bench::now_ns ());
            } else if (reply_token != 0) {
                const std::string &control = payload.body ();
                if (control.rfind (zlink_c_bench::k_send_stats_start, 0) == 0) {
                    metrics.start (std::stoull (
                      control.substr (std::strlen (zlink_c_bench::k_send_stats_start))));
                } else if (control == zlink_c_bench::k_send_stats_snapshot) {
                    const auto snapshot = metrics.snapshot (zlink_c_bench::now_ns ());
                    std::ostringstream text;
                    text.precision (17);
                    text << snapshot.completed << ' ' << snapshot.server_received.value () << ' '
                         << snapshot.server_errors << ' ' << snapshot.mean_us << ' '
                         << snapshot.p95_us << ' ' << snapshot.p99_us;
                    payload.set_body (text.str ());
                } else {
                    throw std::runtime_error ("unknown send stats control request");
                }
                zlink_msg_t response;
                if (!serialize_bench_payload (payload, &response)
                    || !reply_multipart (router, &rid, reply_token, &response))
                    throw std::runtime_error ("send stats reply failed");
                zlink_msg_close (&response);
            } else {
                metrics.record (payload.body ().data (), payload.body ().size (),
                                zlink_c_bench::now_ns ());
            }
        }
        zlink_msg_close (&body);
    }
    zlink_poller_destroy (&poller);
}

}

int main ()
{
    std::signal (SIGINT, on_signal);
    std::signal (SIGTERM, on_signal);
    const std::string request_endpoint =
      zlink_c_bench::env_string ("ZLINK_REQUEST_ENDPOINT", zlink_c_bench::k_request_endpoint);
    const std::string send_endpoint =
      zlink_c_bench::env_string ("ZLINK_SEND_ENDPOINT", zlink_c_bench::k_send_endpoint);

    void *ctx = zlink_ctx_new ();
    void *request_router = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    void *send_router = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    if (!ctx || !request_router || !send_router) {
        std::fprintf (stderr, "zlink server: failed to create context/socket\n");
        return 2;
    }
    // The raw C row is measured as ROUTER<->ROUTER (README §3). The client ROUTER addresses
    // these sockets by routing id, so both announce a well-known routing id before bind.
    const std::string request_routing_id =
      zlink_c_bench::env_string ("ZLINK_REQUEST_ROUTING_ID", "zlink-c-bench-request-server");
    const std::string send_routing_id =
      zlink_c_bench::env_string ("ZLINK_SEND_ROUTING_ID", "zlink-c-bench-send-server");
    if (zlink_set_routing_id (request_router, request_routing_id.data (),
                              request_routing_id.size ())
          != ZLINK_CONFIG_OK
        || zlink_set_routing_id (send_router, send_routing_id.data (), send_routing_id.size ())
             != ZLINK_CONFIG_OK) {
        std::fprintf (stderr, "zlink server: set_routing_id failed errno=%d\n", zlink_errno ());
        return 2;
    }
    if (zlink_bind (request_router, request_endpoint.c_str ()) != ZLINK_BIND_OK
        || zlink_bind (send_router, send_endpoint.c_str ()) != ZLINK_BIND_OK) {
        std::fprintf (stderr, "zlink server: bind failed errno=%d\n", zlink_errno ());
        return 2;
    }

    std::thread request_thread (request_loop, request_router);
    std::thread send_thread (send_loop, send_router);
    while (!g_stop.load ())
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    request_thread.join ();
    send_thread.join ();
    zlink_close (request_router);
    zlink_close (send_router);
    zlink_ctx_term (ctx);
    return 0;
}

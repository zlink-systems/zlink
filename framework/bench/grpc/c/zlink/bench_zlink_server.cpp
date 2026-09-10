#include "../common/bench_common.hpp"

#include "raw_wire.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <thread>

namespace
{
std::atomic<bool> g_stop {false};
constexpr const char *k_response_envelope =
  "{\"kind\":2,\"channelName\":\"bench\",\"messageName\":\"BenchPayload\",\"contentType\":\"application/x-protobuf\",\"correlationId\":null,\"deadline\":null,\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null}";

void on_signal (int) { g_stop.store (true); }

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
                          zlink_msg_t *body_out)
{
    const zlink_routing_id_t *rid = nullptr;
    zlink_reply_token_t reply_token = 0;
    zlink_part_flag_t more = ZLINK_PART_FINAL;
    zlink_msg_t header;
    if (zlink_msg_init (&header) != ZLINK_CONFIG_OK)
        return false;

    const int header_rc =
      zlink_router_recv_part (router, &rid, &reply_token, &header, &more,
                              ZLINK_RECV_FLAGS_NONE);
    if (header_rc != ZLINK_RECV_OK) {
        zlink_msg_close (&header);
        return false;
    }
    if (!rid || !rid_out || !reply_token_out) {
        zlink_msg_close (&header);
        return false;
    }
    *rid_out = *rid;
    *reply_token_out = reply_token;

    if (more == ZLINK_PART_FINAL) {
        zlink_msg_move (body_out, &header);
        zlink_msg_close (&header);
        return true;
    }

    zlink_msg_close (&header);
    const zlink_routing_id_t *body_rid = nullptr;
    zlink_reply_token_t body_reply_token = 0;
    const int body_rc =
      zlink_router_recv_part (router, &body_rid, &body_reply_token, body_out, &more,
                              ZLINK_RECV_FLAGS_NONE);
    return body_rc == ZLINK_RECV_OK && more == ZLINK_PART_FINAL && body_rid
           && body_reply_token == reply_token;
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

    zlink_submit_result_t rc =
      zlink_reply_part (router, rid, reply_token, &parts[0], ZLINK_PART_MORE);
    const bool header_submitted = rc == ZLINK_SUBMIT_OK;
    if (rc == ZLINK_SUBMIT_OK)
        rc = zlink_reply_part (router, rid, reply_token, &parts[1], ZLINK_PART_FINAL);
    if (rc != ZLINK_SUBMIT_OK) {
        zlink_msg_close (&parts[1]);
        if (!header_submitted)
            zlink_msg_close (&parts[0]);
    }
    return rc == ZLINK_SUBMIT_OK;
}

bool send_multipart (void *router, const zlink_routing_id_t *rid, const zlink_msg_t *request_body)
{
    zlink_msg_t parts[2];
    if (!make_response_header (&parts[0]))
        return false;
    if (!make_response_body (request_body, &parts[1])) {
        zlink_msg_close (&parts[0]);
        return false;
    }

    zlink_submit_result_t rc =
      zlink_send_part_rid (router, rid, &parts[0], ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_MORE, NULL, NULL);
    const bool header_submitted = rc == ZLINK_SUBMIT_OK;
    if (rc == ZLINK_SUBMIT_OK)
        rc = zlink_send_part_rid (router, rid, &parts[1], ZLINK_SEND_FLAGS_NONE,
                                  ZLINK_PART_FINAL, NULL, NULL);
    if (rc != ZLINK_SUBMIT_OK) {
        zlink_msg_close (&parts[1]);
        if (!header_submitted)
            zlink_msg_close (&parts[0]);
    }
    return rc == ZLINK_SUBMIT_OK;
}

void request_loop (void *router)
{
    while (!g_stop.load ()) {
        zlink_routing_id_t rid {};
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
    while (!g_stop.load ()) {
        zlink_routing_id_t rid {};
        zlink_reply_token_t reply_token = 0;
        zlink_msg_t body;
        if (zlink_msg_init (&body) != 0)
            continue;
        if (recv_multipart_body (router, &rid, &reply_token, &body)) {
            zlink::framework::bench::withgrpc::BenchPayload payload;
            (void) payload.ParseFromArray (zlink_msg_data (&body),
                                          static_cast<int> (zlink_msg_size (&body)));
        }
        zlink_msg_close (&body);
    }
}

void send_echo_loop (void *router)
{
    while (!g_stop.load ()) {
        zlink_routing_id_t rid {};
        zlink_reply_token_t reply_token = 0;
        zlink_msg_t body;
        if (zlink_msg_init (&body) != 0)
            continue;
        if (!recv_multipart_body (router, &rid, &reply_token, &body)) {
            zlink_msg_close (&body);
            continue;
        }
        if (reply_token == 0)
            (void) send_multipart (router, &rid, &body);
        zlink_msg_close (&body);
    }
}
}

int main ()
{
    std::signal (SIGINT, on_signal);
    std::signal (SIGTERM, on_signal);
    const std::string request_endpoint =
      zlink_c_bench::env_string ("ZLINK_REQUEST_ENDPOINT", "tcp://127.0.0.1:6075");
    const std::string send_endpoint =
      zlink_c_bench::env_string ("ZLINK_SEND_ENDPOINT", "tcp://127.0.0.1:6077");
    const std::string scenarios = zlink_c_bench::env_string ("ZLINK_BENCH_SCENARIOS", "all");
    const bool run_request = zlink_c_bench::scenario_enabled (scenarios, "request-serial")
                             || zlink_c_bench::scenario_enabled (scenarios, "request-window")
                             || zlink_c_bench::scenario_enabled (scenarios, "request-saturation");
    const bool run_send = zlink_c_bench::scenario_enabled (scenarios, "send-blocking")
                          || zlink_c_bench::scenario_enabled (scenarios, "send-saturation");
    const bool run_send_echo =
      scenarios != "all" && zlink_c_bench::scenario_enabled (scenarios, "send-send-serial");

    void *ctx = zlink_ctx_new ();
    void *request_router = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    void *send_router = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    if (!ctx || !request_router || !send_router) {
        std::fprintf (stderr, "zlink server: failed to create context/socket\n");
        return 2;
    }
    // FB-001: the raw C row is measured as ROUTER<->ROUTER. The client ROUTER
    // addresses these sockets by routing id, so both sockets must announce a
    // well-known routing id before bind. A DEALER client ignores these ids, so
    // setting them keeps the legacy DEALER->ROUTER configuration working too.
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

    std::thread request_thread;
    std::thread send_thread;
    if (run_request)
        request_thread = std::thread (request_loop, request_router);
    if (run_send_echo)
        send_thread = std::thread (send_echo_loop, send_router);
    else if (run_send)
        send_thread = std::thread (send_loop, send_router);
    while (!g_stop.load ())
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    if (request_thread.joinable ())
        request_thread.join ();
    if (send_thread.joinable ())
        send_thread.join ();
    zlink_close (request_router);
    zlink_close (send_router);
    zlink_ctx_term (ctx);
    return 0;
}

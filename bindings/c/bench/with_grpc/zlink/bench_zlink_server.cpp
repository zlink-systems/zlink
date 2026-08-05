#include "../common/bench_common.hpp"

#include <zlink.h>

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

size_t varint_size (size_t value)
{
    size_t n = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++n;
    }
    return n;
}

unsigned char *write_varint (unsigned char *dst, size_t value)
{
    while (value >= 0x80) {
        *dst++ = static_cast<unsigned char> ((value & 0x7fU) | 0x80U);
        value >>= 7;
    }
    *dst++ = static_cast<unsigned char> (value);
    return dst;
}

bool read_varint (const unsigned char *&p, const unsigned char *end, size_t *value)
{
    size_t result = 0;
    unsigned shift = 0;
    while (p < end && shift < sizeof (size_t) * 8) {
        const unsigned char byte = *p++;
        result |= static_cast<size_t> (byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0) {
            *value = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

bool decode_bench_payload_body (const zlink_msg_t *msg,
                                const unsigned char **body,
                                size_t *body_size)
{
    if (!msg || !body || !body_size)
        return false;
    const unsigned char *p =
      static_cast<const unsigned char *> (zlink_msg_data (const_cast<zlink_msg_t *> (msg)));
    const unsigned char *end = p + zlink_msg_size (msg);
    while (p < end) {
        size_t key = 0;
        if (!read_varint (p, end, &key))
            return false;
        const size_t field = key >> 3;
        const size_t wire_type = key & 0x07U;
        if (wire_type != 2)
            return false;
        size_t len = 0;
        if (!read_varint (p, end, &len) || static_cast<size_t> (end - p) < len)
            return false;
        if (field == 1) {
            *body = p;
            *body_size = len;
            return true;
        }
        p += len;
    }
    return false;
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
    const unsigned char *payload = nullptr;
    size_t payload_size = 0;
    if (!decode_bench_payload_body (request_body, &payload, &payload_size))
        return false;

    const size_t encoded_size = 1 + varint_size (payload_size) + payload_size;
    if (zlink_msg_init_size (reply_body, encoded_size) != ZLINK_CONFIG_OK)
        return false;
    unsigned char *encoded = static_cast<unsigned char *> (zlink_msg_data (reply_body));
    encoded[0] = 0x0a;
    unsigned char *body = write_varint (encoded + 1, payload_size);
    std::memcpy (body, payload, payload_size);
    return true;
}

bool recv_multipart_body (void *router,
                          zlink_routing_id_t *rid_out,
                          uint64_t *seq_out,
                          zlink_msg_t *body_out)
{
    const zlink_routing_id_t *rid = nullptr;
    uint64_t seq = 0;
    zlink_part_flag_t more = ZLINK_PART_FINAL;
    zlink_msg_t header;
    if (zlink_msg_init (&header) != ZLINK_CONFIG_OK)
        return false;

    const int header_rc =
      zlink_router_recv_part (router, &rid, &seq, &header, &more,
                              ZLINK_RECV_FLAGS_NONE);
    if (header_rc != ZLINK_RECV_OK) {
        zlink_msg_close (&header);
        return false;
    }
    if (!rid || !rid_out || !seq_out) {
        zlink_msg_close (&header);
        return false;
    }
    *rid_out = *rid;
    *seq_out = seq;

    if (more == ZLINK_PART_FINAL) {
        zlink_msg_move (body_out, &header);
        zlink_msg_close (&header);
        return true;
    }

    zlink_msg_close (&header);
    const zlink_routing_id_t *body_rid = nullptr;
    uint64_t body_seq = 0;
    const int body_rc =
      zlink_router_recv_part (router, &body_rid, &body_seq, body_out, &more,
                              ZLINK_RECV_FLAGS_NONE);
    return body_rc == ZLINK_RECV_OK && more == ZLINK_PART_FINAL && body_rid && body_seq == seq;
}

bool reply_multipart (void *router,
                      const zlink_routing_id_t *rid,
                      uint64_t seq,
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
      zlink_router_reply_part (router, rid, seq, &parts[0], ZLINK_PART_MORE);
    const bool header_submitted = rc == ZLINK_SUBMIT_OK;
    if (rc == ZLINK_SUBMIT_OK)
        rc = zlink_router_reply_part (router, rid, seq, &parts[1], ZLINK_PART_FINAL);
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
      zlink_send_part_rid (router, rid, &parts[0], ZLINK_SEND_FLAGS_NONE, ZLINK_PART_MORE);
    const bool header_submitted = rc == ZLINK_SUBMIT_OK;
    if (rc == ZLINK_SUBMIT_OK)
        rc = zlink_send_part_rid (router, rid, &parts[1], ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL);
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
        uint64_t seq = 0;
        zlink_msg_t body;
        if (zlink_msg_init (&body) != 0)
            continue;
        if (!recv_multipart_body (router, &rid, &seq, &body)) {
            zlink_msg_close (&body);
            continue;
        }
        if (seq != 0)
            (void) reply_multipart (router, &rid, seq, &body);
        zlink_msg_close (&body);
    }
}

void send_loop (void *router)
{
    while (!g_stop.load ()) {
        zlink_routing_id_t rid {};
        uint64_t seq = 0;
        zlink_msg_t body;
        if (zlink_msg_init (&body) != 0)
            continue;
        (void) recv_multipart_body (router, &rid, &seq, &body);
        zlink_msg_close (&body);
    }
}

void send_echo_loop (void *router)
{
    while (!g_stop.load ()) {
        zlink_routing_id_t rid {};
        uint64_t seq = 0;
        zlink_msg_t body;
        if (zlink_msg_init (&body) != 0)
            continue;
        if (!recv_multipart_body (router, &rid, &seq, &body)) {
            zlink_msg_close (&body);
            continue;
        }
        if (seq == 0)
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

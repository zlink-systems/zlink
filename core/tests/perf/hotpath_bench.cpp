/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink.h>

#if defined(__has_include)
#if __has_include(<valgrind/callgrind.h>)
#include <valgrind/callgrind.h>
#endif
#endif

#ifndef CALLGRIND_TOGGLE_COLLECT
#define CALLGRIND_TOGGLE_COLLECT ((void) 0)
#endif

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace
{
const size_t payload_size = 1024;
const size_t warmup_iterations = 100;

class collect_scope_t
{
  public:
    collect_scope_t () { CALLGRIND_TOGGLE_COLLECT; }
    ~collect_scope_t () { CALLGRIND_TOGGLE_COLLECT; }

  private:
    collect_scope_t (const collect_scope_t &);
    collect_scope_t &operator= (const collect_scope_t &);
};

class fixture_t
{
  public:
    fixture_t () : context (zlink_ctx_new ()), sender (NULL), receiver (NULL) {}

    ~fixture_t ()
    {
        if (sender)
            (void) zlink_close (sender);
        if (receiver)
            (void) zlink_close (receiver);
        if (context)
            (void) zlink_ctx_term (context);
    }

    void *context;
    void *sender;
    void *receiver;

  private:
    fixture_t (const fixture_t &);
    fixture_t &operator= (const fixture_t &);
};

bool api_error (const char *operation_, int result_)
{
    std::fprintf (stderr, "%s failed: result=%d errno=%d (%s)\n", operation_,
                  result_, zlink_errno (), zlink_strerror (zlink_errno ()));
    return false;
}

bool configure_socket (void *socket_)
{
    const int zero_linger = 0;
    const int timeout_ms = 10000;
    const uint64_t unlimited_hwm = 0;
    if (zlink_set_option (socket_, ZLINK_OPT_LINGER, &zero_linger,
                          sizeof (zero_linger)) != ZLINK_CONFIG_OK)
        return api_error ("zlink_set_option(LINGER)", -1);
    if (zlink_set_option (socket_, ZLINK_OPT_SNDTIMEO, &timeout_ms,
                          sizeof (timeout_ms)) != ZLINK_CONFIG_OK)
        return api_error ("zlink_set_option(SNDTIMEO)", -1);
    if (zlink_set_option (socket_, ZLINK_OPT_RCVTIMEO, &timeout_ms,
                          sizeof (timeout_ms)) != ZLINK_CONFIG_OK)
        return api_error ("zlink_set_option(RCVTIMEO)", -1);
    if (zlink_set_option (socket_, ZLINK_OPT_SNDHWM, &unlimited_hwm,
                          sizeof (unlimited_hwm)) != ZLINK_CONFIG_OK)
        return api_error ("zlink_set_option(SNDHWM)", -1);
    if (zlink_set_option (socket_, ZLINK_OPT_RCVHWM, &unlimited_hwm,
                          sizeof (unlimited_hwm)) != ZLINK_CONFIG_OK)
        return api_error ("zlink_set_option(RCVHWM)", -1);
    return true;
}

bool init_payload (zlink_msg_t *message_, unsigned char fill_)
{
    const zlink_config_result_t result =
      zlink_msg_init_size (message_, payload_size);
    if (result != ZLINK_CONFIG_OK)
        return api_error ("zlink_msg_init_size", result);
    std::memset (zlink_msg_data (message_), fill_, payload_size);
    return true;
}

bool init_payloads (std::vector<zlink_msg_t> *messages_, unsigned char fill_)
{
    for (size_t i = 0; i < messages_->size (); ++i) {
        if (!init_payload (&(*messages_)[i], fill_))
            return false;
    }
    return true;
}

bool init_empty_messages (std::vector<zlink_msg_t> *messages_)
{
    for (size_t i = 0; i < messages_->size (); ++i) {
        const zlink_config_result_t result = zlink_msg_init (&(*messages_)[i]);
        if (result != ZLINK_CONFIG_OK)
            return api_error ("zlink_msg_init", result);
    }
    return true;
}

bool close_messages (std::vector<zlink_msg_t> *messages_)
{
    bool ok = true;
    for (size_t i = 0; i < messages_->size (); ++i) {
        const zlink_config_result_t result = zlink_msg_close (&(*messages_)[i]);
        if (result != ZLINK_CONFIG_OK) {
            api_error ("zlink_msg_close", result);
            ok = false;
        }
    }
    return ok;
}

zlink_routing_id_t make_routing_id (const char *text_)
{
    zlink_routing_id_t rid;
    std::memset (&rid, 0, sizeof (rid));
    const size_t size = std::strlen (text_);
    rid.size = static_cast<uint8_t> (size);
    std::memcpy (rid.data, text_, size);
    return rid;
}

bool raw_send (void *socket_, zlink_msg_t *message_,
               const zlink_routing_id_t *target_)
{
    const zlink_submit_result_t result =
      target_ ? zlink_send_part_rid (socket_, target_, message_,
                                     ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL,
                                     NULL, NULL)
              : zlink_send_part (socket_, message_, ZLINK_SEND_FLAGS_NONE,
                                 ZLINK_PART_FINAL, NULL, NULL);
    return result == ZLINK_SUBMIT_OK
             ? true
             : api_error (target_ ? "zlink_send_part_rid" : "zlink_send_part",
                          result);
}

bool raw_receive (void *socket_, zlink_msg_t *message_, bool router_)
{
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = 0;
    zlink_part_flag_t part_flag = ZLINK_PART_MORE;
    const zlink_recv_result_t result =
      router_ ? zlink_router_recv_part (socket_, &source, &token, message_,
                                        &part_flag, ZLINK_RECV_FLAGS_NONE)
              : zlink_recv_part (socket_, NULL, message_, &part_flag,
                                 ZLINK_RECV_FLAGS_NONE);
    if (result != ZLINK_RECV_OK)
        return api_error (router_ ? "zlink_router_recv_part"
                                  : "zlink_recv_part",
                          result);
    if (part_flag != ZLINK_PART_FINAL) {
        std::fprintf (stderr, "receive returned a non-FINAL part\n");
        return false;
    }
    if (router_ && (!source || token != 0)) {
        std::fprintf (stderr, "ROUTER data receive returned invalid metadata\n");
        return false;
    }
    return true;
}

bool warm_up_one_way (void *sender_, void *receiver_,
                      const zlink_routing_id_t *target_, bool router_receiver_)
{
    for (size_t i = 0; i < warmup_iterations; ++i) {
        zlink_msg_t outbound;
        zlink_msg_t inbound;
        if (!init_payload (&outbound, static_cast<unsigned char> (i))
            || zlink_msg_init (&inbound) != ZLINK_CONFIG_OK)
            return false;
        if (!raw_send (sender_, &outbound, target_)
            || !raw_receive (receiver_, &inbound, router_receiver_))
            return false;
        if (zlink_msg_close (&outbound) != ZLINK_CONFIG_OK
            || zlink_msg_close (&inbound) != ZLINK_CONFIG_OK)
            return api_error ("warm-up zlink_msg_close", -1);
    }
    return true;
}

bool run_one_way (const char *cell_, size_t iterations_,
                  zlink_socket_type_t socket_type_, bool tcp_, bool routed_)
{
    fixture_t fixture;
    if (!fixture.context) {
        std::fprintf (stderr, "zlink_ctx_new failed\n");
        return false;
    }
    fixture.sender = zlink_socket (fixture.context, socket_type_);
    fixture.receiver = zlink_socket (fixture.context, socket_type_);
    if (!fixture.sender || !fixture.receiver) {
        std::fprintf (stderr, "zlink_socket failed: errno=%d\n", zlink_errno ());
        return false;
    }
    if (!configure_socket (fixture.sender)
        || !configure_socket (fixture.receiver))
        return false;

    const char *const receiver_name = "hotpath-router-receiver";
    const char *const sender_name = "hotpath-router-sender";
    zlink_routing_id_t target = make_routing_id (receiver_name);
    const zlink_routing_id_t *target_ptr = routed_ ? &target : NULL;
    if (routed_) {
        if (zlink_set_routing_id (fixture.receiver, receiver_name,
                                  std::strlen (receiver_name))
              != ZLINK_CONFIG_OK
            || zlink_set_routing_id (fixture.sender, sender_name,
                                     std::strlen (sender_name))
                 != ZLINK_CONFIG_OK
            || zlink_set_router_option (
                 fixture.sender, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                 receiver_name, std::strlen (receiver_name)) != ZLINK_CONFIG_OK)
            return api_error ("ROUTER routing-id setup", -1);
    }

    char endpoint[256];
    std::memset (endpoint, 0, sizeof (endpoint));
    if (tcp_) {
        const char wildcard[] = "tcp://127.0.0.1:*";
        if (zlink_bind (fixture.receiver, wildcard) != ZLINK_BIND_OK)
            return api_error ("zlink_bind(tcp)", -1);
        size_t endpoint_size = sizeof (endpoint);
        if (zlink_get_option (fixture.receiver, ZLINK_OPT_LAST_ENDPOINT,
                              endpoint, &endpoint_size) != ZLINK_CONFIG_OK)
            return api_error ("zlink_get_option(LAST_ENDPOINT)", -1);
    } else {
        std::snprintf (endpoint, sizeof (endpoint), "inproc://hotpath-%s",
                       cell_);
        if (zlink_bind (fixture.receiver, endpoint) != ZLINK_BIND_OK)
            return api_error ("zlink_bind(inproc)", -1);
    }
    if (zlink_connect (fixture.sender, endpoint) != ZLINK_CONNECT_OK)
        return api_error ("zlink_connect", -1);
    if (!warm_up_one_way (fixture.sender, fixture.receiver, target_ptr,
                          routed_))
        return false;

    std::vector<zlink_msg_t> outbound (iterations_);
    std::vector<zlink_msg_t> inbound (iterations_);
    if (!init_payloads (&outbound, 0x5a) || !init_empty_messages (&inbound))
        return false;

    bool measured_ok = true;
    {
        collect_scope_t collect;
        for (size_t i = 0; i < iterations_; ++i) {
            if (!raw_send (fixture.sender, &outbound[i], target_ptr)) {
                measured_ok = false;
                break;
            }
        }
    }

    // TCP transport work is asynchronous. Let the already-submitted batch
    // reach the receive queue outside collection so the first blocking receive
    // does not count a scheduling-dependent amount of command-drain work.
    if (measured_ok && tcp_)
        std::this_thread::sleep_for (std::chrono::seconds (1));

    if (measured_ok) {
        collect_scope_t collect;
        for (size_t i = 0; i < iterations_; ++i) {
            if (!raw_receive (fixture.receiver, &inbound[i], routed_)) {
                measured_ok = false;
                break;
            }
        }
    }

    const bool cleanup_ok = close_messages (&outbound) && close_messages (&inbound);
    return measured_ok && cleanup_ok;
}

bool request_reply_once (void *dealer_, void *router_, zlink_msg_t *request_,
                         zlink_msg_t *router_receive_, zlink_msg_t *reply_,
                         zlink_completion_t *completion_,
                         zlink_completion_id_t *completion_id_)
{
    const zlink_submit_result_t request_result = zlink_request_part (
      dealer_, NULL, request_, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL,
      120000, NULL, completion_id_);
    if (request_result != ZLINK_SUBMIT_OK)
        return api_error ("zlink_request_part", request_result);

    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = 0;
    zlink_part_flag_t part_flag = ZLINK_PART_MORE;
    const zlink_recv_result_t receive_result = zlink_router_recv_part (
      router_, &source, &token, router_receive_, &part_flag,
      ZLINK_RECV_FLAGS_NONE);
    if (receive_result != ZLINK_RECV_OK)
        return api_error ("zlink_router_recv_part(request)", receive_result);
    if (!source || token == 0 || part_flag != ZLINK_PART_FINAL) {
        std::fprintf (stderr, "invalid request metadata\n");
        return false;
    }

    const zlink_submit_result_t reply_result =
      zlink_reply_part (router_, source, token, reply_, ZLINK_PART_FINAL);
    if (reply_result != ZLINK_SUBMIT_OK)
        return api_error ("zlink_reply_part", reply_result);

    const zlink_recv_result_t completion_result = zlink_completion_recv (
      dealer_, completion_, ZLINK_RECV_FLAGS_NONE);
    if (completion_result != ZLINK_RECV_OK)
        return api_error ("zlink_completion_recv", completion_result);
    if (completion_->kind != ZLINK_COMPLETION_REQUEST
        || completion_->completion_id != *completion_id_
        || completion_->request_result != ZLINK_REQUEST_OK
        || completion_->reply_part_count != 1) {
        std::fprintf (stderr, "invalid REQUEST completion\n");
        return false;
    }
    return true;
}

bool warm_up_request_reply (void *dealer_, void *router_)
{
    for (size_t i = 0; i < warmup_iterations; ++i) {
        zlink_msg_t request;
        zlink_msg_t router_receive;
        zlink_msg_t reply;
        zlink_completion_t completion;
        std::memset (&completion, 0, sizeof (completion));
        completion.struct_size = sizeof (completion);
        zlink_completion_id_t completion_id = 0;
        if (!init_payload (&request, 0x71)
            || zlink_msg_init (&router_receive) != ZLINK_CONFIG_OK
            || !init_payload (&reply, 0x72))
            return false;
        if (!request_reply_once (dealer_, router_, &request, &router_receive,
                                 &reply, &completion, &completion_id))
            return false;
        const bool close_ok = zlink_msg_close (&request) == ZLINK_CONFIG_OK
                              && zlink_msg_close (&router_receive)
                                   == ZLINK_CONFIG_OK
                              && zlink_msg_close (&reply) == ZLINK_CONFIG_OK;
        zlink_completion_close (&completion);
        if (!close_ok)
            return api_error ("warm-up request/reply close", -1);
    }
    return true;
}

bool run_request_reply (size_t iterations_)
{
    fixture_t fixture;
    if (!fixture.context) {
        std::fprintf (stderr, "zlink_ctx_new failed\n");
        return false;
    }
    fixture.receiver = zlink_socket (fixture.context, ZLINK_SOCKET_ROUTER);
    fixture.sender = zlink_socket (fixture.context, ZLINK_SOCKET_DEALER);
    if (!fixture.sender || !fixture.receiver)
        return api_error ("zlink_socket", -1);
    if (!configure_socket (fixture.sender)
        || !configure_socket (fixture.receiver))
        return false;
    const char dealer_name[] = "hotpath-dealer";
    if (zlink_set_routing_id (fixture.sender, dealer_name,
                              sizeof (dealer_name) - 1) != ZLINK_CONFIG_OK)
        return api_error ("zlink_set_routing_id", -1);
    const char endpoint[] = "inproc://hotpath-dealer-router-reqrep";
    if (zlink_bind (fixture.receiver, endpoint) != ZLINK_BIND_OK
        || zlink_connect (fixture.sender, endpoint) != ZLINK_CONNECT_OK)
        return api_error ("request/reply bind/connect", -1);
    if (!warm_up_request_reply (fixture.sender, fixture.receiver))
        return false;

    std::vector<zlink_msg_t> requests (iterations_);
    std::vector<zlink_msg_t> router_receives (iterations_);
    std::vector<zlink_msg_t> replies (iterations_);
    std::vector<zlink_completion_t> completions (iterations_);
    std::vector<zlink_completion_id_t> completion_ids (iterations_, 0);
    if (!init_payloads (&requests, 0x51)
        || !init_empty_messages (&router_receives)
        || !init_payloads (&replies, 0x52))
        return false;
    for (size_t i = 0; i < iterations_; ++i) {
        std::memset (&completions[i], 0, sizeof (completions[i]));
        completions[i].struct_size = sizeof (completions[i]);
    }

    bool measured_ok = true;
    {
        collect_scope_t collect;
        for (size_t i = 0; i < iterations_; ++i) {
            if (!request_reply_once (
                  fixture.sender, fixture.receiver, &requests[i],
                  &router_receives[i], &replies[i], &completions[i],
                  &completion_ids[i])) {
                measured_ok = false;
                break;
            }
        }
    }

    for (size_t i = 0; i < iterations_; ++i)
        zlink_completion_close (&completions[i]);
    const bool cleanup_ok = close_messages (&requests)
                            && close_messages (&router_receives)
                            && close_messages (&replies);
    return measured_ok && cleanup_ok;
}

bool parse_iterations (const char *text_, size_t *iterations_out_)
{
    if (!text_ || !*text_ || *text_ == '-')
        return false;
    errno = 0;
    char *end = NULL;
    const unsigned long long parsed = std::strtoull (text_, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed == 0
        || parsed > std::numeric_limits<size_t>::max ())
        return false;
    *iterations_out_ = static_cast<size_t> (parsed);
    return true;
}
} // namespace

int main (int argc_, char **argv_)
{
    if (argc_ != 3) {
        std::fprintf (
          stderr,
          "usage: %s <cell> <iterations>\n"
          "cells: dealer_dealer_inproc, dealer_router_reqrep_inproc, "
          "pair_inproc, router_router_tcp\n",
          argv_[0]);
        return 2;
    }

    size_t iterations = 0;
    if (!parse_iterations (argv_[2], &iterations)) {
        std::fprintf (stderr, "invalid iterations: %s\n", argv_[2]);
        return 2;
    }

    const std::string cell (argv_[1]);
    bool ok = false;
    if (cell == "dealer_dealer_inproc")
        ok = run_one_way (argv_[1], iterations, ZLINK_SOCKET_DEALER, false,
                          false);
    else if (cell == "dealer_router_reqrep_inproc")
        ok = run_request_reply (iterations);
    else if (cell == "pair_inproc")
        ok = run_one_way (argv_[1], iterations, ZLINK_SOCKET_PAIR, false,
                          false);
    else if (cell == "router_router_tcp")
        ok = run_one_way (argv_[1], iterations, ZLINK_SOCKET_ROUTER, true,
                          true);
    else {
        std::fprintf (stderr, "unknown cell: %s\n", argv_[1]);
        return 2;
    }

    return ok ? 0 : 1;
}

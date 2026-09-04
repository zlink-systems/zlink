/* SPDX-License-Identifier: MPL-2.0 */

// Blocking routed send waits inside Core and SNDTIMEO bounds that wait. Raw
// DONTWAIT instead returns a wait token without retaining the payload; the
// caller drains WRITABLE and recreates exactly that logical packet for retry.

#include "support.hpp"

#include <zlink.h>
#include <zlink/message/api.h>

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

namespace
{

const size_t raw_payload_size = 64;
const size_t max_fill_attempts = 512;

bool send_dealer_with_zero_timeout (zlink::dealer_socket_t &dealer_,
                                    const std::string &payload_)
{
    zlink::message_t payload = zlink_cpp_contract::make_message (payload_);
    try {
        dealer_.send ().message (payload).submit ();
        assert (!payload.valid ());
        return true;
    }
    catch (const zlink::submit_error_t &error) {
        assert (error.result () == zlink::submit_result_t::backpressured);
        assert (error.internal_errno () == EAGAIN);
        assert (payload.valid ());
        return false;
    }
}

bool fill_until_backpressured (zlink::dealer_socket_t &dealer_,
    const std::string &filler_)
{
    for (int attempt = 0; attempt < 64; ++attempt) {
        if (!send_dealer_with_zero_timeout (dealer_, filler_))
            return true;
    }
    return false;
}

void init_raw_part (zlink_msg_t &part_, const std::string &payload_)
{
    assert (zlink_msg_init_size (&part_, payload_.size ()) == ZLINK_CONFIG_OK);
    if (!payload_.empty ())
        std::memcpy (zlink_msg_data (&part_), payload_.data (), payload_.size ());
}

void assert_raw_part_consumed (zlink_msg_t &part_)
{
    assert (zlink_msg_size (&part_) == 0);
    assert (zlink_msg_close (&part_) == ZLINK_CONFIG_OK);
}

void configure_raw_small_hwm (void *socket_)
{
    const uint64_t hwm =
      4u * (static_cast<uint64_t> (raw_payload_size) + sizeof (zlink_msg_t));
    assert (zlink_set_option (socket_, ZLINK_OPT_SNDHWM, &hwm,
                              sizeof (hwm)) == ZLINK_CONFIG_OK);
    assert (zlink_set_option (socket_, ZLINK_OPT_RCVHWM, &hwm,
                              sizeof (hwm)) == ZLINK_CONFIG_OK);
}

void wait_raw_socket (void *socket_, short events_)
{
    zlink_pollitem_t item = {socket_, 0, events_, 0};
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    assert (zlink_poll (&item, 1, 5000, &error) == 1);
    assert (error == ZLINK_CONFIG_OK);
    assert ((item.revents & events_) == events_);
}

std::string receive_raw_dealer_part (void *dealer_)
{
    wait_raw_socket (dealer_, ZLINK_POLLIN);
    zlink_msg_t part;
    assert (zlink_msg_init (&part) == ZLINK_CONFIG_OK);
    zlink_part_flag_t part_flag = ZLINK_PART_MORE;
    assert (zlink_recv_part (dealer_, nullptr, &part, &part_flag,
                             ZLINK_RECV_FLAGS_DONTWAIT) == ZLINK_RECV_OK);
    assert (part_flag == ZLINK_PART_FINAL);
    const std::string payload (
      static_cast<const char *> (zlink_msg_data (&part)),
      zlink_msg_size (&part));
    assert (zlink_msg_close (&part) == ZLINK_CONFIG_OK);
    return payload;
}

void assert_raw_no_completion (void *socket_)
{
    zlink_completion_t completion{};
    completion.struct_size = sizeof (completion);
    errno = 0;
    assert (zlink_completion_recv (socket_, &completion,
                                   ZLINK_RECV_FLAGS_DONTWAIT)
            == ZLINK_RECV_NO_DATA);
    assert (zlink_errno () == EAGAIN);
    zlink_completion_close (&completion);
}

size_t fill_raw_router_until_backpressured (
  void *router_, const zlink_routing_id_t &target_,
  const std::string &logical_payload_, void *user_context_,
  zlink_completion_id_t &wait_token_)
{
    for (size_t attempt = 0; attempt != max_fill_attempts; ++attempt) {
        zlink_msg_t part;
        init_raw_part (part, logical_payload_);
        zlink_completion_id_t completion_id = UINT64_MAX;
        errno = 0;
        const zlink_submit_result_t result = zlink_send_part_rid (
          router_, &target_, &part, ZLINK_SEND_FLAGS_DONTWAIT,
          ZLINK_PART_FINAL, user_context_, &completion_id);
        const int submit_errno = zlink_errno ();
        assert_raw_part_consumed (part);
        if (result == ZLINK_SUBMIT_BACKPRESSURED) {
            assert (submit_errno == EAGAIN);
            assert (completion_id != 0);
            assert (attempt != 0);
            wait_token_ = completion_id;
            return attempt;
        }
        assert (result == ZLINK_SUBMIT_OK);
        assert (completion_id == 0);
    }
    assert (!"raw routed DONTWAIT did not reach its physical HWM");
    return 0;
}

zlink_routing_id_t make_raw_routing_id (const std::string &value_)
{
    zlink_routing_id_t rid{};
    assert (value_.size () <= sizeof (rid.data));
    rid.size = static_cast<uint8_t> (value_.size ());
    std::memcpy (rid.data, value_.data (), value_.size ());
    return rid;
}

void test_raw_dontwait_backpressure_writable_and_exact_retry ()
{
    void *context = zlink_ctx_new ();
    assert (context != nullptr);
    void *router = zlink_socket (context, ZLINK_SOCKET_ROUTER);
    void *dealer = zlink_socket (context, ZLINK_SOCKET_DEALER);
    assert (router != nullptr && dealer != nullptr);

    const int zero = 0;
    const int mandatory = 1;
    const int five_seconds = 5000;
    assert (zlink_set_option (router, ZLINK_OPT_LINGER, &zero,
                              sizeof (zero)) == ZLINK_CONFIG_OK);
    assert (zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero,
                              sizeof (zero)) == ZLINK_CONFIG_OK);
    assert (zlink_set_option (dealer, ZLINK_OPT_SNDTIMEO, &five_seconds,
                              sizeof (five_seconds)) == ZLINK_CONFIG_OK);
    assert (zlink_set_router_option (router, ZLINK_ROUTER_OPT_MANDATORY,
                                     &mandatory, sizeof (mandatory))
            == ZLINK_CONFIG_OK);
    const std::string dealer_name = "raw-exact-retry-peer";
    assert (zlink_set_routing_id (dealer, dealer_name.data (),
                                  dealer_name.size ()) == ZLINK_CONFIG_OK);
    configure_raw_small_hwm (router);
    configure_raw_small_hwm (dealer);

    const std::string endpoint =
      zlink_cpp_contract::unique_inproc ("raw-exact-target-retry");
    assert (zlink_bind (router, endpoint.c_str ()) == ZLINK_BIND_OK);
    assert (zlink_connect (dealer, endpoint.c_str ()) == ZLINK_CONNECT_OK);

    // A bounded blocking prime is the connection synchronization point and
    // registers the DEALER routing id with the ROUTER without a settle sleep.
    zlink_msg_t prime;
    init_raw_part (prime, "route-prime");
    zlink_completion_id_t prime_id = UINT64_MAX;
    assert (zlink_send_part (dealer, &prime, ZLINK_SEND_FLAGS_NONE,
                             ZLINK_PART_FINAL, nullptr, &prime_id)
            == ZLINK_SUBMIT_OK);
    assert (prime_id == 0);
    assert_raw_part_consumed (prime);

    wait_raw_socket (router, ZLINK_POLLIN);
    const zlink_routing_id_t *source_rid = nullptr;
    zlink_reply_token_t reply_token = UINT64_MAX;
    zlink_msg_t received_prime;
    assert (zlink_msg_init (&received_prime) == ZLINK_CONFIG_OK);
    zlink_part_flag_t prime_flag = ZLINK_PART_MORE;
    assert (zlink_router_recv_part (
              router, &source_rid, &reply_token, &received_prime, &prime_flag,
              ZLINK_RECV_FLAGS_DONTWAIT)
            == ZLINK_RECV_OK);
    assert (source_rid != nullptr);
    assert (source_rid->size == dealer_name.size ());
    assert (std::memcmp (source_rid->data, dealer_name.data (),
                         dealer_name.size ()) == 0);
    assert (reply_token == 0 && prime_flag == ZLINK_PART_FINAL);
    assert (zlink_msg_close (&received_prime) == ZLINK_CONFIG_OK);

    const zlink_routing_id_t target = make_raw_routing_id (dealer_name);
    int poller_context = 71;
    void *poller = zlink_poller_new ();
    assert (poller != nullptr);
    const short retry_events =
      static_cast<short> (ZLINK_POLLOUT | ZLINK_POLLCOMPLETION);
    assert (zlink_poller_add (poller, router, &poller_context, retry_events)
            == ZLINK_CONFIG_OK);

    std::string logical_payload = "raw-exact-target-retry-payload";
    logical_payload.resize (raw_payload_size, 'r');
    int operation_context = 72;
    zlink_completion_id_t wait_token = 0;
    const size_t accepted = fill_raw_router_until_backpressured (
      router, target, logical_payload, &operation_context, wait_token);
    assert (accepted != 0 && wait_token != 0);
    assert_raw_no_completion (router);

    zlink_poller_event_t event{};
    zlink_config_result_t poller_error = ZLINK_CONFIG_INTERNAL_ERROR;
    assert (zlink_poller_wait (poller, &event, 1, 0, &poller_error) == 0);
    assert (poller_error == ZLINK_CONFIG_OK);

    for (size_t index = 0; index != accepted; ++index)
        assert (receive_raw_dealer_part (dealer) == logical_payload);

    // Core retained no payload: restoring credit must not deliver the refused
    // packet before this application receives WRITABLE and retries it.
    zlink_pollitem_t before_retry = {dealer, 0, ZLINK_POLLIN, 0};
    zlink_config_result_t before_retry_error = ZLINK_CONFIG_INTERNAL_ERROR;
    assert (zlink_poll (&before_retry, 1, 20, &before_retry_error) == 0);
    assert (before_retry_error == ZLINK_CONFIG_OK);
    assert (before_retry.revents == 0);

    event = zlink_poller_event_t{};
    poller_error = ZLINK_CONFIG_INTERNAL_ERROR;
    assert (zlink_poller_wait (poller, &event, 1, 5000,
                               &poller_error) == 1);
    assert (poller_error == ZLINK_CONFIG_OK);
    assert (event.source_kind == ZLINK_POLLER_SOURCE_SOCKET);
    assert (event.socket == router);
    assert (event.user_data == &poller_context);
    assert ((event.events & retry_events) == retry_events);

    bool matching_writable_seen = false;
    for (;;) {
        zlink_completion_t completion{};
        completion.struct_size = sizeof (completion);
        errno = 0;
        const zlink_recv_result_t result = zlink_completion_recv (
          router, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_NO_DATA) {
            assert (zlink_errno () == EAGAIN);
            zlink_completion_close (&completion);
            break;
        }
        assert (result == ZLINK_RECV_OK);
        assert (!matching_writable_seen);
        assert (completion.kind == ZLINK_COMPLETION_WRITABLE);
        assert (completion.completion_id == wait_token);
        assert (completion.user_context == &operation_context);
        assert (completion.peer_rid.size == target.size);
        assert (std::memcmp (completion.peer_rid.data, target.data,
                             target.size) == 0);
        assert (completion.send_result == ZLINK_SEND_ADMITTED);
        assert (completion.send_terminal_errno == 0);
        matching_writable_seen = true;
        zlink_completion_close (&completion);
    }
    assert (matching_writable_seen);

    // Core retained only the token and context. Recreate the same logical
    // packet after WRITABLE and make a fresh admission attempt.
    zlink_msg_t retry;
    init_raw_part (retry, logical_payload);
    zlink_completion_id_t retry_id = UINT64_MAX;
    assert (zlink_send_part_rid (
              router, &target, &retry, ZLINK_SEND_FLAGS_DONTWAIT,
              ZLINK_PART_FINAL, nullptr, &retry_id)
            == ZLINK_SUBMIT_OK);
    assert (retry_id == 0);
    assert_raw_part_consumed (retry);
    assert (receive_raw_dealer_part (dealer) == logical_payload);

    assert_raw_no_completion (router);

    zlink_pollitem_t no_duplicate = {dealer, 0, ZLINK_POLLIN, 0};
    zlink_config_result_t poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
    assert (zlink_poll (&no_duplicate, 1, 20, &poll_error) == 0);
    assert (poll_error == ZLINK_CONFIG_OK);
    assert (no_duplicate.revents == 0);

    assert (zlink_poller_remove (poller, router) == ZLINK_CONFIG_OK);
    assert (zlink_poller_destroy (&poller) == ZLINK_CLOSE_OK);
    assert (poller == nullptr);
    assert (zlink_close (dealer) == ZLINK_CLOSE_OK);
    assert (zlink_close (router) == ZLINK_CLOSE_OK);
    assert (zlink_ctx_term (context) == ZLINK_CLOSE_OK);
}

int test_blocking_routed_send_resumes_on_core_credit ()
{
    zlink::context_t ctx;
    zlink::dealer_socket_t dealer (ctx);
    zlink::router_socket_t router (ctx);
    dealer.set_routing_id (zlink::routing_id_t::from ("resume-source"));
    router.set_routing_id (zlink::routing_id_t::from ("resume-sink"));

    const uint64_t hwm = UINT64_C (65536) + sizeof (zlink_msg_t);
    dealer.options ().send_hwm (zlink::byte_count_t::bytes (hwm));
    router.options ().recv_hwm (zlink::byte_count_t::bytes (hwm));
    dealer.options ().send_timeout (std::chrono::milliseconds (0));

    const std::string endpoint =
      zlink_cpp_contract::unique_inproc ("routed-send-resume");
    router.bind (endpoint);
    dealer.connect (endpoint);
    std::this_thread::sleep_for (std::chrono::milliseconds (200));

    const std::string filler (65536, 'f');
    if (!fill_until_backpressured (dealer, filler)) {
        std::fprintf (stderr, "could not reach the send HWM\n");
        return 1;
    }
    dealer.options ().send_timeout (std::chrono::seconds (5));

    // Nothing but Core can release this submit: the binding has no reactor
    // thread, no retry queue, and no timer left to move it along.
    std::atomic<bool> submitted{false};
    std::atomic<bool> failed{false};
    const auto started = std::chrono::steady_clock::now ();
    std::thread sender ([&] {
        zlink::message_t payload =
          zlink_cpp_contract::make_message ("resumed-by-core");
        try {
            dealer.send ().message (std::move (payload)).submit ();
        }
        catch (const zlink::submit_error_t &error) {
            std::fprintf (stderr, "blocking submit failed result=%d errno=%d\n",
                          static_cast<int> (error.result ()),
                          error.internal_errno ());
            failed.store (true, std::memory_order_release);
        }
        submitted.store (true, std::memory_order_release);
    });

    // The submit must still be parked inside Core while no credit exists.
    std::this_thread::sleep_for (std::chrono::milliseconds (200));
    const bool parked = !submitted.load (std::memory_order_acquire);

    bool resumed_payload_seen = false;
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (std::chrono::steady_clock::now () < deadline && !resumed_payload_seen) {
        zlink::received_t received;
        if (router.recv (received, zlink::recv_flags_t::dontwait) != 0) {
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
            continue;
        }
        resumed_payload_seen =
          received.first_part ().to_string () == "resumed-by-core";
        received.close ();
    }
    sender.join ();
    const auto elapsed = std::chrono::steady_clock::now () - started;

    if (!parked) {
        std::fprintf (stderr, "blocking submit did not wait for credit\n");
        return 2;
    }
    if (failed.load (std::memory_order_acquire)) {
        std::fprintf (stderr, "blocking submit did not resume on Core credit\n");
        return 3;
    }
    if (!resumed_payload_seen) {
        std::fprintf (stderr, "resumed send was never delivered\n");
        return 4;
    }
    if (elapsed >= std::chrono::seconds (10)) {
        std::fprintf (stderr, "blocking submit took too long to resume\n");
        return 5;
    }
    return 0;
}

int test_routed_send_without_credit_reports_backpressure ()
{
    zlink::context_t ctx;
    zlink::dealer_socket_t dealer (ctx);
    zlink::router_socket_t router (ctx);
    dealer.set_routing_id (zlink::routing_id_t::from ("timeout-source"));

    const uint64_t hwm = UINT64_C (65536) + sizeof (zlink_msg_t);
    dealer.options ().send_hwm (zlink::byte_count_t::bytes (hwm));
    router.options ().recv_hwm (zlink::byte_count_t::bytes (hwm));

    const std::string endpoint =
      zlink_cpp_contract::unique_inproc ("routed-send-timeout");
    router.bind (endpoint);
    dealer.connect (endpoint);
    std::this_thread::sleep_for (std::chrono::milliseconds (200));
    dealer.options ().send_timeout (std::chrono::milliseconds (0));

    const std::string filler (65536, 'f');
    if (!fill_until_backpressured (dealer, filler)) {
        std::fprintf (stderr, "could not reach the send HWM\n");
        return 1;
    }

    dealer.options ().send_timeout (std::chrono::milliseconds (100));
    zlink::message_t payload = zlink_cpp_contract::make_message ("not-admitted");
    const auto started = std::chrono::steady_clock::now ();
    try {
        dealer.send ()
          .message (payload)
          .submit ();
        std::fprintf (stderr, "blocking NONE must fail at the HWM timeout\n");
        return 2;
    }
    catch (const zlink::submit_error_t &error) {
        if (error.result () != zlink::submit_result_t::backpressured) {
            std::fprintf (stderr, "unexpected result=%d errno=%d\n",
                          static_cast<int> (error.result ()),
                          error.internal_errno ());
            return 3;
        }
    }
    if (std::chrono::steady_clock::now () - started >= std::chrono::seconds (2)) {
        std::fprintf (stderr, "blocking NONE exceeded the configured timeout\n");
        return 4;
    }
    // The C++ staging policy preserves the public lvalue even though Core
    // consumes the native part used for the failed synchronous attempt.
    if (!payload.valid ()) {
        std::fprintf (stderr, "a refused submit must not consume the part\n");
        return 5;
    }
    return 0;
}

} // namespace

int main ()
{
    test_raw_dontwait_backpressure_writable_and_exact_retry ();
    if (const int rc = test_blocking_routed_send_resumes_on_core_credit ())
        return rc;
    if (const int rc = test_routed_send_without_credit_reports_backpressure ())
        return 10 + rc;
    return 0;
}

/* SPDX-License-Identifier: MPL-2.0 */

/*
 * Focused contract test for the receive-flow-state binding parity
 * (doc/plan/autohwm/core-byte-hwm-flow-control-plan.ko.md §5.1, §7.3, §8.1.1).
 *
 * Scope: enum ABI parity, DEALER/ROUTER success + idempotent repeat,
 * not-supported mapping for PAIR/PUB/SUB/STREAM, invalid handle/argument
 * mapping, absence of any flow-frame API on the public surface, and an
 * existing-HWM/EAGAIN-equivalent smoke check that this feature does not
 * change unrelated send back-pressure behavior.
 */

#include "support.hpp"

#include <type_traits>

namespace
{

// The public surface must expose exactly one setter and the two-value enum;
// it must not expose any way to encode/decode or receive a flow frame, and
// must not expose a PAUSE-bypass send variant.
template <typename SocketT> class has_receive_flow_state_setter_t
{
  private:
    template <typename T>
    static auto test (int)
      -> decltype (std::declval<T &> ().set_receive_flow_state (
                     zlink::receive_flow_state_t::running),
                   std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<SocketT> (0))::value;
};

template <typename SocketT> class has_flow_frame_api_t
{
  private:
    template <typename T>
    static auto test (int)
      -> decltype (std::declval<T &> ().recv_flow_frame (), std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<SocketT> (0))::value;
};

template <typename SocketT> class has_pause_bypass_send_t
{
  private:
    template <typename T>
    static auto test (int)
      -> decltype (std::declval<T &> ().send_infrastructure (), std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<SocketT> (0))::value;
};

} // namespace

// Enum values must match the C ABI (zlink_receive_flow_state_t) exactly.
static_assert (static_cast<int> (zlink::receive_flow_state_t::running) == 0,
               "receive_flow_state_t::running must equal ZLINK_RECEIVE_FLOW_RUNNING (0)");
static_assert (static_cast<int> (zlink::receive_flow_state_t::paused) == 1,
               "receive_flow_state_t::paused must equal ZLINK_RECEIVE_FLOW_PAUSED (1)");

// One socket operation, exposed on every socket type via the common socket_t
// base (runtime not-supported mapping covers types without a completion lane).
static_assert (has_receive_flow_state_setter_t<zlink::dealer_socket_t>::value,
               "dealer_socket_t must expose set_receive_flow_state");
static_assert (has_receive_flow_state_setter_t<zlink::router_socket_t>::value,
               "router_socket_t must expose set_receive_flow_state");
static_assert (has_receive_flow_state_setter_t<zlink::pair_socket_t>::value,
               "pair_socket_t must expose set_receive_flow_state (runtime not-supported)");
static_assert (has_receive_flow_state_setter_t<zlink::pub_socket_t>::value,
               "pub_socket_t must expose set_receive_flow_state (runtime not-supported)");
static_assert (has_receive_flow_state_setter_t<zlink::sub_socket_t>::value,
               "sub_socket_t must expose set_receive_flow_state (runtime not-supported)");
static_assert (has_receive_flow_state_setter_t<zlink::stream_socket_t>::value,
               "stream_socket_t must expose set_receive_flow_state (runtime not-supported)");

// No flow-frame receive/encode API and no PAUSE-bypass send variant anywhere
// on the public surface (plan §5.1 forbidden list).
static_assert (!has_flow_frame_api_t<zlink::dealer_socket_t>::value,
               "no socket may expose a flow-frame receive API");
static_assert (!has_flow_frame_api_t<zlink::router_socket_t>::value,
               "no socket may expose a flow-frame receive API");
static_assert (!has_pause_bypass_send_t<zlink::dealer_socket_t>::value,
               "no socket may expose a PAUSE-bypass send variant");
static_assert (!has_pause_bypass_send_t<zlink::router_socket_t>::value,
               "no socket may expose a PAUSE-bypass send variant");

namespace
{

void test_dealer_router_set_succeeds_and_is_idempotent ()
{
    zlink::context_t ctx;
    zlink::dealer_socket_t dealer (ctx);
    zlink::router_socket_t router (ctx);

    // First application and idempotent repeat both succeed (no throw).
    dealer.set_receive_flow_state (zlink::receive_flow_state_t::paused);
    dealer.set_receive_flow_state (zlink::receive_flow_state_t::paused);
    dealer.set_receive_flow_state (zlink::receive_flow_state_t::running);
    dealer.set_receive_flow_state (zlink::receive_flow_state_t::running);

    router.set_receive_flow_state (zlink::receive_flow_state_t::paused);
    router.set_receive_flow_state (zlink::receive_flow_state_t::paused);
    router.set_receive_flow_state (zlink::receive_flow_state_t::running);
}

template <typename SocketT> void assert_not_supported (SocketT &socket_)
{
    bool threw_not_supported = false;
    try {
        socket_.set_receive_flow_state (zlink::receive_flow_state_t::paused);
    } catch (const zlink::config_error_t &err) {
        threw_not_supported = (err.result () == zlink::config_result_t::not_supported);
    }
    assert (threw_not_supported);
}

void test_unsupported_socket_types_report_not_supported ()
{
    zlink::context_t ctx;

    zlink::pair_socket_t pair_a (ctx);
    assert_not_supported (pair_a);

    zlink::pub_socket_t pub (ctx);
    assert_not_supported (pub);

    zlink::sub_socket_t sub (ctx);
    assert_not_supported (sub);

    zlink::stream_socket_t stream (ctx);
    assert_not_supported (stream);
}

void test_unsupported_socket_send_recv_is_unchanged ()
{
    // Calling the not-supported flow-state API on a PAIR socket must not
    // disturb its ordinary send/recv behavior.
    zlink::context_t ctx;
    zlink::pair_socket_t left (ctx);
    zlink::pair_socket_t right (ctx);
    zlink::socket_monitor_t left_monitor = left.monitor_open ();
    zlink::socket_monitor_t right_monitor = right.monitor_open ();

    const std::string endpoint = zlink_cpp_contract::unique_inproc ("flow-state-pair-unaffected");
    left.bind (endpoint);
    right.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      left_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      right_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));

    assert_not_supported (left);

    zlink::message_t outbound = zlink_cpp_contract::make_message ("still-works");
    assert (right.send ().message (outbound).submit ());
    zlink::message_t inbound;
    assert (left.recv (inbound) == 0);
    assert (inbound.to_string () == "still-works");
}

void test_invalid_handle_reports_invalid_handle ()
{
    zlink::context_t ctx;
    zlink::dealer_socket_t dealer (ctx);
    zlink::dealer_socket_t moved = std::move (dealer);
    // `dealer` is now the moved-from, invalid handle.
    assert (!dealer.valid ());

    bool threw_invalid_handle = false;
    try {
        dealer.set_receive_flow_state (zlink::receive_flow_state_t::running);
    } catch (const zlink::config_error_t &err) {
        threw_invalid_handle = (err.result () == zlink::config_result_t::invalid_handle);
    }
    assert (threw_invalid_handle);
}

void test_close_then_set_reports_invalid_handle_or_invalid_state ()
{
    // A concurrent close and a flow-state set can only ever leave one of the
    // two observable outcomes: the close wins first (invalid_state) or the
    // close has already fully unregistered the handle (invalid_handle). Both
    // are the language binding's mapping of the plan's "close raced first"
    // outcome (worklog/stage7-c-api.md §1.2).
    zlink::context_t ctx;
    zlink::dealer_socket_t dealer (ctx);
    dealer.close ();

    bool threw_expected = false;
    try {
        dealer.set_receive_flow_state (zlink::receive_flow_state_t::paused);
    } catch (const zlink::config_error_t &err) {
        threw_expected = (err.result () == zlink::config_result_t::invalid_handle
                          || err.result () == zlink::config_result_t::invalid_state);
    }
    assert (threw_expected);
}

// Smoke check (plan §8.1.1 last bullet): adding set_receive_flow_state to the
// common socket_t base does not alter existing byte-HWM/EAGAIN-equivalent
// back-pressure behavior for an unrelated pair. DEALER/ROUTER routed sends
// only expose an async() terminal, so PAIR (send_operation_t with flags()
// and a nonblocking submit() terminal) is used to observe backpressure
// synchronously, matching the existing pattern in
// test_stream_send_ready_handler_survives_move_and_source_destruction.
void test_existing_hwm_backpressure_is_unchanged ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t left (ctx);
    zlink::pair_socket_t right (ctx);
    zlink::socket_monitor_t left_monitor = left.monitor_open ();
    zlink::socket_monitor_t right_monitor = right.monitor_open ();

    right.options ().send_hwm (zlink::byte_count_t::bytes (64));

    const std::string endpoint = zlink_cpp_contract::unique_inproc ("flow-state-hwm-smoke");
    left.bind (endpoint);
    right.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      left_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      right_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));

    bool backpressured = false;
    const std::string payload (256, 'x');
    for (int i = 0; i < 4096; ++i) {
        zlink::message_t message = zlink_cpp_contract::make_message (payload);
        if (!right.send ()
               .message (message)
               .flags (static_cast<int> (zlink::send_flags_t::dontwait))
               .submit ()) {
            backpressured = true;
            break;
        }
    }
    assert (backpressured);
}

} // namespace

int main ()
{
    test_dealer_router_set_succeeds_and_is_idempotent ();
    test_unsupported_socket_types_report_not_supported ();
    test_unsupported_socket_send_recv_is_unchanged ();
    test_invalid_handle_reports_invalid_handle ();
    test_close_then_set_reports_invalid_handle_or_invalid_state ();
    test_existing_hwm_backpressure_is_unchanged ();
    return 0;
}

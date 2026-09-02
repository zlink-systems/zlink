/* SPDX-License-Identifier: MPL-2.0 */

/*
 * Focused contract test for the receive-flow-state binding parity
 * (doc/plan/autohwm/core-byte-hwm-flow-control-plan.ko.md §5.1, §7.3, §8.1.1)
 * and the follow-up flow-state monitor parity (events + status metrics).
 *
 * Scope: enum ABI parity, DEALER/ROUTER success + idempotent repeat,
 * not-supported mapping for PAIR/PUB/SUB/STREAM, invalid handle/argument
 * mapping, absence of any flow-frame API on the public surface, an
 * existing-HWM/EAGAIN-equivalent smoke check that this feature does not
 * change unrelated send back-pressure behavior, and monitor parity: the new
 * event constants and flag constants match the C ABI, the ALL mask covers
 * them, and the new status metrics are present (and zero on a fresh socket).
 */

#include "support.hpp"

#include <zlink.h>

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

// Monitor event constants must match the C ABI bit values exactly, and the
// ALL mask must cover them.
static_assert (static_cast<uint32_t> (zlink::monitor_event::send_flow_paused)
                 == static_cast<uint32_t> (ZLINK_EVENT_SEND_FLOW_PAUSED),
               "monitor_event::send_flow_paused must equal ZLINK_EVENT_SEND_FLOW_PAUSED");
static_assert (static_cast<uint32_t> (zlink::monitor_event::send_flow_resumed)
                 == static_cast<uint32_t> (ZLINK_EVENT_SEND_FLOW_RESUMED),
               "monitor_event::send_flow_resumed must equal ZLINK_EVENT_SEND_FLOW_RESUMED");
static_assert (static_cast<uint32_t> (zlink::monitor_event::flow_state_stale)
                 == static_cast<uint32_t> (ZLINK_EVENT_FLOW_STATE_STALE),
               "monitor_event::flow_state_stale must equal ZLINK_EVENT_FLOW_STATE_STALE");
static_assert (static_cast<uint32_t> (zlink::monitor_event::all)
                 == static_cast<uint32_t> (ZLINK_EVENT_ALL),
               "monitor_event::all must equal ZLINK_EVENT_ALL (0x7FFFF)");
static_assert ((static_cast<uint32_t> (zlink::monitor_event::all)
                & static_cast<uint32_t> (zlink::monitor_event::send_flow_paused))
                 == static_cast<uint32_t> (zlink::monitor_event::send_flow_paused),
               "monitor_event::all must cover send_flow_paused");
static_assert ((static_cast<uint32_t> (zlink::monitor_event::all)
                & static_cast<uint32_t> (zlink::monitor_event::send_flow_resumed))
                 == static_cast<uint32_t> (zlink::monitor_event::send_flow_resumed),
               "monitor_event::all must cover send_flow_resumed");
static_assert ((static_cast<uint32_t> (zlink::monitor_event::all)
                & static_cast<uint32_t> (zlink::monitor_event::flow_state_stale))
                 == static_cast<uint32_t> (zlink::monitor_event::flow_state_stale),
               "monitor_event::all must cover flow_state_stale");

// Monitor event flag constants (carried in monitor_event_t::flags) must match
// the C ABI bit values exactly.
static_assert (static_cast<uint32_t> (zlink::monitor_event_flag_t::send_flow_writable)
                 == ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE,
               "monitor_event_flag_t::send_flow_writable must equal the C flag bit");
static_assert (static_cast<uint32_t> (zlink::monitor_event_flag_t::flow_state_stale_epoch)
                 == ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH,
               "monitor_event_flag_t::flow_state_stale_epoch must equal the C flag bit");
static_assert (static_cast<uint32_t> (zlink::monitor_event_flag_t::connection_ready_edge)
                 == ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE,
               "monitor_event_flag_t::connection_ready_edge must equal the C flag bit");

// The event type surfaces only contract-owned logical fields.
static_assert (std::is_same<decltype (zlink::monitor_event_t ().flags), std::uint32_t>::value,
               "monitor_event_t must expose flags as uint32_t");
static_assert (std::is_same<decltype (zlink::monitor_event_t ().value), std::uint64_t>::value,
               "monitor_event_t must expose value as uint64_t");
static_assert (
  std::is_same<decltype (zlink::monitor_event_t ().routing_id),
               std::optional<zlink::routing_id_t>>::value,
  "monitor_event_t must expose an optional routing_id");

// The status projection must expose the five flow metrics as uint64_t.
static_assert (
  std::is_same<decltype (zlink::monitor_status_t ().flow_paused_connections), uint64_t>::value,
  "monitor status must expose flow_paused_connections as uint64_t");
static_assert (
  std::is_same<decltype (zlink::monitor_status_t ().flow_pause_applied_total), uint64_t>::value,
  "monitor status must expose flow_pause_applied_total as uint64_t");
static_assert (
  std::is_same<decltype (zlink::monitor_status_t ().flow_resume_applied_total), uint64_t>::value,
  "monitor status must expose flow_resume_applied_total as uint64_t");
static_assert (
  std::is_same<decltype (zlink::monitor_status_t ().flow_state_stale_total), uint64_t>::value,
  "monitor status must expose flow_state_stale_total as uint64_t");
static_assert (
  std::is_same<decltype (zlink::monitor_status_t ().flow_pause_duration_ms), uint64_t>::value,
  "monitor status must expose flow_pause_duration_ms as uint64_t");

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
    right.send ().message (outbound).submit ();
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
// synchronously.
void test_existing_hwm_backpressure_is_unchanged ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t left (ctx);
    zlink::pair_socket_t right (ctx);
    zlink::socket_monitor_t left_monitor = left.monitor_open ();
    zlink::socket_monitor_t right_monitor = right.monitor_open ();

    right.options ().send_hwm (zlink::byte_count_t::bytes (64));
    right.options ().send_timeout (std::chrono::milliseconds (0));

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
        try {
            right.send ().message (message).submit ();
        }
        catch (const zlink::submit_error_t &error) {
            if (error.result () == zlink::submit_result_t::backpressured) {
                backpressured = true;
                break;
            }
            throw;
        }
    }
    assert (backpressured);
}

// Monitor status parity: the five new flow metrics are present on the public
// projection and read back as zero on a fresh, unconnected PAIR socket (a
// socket type with no completion lane, so these fields never populate).
void test_monitor_status_flow_metrics_are_present_and_zero ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t pair (ctx);
    zlink::socket_monitor_t monitor = pair.monitor_open (zlink::monitor_event::all);

    const zlink::monitor_status_t snapshot = monitor.status ();
    assert (snapshot.abi_version == ZLINK_MONITOR_STATUS_ABI_VERSION);
    assert (snapshot.struct_size == sizeof (zlink_monitor_status_t));
    assert (snapshot.flow_paused_connections == 0u);
    assert (snapshot.flow_pause_applied_total == 0u);
    assert (snapshot.flow_resume_applied_total == 0u);
    assert (snapshot.flow_state_stale_total == 0u);
    assert (snapshot.flow_pause_duration_ms == 0u);
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
    test_monitor_status_flow_metrics_are_present_and_zero ();
    return 0;
}

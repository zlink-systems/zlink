/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"

#include <cstdlib>
#include <functional>
#include <future>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <atomic>
#include <vector>

namespace
{

template <typename SocketT> class has_try_send_t
{
  private:
    template <typename T>
    static auto
    test (int) -> decltype (std::declval<T &> ().try_send (std::declval<zlink::message_t &> ()),
                            std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<SocketT> (0))::value;
};

template <typename SocketT> class has_try_subscribe_t
{
  private:
    template <typename T>
    static auto test (int) -> decltype (std::declval<T &> ().subscribe_no_wait (),
                                        std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<SocketT> (0))::value;
};

template <typename SocketT> class has_try_receive_subscription_event_t
{
  private:
    template <typename T>
    static auto test (int) -> decltype (std::declval<T &> ().try_receive_subscription_event (),
                                        std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<SocketT> (0))::value;
};

template <typename SocketT> class has_set_receive_handler_t
{
  private:
    template <typename T>
    static auto test (int) -> decltype (std::declval<T &> ().set_receive_handler (
                                          std::function<void (zlink::received_t)> ()),
                                        std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<SocketT> (0))::value;
};

static_assert (!has_try_send_t<zlink::pair_socket_t>::value,
               "pair_socket_t must not expose try_send");
static_assert (!has_try_subscribe_t<zlink::sub_socket_t>::value,
               "sub_socket_t must not expose subscribe_no_wait");
static_assert (!has_try_receive_subscription_event_t<zlink::xpub_socket_t>::value,
               "xpub_socket_t must not expose try_receive_subscription_event");
static_assert (!has_set_receive_handler_t<zlink::pair_socket_t>::value,
               "pair_socket_t must not expose set_receive_handler");
static_assert (!has_set_receive_handler_t<zlink::dealer_socket_t>::value,
               "dealer_socket_t must not expose set_receive_handler");
static_assert (!has_set_receive_handler_t<zlink::router_socket_t>::value,
               "router_socket_t must not expose set_receive_handler");
static_assert (!has_set_receive_handler_t<zlink::stream_socket_t>::value,
               "stream_socket_t must not expose raw set_receive_handler");

template <typename T>
concept has_blocking_get_t = requires (T &result) { result.get (); };
template <typename T>
concept has_blocking_wait_t = requires (T &result) { result.wait (); };
template <typename T>
concept has_blocking_wait_for_t = requires (T &result) {
    result.wait_for (std::chrono::milliseconds (1));
};
template <typename T>
concept has_managed_send_callback_t = requires (T &&operation) {
    std::move (operation).submit (
      std::function<void (zlink::submit_result_t)> ());
};
template <typename T>
concept has_managed_request_callback_t = requires (T &&operation) {
    std::move (operation).submit (
      std::function<void (zlink::request_result_t,
                          std::vector<zlink::message_t>)> ());
};
template <typename T>
concept has_sync_submit_t = requires (T &&operation) {
    std::move (operation).submit ();
};
template <typename T>
concept has_flags_t = requires (T &&operation) {
    std::move (operation).flags (0);
};

static_assert (std::is_move_constructible_v<zlink::async_result_t<void>>);
static_assert (!std::is_copy_constructible_v<zlink::async_result_t<void>>);
static_assert (!has_blocking_get_t<zlink::async_result_t<void>>);
static_assert (!has_blocking_wait_t<zlink::async_result_t<void>>);
static_assert (!has_blocking_wait_for_t<zlink::async_result_t<void>>);
static_assert (!has_managed_send_callback_t<zlink::send_submit_operation_t>);
static_assert (has_managed_request_callback_t<zlink::request_submit_operation_t>);
template <typename T>
concept has_async_t = requires (T &&operation) { std::move (operation).async (); };

// Routed send exposes Core-owned blocking/non-blocking sync submit via flags()
// and completion-driven async().
static_assert (has_sync_submit_t<zlink::routed_send_submit_operation_t>);
static_assert (has_flags_t<zlink::routed_send_submit_operation_t>);
static_assert (has_async_t<zlink::routed_send_submit_operation_t>);
static_assert (std::is_same_v<
               decltype (std::declval<zlink::routed_send_submit_operation_t &&> ().submit ()),
               void>);
// Plain HWM-managed send exposes both terminals. Publish uses a separate
// synchronous-only builder so its public type has no async() member.
static_assert (has_async_t<zlink::send_submit_operation_t>);
static_assert (!has_async_t<zlink::publish_submit_operation_t>);

template <typename Fn> void expect_runtime_error (Fn fn_)
{
    bool threw = false;
    try {
        fn_ ();
    }
    catch (const std::runtime_error &) {
        threw = true;
    }
    assert (threw);
}

void test_pair_recv_nonblocking_returns_empty_without_data ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t socket (ctx);
    zlink::received_t received;
    const int rc = socket.recv (received, zlink::recv_flags_t::dontwait);
    assert (rc == static_cast<int> (zlink::recv_result_t::no_data));
}

void test_receive_reuses_caller_storage_capacity ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t sender (ctx);
    zlink::pair_socket_t receiver (ctx);
    zlink::socket_monitor_t sender_monitor = sender.monitor_open ();
    zlink::socket_monitor_t receiver_monitor = receiver.monitor_open ();

    const std::string endpoint = zlink_cpp_contract::unique_inproc ("receive-storage");
    sender.bind (endpoint);
    receiver.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      sender_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      receiver_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));

    zlink::message_t first = zlink_cpp_contract::make_message ("first");
    zlink::message_t second = zlink_cpp_contract::make_message ("second");
    assert (receiver.send ().message (first).message (second).submit ());

    zlink::received_t received;
    assert (sender.recv (received) == 0);
    assert (received.parts ().size () == 2);
    const std::size_t retained_capacity = received.parts ().capacity ();
    assert (retained_capacity >= 2);
    received.close ();

    zlink::message_t third = zlink_cpp_contract::make_message ("third");
    zlink::message_t fourth = zlink_cpp_contract::make_message ("fourth");
    assert (receiver.send ().message (third).message (fourth).submit ());
    assert (sender.recv (received) == 0);
    assert (received.parts ().size () == 2);
    assert (received.parts ().capacity () >= retained_capacity);
}

void test_sub_subscribe_nonblocking_returns_empty_without_data ()
{
    zlink::context_t ctx;
    zlink::sub_socket_t socket (ctx);
    zlink::topic_message_t received;
    const int rc = socket.subscribe (received, zlink::recv_flags_t::dontwait);
    assert (rc == static_cast<int> (zlink::recv_result_t::no_data));
}

void test_xpub_receive_subscription_event_nonblocking_returns_empty_without_data ()
{
    zlink::context_t ctx;
    zlink::xpub_socket_t socket (ctx);
    zlink::subscription_event_t event;
    const int rc = socket.receive_subscription_event (event, zlink::recv_flags_t::dontwait);
    assert (rc == static_cast<int> (zlink::recv_result_t::no_data));
}

void test_pair_send_without_peer_preserves_submit_surface ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t sender (ctx);

    sender.bind (zlink_cpp_contract::unique_inproc ("pair-send"));

    zlink::message_t outbound = zlink_cpp_contract::make_message ("payload");
    (void) sender.send ().message (outbound).flags (zlink::recv_flags_t::dontwait).submit ();
}

void test_router_send_throws_for_closed_socket ()
{
    zlink::context_t ctx;
    zlink::router_socket_t router (ctx);
    router.close ();

    const std::string rid_text = "UNKNOWN";
    zlink::routing_id_t routing_id = zlink::routing_id_t::from (
      reinterpret_cast<const uint8_t *> (rid_text.data ()), rid_text.size ());
    zlink::message_t outbound = zlink_cpp_contract::make_message ("no-route");
    expect_runtime_error ([&] { router.send (routing_id).message (outbound).submit (); });
    assert (outbound.valid ());
}

void test_send_throws_on_general_error ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t socket (ctx);
    socket.close ();

    zlink::message_t outbound = zlink_cpp_contract::make_message ("send-error");
    expect_runtime_error ([&] { socket.send ().message (outbound).submit (); });
    assert (outbound.valid ());
}

void test_publish_throws_on_general_error ()
{
    zlink::context_t ctx;
    zlink::pub_socket_t socket (ctx);
    socket.close ();

    zlink::message_t outbound = zlink_cpp_contract::make_message ("publish-error");
    expect_runtime_error ([&] { socket.publish ("topic:error").message (outbound).submit (); });
    assert (outbound.valid ());
}

void test_stream_receive_returns_busy_in_packet_callback_mode ()
{
    zlink::context_t ctx;
    zlink::stream_socket_t socket (ctx);

    socket.set_packet_handler (
      [] (const zlink::routing_id_t &, zlink::message_t, zlink::message_t) {});
    zlink::received_t received;
    const int rc = socket.recv (received);
    assert (rc == static_cast<int> (zlink::recv_result_t::busy) || rc == -1);
    if (rc == -1)
        assert (errno == EBUSY);
}

void test_stream_receive_with_poller_readiness ()
{
    zlink::context_t ctx;
    zlink::stream_socket_t server (ctx);
    zlink::socket_monitor_t monitor = server.monitor_open ();
    server.options ().notify (false);
    server.bind ("tcp://127.0.0.1:0");
    const std::string endpoint = server.options ().last_endpoint ();
    assert (!endpoint.empty ());

    zlink::poller_t poller;
    poller.add (server, zlink::poll_event_flag_t::pollin, 1);
    zlink_cpp_contract::raw_tcp_client_t client (endpoint);
    assert (zlink_cpp_contract::wait_stream_connected (monitor));

    const std::vector<unsigned char> frame =
      zlink_cpp_contract::encode_stream_packet_frame ("poller-recv");
    client.send_all (reinterpret_cast<const char *> (frame.data ()), frame.size ());

    zlink::poll_event_t event;
    assert (poller.wait (&event, 1, std::chrono::seconds (2)) == 1);
    assert (event.slot == 1);
    assert ((static_cast<short> (event.revents)
             & static_cast<short> (zlink::poll_event_flag_t::pollin))
            != 0);

    zlink::received_t received;
    assert (server.recv (received, zlink::recv_flags_t::dontwait) == 0);
    assert (received.routing_id ().has_value ());
    assert (received.parts ().size () == 1);
    assert (received.parts ()[0].to_bytes ()
            == std::vector<std::uint8_t> (frame.begin (), frame.end ()));
}

void test_stream_packet_handler_survives_move_and_source_destruction ()
{
    zlink::context_t ctx;
    std::promise<std::string> payload_promise;
    std::future<std::string> payload_future = payload_promise.get_future ();

    zlink::stream_socket_t moved_server = [&] {
        zlink::stream_socket_t server (ctx);
        zlink::socket_monitor_t monitor = server.monitor_open ();
        server.options ().notify (false);
        server.bind ("tcp://127.0.0.1:0");
        const std::string endpoint = server.options ().last_endpoint ();
        assert (!endpoint.empty ());

        server.set_packet_handler (
          [&payload_promise] (const zlink::routing_id_t &, zlink::message_t header_,
                              zlink::message_t body_) {
              assert (header_.size () == 0);
              payload_promise.set_value (body_.to_string ());
          });

        zlink::stream_socket_t moved = std::move (server);
        zlink_cpp_contract::raw_tcp_client_t client (endpoint);
        assert (zlink_cpp_contract::wait_stream_connected (monitor));

        const std::string payload = "move-safe-packet";
        const std::vector<unsigned char> frame =
          zlink_cpp_contract::encode_stream_packet_frame (payload);
        client.send_all (reinterpret_cast<const char *> (frame.data ()), frame.size ());

        assert (zlink_cpp_contract::wait_future (payload_future, 2000) == payload);
        return moved;
    } ();

    moved_server.close ();
}

void test_socket_monitor_receive_returns_empty_without_event ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t socket (ctx);
    zlink::socket_monitor_t monitor = socket.monitor_open ();

    const std::optional<zlink::monitor_event_t> event =
      monitor.recv (zlink::recv_flags_t::dontwait);
    assert (!event);
    monitor.close ();
}

void test_routing_id_accepts_maximum_size ()
{
    const std::string bytes (255, 'r');

    const zlink::routing_id_t routing_id =
      zlink::routing_id_t::from (reinterpret_cast<const uint8_t *> (bytes.data ()), bytes.size ());
    assert (routing_id.size () == bytes.size ());
    assert (routing_id.to_bytes () == std::vector<uint8_t> (bytes.begin (), bytes.end ()));
}

void test_routing_id_rejects_oversize_input ()
{
    const std::string bytes (256, 'r');

    bool threw = false;
    try {
        (void) zlink::routing_id_t::from (reinterpret_cast<const uint8_t *> (bytes.data ()),
                                          bytes.size ());
    }
    catch (const std::invalid_argument &) {
        threw = true;
    }
    assert (threw);
}

void test_core_utilities_surface ()
{
    zlink::atomic_counter_t counter;
    assert (counter.valid ());
    counter.set (1);
    (void) counter.increment ();
    assert (counter.value () == 2);
    (void) counter.decrement ();
    assert (counter.value () == 1);
    counter.close ();

    zlink::stopwatch_t stopwatch;
    assert (stopwatch.valid ());
    (void) stopwatch.intermediate ();
    (void) stopwatch.stop ();

    std::atomic<int> value {0};
    zlink::thread_t thread ([&] { value.store (7); });
    thread.join ();
    assert (value.load () == 7);
}

void test_one_shot_poll_empty_set ()
{
    std::vector<zlink::poll_item_t> items;
    assert (zlink::poll (items, std::chrono::milliseconds (0)) == 0);
}

void test_routing_id_rejects_null_pointer_for_non_empty_bytes ()
{
    bool threw = false;
    try {
        (void) zlink::routing_id_t::from (nullptr, 1);
    }
    catch (const std::invalid_argument &) {
        threw = true;
    }
    assert (threw);
}

void test_routing_id_copy_assignment_preserves_short_value ()
{
    const std::string long_bytes (128, 'L');
    const std::string short_bytes ("rid");

    zlink::routing_id_t routing_id = zlink::routing_id_t::from (
      reinterpret_cast<const uint8_t *> (long_bytes.data ()), long_bytes.size ());
    const zlink::routing_id_t short_id = zlink::routing_id_t::from (
      reinterpret_cast<const uint8_t *> (short_bytes.data ()), short_bytes.size ());

    routing_id = short_id;
    assert (routing_id.size () == short_bytes.size ());
    assert (routing_id.to_bytes ()
            == std::vector<uint8_t> (short_bytes.begin (), short_bytes.end ()));
    assert (routing_id == short_id);
}

} // namespace

int main ()
{
    test_pair_recv_nonblocking_returns_empty_without_data ();
    test_receive_reuses_caller_storage_capacity ();
    test_sub_subscribe_nonblocking_returns_empty_without_data ();
    test_xpub_receive_subscription_event_nonblocking_returns_empty_without_data ();
    test_pair_send_without_peer_preserves_submit_surface ();
    test_router_send_throws_for_closed_socket ();
    test_send_throws_on_general_error ();
    test_publish_throws_on_general_error ();
    test_stream_receive_returns_busy_in_packet_callback_mode ();
    test_stream_receive_with_poller_readiness ();
    test_stream_packet_handler_survives_move_and_source_destruction ();
    test_socket_monitor_receive_returns_empty_without_event ();
    test_routing_id_accepts_maximum_size ();
    test_routing_id_rejects_oversize_input ();
    test_core_utilities_surface ();
    test_one_shot_poll_empty_set ();
    test_routing_id_rejects_null_pointer_for_non_empty_bytes ();
    test_routing_id_copy_assignment_preserves_short_value ();
    std::quick_exit (0);
}

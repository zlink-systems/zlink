/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"

#include <cassert>
#include <chrono>
#include <type_traits>

namespace
{

template <typename T>
concept has_async_t = requires (T &&operation) { std::move (operation).async (); };

static_assert (std::is_move_constructible_v<zlink::async_result_t<void>>);
static_assert (!std::is_copy_constructible_v<zlink::async_result_t<void>>);
static_assert (std::is_same_v<
               decltype (std::declval<zlink::send_submit_operation_t &&> ().submit ()),
               void>);
static_assert (has_async_t<zlink::send_submit_operation_t>);
static_assert (!has_async_t<zlink::publish_submit_operation_t>);
static_assert (std::is_move_constructible_v<zlink::stream_packet_t>);
static_assert (!std::is_copy_constructible_v<zlink::stream_packet_t>);

void test_receive_reuses_caller_storage_capacity ()
{
    zlink::context_t context;
    zlink::pair_socket_t sender (context);
    zlink::pair_socket_t receiver (context);
    const std::string endpoint = zlink_cpp_contract::unique_inproc ("receive-storage");
    sender.bind (endpoint);
    receiver.connect (endpoint);

    zlink::message_t first = zlink_cpp_contract::make_message ("first");
    zlink::message_t second = zlink_cpp_contract::make_message ("second");
    receiver.send ().message (first).message (second).submit ();
    zlink::received_t received;
    assert (sender.recv (received) == 0);
    const std::size_t retained_capacity = received.parts ().capacity ();
    received.close ();

    zlink::message_t third = zlink_cpp_contract::make_message ("third");
    zlink::message_t fourth = zlink_cpp_contract::make_message ("fourth");
    receiver.send ().message (third).message (fourth).submit ();
    assert (sender.recv (received) == 0);
    assert (received.parts ().size () == 2);
    assert (received.parts ().capacity () >= retained_capacity);
}

void test_stream_packet_reset_and_reuse ()
{
    zlink::context_t context;
    zlink::stream_socket_t server (context);
    zlink::socket_monitor_t monitor = server.monitor_open ();
    server.options ().notify (false);
    server.options ().recv_mode (zlink::stream_recv_mode_t::packet);
    assert (server.options ().recv_mode () == zlink::stream_recv_mode_t::packet);
    server.bind ("tcp://127.0.0.1:0");
    const std::string endpoint = server.options ().last_endpoint ();
    zlink_cpp_contract::raw_tcp_client_t client (endpoint);
    assert (zlink_cpp_contract::wait_stream_connected (monitor));

    zlink::stream_packet_t packet;
    const auto send_packet = [&] (const std::string &payload_) {
        const std::vector<unsigned char> frame =
          zlink_cpp_contract::encode_stream_packet_frame (payload_);
        client.send_all (reinterpret_cast<const char *> (frame.data ()), frame.size ());
        while (!server.recv_packet (packet, zlink::recv_flags_t::dontwait))
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        assert (packet.routing_id ().has_value ());
        assert (packet.body ().to_string () == payload_);
    };

    send_packet ("first-packet");
    const auto first_rid = *packet.routing_id ();
    send_packet ("second-packet");
    assert (*packet.routing_id () == first_rid);
    packet.close ();
    assert (!packet.routing_id ().has_value ());
    assert (packet.empty ());
}

void test_pull_no_data_surfaces ()
{
    zlink::context_t context;
    zlink::pair_socket_t pair (context);
    zlink::received_t received;
    assert (pair.recv (received, zlink::recv_flags_t::dontwait)
            == static_cast<int> (zlink::recv_result_t::no_data));

    zlink::socket_monitor_t monitor = pair.monitor_open ();
    assert (!monitor.recv (zlink::recv_flags_t::dontwait).has_value ());
    zlink::timer_t timer;
    assert (!timer.recv ().has_value ());
}

} // namespace

int main ()
{
    test_receive_reuses_caller_storage_capacity ();
    test_stream_packet_reset_and_reuse ();
    test_pull_no_data_surfaces ();
    return 0;
}

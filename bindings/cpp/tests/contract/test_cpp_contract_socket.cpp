/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"

#include <atomic>
#include <barrier>
#include <cerrno>
#include <climits>
#include <cstring>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace
{

template <typename SocketT> class has_routed_send_t
{
  private:
    template <typename T>
    static auto
    test (int) -> decltype (std::declval<T &> ().send (std::declval<const zlink::routing_id_t &> (),
                                                       std::declval<zlink::message_t &> ()),
                            std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<SocketT> (0))::value;
};

template <typename SocketT> class has_send_builder_t
{
  private:
    template <typename T>
    static auto test (int) -> decltype (std::declval<T &> ().send (), std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<SocketT> (0))::value;
};

template <typename SocketT> class has_routed_send_builder_t
{
  private:
    template <typename T>
    static auto test (int)
      -> decltype (std::declval<T &> ().send (std::declval<const zlink::routing_id_t &> ()),
                   std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<SocketT> (0))::value;
};

template <typename SocketT> class has_publish_builder_t
{
  private:
    template <typename T>
    static auto
    test (int) -> decltype (std::declval<T &> ().publish (std::declval<const std::string &> ()),
                            std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<SocketT> (0))::value;
};

template <typename SocketT> class has_receive_t
{
  private:
    template <typename T>
    static auto
    test (int) -> decltype (std::declval<T &> ().recv (std::declval<zlink::received_t &> ()),
                            std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<SocketT> (0))::value;
};

template <typename SocketT> class has_single_part_recv_t
{
  private:
    template <typename T>
    static auto
    test (int) -> decltype (std::declval<T &> ().recv (std::declval<zlink::message_t &> ()),
                            std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<SocketT> (0))::value;
};

template <typename SocketT> class has_subscribe_part_t
{
  private:
    template <typename T>
    static auto test (int) -> decltype (std::declval<T &> ().subscribe_part (
                                          std::declval<std::optional<zlink::routing_id_t> &> (),
                                          std::declval<std::string &> (),
                                          std::declval<zlink::message_t &> (),
                                          std::declval<bool &> ()),
                                        std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<SocketT> (0))::value;
};

template <typename SocketT> class has_routed_single_part_recv_t
{
  private:
    template <typename T>
    static auto
    test (int) -> decltype (std::declval<T &> ().recv (std::declval<zlink::routing_id_t &> (),
                                                       std::declval<zlink::message_t &> ()),
                            std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<SocketT> (0))::value;
};

template <typename SocketT> class has_raw_common_option_set_t
{
  private:
    template <typename T>
    static auto test (int) -> decltype (std::declval<T &> ().set_option (0, 0), std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<SocketT> (0))::value;
};

template <typename SocketT> class has_raw_common_option_get_t
{
  private:
    template <typename T>
    static auto
    test (int) -> decltype (std::declval<T &> ().get_option (0, static_cast<int *> (nullptr)),
                            std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<SocketT> (0))::value;
};

template <typename SocketT> class has_connect_t
{
  private:
    template <typename T>
    static auto
    test (int) -> decltype (std::declval<T &> ().connect (std::declval<const std::string &> ()),
                            std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<SocketT> (0))::value;
};

template <typename SocketT> class has_disconnect_t
{
  private:
    template <typename T>
    static auto
    test (int) -> decltype (std::declval<T &> ().disconnect (std::declval<const std::string &> ()),
                            std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<SocketT> (0))::value;
};

template <typename SocketT> class has_disconnect_rid_t
{
  private:
    template <typename T>
    static auto test (int) -> decltype (std::declval<T &> ().disconnect_rid (
                                          std::declval<const zlink::routing_id_t &> ()),
                                        std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<SocketT> (0))::value;
};

template <typename SocketT> class has_recv_spot_t
{
  private:
    template <typename T>
    static auto test (int) -> decltype (std::declval<T &> ().recv_spot (), std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<SocketT> (0))::value;
};

static_assert (!has_routed_send_t<zlink::pair_socket_t>::value,
               "pair_socket_t must not expose routed send");
static_assert (has_send_builder_t<zlink::pair_socket_t>::value,
               "pair_socket_t must expose send builder");
static_assert (has_receive_t<zlink::pair_socket_t>::value, "pair_socket_t must expose recv");
static_assert (has_single_part_recv_t<zlink::pair_socket_t>::value,
               "pair_socket_t must expose single-part recv");
static_assert (!has_raw_common_option_set_t<zlink::pair_socket_t>::value,
               "pair_socket_t must not expose raw common option setters");
static_assert (!has_raw_common_option_get_t<zlink::pair_socket_t>::value,
               "pair_socket_t must not expose raw common option getters");
static_assert (!has_routed_send_t<zlink::dealer_socket_t>::value,
               "dealer_socket_t must not expose routed send");
static_assert (has_send_builder_t<zlink::dealer_socket_t>::value,
               "dealer_socket_t must expose send builder");
static_assert (has_receive_t<zlink::dealer_socket_t>::value, "dealer_socket_t must expose recv");
static_assert (has_single_part_recv_t<zlink::dealer_socket_t>::value,
               "dealer_socket_t must expose single-part recv");
static_assert (!has_routed_send_t<zlink::router_socket_t>::value,
               "router_socket_t must not expose direct routed send");
static_assert (has_routed_send_builder_t<zlink::router_socket_t>::value,
               "router_socket_t must expose routed send builder");
static_assert (has_receive_t<zlink::router_socket_t>::value, "router_socket_t must expose recv");
static_assert (has_routed_single_part_recv_t<zlink::router_socket_t>::value,
               "router_socket_t must expose routed single-part recv");
static_assert (!has_recv_spot_t<zlink::router_socket_t>::value,
               "router_socket_t must not expose recv_spot");
static_assert (has_publish_builder_t<zlink::pub_socket_t>::value,
               "pub_socket_t must expose publish builder");
static_assert (has_subscribe_part_t<zlink::sub_socket_t>::value,
               "sub_socket_t must expose single-part subscribe");
static_assert (has_publish_builder_t<zlink::xpub_socket_t>::value,
               "xpub_socket_t must expose publish builder");
static_assert (has_subscribe_part_t<zlink::xsub_socket_t>::value,
               "xsub_socket_t must expose single-part subscribe");
static_assert (!has_routed_send_t<zlink::stream_socket_t>::value,
               "stream_socket_t must not expose direct routed send");
static_assert (has_routed_send_builder_t<zlink::stream_socket_t>::value,
               "stream_socket_t must expose routed send builder");
static_assert (has_receive_t<zlink::stream_socket_t>::value, "stream_socket_t must expose recv");
static_assert (!has_connect_t<zlink::stream_socket_t>::value,
               "stream_socket_t must not expose connect");
static_assert (!has_disconnect_t<zlink::stream_socket_t>::value,
               "stream_socket_t must not expose disconnect");
static_assert (!has_disconnect_rid_t<zlink::stream_socket_t>::value,
               "stream_socket_t must not expose disconnect_rid");

void test_pair_send_recv_single_part ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t left (ctx);
    zlink::pair_socket_t right (ctx);
    zlink::socket_monitor_t left_monitor = left.monitor_open ();
    zlink::socket_monitor_t right_monitor = right.monitor_open ();

    const std::string endpoint = zlink_cpp_contract::unique_inproc ("pair");
    left.bind (endpoint);
    right.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      left_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      right_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));

    zlink::message_t outbound = zlink_cpp_contract::make_message ("ping");
    right.send ().message (outbound).submit ();

    zlink::received_t inbound;
    assert (left.recv (inbound) == 0);
    assert (inbound.parts ().size () == 1);
    assert (inbound.parts ()[0].to_string () == "ping");
}

void test_pair_send_recv_single_part_direct ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t left (ctx);
    zlink::pair_socket_t right (ctx);
    zlink::socket_monitor_t left_monitor = left.monitor_open ();
    zlink::socket_monitor_t right_monitor = right.monitor_open ();

    const std::string endpoint = zlink_cpp_contract::unique_inproc ("pair-direct");
    left.bind (endpoint);
    right.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      left_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      right_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));

    zlink::message_t outbound = zlink_cpp_contract::make_message ("direct");
    right.send ().message (outbound).submit ();

    zlink::message_t inbound;
    assert (left.recv (inbound) == 0);
    assert (inbound.to_string () == "direct");
}

void test_pair_direct_recv_no_data_preserves_output ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t socket (ctx);

    zlink::message_t existing = zlink_cpp_contract::make_message ("keep");
    const int rc = socket.recv (existing, zlink::recv_flags_t::dontwait);
    assert (rc == static_cast<int> (zlink::recv_result_t::no_data) || rc == -1);
    if (rc == -1)
        assert (errno == EAGAIN || errno == EWOULDBLOCK);
    assert (existing.valid ());
    assert (existing.to_string () == "keep");

    zlink::message_t invalid;
    invalid.close ();
    const int invalid_rc = socket.recv (invalid, zlink::recv_flags_t::dontwait);
    assert (invalid_rc == static_cast<int> (zlink::recv_result_t::no_data) || invalid_rc == -1);
    if (invalid_rc == -1)
        assert (errno == EAGAIN || errno == EWOULDBLOCK);
    assert (!invalid.valid ());
}

void test_dealer_unified_send_awaitable_builder ()
{
    zlink::context_t ctx;
    zlink::router_socket_t router (ctx);
    zlink::dealer_socket_t dealer (ctx);
    zlink::socket_monitor_t router_monitor = router.monitor_open ();
    zlink::socket_monitor_t dealer_monitor = dealer.monitor_open ();

    const std::string endpoint = zlink_cpp_contract::unique_inproc ("dealer-send-no-wait");
    const zlink::routing_id_t dealer_id =
      zlink::routing_id_t::from (reinterpret_cast<const uint8_t *> ("dealer-direct"), 13);
    dealer.set_routing_id (dealer_id);

    router.bind (endpoint);
    dealer.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      router_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      dealer_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));

    zlink::message_t outbound = zlink_cpp_contract::make_message ("direct");
    dealer.send ().message (outbound).submit ();
    assert (!outbound.valid ());

    zlink::routing_id_t source =
      zlink::routing_id_t::from (reinterpret_cast<const uint8_t *> ("placeholder"), 11);
    zlink::message_t inbound;
    assert (router.recv (source, inbound) == 0);
    assert (source == dealer_id);
    assert (inbound.to_string () == "direct");
}

void test_pair_direct_recv_multipart_failure_preserves_output ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t left (ctx);
    zlink::pair_socket_t right (ctx);
    zlink::socket_monitor_t left_monitor = left.monitor_open ();
    zlink::socket_monitor_t right_monitor = right.monitor_open ();

    const std::string endpoint = zlink_cpp_contract::unique_inproc ("pair-direct-multipart-fail");
    left.bind (endpoint);
    right.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      left_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      right_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));

    zlink::message_t first = zlink_cpp_contract::make_message ("first");
    zlink::message_t second = zlink_cpp_contract::make_message ("second");
    right.send ().message (first).message (second).submit ();

    zlink::message_t inbound = zlink_cpp_contract::make_message ("keep");
    const int rc = left.recv (inbound);
    assert (rc == -1);
    assert (errno == EMSGSIZE);
    assert (inbound.valid ());
    assert (inbound.to_string () == "keep");
}

void test_router_recv_single_part_direct ()
{
    zlink::context_t ctx;
    zlink::router_socket_t router (ctx);
    zlink::dealer_socket_t dealer (ctx);
    zlink::socket_monitor_t router_monitor = router.monitor_open ();
    zlink::socket_monitor_t dealer_monitor = dealer.monitor_open ();

    const std::string endpoint = zlink_cpp_contract::unique_inproc ("router-direct");
    const zlink::routing_id_t dealer_id =
      zlink::routing_id_t::from (reinterpret_cast<const uint8_t *> ("dealer-a"), 8);
    dealer.set_routing_id (dealer_id);

    router.bind (endpoint);
    dealer.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      router_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      dealer_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));

    zlink::message_t outbound = zlink_cpp_contract::make_message ("routed");
    dealer.send ().message (outbound).submit ();

    zlink::routing_id_t source =
      zlink::routing_id_t::from (reinterpret_cast<const uint8_t *> ("placeholder"), 11);
    zlink::message_t inbound;
    assert (router.recv (source, inbound) == 0);
    assert (source == dealer_id);
    assert (inbound.to_string () == "routed");
}

void test_router_send_builder_owns_target_rid ()
{
    zlink::context_t ctx;
    zlink::router_socket_t router (ctx);
    zlink::dealer_socket_t dealer_a (ctx);
    zlink::dealer_socket_t dealer_b (ctx);
    zlink::socket_monitor_t router_monitor = router.monitor_open ();
    zlink::socket_monitor_t dealer_a_monitor = dealer_a.monitor_open ();
    zlink::socket_monitor_t dealer_b_monitor = dealer_b.monitor_open ();

    const std::string endpoint = zlink_cpp_contract::unique_inproc ("router-send-owned-rid");
    const zlink::routing_id_t dealer_a_id =
      zlink::routing_id_t::from (reinterpret_cast<const uint8_t *> ("dealer-a-owned"), 14);
    const zlink::routing_id_t dealer_b_id =
      zlink::routing_id_t::from (reinterpret_cast<const uint8_t *> ("dealer-b-owned"), 14);
    dealer_a.set_routing_id (dealer_a_id);
    dealer_b.set_routing_id (dealer_b_id);

    router.bind (endpoint);
    dealer_a.connect (endpoint);
    dealer_b.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      router_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      router_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      dealer_a_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      dealer_b_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));

    zlink::message_t hello_a = zlink_cpp_contract::make_message ("hello-a");
    dealer_a.send ().message (hello_a).submit ();
    zlink::routing_id_t source =
      zlink::routing_id_t::from (reinterpret_cast<const uint8_t *> ("placeholder"), 11);
    zlink::message_t inbound;
    assert (router.recv (source, inbound) == 0);
    assert (source == dealer_a_id);

    zlink::message_t hello_b = zlink_cpp_contract::make_message ("hello-b");
    dealer_b.send ().message (hello_b).submit ();
    assert (router.recv (source, inbound) == 0);
    assert (source == dealer_b_id);

    zlink::routing_id_t target = dealer_a_id;
    zlink::message_t outbound = zlink_cpp_contract::make_message ("owned-target");
    auto pending = router.send (target).message (outbound);
    target = dealer_b_id;

    std::move (pending).submit ();

    zlink::message_t routed_to_a;
    assert (dealer_a.recv (routed_to_a) == 0);
    assert (routed_to_a.to_string () == "owned-target");

    zlink::message_t routed_to_b;
    const int dealer_b_rc = dealer_b.recv (routed_to_b, zlink::recv_flags_t::dontwait);
    assert (dealer_b_rc == static_cast<int> (zlink::recv_result_t::no_data) || dealer_b_rc == -1);
    if (dealer_b_rc == -1)
        assert (errno == EAGAIN || errno == EWOULDBLOCK);
}

void test_router_recv_received_single_part_large ()
{
    zlink::context_t ctx;
    zlink::router_socket_t router (ctx);
    zlink::dealer_socket_t dealer (ctx);
    zlink::socket_monitor_t router_monitor = router.monitor_open ();
    zlink::socket_monitor_t dealer_monitor = dealer.monitor_open ();

    const std::string endpoint = zlink_cpp_contract::unique_inproc ("router-received-large");
    const zlink::routing_id_t dealer_id =
      zlink::routing_id_t::from (reinterpret_cast<const uint8_t *> ("dealer-large"), 12);
    dealer.set_routing_id (dealer_id);

    router.bind (endpoint);
    dealer.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      router_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      dealer_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));

    const size_t payload_size = 128u * 1024u;
    zlink::message_t outbound (payload_size);
    assert (outbound.valid ());
    std::memset (outbound.data (), 0x7b, payload_size);
    dealer.send ().message (outbound).submit ();

    zlink::received_t inbound;
    assert (router.recv (inbound) == 0);
    assert (inbound.routing_id ().has_value ());
    assert (*inbound.routing_id () == dealer_id);
    assert (inbound.is_single_part ());

    zlink::message_t &part = inbound.first_part ();
    assert (part.valid ());
    assert (part.size () == payload_size);
    const std::span<const std::byte> bytes = part.bytes ();
    assert (std::to_integer<unsigned char> (bytes[0]) == 0x7b);
    assert (std::to_integer<unsigned char> (bytes[payload_size - 1]) == 0x7b);

    inbound.send ().message (part).submit ();

    zlink::message_t echoed;
    assert (dealer.recv (echoed) == 0);
    assert (echoed.valid ());
    assert (echoed.size () == payload_size);
}

void test_router_recv_received_multipart ()
{
    zlink::context_t ctx;
    zlink::router_socket_t router (ctx);
    zlink::dealer_socket_t dealer (ctx);
    zlink::socket_monitor_t router_monitor = router.monitor_open ();
    zlink::socket_monitor_t dealer_monitor = dealer.monitor_open ();

    const std::string endpoint = zlink_cpp_contract::unique_inproc ("router-received-multipart");
    const zlink::routing_id_t dealer_id =
      zlink::routing_id_t::from (reinterpret_cast<const uint8_t *> ("dealer-multi"), 12);
    dealer.set_routing_id (dealer_id);

    router.bind (endpoint);
    dealer.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      router_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      dealer_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));

    zlink::message_t first = zlink_cpp_contract::make_message ("one");
    zlink::message_t second = zlink_cpp_contract::make_message ("two");
    dealer.send ().message (first).message (second).submit ();

    zlink::received_t inbound;
    assert (router.recv (inbound) == 0);
    assert (inbound.routing_id ().has_value ());
    assert (*inbound.routing_id () == dealer_id);
    assert (!inbound.is_single_part ());
    assert (inbound.parts ().size () == 2);
    assert (inbound.parts ()[0].to_string () == "one");
    assert (inbound.parts ()[1].to_string () == "two");

    const size_t reusable_capacity = inbound.parts ().capacity ();
    std::vector<zlink::message_t> next_outbound;
    next_outbound.push_back (zlink_cpp_contract::make_message ("three"));
    next_outbound.push_back (zlink_cpp_contract::make_message ("four"));
    dealer.send ().message (next_outbound[0]).message (next_outbound[1]).submit ();

    assert (router.recv (inbound) == 0);
    assert (inbound.parts ().capacity () == reusable_capacity);
    assert (inbound.routing_id ().has_value ());
    assert (*inbound.routing_id () == dealer_id);
    assert (inbound.parts ().size () == 2);
    assert (inbound.parts ()[0].to_string () == "three");
    assert (inbound.parts ()[1].to_string () == "four");

    const int no_data_rc = router.recv (inbound, zlink::recv_flags_t::dontwait);
    assert (no_data_rc == static_cast<int> (zlink::recv_result_t::no_data)
            || no_data_rc == -1);
    assert (!inbound.routing_id ().has_value ());
    assert (inbound.parts ().empty ());
    assert (inbound.parts ().capacity () == reusable_capacity);
}

void test_router_direct_recv_no_data_preserves_output ()
{
    zlink::context_t ctx;
    zlink::router_socket_t router (ctx);

    const zlink::routing_id_t placeholder =
      zlink::routing_id_t::from (reinterpret_cast<const uint8_t *> ("placeholder"), 11);
    zlink::routing_id_t source = placeholder;
    zlink::message_t existing = zlink_cpp_contract::make_message ("keep");

    const int rc = router.recv (source, existing, zlink::recv_flags_t::dontwait);
    assert (rc == static_cast<int> (zlink::recv_result_t::no_data) || rc == -1);
    if (rc == -1)
        assert (errno == EAGAIN || errno == EWOULDBLOCK);
    assert (source == placeholder);
    assert (existing.valid ());
    assert (existing.to_string () == "keep");

    zlink::message_t invalid;
    invalid.close ();
    const int invalid_rc = router.recv (source, invalid, zlink::recv_flags_t::dontwait);
    assert (invalid_rc == static_cast<int> (zlink::recv_result_t::no_data) || invalid_rc == -1);
    if (invalid_rc == -1)
        assert (errno == EAGAIN || errno == EWOULDBLOCK);
    assert (source == placeholder);
    assert (!invalid.valid ());
}

void test_router_direct_recv_multipart_failure_preserves_output ()
{
    zlink::context_t ctx;
    zlink::router_socket_t router (ctx);
    zlink::dealer_socket_t dealer (ctx);
    zlink::socket_monitor_t router_monitor = router.monitor_open ();
    zlink::socket_monitor_t dealer_monitor = dealer.monitor_open ();

    const std::string endpoint = zlink_cpp_contract::unique_inproc ("router-direct-multipart-fail");
    const zlink::routing_id_t dealer_id =
      zlink::routing_id_t::from (reinterpret_cast<const uint8_t *> ("dealer-b"), 8);
    dealer.set_routing_id (dealer_id);

    router.bind (endpoint);
    dealer.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      router_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      dealer_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));

    zlink::message_t first = zlink_cpp_contract::make_message ("first");
    zlink::message_t second = zlink_cpp_contract::make_message ("second");
    dealer.send ().message (first).message (second).submit ();

    const zlink::routing_id_t placeholder =
      zlink::routing_id_t::from (reinterpret_cast<const uint8_t *> ("placeholder"), 11);
    zlink::routing_id_t source = placeholder;
    zlink::message_t inbound = zlink_cpp_contract::make_message ("keep");
    const int rc = router.recv (source, inbound);
    assert (rc == -1);
    assert (errno == EMSGSIZE);
    assert (source == placeholder);
    assert (inbound.valid ());
    assert (inbound.to_string () == "keep");
}

void test_pair_send_recv_multipart ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t left (ctx);
    zlink::pair_socket_t right (ctx);
    zlink::socket_monitor_t left_monitor = left.monitor_open ();
    zlink::socket_monitor_t right_monitor = right.monitor_open ();

    const std::string endpoint = zlink_cpp_contract::unique_inproc ("pair-multipart");
    left.bind (endpoint);
    right.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      left_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      right_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));

    std::vector<zlink::message_t> outbound;
    outbound.push_back (zlink_cpp_contract::make_message ("one"));
    outbound.push_back (zlink_cpp_contract::make_message ("two"));
    right.send ().message (outbound[0]).message (outbound[1]).submit ();

    zlink::received_t inbound;
    assert (left.recv (inbound) == 0);
    assert (inbound.parts ().size () == 2);
    assert (inbound.parts ()[0].to_string () == "one");
    assert (inbound.parts ()[1].to_string () == "two");
}

void test_pair_multipart_invalid_part_returns_lvalues ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t socket (ctx);

    zlink::message_t first = zlink_cpp_contract::make_message ("first");
    zlink::message_t invalid;
    invalid.close ();
    zlink::message_t third = zlink_cpp_contract::make_message ("third");

    bool rejected = false;
    try {
        socket.send ().message (first).message (invalid).message (third).submit ();
    }
    catch (const zlink::binding_error_t &) {
        rejected = true;
    }

    // Multipart staging owns the record until Core accepts every part. An
    // invalid middle part must not consume either caller-owned neighbor.
    assert (rejected);
    assert (first.valid ());
    assert (first.to_string () == "first");
    assert (!invalid.valid ());
    assert (third.valid ());
    assert (third.to_string () == "third");
}

void test_concurrent_pair_multipart_exposes_core_rejection_and_returns_lvalues ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t receiver (ctx);
    zlink::pair_socket_t sender (ctx);
    zlink::socket_monitor_t receiver_monitor = receiver.monitor_open ();
    zlink::socket_monitor_t sender_monitor = sender.monitor_open ();

    const std::string endpoint =
      zlink_cpp_contract::unique_inproc ("pair-concurrent-multipart");
    receiver.bind (endpoint);
    sender.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      receiver_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      sender_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));

    constexpr int k_sender_count = 8;
    constexpr int k_attempts_per_sender = 2000;
    std::barrier start_line (k_sender_count);
    std::atomic<int> accepted{0};
    std::atomic<int> rejected{0};
    std::atomic<int> ownership_failures{0};
    std::atomic<int> unexpected_results{0};

    std::vector<std::thread> senders;
    senders.reserve (k_sender_count);
    for (int thread_id = 0; thread_id < k_sender_count; ++thread_id) {
        senders.emplace_back ([&, thread_id] {
            start_line.arrive_and_wait ();
            for (int attempt = 0; attempt < k_attempts_per_sender; ++attempt) {
                const std::string record = std::to_string (thread_id) + ":"
                                           + std::to_string (attempt);
                const std::string first_text = record + ":0";
                const std::string second_text = record + ":1";
                const std::string third_text = record + ":2";
                zlink::message_t first = zlink_cpp_contract::make_message (first_text);
                zlink::message_t second = zlink_cpp_contract::make_message (second_text);
                zlink::message_t third = zlink_cpp_contract::make_message (third_text);

                try {
                    sender.send ().message (first).message (second).message (third).submit ();
                    accepted.fetch_add (1, std::memory_order_relaxed);
                    if (first.valid () || second.valid () || third.valid ())
                        ownership_failures.fetch_add (1, std::memory_order_relaxed);
                }
                catch (const zlink::submit_error_t &error) {
                    if (error.result () != zlink::submit_result_t::invalid_argument
                        || error.internal_errno () != EINVAL) {
                        unexpected_results.fetch_add (1, std::memory_order_relaxed);
                        continue;
                    }
                    rejected.fetch_add (1, std::memory_order_relaxed);
                    if (!first.valid () || first.to_string () != first_text
                        || !second.valid () || second.to_string () != second_text
                        || !third.valid () || third.to_string () != third_text)
                        ownership_failures.fetch_add (1, std::memory_order_relaxed);
                }
                catch (...) {
                    unexpected_results.fetch_add (1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread &thread : senders)
        thread.join ();

    const int accepted_count = accepted.load (std::memory_order_relaxed);
    const int rejected_count = rejected.load (std::memory_order_relaxed);
    assert (accepted_count + rejected_count == k_sender_count * k_attempts_per_sender);
    assert (rejected_count > 0);
    assert (ownership_failures.load (std::memory_order_relaxed) == 0);
    assert (unexpected_results.load (std::memory_order_relaxed) == 0);

    int received_count = 0;
    while (received_count < accepted_count) {
        zlink::received_t inbound;
        assert (receiver.recv (inbound) == 0);
        assert (inbound.parts ().size () == 3);
        const std::string first_text = inbound.parts ()[0].to_string ();
        assert (first_text.size () > 2);
        assert (first_text.ends_with (":0"));
        const std::string record = first_text.substr (0, first_text.size () - 2);
        assert (inbound.parts ()[1].to_string () == record + ":1");
        assert (inbound.parts ()[2].to_string () == record + ":2");
        ++received_count;
    }
}

void test_publisher_synchronous_multipart ()
{
    zlink::context_t ctx;
    zlink::xpub_socket_t publisher (ctx);
    zlink::sub_socket_t subscriber (ctx);
    zlink::socket_monitor_t pub_monitor = publisher.monitor_open ();
    zlink::socket_monitor_t sub_monitor = subscriber.monitor_open ();

    const std::string endpoint = zlink_cpp_contract::unique_inproc ("pubsub-multipart");
    publisher.bind (endpoint);
    subscriber.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      pub_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      sub_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));

    const std::string topic = "topic:multipart";
    subscriber.set_subscription (topic);

    zlink::subscription_event_t event;
    assert (publisher.receive_subscription_event (event)
            == static_cast<int> (zlink::recv_result_t::ok));
    assert (event.subscribed);
    assert (event.topic == topic);

    std::vector<zlink::message_t> outbound;
    outbound.push_back (zlink_cpp_contract::make_message ("alpha"));
    outbound.push_back (zlink_cpp_contract::make_message ("beta"));
    publisher.publish (topic).message (outbound[0]).message (outbound[1]).submit ();

    zlink::topic_message_t inbound;
    assert (subscriber.subscribe (inbound)
            == static_cast<int> (zlink::recv_result_t::ok));
    assert (inbound.topic () == topic);
    assert (inbound.parts ().size () == 2);
    assert (inbound.parts ()[0].to_string () == "alpha");
    assert (inbound.parts ()[1].to_string () == "beta");

}

void test_pair_ipc_large_message_shutdown ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t left (ctx);
    zlink::pair_socket_t right (ctx);
    zlink::socket_monitor_t left_monitor = left.monitor_open ();
    zlink::socket_monitor_t right_monitor = right.monitor_open ();

    const std::string endpoint = zlink_cpp_contract::unique_ipc ("pair-large-shutdown");
    left.bind (endpoint);
    right.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      left_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      right_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));

    const size_t payload_size = 262144;
    zlink::message_t outbound (payload_size);
    assert (outbound.valid ());
    std::memset (outbound.data (), 0x5a, payload_size);
    right.send ().message (outbound).submit ();

    zlink::received_t inbound;
    assert (left.recv (inbound) == 0);
    assert (inbound.parts ().size () == 1);
    assert (inbound.parts ()[0].size () == payload_size);
}

} // namespace

int main ()
{
    test_pair_send_recv_single_part ();
    test_pair_send_recv_single_part_direct ();
    test_pair_direct_recv_no_data_preserves_output ();
    test_dealer_unified_send_awaitable_builder ();
    test_pair_direct_recv_multipart_failure_preserves_output ();
    test_router_recv_single_part_direct ();
    test_router_send_builder_owns_target_rid ();
    test_router_recv_received_single_part_large ();
    test_router_recv_received_multipart ();
    test_router_direct_recv_no_data_preserves_output ();
    test_router_direct_recv_multipart_failure_preserves_output ();
    test_pair_send_recv_multipart ();
    test_pair_multipart_invalid_part_returns_lvalues ();
    test_concurrent_pair_multipart_exposes_core_rejection_and_returns_lvalues ();
    test_publisher_synchronous_multipart ();
#if !defined(_WIN32)
    test_pair_ipc_large_message_shutdown ();
#endif
    return 0;
}

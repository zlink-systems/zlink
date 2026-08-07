/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"

#include <cerrno>
#include <climits>
#include <cstring>
#include <optional>
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
    assert (right.send ().message (outbound).submit ());

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
    assert (right.send ().message (outbound).submit ());

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

void test_dealer_send_dontwait_builder ()
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
    const bool sent =
      dealer.send ().message (outbound).flags (zlink::send_flags_t::dontwait).submit ();
    assert (sent);
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
    assert (right.send ().message (first).message (second).submit ());

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
    assert (dealer.send ().message (outbound).submit ());

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
    assert (dealer_a.send ().message (hello_a).submit ());
    zlink::routing_id_t source =
      zlink::routing_id_t::from (reinterpret_cast<const uint8_t *> ("placeholder"), 11);
    zlink::message_t inbound;
    assert (router.recv (source, inbound) == 0);
    assert (source == dealer_a_id);

    zlink::message_t hello_b = zlink_cpp_contract::make_message ("hello-b");
    assert (dealer_b.send ().message (hello_b).submit ());
    assert (router.recv (source, inbound) == 0);
    assert (source == dealer_b_id);

    zlink::routing_id_t target = dealer_a_id;
    zlink::message_t outbound = zlink_cpp_contract::make_message ("owned-target");
    auto pending = router.send (target).message (outbound);
    target = dealer_b_id;

    assert (std::move (pending).submit ());

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
    assert (dealer.send ().message (outbound).submit ());

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

    assert (inbound.send ().message (part).submit ());

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
    assert (dealer.send ().message (first).message (second).submit ());

    zlink::received_t inbound;
    assert (router.recv (inbound) == 0);
    assert (inbound.routing_id ().has_value ());
    assert (*inbound.routing_id () == dealer_id);
    assert (!inbound.is_single_part ());
    assert (inbound.parts ().size () == 2);
    assert (inbound.parts ()[0].to_string () == "one");
    assert (inbound.parts ()[1].to_string () == "two");
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
    assert (dealer.send ().message (first).message (second).submit ());

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
    assert (right.send ().message (outbound[0]).message (outbound[1]).submit ());

    zlink::received_t inbound;
    assert (left.recv (inbound) == 0);
    assert (inbound.parts ().size () == 2);
    assert (inbound.parts ()[0].to_string () == "one");
    assert (inbound.parts ()[1].to_string () == "two");
}

void test_pubsub_subscribe_multipart ()
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
    assert (publisher.publish (topic).message (outbound[0]).message (outbound[1]).submit ());

    zlink::topic_message_t inbound;
    assert (subscriber.subscribe (inbound) == static_cast<int> (zlink::recv_result_t::ok));
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
    assert (right.send ().message (outbound).submit ());

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
    test_dealer_send_dontwait_builder ();
    test_pair_direct_recv_multipart_failure_preserves_output ();
    test_router_recv_single_part_direct ();
    test_router_send_builder_owns_target_rid ();
    test_router_recv_received_single_part_large ();
    test_router_recv_received_multipart ();
    test_router_direct_recv_no_data_preserves_output ();
    test_router_direct_recv_multipart_failure_preserves_output ();
    test_pair_send_recv_multipart ();
    test_pubsub_subscribe_multipart ();
#if !defined(_WIN32)
    test_pair_ipc_large_message_shutdown ();
#endif
    return 0;
}

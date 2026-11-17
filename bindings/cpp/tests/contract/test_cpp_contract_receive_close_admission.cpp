/* SPDX-License-Identifier: MPL-2.0 */
#include "support.hpp"

#include <atomic>
#include <thread>

namespace
{
struct boundary_t
{
    std::atomic<bool> entered{false};
    std::atomic<bool> released{false};
    zlink_recv_result_t result = ZLINK_RECV_OK;
    zlink_routing_id_t rid{};

    boundary_t ()
    {
        rid.size = 3;
        rid.data[0] = 4;
        rid.data[1] = 5;
        rid.data[2] = 6;
    }

    zlink_recv_result_t finish ()
    {
        entered.store (true);
        entered.notify_one ();
        released.wait (false);
        errno = result == ZLINK_RECV_NO_DATA ? EAGAIN : EINTR;
        return result;
    }
};
boundary_t *active = nullptr;

template <class Socket, class Receive>
void check (Socket &socket_, Receive receive_, bool closed_only_)
{
    if (!closed_only_) {
        for (auto result : {ZLINK_RECV_OK, ZLINK_RECV_NO_DATA, ZLINK_RECV_INTERNAL_ERROR}) {
            boundary_t boundary;
            boundary.result = result;
            active = &boundary;
            std::thread receiver ([&] {
                assert (receive_ (zlink::recv_flags_t::dontwait) == result);
                if (result != ZLINK_RECV_OK)
                    assert (errno == (result == ZLINK_RECV_NO_DATA ? EAGAIN : EINTR));
            });
            boundary.entered.wait (false);
            bool busy = false;
            try {
                socket_.close ();
            }
            catch (const zlink::close_error_t &error) {
                assert (error.result () == zlink::close_result_t::busy);
                assert (error.internal_errno () == EBUSY);
                busy = true;
            }
            boundary.released.store (true);
            boundary.released.notify_one ();
            receiver.join ();
            active = nullptr;
            assert (busy);
            assert (socket_.valid ());
        }
    }
    socket_.close ();
    for (auto flags : {zlink::recv_flags_t::none, zlink::recv_flags_t::dontwait}) {
        errno = 0;
        const int result = receive_ (flags);
        assert (result == ZLINK_RECV_INVALID_STATE);
        assert (errno == ESHUTDOWN);
    }
}
} // namespace

extern "C" zlink_recv_result_t __real_zlink_recv (
  void *, const zlink_routing_id_t **, zlink_msg_t *, size_t, size_t *, zlink_recv_flags_t);
extern "C" zlink_recv_result_t __wrap_zlink_recv (void *socket_,
                                                  const zlink_routing_id_t **rid_,
                                                  zlink_msg_t *parts_,
                                                  size_t capacity_,
                                                  size_t *count_,
                                                  zlink_recv_flags_t flags_)
{
    if (!active)
        return __real_zlink_recv (socket_, rid_, parts_, capacity_, count_, flags_);
    if (active->result == ZLINK_RECV_OK) {
        assert (capacity_ >= 1);
        *rid_ = &active->rid;
        *count_ = 1;
        assert (zlink_msg_init_size (parts_, 2) == 0);
        auto *data = static_cast<unsigned char *> (zlink_msg_data (parts_));
        data[0] = 1;
        data[1] = 'a';
    }
    return active->finish ();
}

extern "C" zlink_recv_result_t __real_zlink_router_recv (void *,
                                                         const zlink_routing_id_t **,
                                                         zlink_reply_token_t *,
                                                         zlink_msg_t *,
                                                         size_t,
                                                         size_t *,
                                                         zlink_recv_flags_t);
extern "C" zlink_recv_result_t __wrap_zlink_router_recv (void *socket_,
                                                         const zlink_routing_id_t **rid_,
                                                         zlink_reply_token_t *token_,
                                                         zlink_msg_t *parts_,
                                                         size_t capacity_,
                                                         size_t *count_,
                                                         zlink_recv_flags_t flags_)
{
    if (!active)
        return __real_zlink_router_recv (socket_, rid_, token_, parts_, capacity_, count_, flags_);
    *token_ = 0;
    return __wrap_zlink_recv (socket_, rid_, parts_, capacity_, count_, flags_);
}

extern "C" zlink_recv_result_t __real_zlink_subscribe (void *,
                                                       const zlink_routing_id_t **,
                                                       char *,
                                                       size_t,
                                                       size_t *,
                                                       zlink_msg_t *,
                                                       size_t,
                                                       size_t *,
                                                       zlink_recv_flags_t);
extern "C" zlink_recv_result_t __wrap_zlink_subscribe (void *socket_,
                                                       const zlink_routing_id_t **rid_,
                                                       char *topic_,
                                                       size_t topic_capacity_,
                                                       size_t *topic_size_,
                                                       zlink_msg_t *parts_,
                                                       size_t capacity_,
                                                       size_t *count_,
                                                       zlink_recv_flags_t flags_)
{
    if (!active)
        return __real_zlink_subscribe (socket_, rid_, topic_, topic_capacity_, topic_size_, parts_,
                                       capacity_, count_, flags_);
    assert (topic_capacity_ >= 1);
    topic_[0] = 'a';
    *topic_size_ = 1;
    return __wrap_zlink_recv (socket_, rid_, parts_, capacity_, count_, flags_);
}

extern "C" zlink_recv_result_t __real_zlink_stream_recv_packet (
  void *, const zlink_routing_id_t **, zlink_msg_t *, zlink_msg_t *, zlink_recv_flags_t);
extern "C" zlink_recv_result_t __wrap_zlink_stream_recv_packet (void *socket_,
                                                                const zlink_routing_id_t **rid_,
                                                                zlink_msg_t *header_,
                                                                zlink_msg_t *body_,
                                                                zlink_recv_flags_t flags_)
{
    if (!active)
        return __real_zlink_stream_recv_packet (socket_, rid_, header_, body_, flags_);
    // The binding already supplies initialized empty messages.
    *rid_ = &active->rid;
    return active->finish ();
}

extern "C" zlink_recv_result_t __real_zlink_xpub_recv (
  void *, const zlink_routing_id_t **, int *, char *, size_t, size_t *, zlink_recv_flags_t);
extern "C" zlink_recv_result_t __wrap_zlink_xpub_recv (void *socket_,
                                                       const zlink_routing_id_t **rid_,
                                                       int *subscribed_,
                                                       char *topic_,
                                                       size_t capacity_,
                                                       size_t *size_,
                                                       zlink_recv_flags_t flags_)
{
    if (!active)
        return __real_zlink_xpub_recv (socket_, rid_, subscribed_, topic_, capacity_, size_,
                                       flags_);
    assert (capacity_ >= 1);
    *rid_ = &active->rid;
    *subscribed_ = 1;
    topic_[0] = 'a';
    *size_ = 1;
    return active->finish ();
}

int main (int argc, char **)
{
    const bool closed_only = argc > 1;
    zlink::context_t context;
    zlink::dealer_socket_t dealer (context);
    zlink::router_socket_t router (context);
    zlink::sub_socket_t subscriber (context);
    zlink::xpub_socket_t publisher (context);
    zlink::xpub_socket_t event_publisher (context);
    zlink::sub_socket_t part_subscriber (context);
    zlink::stream_socket_t stream (context);
    zlink::received_t received;
    check (dealer, [&] (auto flags) { return dealer.recv (received, flags); }, closed_only);
    check (router, [&] (auto flags) { return router.recv (received, flags); }, closed_only);
    zlink::topic_message_t message;
    check (
      subscriber, [&] (auto flags) { return subscriber.subscribe (message, flags); }, closed_only);
    zlink::subscription_event_t event;
    check (
      publisher, [&] (auto flags) { return publisher.subscription_event (event, flags); },
      closed_only);
    check (
      event_publisher,
      [&] (auto flags) {
          const int result = event_publisher.receive_subscription_event (event, flags);
          if (result == ZLINK_RECV_OK) {
              assert (event.routing_id.has_value ());
              assert (event.routing_id->size () == 3);
              assert (std::memcmp (event.routing_id->data (), "\4\5\6", 3) == 0);
              assert (event.topic == "a" && event.subscribed);
          }
          return result;
      },
      closed_only);
    std::optional<zlink::routing_id_t> rid;
    std::string topic;
    zlink::message_t part;
    bool more = false;
    check (
      part_subscriber,
      [&] (auto flags) { return part_subscriber.subscribe_part (rid, topic, part, more, flags); },
      closed_only);
    zlink::stream_packet_t packet;
    check (
      stream,
      [&] (auto flags) {
          try {
              const bool success = stream.recv_packet (packet, flags);
              if (success) {
                  assert (packet.routing_id ().has_value ());
                  assert (packet.routing_id ()->size () == 3);
                  assert (std::memcmp (packet.routing_id ()->data (), "\4\5\6", 3) == 0);
                  return static_cast<int> (ZLINK_RECV_OK);
              }
              return static_cast<int> (ZLINK_RECV_NO_DATA);
          }
          catch (const zlink::recv_error_t &error) {
              assert (
                error.internal_errno ()
                == (error.result () == zlink::recv_result_t::invalid_state ? ESHUTDOWN : EINTR));
              return static_cast<int> (error.result ());
          }
      },
      closed_only);
    if (!closed_only) {
        zlink::stream_socket_t invalid_packet_stream (context);
        boundary_t boundary;
        boundary.rid.size = 0;
        boundary.released.store (true);
        active = &boundary;
        bool rejected = false;
        try {
            (void) invalid_packet_stream.recv_packet (packet, zlink::recv_flags_t::dontwait);
        }
        catch (const zlink::recv_error_t &error) {
            assert (error.result () == zlink::recv_result_t::internal_error);
            assert (error.internal_errno () == EPROTO);
            rejected = true;
        }
        active = nullptr;
        assert (rejected);
        invalid_packet_stream.close ();
        assert (packet.empty ());
    }
}

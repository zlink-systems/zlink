/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>

#include "runtime/channels/channel_runtime.hpp"
#include "runtime/channels/channel_socket_options.hpp"

#include <zlink/Contracts/Core/byte_count.hpp>
#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Errors/errors.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/Contracts/Messaging/subscription_event.hpp>
#include <zlink/Contracts/Messaging/topic_message.hpp>
#include <zlink/Contracts/Sockets/pubsub_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/results.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>

namespace
{

using namespace std::chrono_literals;

const std::string large_filler (65'536, 'f');
const std::string large_record (65'536, 'r');
// One record (topic frame + payload frame) fills the pipe exactly, so the next
// publish is refused at its first frame and enters the send-timeout wait
// instead of Core's multipart-abort path.
const std::uint64_t record_hwm =
  large_filler.size () + std::string ("fanout.no-drop").size ();

std::string unique_endpoint ()
{
    static std::atomic_uint64_t next{0};
    return "inproc://framework-fanout-no-drop-"
           + std::to_string (next.fetch_add (1, std::memory_order_relaxed));
}

class binding_fanout_pair_t
{
  public:
    explicit binding_fanout_pair_t (bool no_drop)
    {
        context.options ().auto_hwm_enabled (false);
        publisher = std::make_unique<zlink::xpub_socket_t> (context);
        fast = std::make_unique<zlink::sub_socket_t> (context);
        slow = std::make_unique<zlink::sub_socket_t> (context);

        if (no_drop) {
            zlink::framework::detail::apply_fanout_publisher_socket_options (
              *publisher, true);
        }
        publisher->options ().linger (0ms);
        publisher->options ().send_hwm (
          zlink::byte_count_t::bytes (record_hwm));
        publisher->options ().verbose (true);
        publisher->options ().recv_timeout (5s);
        fast->options ().linger (0ms);
        fast->options ().recv_hwm (
          zlink::byte_count_t::bytes (record_hwm));
        slow->options ().linger (0ms);
        slow->options ().recv_hwm (
          zlink::byte_count_t::bytes (record_hwm));
        fast->options ().recv_timeout (5s);
        slow->options ().recv_timeout (5s);

        publisher->bind (endpoint);
        fast->connect (endpoint);
        slow->connect (endpoint);
        fast->set_subscription (topic);
        slow->set_subscription (topic);

        for (int index = 0; index != 2; ++index) {
            zlink::subscription_event_t event;
            EXPECT_EQ (publisher->receive_subscription_event (event),
                       static_cast<int> (zlink::recv_result_t::ok));
            EXPECT_TRUE (event.subscribed);
            EXPECT_EQ (event.topic, topic);
        }
    }

    bool try_publish (const std::string &payload)
    {
        auto message = zlink::message_t::from (payload);
        return publisher->publish (topic)
          .message (message)
          .flags (static_cast<int> (zlink::send_flags_t::dontwait))
          .submit ();
    }

    void publish (const std::string &payload)
    {
        auto message = zlink::message_t::from (payload);
        publisher->publish (topic).message (message).submit ();
    }

    std::string receive (zlink::sub_socket_t &subscriber)
    {
        zlink::topic_message_t message;
        EXPECT_EQ (subscriber.subscribe (message),
                   static_cast<int> (zlink::recv_result_t::ok));
        EXPECT_EQ (message.topic (), topic);
        EXPECT_EQ (message.parts ().size (), 1u);
        return message.parts ().empty () ? std::string ()
                                         : message.parts ()[0].to_string ();
    }

    bool try_receive (zlink::sub_socket_t &subscriber,
                      std::string *payload = nullptr)
    {
        zlink::topic_message_t message;
        const auto result = subscriber.subscribe (
          message, zlink::recv_flags_t::dontwait);
        if (result == static_cast<int> (zlink::recv_result_t::no_data)) {
            return false;
        }
        EXPECT_EQ (result, static_cast<int> (zlink::recv_result_t::ok));
        if (result != static_cast<int> (zlink::recv_result_t::ok)) {
            return false;
        }
        EXPECT_EQ (message.topic (), topic);
        EXPECT_EQ (message.parts ().size (), 1u);
        if (payload != nullptr && !message.parts ().empty ()) {
            *payload = message.parts ()[0].to_string ();
        }
        return true;
    }

    void fill_slow_pipe ()
    {
        for (int attempt = 0; attempt != 64; ++attempt) {
            if (!try_publish (large_filler)) {
                return;
            }
            EXPECT_EQ (receive (*fast), large_filler);
        }
        FAIL () << "publisher pipe did not reach its configured HWM";
    }

    zlink::context_t context;
    std::unique_ptr<zlink::xpub_socket_t> publisher;
    std::unique_ptr<zlink::sub_socket_t> fast;
    std::unique_ptr<zlink::sub_socket_t> slow;
    const std::string endpoint = unique_endpoint ();
    const std::string topic = "fanout.no-drop";
};

struct fanout_record_t
{
    static constexpr const char *packet_name = "fanout.no-drop.record";
    std::string payload;
};

struct framework_options_fixture_t
{
    framework_options_fixture_t () : options (services, handlers, serializers, zlink)
    {
        serializers.add<fanout_record_t> (
          [] (const fanout_record_t &value) {
              return zlink::framework::encoded_payload_t::from_string (
                value.payload);
          },
          [] (const zlink::framework::encoded_payload_t &payload) {
              return fanout_record_t{payload.to_string ()};
          },
          "application/x-zlink-fanout-no-drop");
    }

    zlink::framework::service_collection_t services;
    zlink::framework::handler_registry_t handlers;
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t zlink;
    zlink::framework::zlink_framework_options_t options;
};

TEST (cpp_framework_fanout_no_drop,
      default_drops_only_slow_subscriber_share_after_hwm)
{
    binding_fanout_pair_t pair (false);
    EXPECT_FALSE (pair.publisher->options ().no_drop ());

    constexpr int publish_count = 64;
    for (int index = 0; index != publish_count; ++index) {
        ASSERT_TRUE (pair.try_publish (large_filler));
        EXPECT_EQ (pair.receive (*pair.fast), large_filler);
    }

    int slow_count = 0;
    while (pair.try_receive (*pair.slow)) {
        ++slow_count;
    }
    EXPECT_LT (slow_count, publish_count);
}

TEST (cpp_framework_fanout_no_drop,
      no_drop_waits_then_delivers_record_to_every_matching_subscriber)
{
    binding_fanout_pair_t pair (true);
    pair.publisher->options ().send_timeout (2s);
    pair.fill_slow_pipe ();

    auto pending = std::async (std::launch::async, [&] {
        pair.publish (large_record);
    });
    EXPECT_EQ (pending.wait_for (50ms), std::future_status::timeout);
    EXPECT_FALSE (pair.try_receive (*pair.fast));

    EXPECT_EQ (pair.receive (*pair.slow), large_filler);
    ASSERT_EQ (pending.wait_for (2s), std::future_status::ready);
    EXPECT_NO_THROW (pending.get ());
    EXPECT_EQ (pair.receive (*pair.fast), large_record);

    std::string slow_payload;
    do {
        ASSERT_TRUE (pair.try_receive (*pair.slow, &slow_payload));
    } while (slow_payload != large_record);
}

TEST (cpp_framework_fanout_no_drop,
      no_drop_send_timeout_maps_to_deadline_exceeded)
{
    framework_options_fixture_t fixture;
    fixture.options.add_fanout_channel ("events")
      .enable_publisher (unique_endpoint ())
      .set_no_drop ();
    fixture.options.apply ();

    auto runtime = zlink::framework::detail::channel_runtime_t::from (
      fixture.zlink.message_bus ());
    runtime.bind_serializers (fixture.serializers);
    runtime.bind_fanout_transport (
      "events", [] (std::string, std::string, std::string, zlink::message_t,
                     std::chrono::milliseconds) -> zlink::framework::task_t<void> {
          throw zlink::submit_error_t (
            zlink::submit_result_t::backpressured, ETIMEDOUT);
      });

    const auto result = fixture.zlink.publisher ()
                          .publish ("events", "fanout.no-drop",
                                    fanout_record_t{"payload"})
                          .async ().result ();

    ASSERT_FALSE (result);
    EXPECT_EQ (result.error_kind (),
               zlink::framework::framework_error_kind_t::deadline_exceeded);
}

TEST (cpp_framework_fanout_no_drop,
      subscriber_only_channel_rejects_no_drop_at_startup)
{
    framework_options_fixture_t fixture;
    fixture.options.add_fanout_channel ("events")
      .enable_subscriber ()
      .set_no_drop ();

    try {
        fixture.options.apply ();
        FAIL () << "subscriber-only fanout channel accepted NoDrop";
    }
    catch (const zlink::framework::framework_exception_t &error) {
        EXPECT_EQ (error.kind (),
                   zlink::framework::framework_error_kind_t::protocol_error);
        EXPECT_NE (std::string (error.what ()).find ("NoDrop"),
                   std::string::npos);
    }
}

TEST (cpp_framework_fanout_no_drop,
      builder_value_reaches_pub_and_xpub_socket_option)
{
    framework_options_fixture_t fixture;
    fixture.options.add_fanout_channel ("default")
      .enable_publisher (unique_endpoint ());
    fixture.options.add_fanout_channel ("no-drop")
      .enable_publisher (unique_endpoint ())
      .set_no_drop ();
    fixture.options.apply ();

    const auto snapshots =
      zlink::framework::detail::channel_runtime_t::from (
        fixture.zlink.message_bus ()).channel_snapshots ();
    const auto find_publisher = [&] (const std::string &name) {
        return std::find_if (
          snapshots.begin (), snapshots.end (),
          [&] (const auto &channel) { return channel.name == name; });
    };
    const auto default_channel = find_publisher ("default");
    const auto no_drop_channel = find_publisher ("no-drop");
    ASSERT_NE (default_channel, snapshots.end ());
    ASSERT_NE (no_drop_channel, snapshots.end ());
    EXPECT_FALSE (default_channel->publisher.no_drop);
    EXPECT_TRUE (no_drop_channel->publisher.no_drop);

    zlink::context_t context;
    zlink::pub_socket_t automatic_socket (context);
    zlink::xpub_socket_t manual_socket (context);
    zlink::framework::detail::apply_fanout_publisher_socket_options (
      automatic_socket, no_drop_channel->publisher.no_drop);
    zlink::framework::detail::apply_fanout_publisher_socket_options (
      manual_socket, no_drop_channel->publisher.no_drop);
    EXPECT_TRUE (automatic_socket.options ().no_drop ());
    EXPECT_TRUE (manual_socket.options ().no_drop ());
}

} // namespace

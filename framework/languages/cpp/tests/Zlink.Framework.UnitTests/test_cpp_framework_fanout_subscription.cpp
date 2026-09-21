/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>

#include "runtime/channels/channel_runtime.hpp"
#include "runtime/fanout/fanout_subscription.hpp"
#include "runtime/fanout/raw_fanout_owner.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

using namespace std::chrono_literals;
namespace fanout = zlink::framework::runtime::fanout;
namespace mesh = zlink::framework::runtime::mesh;

std::vector<std::uint8_t> bytes (std::string value)
{
    return {value.begin (), value.end ()};
}

std::vector<std::string> configured_subscription_topics (std::vector<std::string> topics)
{
    auto options = std::make_shared<zlink::framework::detail::framework_options_state_t> ();
    auto handler_groups =
      std::make_shared<zlink::framework::detail::handler_group_options_state_t> ();
    zlink::framework::fanout_channel_builder_t channel ("events", options, handler_groups);
    channel.enable_subscriber ();
    for (auto &topic : topics) {
        channel.subscribe (std::move (topic));
    }

    zlink::framework::zlink_builder_t zlink;
    options->keyed_zlink_actions.at ("fanout_channel:events") (zlink);
    const auto channels =
      zlink::framework::detail::channel_runtime_t::from (zlink.message_bus ()).channel_snapshots ();
    EXPECT_EQ (channels.size (), 1u);
    if (channels.size () != 1) {
        return {};
    }
    return channels.front ().subscriber.subscription_topics;
}

bool rejects_as_call_argument (const std::function<void ()> &call)
{
    try {
        call ();
    }
    catch (const zlink::framework::framework_exception_t &error) {
        return error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
    }
    return false;
}

void wait_for_beacon (fanout::raw_fanout_publisher_t &publisher,
                      fanout::raw_fanout_subscriber_t &subscriber,
                      const std::vector<std::uint8_t> &publisher_id,
                      std::chrono::steady_clock::time_point receive_now)
{
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    std::size_t beacon_tick = 1;
    while (!subscriber.ready (publisher_id) && std::chrono::steady_clock::now () < deadline) {
        (void) publisher.tick (receive_now
                               + fanout::fanout_beacon_interval * static_cast<int> (beacon_tick++));
        (void) subscriber.try_receive (receive_now);
        if (!subscriber.ready (publisher_id)) {
            std::this_thread::sleep_for (2ms);
        }
    }
    ASSERT_TRUE (subscriber.ready (publisher_id));
}

void publish (fanout::raw_fanout_publisher_t &publisher, const std::string &topic)
{
    publisher.publish ("events", topic, {"FanoutEvent", "application/json", bytes (topic)})
      .result ()
      .value ();
}

} // namespace

TEST (CppFrameworkFanoutSubscription, NoApplicationSubscriptionReceivesDifferentTopics)
{
    fanout::raw_fanout_publisher_t publisher ("tcp://127.0.0.1:0");
    publisher.start ();
    const auto publisher_id = bytes ("publisher-default");
    fanout::raw_fanout_subscriber_t subscriber (nullptr, configured_subscription_topics ({}));
    ASSERT_TRUE (subscriber.connect_manual (publisher_id, publisher.endpoint ()));
    const auto receive_now = std::chrono::steady_clock::now ();
    wait_for_beacon (publisher, subscriber, publisher_id, receive_now);

    std::set<std::string> received_topics;
    for (std::size_t attempt = 0; attempt < 100 && received_topics.size () != 2; ++attempt) {
        publish (publisher, "order.created");
        publish (publisher, "payment");
        for (std::size_t receive = 0; receive < 2; ++receive) {
            const auto [status, record] = subscriber.try_receive (receive_now);
            if (status == fanout::fanout_receive_status_t::application) {
                ASSERT_TRUE (record);
                received_topics.insert (record->topic);
            }
        }
        if (received_topics.size () != 2) {
            std::this_thread::sleep_for (2ms);
        }
    }

    EXPECT_EQ (received_topics, (std::set<std::string>{"order.created", "payment"}));
}

TEST (CppFrameworkFanoutSubscription, PrefixSubscriptionReceivesMatchingTopicAndRejectsOtherTopic)
{
    fanout::raw_fanout_publisher_t publisher ("tcp://127.0.0.1:0");
    publisher.start ();
    const auto publisher_id = bytes ("publisher-prefix");
    fanout::raw_fanout_subscriber_t subscriber (nullptr,
                                                configured_subscription_topics ({"order"}));
    ASSERT_TRUE (subscriber.connect_manual (publisher_id, publisher.endpoint ()));
    const auto receive_now = std::chrono::steady_clock::now ();
    wait_for_beacon (publisher, subscriber, publisher_id, receive_now);

    std::optional<std::string> received_topic;
    for (std::size_t attempt = 0; attempt < 100 && !received_topic; ++attempt) {
        publish (publisher, "payment");
        publish (publisher, "order.created");
        const auto [status, record] = subscriber.try_receive (receive_now);
        if (status == fanout::fanout_receive_status_t::application) {
            ASSERT_TRUE (record);
            received_topic = record->topic;
        } else {
            std::this_thread::sleep_for (2ms);
        }
    }

    ASSERT_TRUE (received_topic);
    EXPECT_EQ (*received_topic, "order.created");
}

TEST (CppFrameworkFanoutSubscription, RestrictedAutomaticSubscriberStaysReadyFromBeacon)
{
    fanout::raw_fanout_publisher_t publisher ("tcp://127.0.0.1:0");
    publisher.start ();
    const auto publisher_id = bytes ("publisher-liveness");
    fanout::raw_fanout_subscriber_t subscriber (nullptr,
                                                configured_subscription_topics ({"order"}));
    subscriber.reconcile_automatic ({fanout::fanout_publisher_intent_t{
      publisher_id, 1, publisher.endpoint (), mesh::service_node_state_t::serving}});
    const auto receive_now = std::chrono::steady_clock::now ();

    wait_for_beacon (publisher, subscriber, publisher_id, receive_now);
    EXPECT_TRUE (subscriber.tick (receive_now + fanout::fanout_receive_deadline - 1ms).empty ());
    EXPECT_TRUE (subscriber.ready (publisher_id));
}

TEST (CppFrameworkFanoutSubscription, ReservedPrefixIsRejectedByPublishAndSubscribe)
{
    const auto reserved = fanout::raw_fanout_publisher_t::reserved_topic ();
    const auto extended = reserved + std::string (1, '\0');
    const auto shorter = reserved.substr (0, reserved.size () - 1);
    auto different = reserved;
    different.back () = static_cast<char> (0x32);

    auto options = std::make_shared<zlink::framework::detail::framework_options_state_t> ();
    auto handler_groups =
      std::make_shared<zlink::framework::detail::handler_group_options_state_t> ();
    zlink::framework::fanout_channel_builder_t channel ("events", options, handler_groups);
    EXPECT_TRUE (rejects_as_call_argument ([&] { channel.subscribe (reserved); }));
    EXPECT_TRUE (rejects_as_call_argument ([&] { channel.subscribe (extended); }));
    EXPECT_FALSE (rejects_as_call_argument ([&] { channel.subscribe (shorter); }));
    EXPECT_FALSE (rejects_as_call_argument ([&] { channel.subscribe (different); }));
    ASSERT_TRUE (options->fanout_subscription_topics.contains ("events"));
    EXPECT_EQ (options->fanout_subscription_topics.at ("events").size (), 2u);

    zlink::framework::zlink_builder_t zlink;
    auto publisher = zlink.publisher ();
    EXPECT_TRUE (rejects_as_call_argument (
      [&] { (void) publisher.publish ("events", reserved, std::string ("value")); }));
    EXPECT_TRUE (rejects_as_call_argument (
      [&] { (void) publisher.publish ("events", extended, std::string ("value")); }));
    EXPECT_FALSE (rejects_as_call_argument (
      [&] { (void) publisher.publish ("events", shorter, std::string ("value")); }));
    EXPECT_FALSE (rejects_as_call_argument (
      [&] { (void) publisher.publish ("events", different, std::string ("value")); }));
}

TEST (CppFrameworkFanoutSubscription, DuplicateSubscriptionHasSameEffectiveTransportSet)
{
    const auto once = configured_subscription_topics ({"order"});
    const auto twice = configured_subscription_topics ({"order", "order"});

    EXPECT_EQ (once, twice);
    EXPECT_EQ (fanout::fanout_subscription_topics (once),
               fanout::fanout_subscription_topics (twice));
    EXPECT_EQ (fanout::fanout_subscription_topics (twice).size (), 2u);
}

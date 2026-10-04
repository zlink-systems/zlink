/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include "../support/runtime_failure_fixture.hpp"

#include <zlink/framework.hpp>

#include "runtime/channels/channel_runtime.hpp"
#include "runtime/fanout/fanout_subscription.hpp"
#include "runtime/fanout/raw_fanout_owner.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
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

struct gate_event_t
{
    static constexpr const char *packet_name = "fanout.gate.event";
    int value{};
};

void to_json (nlohmann::json &json, const gate_event_t &event)
{
    json = nlohmann::json{{"value", event.value}};
}

void from_json (const nlohmann::json &json, gate_event_t &event)
{
    event.value = json.at ("value").get<int> ();
}

struct gate_state_t
{
    zlink::framework::task_completion_source_t<void> first_terminal;
    std::promise<void> first_started;
    std::promise<void> other_started;
    std::atomic_int first_count{0};
    std::atomic_int first_completed{0};
    std::atomic_int other_count{0};
    std::atomic_int later_started{0};
};

std::shared_ptr<gate_state_t> gate_state;

class gate_handler_t
{
  public:
    using event_type = gate_event_t;

    zlink::framework::task_t<void> handle (const gate_event_t &event)
    {
        if (event.value == 1) {
            if (gate_state->first_count.fetch_add (1, std::memory_order_acq_rel) == 0) {
                gate_state->first_started.set_value ();
                co_await gate_state->first_terminal.task ();
                gate_state->first_completed.fetch_add (1, std::memory_order_acq_rel);
                co_return;
            }
        }
        if (event.value == 2)
            gate_state->later_started.fetch_add (1, std::memory_order_acq_rel);
        if (event.value == 3
            && gate_state->other_count.fetch_add (1, std::memory_order_acq_rel) == 0)
            gate_state->other_started.set_value ();
        co_return;
    }
};

class gate_publish_client_t final : public zlink::framework::hosted_service_t
{
  public:
    explicit gate_publish_client_t (zlink::framework::app_t &app) : _app (&app) {}

    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &services) override
    {
        auto publisher = _app->advanced ().zlink ().publisher ();
        auto &runtime = services.get_required<zlink::framework::framework_runtime_t> ();
        auto first = gate_state->first_started.get_future ();
        auto other = gate_state->other_started.get_future ();
        for (int attempt = 0; attempt < 80 && !first_received; ++attempt) {
            try {
                co_await publisher.publish ("gate-a", "gate", gate_event_t{1}).async ();
                first_submitted = true;
            }
            catch (const zlink::framework::framework_exception_t &error) {
                last_error = error.what ();
            }
            first_received = first.wait_for (25ms) == std::future_status::ready;
        }
        if (first_received) {
            try {
                co_await publisher.publish ("gate-a", "gate", gate_event_t{2}).async ();
                later_submitted = true;
            }
            catch (const zlink::framework::framework_exception_t &error) {
                last_error = error.what ();
            }
            if (later_submitted) {
                for (int attempt = 0; attempt < 80 && !later_received; ++attempt) {
                    later_received =
                      runtime.status ().capacity.application_job_queue.queued_application_jobs > 0;
                    if (!later_received)
                        std::this_thread::sleep_for (25ms);
                }
                try {
                    co_await publisher.publish ("gate-b", "gate", gate_event_t{3}).async ();
                    other_submitted = true;
                }
                catch (const zlink::framework::framework_exception_t &error) {
                    last_error = error.what ();
                }
                other_completed =
                  other_submitted && other.wait_for (2s) == std::future_status::ready;
                if (other_completed) {
                    std::this_thread::sleep_for (100ms);
                    later_before_release =
                      gate_state->later_started.load (std::memory_order_acquire);
                }
            }
        }
        _app->stop ();
        terminal_completed_once =
          gate_state->first_terminal.complete (zlink::framework::result_t<void>::success ());
        terminal_repeated =
          gate_state->first_terminal.complete (zlink::framework::result_t<void>::success ());
        for (int attempt = 0;
             attempt < 80 && gate_state->later_started.load (std::memory_order_acquire) == 0;
             ++attempt)
            std::this_thread::sleep_for (25ms);
        later_completed = gate_state->later_started.load (std::memory_order_acquire) > 0;
        co_return;
    }

    void stop () noexcept override {}

    bool first_submitted = false;
    bool first_received = false;
    bool later_submitted = false;
    bool later_received = false;
    bool other_submitted = false;
    bool other_completed = false;
    int later_before_release = -1;
    bool later_completed = false;
    bool terminal_completed_once = false;
    bool terminal_repeated = false;
    std::string last_error;

  private:
    zlink::framework::app_t *_app;
};

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

    zlink::framework::zlink_builder_t zlink = zlink::framework::test::runtime_failure_builder ();
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
    fanout::raw_fanout_publisher_t publisher (
      "tcp://127.0.0.1:0", {}, false, std::nullopt,
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ());
    publisher.start ();
    const auto publisher_id = bytes ("publisher-default");
    fanout::raw_fanout_subscriber_t subscriber (
      nullptr, configured_subscription_topics ({}),
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ());
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
    fanout::raw_fanout_publisher_t publisher (
      "tcp://127.0.0.1:0", {}, false, std::nullopt,
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ());
    publisher.start ();
    const auto publisher_id = bytes ("publisher-prefix");
    fanout::raw_fanout_subscriber_t subscriber (
      nullptr, configured_subscription_topics ({"order"}),
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ());
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
    fanout::raw_fanout_publisher_t publisher (
      "tcp://127.0.0.1:0", {}, false, std::nullopt,
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ());
    publisher.start ();
    const auto publisher_id = bytes ("publisher-liveness");
    fanout::raw_fanout_subscriber_t subscriber (
      nullptr, configured_subscription_topics ({"order"}),
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ());
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

    zlink::framework::zlink_builder_t zlink = zlink::framework::test::runtime_failure_builder ();
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

TEST (CppFrameworkFanoutSubscription, AsyncHandlerKeepsChannelGateUntilTerminal)
{
    gate_state = std::make_shared<gate_state_t> ();
    auto app = zlink::framework::app_t::create ();
    auto store = std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ();
    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.add_location_store (store);
        options.handlers ().group ("gate").add_publish<gate_handler_t> ();
        options.add_fanout_channel ("gate-a")
          .set_routing_id (zlink::routing_id_t::from ("gate-a-publisher"))
          .enable_publisher ("tcp://127.0.0.1:0")
          .enable_subscriber ()
          .use_handler_group ("gate");
        options.add_fanout_channel ("gate-b")
          .set_routing_id (zlink::routing_id_t::from ("gate-b-publisher"))
          .enable_publisher ("tcp://127.0.0.1:0")
          .enable_subscriber ()
          .use_handler_group ("gate");
    });
    auto service = std::make_unique<gate_publish_client_t> (app);
    auto *client = service.get ();
    app.add_hosted_service (std::move (service));

    EXPECT_EQ (app.run (0, nullptr), 0);
    EXPECT_TRUE (client->first_submitted);
    EXPECT_TRUE (client->first_received);
    EXPECT_TRUE (client->last_error.empty ()) << client->last_error;
    EXPECT_TRUE (client->later_submitted);
    EXPECT_TRUE (client->later_received);
    EXPECT_TRUE (client->other_submitted);
    EXPECT_TRUE (client->other_completed);
    EXPECT_EQ (client->later_before_release, 0);
    EXPECT_TRUE (client->later_completed);
    EXPECT_TRUE (client->terminal_completed_once);
    EXPECT_FALSE (client->terminal_repeated);
    EXPECT_EQ (gate_state->first_completed.load (std::memory_order_acquire), 1);
}

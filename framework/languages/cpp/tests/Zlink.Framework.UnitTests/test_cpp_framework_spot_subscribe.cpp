/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include <gtest/gtest.h>
#include <zlink/framework.hpp>

namespace
{
namespace fw = zlink::framework;
namespace detail = fw::detail;
namespace messaging = fw::runtime::messaging;
constexpr auto channel = "border";
constexpr auto other_channel = "other";

struct event_t
{
    static constexpr const char *packet_name = "NamedChannelEvent";
    int value = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE (event_t, value)

struct subscriber_t
{
    std::vector<std::string> channels;
    std::vector<std::string> topics;
    int other_calls = 0;
    void receive (const fw::publish_message_context_t &context, const event_t &)
    {
        channels.push_back (context.channel_name.value_or (""));
        topics.push_back (context.topic);
    }
    void receive_other (const fw::publish_message_context_t &context, const event_t &event)
    {
        ++other_calls;
        receive (context, event);
    }
};

class SpotSubscribe : public testing::Test
{
  protected:
    SpotSubscribe () :
        node (fw::test::runtime_failure_fixture<detail::spot_node_builder_state_t> ("mesh")),
        state (std::make_shared<detail::spot_context_state_t> ()),
        context (detail::spot_context_access_t::create (state)),
        subscriber (std::make_shared<subscriber_t> ()),
        provider (services.build_provider ())
    {
        state->node = node;
        state->spot_id = fw::spot_id_t ("subscriber");
        state->serial_executor =
          std::make_shared<fw::runtime::offload_executor_t> (1, "subscribe-test");
        state->spot_instance = subscriber;
    }

    void subscribe (const std::string &target_channel, const std::string &topic)
    {
        context.handlers ().add_subscribe<&subscriber_t::receive> (target_channel, topic);
    }

    fw::result_t<bool> deliver (const std::string &target_channel,
                                const std::string &topic,
                                const std::string &packet_name = event_t::packet_name)
    {
        messaging::envelope_header_t header;
        header.kind = messaging::message_kind_t::publish;
        header.channel_name = target_channel;
        header.topic = topic;
        header.message_name = packet_name;
        header.content_type = messaging::envelope_codec_t::default_content_type;
        auto parts = messaging::envelope_codec_t{}.encode_raw_body_parts (
          header, zlink::message_t::from (std::string ("{\"value\":1}")));
        return detail::spot_node_runtime_t (node).dispatch_subscription (
          context, topic, parts.items (), provider, serializers);
    }

    std::shared_ptr<detail::spot_node_builder_state_t> node;
    std::shared_ptr<detail::spot_context_state_t> state;
    fw::spot_context_t context;
    std::shared_ptr<subscriber_t> subscriber;
    fw::service_collection_t services;
    fw::service_provider_t provider;
    fw::serializer_registry_t serializers;
};

TEST_F (SpotSubscribe, RuntimeTopicReachesNamedChannel)
{
    const auto topic = std::string ("spot:") + std::to_string (7);
    subscribe (channel, topic);
    ASSERT_TRUE (deliver (channel, topic));
    EXPECT_EQ ((std::vector<std::string>{channel}), subscriber->channels);
    EXPECT_EQ ((std::vector<std::string>{topic}), subscriber->topics);
}

TEST_F (SpotSubscribe, PublisherFrameKeepsNamedChannelThroughLocalDelivery)
{
    namespace host = fw::runtime::host;
    namespace mesh = fw::runtime::mesh;
    fw::logging_builder_t logging;
    logging.use_file ("spot-subscribe.flow").set_min_level (fw::log_level_t::debug);
    node->dispatch.message_flow (fw::message_flow_log_mode_t::detailed);
    detail::dispatch_options_access_t::set_logger (node->dispatch,
                                                   logging.create_logger ("spot-subscribe"));
    auto jobs = std::make_shared<fw::runtime::application_job_queue_t> (
      fw::runtime::application_job_queue_configuration_t{});
    auto held_supply = jobs->try_reserve_supply ();
    ASSERT_TRUE (held_supply);
    mesh::service_node_descriptor_t descriptor;
    descriptor.mesh_name = "mesh";
    descriptor.node_routing_id = zlink::routing_id_t::from ("publisher").to_bytes ();
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.advertised_endpoint = "tcp://127.0.0.1:0";
    descriptor.channels = {{channel, 100}, {other_channel, 100}};
    auto native = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh =
        mesh::raw_mesh_node_options_t{
          .descriptor = std::move (descriptor),
          .application_jobs = jobs,
          .dispatch = node->dispatch,
          .runtime_failures = std::make_shared<fw::runtime::runtime_failure_collector_t> ()},
      .object_stable_types = {"framework.spot"}});
    native->start ();
    detail::spot_node_runtime_t runtime (node);
    runtime.attach_native_node (native);
    const auto topic = std::string ("spot:") + std::to_string (7);
    subscribe (channel, topic);
    fw::spot_publisher_client_t publisher (runtime.manager (), serializers);
    ASSERT_TRUE (publisher.publish (channel, topic, event_t{1}).async ().result ());
    std::size_t received = 0;
    const auto receive = [&] (const auto &, const auto &record,
                              std::vector<zlink::message_t> parts) {
        ++received;
        auto delivered = runtime.dispatch_subscription (
          context, topic, parts, provider, serializers, record.before_application_handler);
        EXPECT_TRUE (delivered);
        if (delivered)
            EXPECT_TRUE (delivered.value ());
    };
    // Submit/completion §6: source commit precedes target submission. Hold its
    // existing supply permit to expose that ordering without a timing wait.
    auto before_release = native->next_dispatch_activity_async ().result ();
    ASSERT_TRUE (before_release);
    EXPECT_FALSE (before_release.value ().first);
    ASSERT_TRUE (native->dispatch_ready (receive).result ());
    EXPECT_EQ (0u, received);
    held_supply->release_without_handler ();

    // Drive the receiver's existing activity protocol; the publish is submitted once.
    while (received == 0) {
        auto activity = native->next_dispatch_activity_async ().result ();
        ASSERT_TRUE (activity);
        if (!activity.value ().first)
            (void) native->wait_for_dispatch_activity (std::chrono::milliseconds{-1}, true,
                                                       activity.value ().second);
        ASSERT_TRUE (native->dispatch_ready (receive).result ());
    }
    EXPECT_EQ (1u, received);
    EXPECT_EQ ((std::vector<std::string>{channel}), subscriber->channels);
    EXPECT_EQ ((std::vector<std::string>{topic}), subscriber->topics);
    native->close ();
}

TEST_F (SpotSubscribe, OtherChannelDoesNotInvokeSameTopic)
{
    const auto topic = std::string ("spot:") + std::to_string (7);
    subscribe (channel, topic);
    ASSERT_TRUE (deliver (other_channel, topic));
    EXPECT_TRUE (subscriber->channels.empty ());
}

TEST_F (SpotSubscribe, SameTopicRegistersIndependentlyOnTwoChannels)
{
    const auto topic = std::string ("spot:") + std::to_string (7);
    subscribe (channel, topic);
    context.handlers ().add_subscribe<&subscriber_t::receive_other> (other_channel, topic);
    ASSERT_TRUE (deliver (other_channel, topic));
    ASSERT_TRUE (deliver (channel, topic));
    EXPECT_EQ ((std::vector<std::string>{other_channel, channel}), subscriber->channels);
    EXPECT_EQ ((std::vector<std::string>{topic, topic}), subscriber->topics);
    EXPECT_EQ (1, subscriber->other_calls);
}

TEST_F (SpotSubscribe, WirePacketNameIsPartOfTheSubscriptionKey)
{
    const auto topic = std::string ("spot:") + std::to_string (7);
    subscribe (channel, topic);
    auto rejected = deliver (channel, topic, std::string (event_t::packet_name) + ".other");
    ASSERT_TRUE (rejected);
    EXPECT_FALSE (rejected.value ());
    EXPECT_TRUE (subscriber->channels.empty ());
    auto accepted = deliver (channel, topic);
    ASSERT_TRUE (accepted);
    EXPECT_TRUE (accepted.value ());
    EXPECT_EQ ((std::vector<std::string>{channel}), subscriber->channels);
    EXPECT_EQ ((std::vector<std::string>{topic}), subscriber->topics);
}

TEST_F (SpotSubscribe, DescriptorPreservesRegistrationAndRejectsEmptyChannel)
{
    const auto topic = std::string ("spot:") + std::to_string (7);
    EXPECT_THROW (subscribe ("", topic), fw::framework_exception_t);
    EXPECT_TRUE (context.handlers ().descriptors ().empty ());
    subscribe (channel, topic);
    const auto descriptors = context.handlers ().descriptors ();
    ASSERT_EQ (1u, descriptors.size ());
    EXPECT_EQ (channel, descriptors.front ().channel_name);
    EXPECT_EQ (topic, descriptors.front ().topic);
}

TEST_F (SpotSubscribe, DuplicateRegistrationIsRejected)
{
    const auto topic = std::string ("spot:") + std::to_string (7);
    subscribe (channel, topic);
    try {
        subscribe (channel, topic);
        FAIL () << "duplicate subscription accepted";
    }
    catch (const fw::framework_exception_t &error) {
        EXPECT_EQ (fw::framework_error_kind_t::invalid_operation, error.kind ());
    }
    EXPECT_EQ (1u, context.handlers ().descriptors ().size ());
}
class membership_entry_t final : public fw::entry_spot_t<fw::actor_t>
{
  public:
    membership_entry_t (fw::entry_spot_context_t context, std::string subscription_channel) :
        _context (std::move (context)), _channel (std::move (subscription_channel))
    {
    }
    fw::entry_spot_context_t &context () noexcept override { return _context; }
    const fw::entry_spot_context_t &context () const noexcept override { return _context; }
    void configure () override
    {
        _context.handlers ().add_subscribe<&membership_entry_t::receive> (_channel,
                                                                          "runtime-topic");
    }
    void receive (const event_t &) {}
    fw::task_t<void> on_actor_joined (fw::actor_t &) override { co_return; }
    fw::task_t<void> on_leave_actor (fw::actor_t &) override { co_return; }

  private:
    fw::entry_spot_context_t _context;
    std::string _channel;
};

class membership_stop_service_t final : public fw::hosted_service_t
{
  public:
    membership_stop_service_t (fw::app_t &app, bool &started) : _app (app), _started (started) {}
    fw::task_t<void> start (fw::service_provider_t &) override
    {
        _started = true;
        _app.stop ();
        co_return;
    }
    void stop () noexcept override {}

  private:
    fw::app_t &_app;
    bool &_started;
};

TEST (SpotSubscriptionMembership, EntryActivationRejectsUnjoinedChannelBeforeHostStarts)
{
    for (const auto &subscription_channel : {std::string (channel), std::string (other_channel)}) {
        SCOPED_TRACE (subscription_channel);
        auto app = fw::app_t::create ();
        app.logging ().use_file ("spot-membership.flow").set_min_level (fw::log_level_t::debug);
        app.add_zlink_framework ([&] (fw::zlink_framework_options_t &options) {
            options.configure_dispatch ().message_flow (fw::message_flow_log_mode_t::detailed);
            options.add_location_store (
              std::make_shared<fw::runtime::in_memory_location_store_t> ());
            auto mesh = options.add_route_mesh ("membership");
            mesh.set_object_role (fw::object_role_t::server)
              .set_routing_id (zlink::routing_id_t::from ("membership-node"))
              .listen ("tcp://127.0.0.1:0")
              .add_entry_spot<membership_entry_t> ([&] (fw::entry_spot_context_t context) {
                  return std::make_shared<membership_entry_t> (std::move (context),
                                                               subscription_channel);
              });
            mesh.channel_name (channel).server ().set_weight (0);
        });
        bool started = false;
        app.add_hosted_service (std::make_unique<membership_stop_service_t> (app, started));
        const auto exit_code = app.run (0, nullptr);
        if (subscription_channel == channel) {
            EXPECT_EQ (0, exit_code);
            EXPECT_TRUE (started);
        } else {
            EXPECT_EQ (1, exit_code);
            EXPECT_FALSE (started);
        }
    }
}
TEST (SpotSubscriptionMembership, NativeRegistrationRejectsUnjoinedChannel)
{
    namespace host = fw::runtime::host;
    auto native = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "membership-native",
                              .node_routing_id =
                                zlink::routing_id_t::from ("membership-native").to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0",
                              .channels = {{channel, 0}}},
               .runtime_failures = std::make_shared<fw::runtime::runtime_failure_collector_t> ()},
      .object_stable_types = {"framework.spot"}});
    native->start ();
    auto spot = native->get_or_create_spot ("subscriber");
    EXPECT_NO_THROW (spot.set_subscription (channel, "runtime-topic"));
    std::optional<fw::framework_error_kind_t> rejected;
    try {
        spot.set_subscription (other_channel, "runtime-topic");
    }
    catch (const fw::framework_exception_t &error) {
        rejected = error.kind ();
    }
    auto node =
      fw::test::runtime_failure_fixture<detail::spot_node_builder_state_t> ("membership-native");
    auto state = std::make_shared<detail::spot_context_state_t> ();
    state->node = node;
    auto attached = std::make_shared<host::spot_handle_t> (std::move (spot));
    state->native_spot = attached;
    auto context = detail::spot_context_access_t::create (state);
    context.handlers ().add_subscribe<&subscriber_t::receive> (channel, "runtime-topic");
    const auto descriptors_before = context.handlers ().descriptors ().size ();
    EXPECT_THROW (
      context.handlers ().add_subscribe<&subscriber_t::receive> (other_channel, "runtime-topic"),
      fw::framework_exception_t);
    EXPECT_EQ (descriptors_before, context.handlers ().descriptors ().size ());
    native->close ();
    EXPECT_EQ (fw::framework_error_kind_t::invalid_operation, rejected);
}
} // namespace

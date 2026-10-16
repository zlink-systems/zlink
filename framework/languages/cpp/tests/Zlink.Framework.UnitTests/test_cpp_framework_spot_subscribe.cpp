/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/spots/spot_runtime.hpp"
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
} // namespace

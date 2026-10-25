/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/stateful_object_sync.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <concepts>
#include <future>
#include <latch>
#include <thread>
#include <utility>
#include <zlink/framework.hpp>

using zlink::framework::test::begin_reserved_object;

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

template <typename TContext>
concept accepts_implicit_publish =
  requires (TContext &context, event_t event) { context.publish (std::string{}, event); };
static_assert (!accepts_implicit_publish<fw::spot_context_t>);
static_assert (std::same_as<decltype (std::declval<fw::spot_context_t &> ().publish (
                              std::string{}, std::string{}, event_t{})),
                            fw::publish_call_t>);

struct subscriber_t
{
    std::vector<std::string> channels;
    std::vector<std::string> topics;
    std::vector<int> values;
    int other_calls = 0;
    std::shared_ptr<std::atomic_size_t> received_calls;
    void receive (const fw::publish_message_context_t &context, const event_t &event)
    {
        channels.push_back (context.channel_name.value_or (""));
        topics.push_back (context.topic);
        values.push_back (event.value);
        if (received_calls)
            received_calls->fetch_add (1, std::memory_order_release);
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

    void TearDown () override
    {
        node->lane.run ([this] { node->spot_contexts_by_id.clear (); }).get ();
    }

    void verify_pre_ready (bool reject, bool fail_initialize);

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
        auto submitted = detail::spot_node_runtime_t (node).dispatch_subscription (
          context, topic, parts.items (), provider, serializers);
        if (!submitted)
            return fw::result_t<bool>::failure (submitted.error_kind (),
                                                submitted.error ()->what ());
        if (!submitted.value ())
            return fw::result_t<bool>::success (false);
        const auto completed = submitted.value ()->result ();
        if (!completed)
            return fw::result_t<bool>::failure (completed.error_kind (),
                                                completed.error ()->what ());
        return fw::result_t<bool>::success (true);
    }

    std::shared_ptr<detail::spot_node_builder_state_t> node;
    std::shared_ptr<detail::spot_context_state_t> state;
    fw::spot_context_t context;
    std::shared_ptr<subscriber_t> subscriber;
    fw::service_collection_t services;
    fw::service_provider_t provider;
    fw::serializer_registry_t serializers;
};

TEST_F (SpotSubscribe, ClosedTurnDoesNotEnterApplicationHandler)
{
    subscribe (channel, "closed-turn");
    state->serial_queue =
      std::make_shared<fw::runtime::serial_execution_queue_t> (*state->serial_executor);
    std::latch worker_started{1};
    std::latch release_worker{1};
    ASSERT_TRUE (state->serial_executor->try_submit_internal ([&] {
        worker_started.count_down ();
        release_worker.wait ();
    }));
    worker_started.wait ();
    std::shared_ptr<void> release_on_exit (nullptr, [&] (void *) { release_worker.count_down (); });

    messaging::envelope_header_t header;
    header.kind = messaging::message_kind_t::publish;
    header.channel_name = channel;
    header.topic = "closed-turn";
    header.message_name = event_t::packet_name;
    header.content_type = messaging::envelope_codec_t::default_content_type;
    auto parts = messaging::envelope_codec_t{}.encode_raw_body_parts (
      header, zlink::message_t::from (std::string ("{\"value\":1}")));
    std::atomic_int handler_entries{0};
    auto submitted = detail::spot_node_runtime_t (node).dispatch_subscription (
      context, "closed-turn", parts.items (), provider, serializers,
      [&] { handler_entries.fetch_add (1, std::memory_order_relaxed); });
    node->lane.run ([this] { state->closed = true; }).get ();
    // The admitted turn must observe the closed incarnation before handler entry.
    release_on_exit.reset ();
    ASSERT_TRUE (submitted);
    ASSERT_TRUE (submitted.value ());
    const auto result = submitted.value ()->result ();
    EXPECT_EQ (0, handler_entries.load (std::memory_order_relaxed));
    EXPECT_TRUE (subscriber->values.empty ());
    ASSERT_FALSE (result);
    EXPECT_EQ (fw::framework_error_kind_t::not_found, result.error_kind ());
}

TEST_F (SpotSubscribe, RuntimeTopicReachesNamedChannel)
{
    const auto topic = std::string ("spot:") + std::to_string (7);
    subscribe (channel, topic);
    ASSERT_TRUE (deliver (channel, topic));
    EXPECT_EQ ((std::vector<std::string>{channel}), subscriber->channels);
    EXPECT_EQ ((std::vector<std::string>{topic}), subscriber->topics);
}

TEST_F (SpotSubscribe, ContextFrameKeepsNamedChannelThroughLocalDelivery)
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
    auto native_subscriber = std::make_shared<detail::service::spot_t> (
      native->get_or_create_spot (std::string (state->spot_id)));
    node->lane
      .run ([&] {
          state->native_spot = native_subscriber;
          node->spot_contexts_by_id.emplace (state->spot_id,
                                             detail::spot_context_access_t::create (state));
      })
      .get ();
    const auto topic = std::string ("spot:") + std::to_string (7);
    subscribe (channel, topic);
    node->channel_runtime = std::make_shared<detail::channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;
    std::weak_ptr<detail::spot_context_state_t> source_lifetime;
    std::optional<fw::task_t<void>> publication;
    {
        // Occupy the existing multicast workers so the temporary call dies
        // before the worker can submit its frame. No timing wait is needed.
        const auto worker_count = std::max<std::size_t> (2, std::thread::hardware_concurrency ());
        std::latch finished (static_cast<std::ptrdiff_t> (worker_count));
        std::promise<void> release;
        const auto released = release.get_future ().share ();
        struct release_guard_t
        {
            std::promise<void> &release;
            std::latch &finished;
            std::ptrdiff_t remaining;
            ~release_guard_t ()
            {
                finished.count_down (remaining);
                release.set_value ();
                finished.wait ();
            }
        } guard{release, finished, static_cast<std::ptrdiff_t> (worker_count)};
        for (std::size_t index = 0; index < worker_count; ++index) {
            fw::publish_call_t occupied ([&finished, released] (const auto &) {
                released.wait ();
                finished.count_down ();
                return fw::result_t<void>::success ();
            });
            ASSERT_TRUE (occupied.async ().result ());
            --guard.remaining;
        }
        publication.emplace ([&] {
            auto source = std::make_shared<detail::spot_context_state_t> ();
            source->node = node;
            source->channel_runtime = node->channel_runtime;
            source_lifetime = source;
            auto publisher_context = detail::spot_context_access_t::create (source);
            return publisher_context.publish (channel, topic, event_t{1}).async ();
        }());
        EXPECT_FALSE (source_lifetime.expired ());
    }
    ASSERT_TRUE (publication->result ());
    std::size_t received = 0;
    const auto receive = [&] (const auto &owner, const auto &record,
                              std::vector<zlink::message_t> parts) {
        ++received;
        auto header = fw::runtime::decode_mesh_record_header (record, parts, true, node->dispatch);
        EXPECT_TRUE (header) << (header.error () ? header.error ()->what () : "");
        if (!header)
            return;
        EXPECT_TRUE (runtime.dispatch_mesh_record (owner, record, parts, header.value (), provider,
                                                   serializers, {}, nullptr,
                                                   record.before_application_handler));
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
    state->drain_serial ();
    EXPECT_EQ ((std::vector<std::string>{channel}), subscriber->channels);
    EXPECT_EQ ((std::vector<std::string>{topic}), subscriber->topics);
    EXPECT_EQ ((std::vector<int>{1}), subscriber->values);
    native->close ();
}

TEST_F (SpotSubscribe, ContextPublishChecksRelocationReadinessAtSubmission)
{
    state->channel_runtime = std::make_shared<detail::channel_runtime_state_t> ();
    state->channel_runtime->serializers = &serializers;
    state->relocation_coordination_mode =
      fw::spot_relocation_coordination_mode_t::application_signaled;
    state->relocation_boundary_active = true;
    state->serial_queue = std::make_shared<fw::runtime::serial_execution_queue_t> (
      *state->serial_executor, fw::runtime::serial_execution_queue_options_t{},
      fw::runtime::serial_execution_queue_t::error_handler_t{},
      fw::runtime::serial_lane_policy_t::spot_wide ());
    std::optional<fw::result_t<void>> result;
    const auto ran = state->run_serial_sync ("publish-after-relocation-ready", [&] {
        auto call = context.publish (channel, "runtime-topic", event_t{1});
        context.relocation_ready ().defer ();
        result = call.async ().result ();
    });
    state->complete_relocation_ready (fw::spot_relocation_ready_outcome_t::continued);
    state->serial_queue->drain ();
    ASSERT_TRUE (ran);
    ASSERT_TRUE (result);
    EXPECT_FALSE (*result);
    EXPECT_EQ (fw::framework_error_kind_t::invalid_operation, result->error_kind ());
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

void SpotSubscribe::verify_pre_ready (bool reject, bool fail_initialize)
{
    namespace host = fw::runtime::host;
    namespace mesh = fw::runtime::mesh;
    constexpr auto topic = "pre-ready";
    fw::logging_builder_t logging;
    logging.use_file ("pre-ready-multicast.flow").set_min_level (fw::log_level_t::debug);
    node->dispatch.message_flow (fw::message_flow_log_mode_t::detailed);
    detail::dispatch_options_access_t::set_logger (node->dispatch,
                                                   logging.create_logger ("pre-ready-multicast"));
    node->worker_executor = std::make_shared<fw::runtime::offload_executor_t> (1);
    node->channel_runtime = std::make_shared<detail::channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;
    auto native = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh =
        mesh::raw_mesh_node_options_t{
          .descriptor = {.mesh_name = "mesh",
                         .node_routing_id = zlink::routing_id_t::from ("publisher").to_bytes (),
                         .lifecycle_generation = 1,
                         .descriptor_revision = 1,
                         .advertised_endpoint = "tcp://127.0.0.1:0",
                         .channels = {{channel, 100}}},
          .dispatch = node->dispatch,
          .runtime_failures = std::make_shared<fw::runtime::runtime_failure_collector_t> ()},
      .object_stable_types = {"framework.spot"}});
    native->start ();
    detail::spot_node_runtime_t runtime (node);
    runtime.attach_native_node (native);
    namespace stateful = fw::runtime::stateful;
    const stateful::object_ref_t reference{
      stateful::object_kind_t::user_spot, "pre-ready-spot", 7, 1, "mesh", "publisher"};
    const auto reservation =
      begin_reserved_object (native->objects (), reference, "framework.spot", {});
    ASSERT_TRUE (reservation.factory_owner);
    std::function<fw::task_t<void> (bool)> preparation_terminal;
    fw::task_completion_source_t<void> entered;
    auto started = entered.task ();
    fw::task_completion_source_t<fw::spot_create_response_t> release_create;
    fw::task_completion_source_t<void> release_initialize;
    std::shared_ptr<detail::spot_context_state_t> preparing;
    std::weak_ptr<subscriber_t> instance;
    auto received_calls = std::make_shared<std::atomic_size_t> (0);
    detail::spot_lifecycle_callbacks_t lifecycle;
    lifecycle.create_spot_context_instance = [&] (fw::spot_context_t value,
                                                  fw::service_provider_t &) {
        preparing = detail::spot_context_access_t::state (value);
        auto created = std::make_shared<subscriber_t> ();
        created->received_calls = received_calls;
        instance = created;
        // The public factory adapter calls Configure before returning the instance.
        value.handlers ().add_subscribe<&subscriber_t::receive> (channel, topic);
        return created;
    };
    lifecycle.on_create = [&] (void *, const zlink::message_t &, fw::serializer_registry_t &) {
        if (!fail_initialize) {
            entered.complete (fw::result_t<void>::success ());
            return release_create.task ();
        }
        return fw::task_t<fw::spot_create_response_t> (
          fw::result_t<fw::spot_create_response_t>::success (
            fw::spot_create_response_t::accept ()));
    };
    lifecycle.on_initialize = [&] (void *) -> fw::task_t<void> {
        if (fail_initialize) {
            entered.complete (fw::result_t<void>::success ());
            co_await release_initialize.task ();
            throw std::runtime_error ("fixture initialization failure");
        }
        co_return;
    };
    node->spot_lifecycles.emplace ("pre-ready", std::move (lifecycle));
    node->spot_factories.emplace ("pre-ready", std::type_index (typeid (subscriber_t)));
    auto creating = std::async (std::launch::async, [&] {
        try {
            return runtime.get_or_create_spot (
              "pre-ready", fw::spot_id_t ("pre-ready-spot"), {}, reference.object_generation,
              reference.mesh_name, reference.authority_owner_generation, &preparation_terminal);
        }
        catch (const std::exception &error) {
            entered.complete (fw::result_t<void>::failure (
              fw::framework_error_kind_t::internal_failure, error.what ()));
            throw;
        }
    });
    const std::shared_ptr<void> release_on_exit (nullptr, [&] (void *) {
        release_create.complete (fw::result_t<fw::spot_create_response_t>::success (
          fw::spot_create_response_t::accept ()));
        release_initialize.complete (fw::result_t<void>::success ());
    });
    started.result ().value ();
    messaging::envelope_header_t header;
    header.kind = messaging::message_kind_t::publish;
    header.channel_name = channel;
    header.topic = topic;
    header.message_name = event_t::packet_name;
    header.content_type = messaging::envelope_codec_t::default_content_type;
    auto parts = messaging::envelope_codec_t{}.encode_raw_body_parts (
      header, zlink::message_t::from (std::string ("{\"value\":37}")));
    auto entry = native->entry_spot ();
    const auto published =
      entry.publish (channel, topic, std::move (parts).take_items ()).result ();
    std::size_t received = 0;
    const auto receive = [&] (const auto &owner, const auto &record,
                              std::vector<zlink::message_t> frames) {
        ++received;
        auto decoded =
          fw::runtime::decode_mesh_record_header (record, frames, true, node->dispatch);
        EXPECT_TRUE (decoded);
        if (decoded)
            EXPECT_TRUE (runtime.dispatch_mesh_record (owner, record, frames, decoded.value (),
                                                       provider, serializers));
    };
    const auto dispatched = native->dispatch_ready (receive).result ();
    EXPECT_FALSE (native->objects ().find (reference.kind, reference.key));
    const auto queued_before_ready =
      preparing->serial_queue->pending_count (fw::runtime::serial_work_lane_t::application);
    const auto calls_before_ready = received_calls->load (std::memory_order_acquire);
    release_create.complete (fw::result_t<fw::spot_create_response_t>::success (
      reject ? fw::spot_create_response_t::reject () : fw::spot_create_response_t::accept ()));
    release_initialize.complete (fw::result_t<void>::success ());
    bool initialization_failed = false;
    std::optional<detail::local_spot_create_result_t> created;
    try {
        created.emplace (creating.get ());
    }
    catch (const std::exception &error) {
        initialization_failed = true;
        EXPECT_NE (std::string::npos,
                   std::string (error.what ()).find ("fixture initialization failure"));
    }
    if (!reject && !fail_initialize && created) {
        EXPECT_FALSE (native->objects ().find (reference.kind, reference.key));
        EXPECT_EQ (0u, received_calls->load (std::memory_order_acquire));
        EXPECT_EQ (1u, preparing->serial_queue->pending_count (
                         fw::runtime::serial_work_lane_t::application));
        EXPECT_EQ (stateful::stateful_error_t::none,
                   native->objects ().commit_create (reservation.attempt));
        EXPECT_TRUE (preparation_terminal);
        if (preparation_terminal)
            EXPECT_TRUE (preparation_terminal (true).result ());
    }
    preparing->serial_queue->drain ();
    EXPECT_TRUE (published);
    EXPECT_TRUE (dispatched);
    EXPECT_EQ (1u, received);
    EXPECT_EQ (1u, queued_before_ready)
      << "received multicast must remain in the Spot application queue";
    EXPECT_EQ (0u, calls_before_ready);
    EXPECT_EQ (fail_initialize, initialization_failed);
    EXPECT_EQ (
      0u, preparing->serial_queue->pending_count (fw::runtime::serial_work_lane_t::application));
    EXPECT_EQ (0u, node->lane.run ([&] { return preparing->callback_depth; }).get ());
    if (reject || fail_initialize) {
        EXPECT_EQ (0u, received_calls->load (std::memory_order_acquire));
        EXPECT_TRUE (instance.expired ());
        EXPECT_TRUE (
          native->objects ().multicast_local_targets (channel, topic).result ().value ().empty ());
        EXPECT_FALSE (
          node->lane.run ([&] { return node->spot_contexts_by_id.contains ("pre-ready-spot"); })
            .get ());
        if (created)
            EXPECT_EQ (fw::spot_create_state_t::rejected, created->state);
    } else {
        EXPECT_EQ (1u, received_calls->load (std::memory_order_acquire));
        EXPECT_TRUE (
          node->lane.run ([&] { return node->spot_contexts_by_id.contains ("pre-ready-spot"); })
            .get ());
        EXPECT_EQ ((std::vector<int>{37}), instance.lock ()->values);
        EXPECT_TRUE (created);
        if (created)
            EXPECT_EQ (fw::spot_create_state_t::created, created->state);
    }
    preparing->spot_serial_executor->close ();
    native->close ();
}

TEST_F (SpotSubscribe, MulticastDuringCreationRunsExactlyOnceAfterReady)
{
    verify_pre_ready (false, false);
}
TEST_F (SpotSubscribe, MulticastDuringRejectedCreationIsReleasedWithoutHandler)
{
    verify_pre_ready (true, false);
}
TEST_F (SpotSubscribe, MulticastDuringFailedInitializationIsReleasedWithoutHandler)
{
    verify_pre_ready (false, true);
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

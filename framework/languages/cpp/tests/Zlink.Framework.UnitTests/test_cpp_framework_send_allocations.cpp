/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <gtest/gtest.h>
#include "task_allocation_counter.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime_failure_fixture.hpp"
#include "stateful_object_sync.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "scope_exit.hpp"

#include <array>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#if defined(__linux__) && defined(__GNUC__) && !defined(__clang__)
#include <pthread.h>
#endif
#include <future>
#include <limits>
#include <memory>
#include <optional>

#if defined(__linux__) && defined(__GNUC__) && !defined(__clang__)
namespace
{
thread_local bool count_sync = false;
thread_local unsigned mutex_locks = 0;
thread_local unsigned notifications = 0;
}

extern "C" int __real_pthread_mutex_lock (pthread_mutex_t *mutex);
extern "C" int __wrap_pthread_mutex_lock (pthread_mutex_t *mutex)
{
    if (count_sync)
        ++mutex_locks;
    return __real_pthread_mutex_lock (mutex);
}

extern "C" void __real__ZNSt18condition_variable10notify_allEv (std::condition_variable *condition);
extern "C" void __wrap__ZNSt18condition_variable10notify_allEv (std::condition_variable *condition)
{
    if (count_sync)
        ++notifications;
    __real__ZNSt18condition_variable10notify_allEv (condition);
}

extern "C" void __real__ZNSt18condition_variable10notify_oneEv (std::condition_variable *condition);
extern "C" void __wrap__ZNSt18condition_variable10notify_oneEv (std::condition_variable *condition)
{
    if (count_sync)
        ++notifications;
    __real__ZNSt18condition_variable10notify_oneEv (condition);
}
#endif

namespace
{
namespace fw = zlink::framework;
namespace detail = fw::detail;
namespace mesh = fw::runtime::mesh;

template <typename TAction, std::size_t N>
std::size_t measure_ready_operation (
  const char *name,
  const std::array<fw::tests::shared_state_allocation_site_t, N> &state_sites,
  TAction &&action)
{
    std::array<fw::tests::allocation_event_t, 32> events;
    fw::tests::allocation_event_log_t log{events.data (), events.size ()};
    const fw::tests::allocation_event_scope_t recording (log);
#if defined(__linux__) && defined(__GNUC__) && !defined(__clang__)
    mutex_locks = 0;
    notifications = 0;
    count_sync = true;
    struct sync_counter_scope_t
    {
        ~sync_counter_scope_t () { count_sync = false; }
    } counter_scope;
#endif
    const auto allocations = fw::tests::count_allocations (std::forward<TAction> (action));
#if defined(__linux__) && defined(__GNUC__) && !defined(__clang__)
    count_sync = false;
#endif
    unsigned shared_results = 0;
    for (std::size_t index = 0; index != log.count; ++index) {
        for (const auto &site : state_sites) {
            if (site.matches (events[index])) {
                ++shared_results;
                break;
            }
        }
    }
    if constexpr (fw::tests::shared_state_stack_metrics_available)
        std::printf ("%s: shared-results=%u allocations=%zu", name, shared_results, allocations);
    else
        std::printf ("%s: shared-results=not-measured allocations=%zu", name, allocations);
#if defined(__linux__) && defined(__GNUC__) && !defined(__clang__)
    std::printf (" locks=%u notifications=%u", mutex_locks, notifications);
#else
    std::printf (" synchronization=not-measured");
#endif
    std::printf ("\n");
    EXPECT_FALSE (log.overflow);
    if constexpr (fw::tests::shared_state_stack_metrics_available)
        EXPECT_EQ (shared_results, 0u);
    return allocations;
}

TEST (SendAllocations, SynchronizationCountersObserveLocksAndBothNotifications)
{
#if defined(__linux__) && defined(__GNUC__) && !defined(__clang__)
    std::mutex mutex;
    std::condition_variable condition;
    mutex_locks = 0;
    notifications = 0;
    count_sync = true;
    mutex.lock ();
    mutex.unlock ();
    condition.notify_one ();
    condition.notify_all ();
    count_sync = false;
    EXPECT_EQ (mutex_locks, 1u);
    EXPECT_EQ (notifications, 2u);
#else
    GTEST_SKIP () << "synchronization linker wrapping requires Linux/GNU";
#endif
}

TEST (SendAllocations, SharedStateCounterExcludesOrdinaryAllocationOfTheSameSize)
{
    if constexpr (!fw::tests::shared_state_stack_metrics_available)
        GTEST_SKIP () << "shared-state stack classification requires Linux/GNU Debug";
    const std::array sites{
      fw::tests::shared_state_allocation_site<detail::task_shared_state_t<fw::result_t<void>>> ()};
    const auto allocations = measure_ready_operation ("same-size ordinary allocation", sites, [&] {
        void *memory = ::operator new (sites[0].size);
        ::operator delete (memory);
    });
    EXPECT_EQ (allocations, 1u);
}

struct route_payload_t
{
    static constexpr const char *packet_name = "p";
    int value = 7;
};

TEST (SendAllocations, SynchronousNodeSendKeepsOnlyItsEncodedPartsAllocation)
{
    fw::serializer_registry_t serializers;
    std::size_t before_serializer = 0;
    std::size_t ready_transport_allocations = 0;
    unsigned submitted = 0;
    serializers.add<route_payload_t> (
      [&] (const route_payload_t &) {
          before_serializer = fw::tests::tracked_allocation_count;
          return fw::encoded_payload_t::from_string ("7");
      },
      [] (const fw::encoded_payload_t &) { return route_payload_t{}; }, "x/y");
    auto builder = fw::test::runtime_failure_builder ();
    auto runtime = detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    runtime.bind_mesh_node_transport (
      "m",
      [&] (const auto &, auto) {
          ++submitted;
          const auto before = fw::tests::tracked_allocation_count;
          auto ready = fw::task_t<fw::result_t<void>> (
            fw::result_t<fw::result_t<void>>::success (fw::result_t<void>::success ()));
          ready_transport_allocations = fw::tests::tracked_allocation_count - before;
          return ready;
      },
      {});
    auto client = builder.route_client (serializers);
    const auto target = zlink::routing_id_t::from ("n");
    for (unsigned warmup = 0; warmup < 4; ++warmup)
        client.send_to_node ("m", target, route_payload_t{}).async ().result ().value ();
    auto call = client.send_to_node ("m", target, route_payload_t{});
    const std::array state_sites{
      fw::tests::shared_state_allocation_site<detail::task_shared_state_t<fw::result_t<void>>> (),
      fw::tests::shared_state_allocation_site<detail::coroutine_task_shared_state_t<void>> (),
      fw::tests::shared_state_allocation_site<detail::application_task_shared_state_t<void>> ()};
    const auto allocations = measure_ready_operation ("synchronous node send", state_sites,
                                                      [&] { call.async ().result ().value (); });
    std::printf ("synchronous node send: before-encode=%zu ready-transport=%zu\n",
                 before_serializer, ready_transport_allocations);
    EXPECT_EQ (submitted, 5u);
    EXPECT_EQ (before_serializer, 0u);
    EXPECT_EQ (ready_transport_allocations, 0u);
    EXPECT_EQ (allocations, 1u);
}

TEST (SendAllocations, PendingNodeSendRetainsReplacedTransport)
{
    fw::serializer_registry_t serializers;
    serializers.add<route_payload_t> (
      [] (const auto &) { return fw::encoded_payload_t::from_string ("7"); },
      [] (const auto &) { return route_payload_t{}; }, "x/y");
    auto builder = fw::test::runtime_failure_builder ();
    auto runtime = detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    auto lifetime = std::make_shared<int> ();
    const std::weak_ptr<int> retained = lifetime;
    fw::task_completion_source_t<void> release;
    runtime.bind_mesh_node_transport (
      "m",
      [lifetime, &release] (const auto &, auto) -> fw::task_t<fw::result_t<void>> {
          (void) lifetime;
          co_await release.task ();
          co_return fw::result_t<void>::success ();
      },
      {});
    lifetime.reset ();
    auto client = builder.route_client (serializers);
    auto pending =
      client.send_to_node ("m", zlink::routing_id_t::from ("n"), route_payload_t{}).async ();
    EXPECT_FALSE (pending.await_ready ());
    runtime.bind_mesh_node_transport ("m", {}, {});
    EXPECT_FALSE (retained.expired ());
    ASSERT_TRUE (release.complete (fw::result_t<void>::success ()));
    EXPECT_TRUE (pending.result ());
}

TEST (SendAllocations, NodeDirectSendPreservesTransportExceptionClassification)
{
    enum class completion_t
    {
        synchronous_throw,
        ready_result,
        pending_result
    };
    enum class exception_t
    {
        framework,
        standard,
        nonstandard
    };
    struct nonstandard_failure_t
    {
        const void *origin;
    };
    const int origin = 7;
    for (const auto completion : {completion_t::synchronous_throw, completion_t::ready_result,
                                  completion_t::pending_result}) {
        for (const auto exception :
             {exception_t::framework, exception_t::standard, exception_t::nonstandard}) {
            for (const bool outer_failure : {false, true}) {
                if (completion == completion_t::synchronous_throw && outer_failure)
                    continue;
                SCOPED_TRACE (static_cast<int> (completion));
                SCOPED_TRACE (static_cast<int> (exception));
                SCOPED_TRACE (outer_failure);
                const auto failure = [&] {
                    switch (exception) {
                        case exception_t::framework:
                            return std::make_exception_ptr (fw::framework_exception_t (
                              fw::framework_error_kind_t::unavailable, "transport failure"));
                        case exception_t::standard:
                            return std::make_exception_ptr (
                              std::runtime_error ("transport failure"));
                        case exception_t::nonstandard:
                            return std::make_exception_ptr (nonstandard_failure_t{&origin});
                    }
                    return std::exception_ptr{};
                }();
                auto terminal = [&] {
                    if (outer_failure)
                        return detail::result_access_t::failure<fw::result_t<void>> (failure);
                    return fw::result_t<fw::result_t<void>>::success (
                      detail::result_access_t::failure<void> (failure));
                }();
                fw::serializer_registry_t serializers;
                serializers.add<route_payload_t> (
                  [] (const auto &) { return fw::encoded_payload_t::from_string ("7"); },
                  [] (const auto &) { return route_payload_t{}; }, "x/y");
                auto builder = fw::test::runtime_failure_builder ();
                auto runtime = detail::channel_runtime_t::from (builder.message_bus ());
                runtime.bind_serializers (serializers);
                fw::task_completion_source_t<fw::result_t<void>> release;
                unsigned submitted = 0;
                runtime.bind_mesh_node_transport (
                  "m",
                  [&] (const auto &, auto) -> fw::task_t<fw::result_t<void>> {
                      ++submitted;
                      if (completion == completion_t::synchronous_throw)
                          std::rethrow_exception (failure);
                      if (completion == completion_t::ready_result)
                          return fw::task_t<fw::result_t<void>> (std::move (terminal));
                      return release.task ();
                  },
                  {});
                auto client = builder.route_client (serializers);
                auto pending =
                  client.send_to_node ("m", zlink::routing_id_t::from ("n"), route_payload_t{})
                    .async ();
                ASSERT_EQ (submitted, 1u);
                EXPECT_EQ (pending.await_ready (), completion != completion_t::pending_result);
                if (completion == completion_t::pending_result)
                    ASSERT_TRUE (release.complete (std::move (terminal)));
                const auto &result = pending.result ();
                ASSERT_FALSE (result);
                if (exception == exception_t::nonstandard) {
                    EXPECT_EQ (result.error (), nullptr);
                    try {
                        result.value ();
                        FAIL () << "nonstandard transport exception was lost";
                    }
                    catch (const nonstandard_failure_t &error) {
                        EXPECT_EQ (error.origin, &origin);
                    }
                } else {
                    ASSERT_NE (result.error (), nullptr);
                    EXPECT_EQ (result.error_kind (),
                               exception == exception_t::framework
                                 ? fw::framework_error_kind_t::unavailable
                                 : fw::framework_error_kind_t::internal_failure);
                    EXPECT_STREQ (result.error ()->what (), "transport failure");
                }
            }
        }
    }
}

struct synchronous_route_handler_t
{
    unsigned calls = 0;
    void handle (const route_payload_t &) { ++calls; }
};

TEST (SendAllocations, SynchronousNodeHandlerDoesNotConstructSharedTaskStates)
{
    fw::serializer_registry_t serializers;
    serializers.add<route_payload_t> (
      [] (const route_payload_t &) { return fw::encoded_payload_t::from_string ("7"); },
      [] (const fw::encoded_payload_t &) { return route_payload_t{}; }, "x/y");
    fw::service_collection_t services;
    services.add_singleton<synchronous_route_handler_t> ();
    auto provider = services.build_provider ();
    auto builder = fw::test::runtime_failure_builder ();
    builder.add_route_mesh ("m")
      .add_route_send_handler<synchronous_route_handler_t, route_payload_t> ();
    const auto state = detail::mesh_node_runtime_t::registrations (builder).front ();
    const auto *handler =
      state->handlers.resolve ("m", fw::runtime::messaging::message_kind_t::command, "p");
    ASSERT_NE (handler, nullptr);
    auto message = zlink::message_t::from ("7");
    fw::route_message_context_t context{{}, zlink::routing_id_t::from ("source")};
    context.content_type = "x/y";
    const auto adapter = [&] {
        handler->invoker (provider, serializers, message, context, {}).result ().value ();
    };
    adapter ();
    const std::array state_sites{fw::tests::shared_state_allocation_site<
      detail::coroutine_task_shared_state_t<zlink::message_t>> ()};
    const auto adapter_allocations =
      measure_ready_operation ("synchronous node handler", state_sites, adapter);
    EXPECT_EQ (adapter_allocations, 0u);
    EXPECT_EQ (provider.get_required<synchronous_route_handler_t> ().calls, 2u);
}

struct retained_route_payload_t
{
    std::shared_ptr<int> value;
};

struct asynchronous_route_handler_t
{
    fw::task_completion_source_t<void> release;
    int observed = 0;

    fw::task_t<void> handle (const retained_route_payload_t &payload,
                             const fw::route_message_context_t &)
    {
        const auto lifetime = std::weak_ptr<int> (payload.value);
        co_await release.task ();
        if (!lifetime.expired ())
            observed = *payload.value;
    }
};

TEST (SendAllocations, AsynchronousMeshHandlerRetainsItsBorrowedPayloadUntilTerminal)
{
    std::weak_ptr<int> lifetime;
    fw::serializer_registry_t serializers;
    serializers.add<retained_route_payload_t> (
      [] (const auto &) { return fw::encoded_payload_t::from_string ("7"); },
      [&] (const fw::encoded_payload_t &) {
          auto value = std::make_shared<int> (7);
          lifetime = value;
          return retained_route_payload_t{std::move (value)};
      },
      "x/y");
    fw::service_collection_t services;
    services.add_singleton<asynchronous_route_handler_t> ();
    auto provider = services.build_provider ();
    auto builder = fw::test::runtime_failure_builder ();
    builder.add_route_mesh ("m")
      .add_route_send_handler<asynchronous_route_handler_t, retained_route_payload_t> ("owned");
    const auto state = detail::mesh_node_runtime_t::registrations (builder).front ();
    const auto *handler =
      state->handlers.resolve ("m", fw::runtime::messaging::message_kind_t::command, "owned");
    ASSERT_NE (handler, nullptr);
    auto message = zlink::message_t::from ("7");
    fw::route_message_context_t context{{}, zlink::routing_id_t::from ("source")};
    context.content_type = "x/y";
    auto pending = handler->invoker (provider, serializers, message, context, {});
    EXPECT_FALSE (pending.await_ready ());
    EXPECT_FALSE (lifetime.expired ());
    auto &owner = provider.get_required<asynchronous_route_handler_t> ();
    ASSERT_TRUE (owner.release.complete (fw::result_t<void>::success ()));
    pending.result ().value ();
    EXPECT_EQ (owner.observed, 7);
    EXPECT_TRUE (lifetime.expired ());
}

TEST (SendAllocations, TypedRouteEncoderUsesOnlyTheBorrowedFunctionContainer)
{
    fw::serializer_registry_t serializers;
    std::size_t before_serializer = 0;
    serializers.add<route_payload_t> (
      [&] (const route_payload_t &payload) {
          before_serializer = fw::tests::tracked_allocation_count;
          return fw::encoded_payload_t::from_string (std::to_string (payload.value));
      },
      [] (const fw::encoded_payload_t &) { return route_payload_t{}; }, "x/y");
    auto builder = fw::test::runtime_failure_builder ();
    auto runtime = detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    runtime.bind_mesh_node_transport ("m",
                                      [] (const auto &, auto) -> fw::task_t<fw::result_t<void>> {
                                          co_return fw::result_t<void>::success ();
                                      },
                                      {});
    auto client = builder.route_client (serializers);
    const auto target = zlink::routing_id_t::from ("n");
    const auto payload = std::make_shared<route_payload_t> ();
    const fw::dispatch_options_t dispatch;
    // Compare ready submission with the standard allocation-free reference_wrapper target.
    auto baseline_submit = [&] (const std::string &packet_name,
                                const auto &metadata) -> fw::task_t<void> {
        auto encode = [payload] (fw::serializer_registry_t &registry) {
            return registry.get<route_payload_t> ().serialize_with_content_type (*payload);
        };
        fw::route_client_t::payload_encoder_t borrowed (std::ref (encode));
        fw::runtime::messaging::client_call_codec_t codec;
        (void) codec.encode_node_send_parts ("m", packet_name, target, metadata, dispatch,
                                             serializers, borrowed);
        return fw::task_t<void> (fw::result_t<void>::success ());
    };
    const std::string packet_name = "p";
    const fw::route_send_call_t::metadata_map_t metadata;
    auto actual = client.send_to_node ("m", target, route_payload_t{});
    baseline_submit (packet_name, metadata).result ().value ();
    client.send_to_node ("m", target, route_payload_t{}).async ().result ().value ();
    (void) fw::tests::count_allocations (
      [&] { baseline_submit (packet_name, metadata).result ().value (); });
    const auto expected = before_serializer;
    EXPECT_EQ (expected, 0u);
    (void) fw::tests::count_allocations ([&] { actual.async ().result ().value (); });
    EXPECT_EQ (before_serializer, expected);
    std::printf ("route encoder allocations before serializer: baseline=%zu typed=%zu\n", expected,
                 before_serializer);
}

TEST (SendAllocations, TypedSendEncodesBeforePendingTransportAndPreservesItsFailure)
{
    for (const bool channel_send : {false, true}) {
        fw::serializer_registry_t serializers;
        unsigned encoded = 0;
        serializers.add<route_payload_t> (
          [&] (const route_payload_t &payload) {
              ++encoded;
              return fw::encoded_payload_t::from_string (std::to_string (payload.value));
          },
          [] (const fw::encoded_payload_t &) { return route_payload_t{}; }, "x/y");
        auto builder = fw::test::runtime_failure_builder ();
        auto runtime = detail::channel_runtime_t::from (builder.message_bus ());
        runtime.bind_serializers (serializers);
        fw::task_completion_source_t<void> release;
        const auto transport = [&] (auto) -> fw::task_t<fw::result_t<void>> {
            co_await release.task ();
            throw std::runtime_error ("delayed send failure");
            co_return fw::result_t<void>::success ();
        };
        runtime.bind_mesh_channel_transport ("c", transport, {});
        runtime.bind_mesh_node_transport (
          "m", [&] (const auto &, auto parts) { return transport (std::move (parts)); }, {});
        auto client = builder.route_client (serializers);
        auto call = channel_send ? client.send_to_channel ("c", route_payload_t{})
                                 : client.send_to_node ("m", zlink::routing_id_t::from ("n"),
                                                        route_payload_t{});
        auto pending = call.async ();
        EXPECT_EQ (encoded, 1u);
        EXPECT_FALSE (pending.await_ready ());
        EXPECT_TRUE (release.complete (fw::result_t<void>::success ()));
        const auto &result = pending.result ();
        EXPECT_EQ (result.error_kind (), fw::framework_error_kind_t::internal_failure);
        ASSERT_NE (result.error (), nullptr);
        EXPECT_STREQ (result.error ()->what (), "delayed send failure");
        EXPECT_EQ (encoded, 1u);
    }
}

std::shared_ptr<detail::mesh_node_builder_state_t> mesh_state ()
{
    auto state =
      fw::test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("send-allocation");
    state->core_context = std::make_shared<zlink::context_t> ();
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->listen_port.reset ();
    state->routing_id = zlink::routing_id_t::from ("source");
    return state;
}

TEST (SendAllocations, LocalRoutingIdentityUsesTheNativeStartUntilShutdown)
{
    const auto first = zlink::routing_id_t::from ("source");
    const auto next = zlink::routing_id_t::from ("next");
    auto state = mesh_state ();
    detail::mesh_node_runtime_t node (state);
    const auto check_identity = [&] (std::optional<zlink::routing_id_t> expected) {
        EXPECT_EQ (node.routing_id (), expected);
        EXPECT_EQ (node.routing_id_async ().result ().value (), expected);
        const auto identity = node.identity_async ().result ().value ();
        EXPECT_EQ (identity.first, "send-allocation");
        EXPECT_EQ (identity.second, expected);
    };
    check_identity (first);
    state->lane.run_sync ([&] { state->routing_id = next; });
    check_identity (next);
    state->lane.run_sync ([&] { state->routing_id = first; });
    node.start ();
    check_identity (first);
    state->lane.run_sync ([&] { state->routing_id = next; });
    check_identity (first);
    state->lane.run_sync ([&] { state->routing_id.reset (); });
    check_identity (first);
    state->lane.run_sync ([&] { state->routing_id = next; });
    EXPECT_EQ (node.native_node ().transport ().topology ().local_descriptor ().node_routing_id,
               first.to_bytes ());
    auto &topology = node.native_node ().transport ().topology ();
    auto changed_descriptor = topology.local_descriptor ();
    changed_descriptor.node_routing_id = next.to_bytes ();
    ++changed_descriptor.descriptor_revision;
    EXPECT_THROW (topology.publish_local (changed_descriptor), std::invalid_argument);
    EXPECT_EQ (topology.local_descriptor ().node_routing_id, first.to_bytes ());
    node.stop ();
    check_identity (next);
    state->lane.run_sync ([&] { state->routing_id.reset (); });
    check_identity (std::nullopt);
    state->lane.run_sync ([&] { state->routing_id = next; });
    node.start ();
    check_identity (next);
    EXPECT_EQ (node.native_node ().transport ().topology ().local_descriptor ().node_routing_id,
               next.to_bytes ());
    state->lane.run_sync ([&] { state->routing_id = first; });
    check_identity (next);
    node.stop ();
    check_identity (first);
}

TEST (SendAllocations, RunningIdentityDoesNotEnterTheConfigurationLane)
{
    auto state = mesh_state ();
    detail::mesh_node_runtime_t node (state);
    node.start ();
    state->lane.run_sync ([&] {
        EXPECT_NO_THROW (EXPECT_EQ (node.routing_id (), state->routing_id));
        EXPECT_NO_THROW (
          EXPECT_EQ (node.routing_id_async ().result ().value (), state->routing_id));
        EXPECT_NO_THROW (
          EXPECT_EQ (node.identity_async ().result ().value ().second, state->routing_id));
    });
    node.stop ();
}

TEST (SendAllocations, LocalSendIdentityReadsKeepOnlyTheReadyResultAllocations)
{
    auto state = mesh_state ();
    detail::mesh_node_runtime_t node (state);
    node.start ();
    const auto identity = node.routing_id ();
    using identity_task_t = fw::task_t<std::optional<zlink::routing_id_t>>;
    std::array<std::optional<identity_task_t>, 2> baseline_tasks, actual_tasks;
    // app.cpp's NodeDirect send decision and submit_local_node_send each read
    // routing_id_async. Hold the mutable configuration owner to expose any
    // identity turn those two real reads would otherwise execute inline.
    std::promise<void> entered, release;
    auto released = release.get_future ().share ();
    ASSERT_TRUE (state->lane.try_post ([&] {
        entered.set_value ();
        released.wait ();
    }));
    entered.get_future ().wait ();
    fw::test::scope_exit_t cleanup ([&] { release.set_value (); });
    const auto baseline = fw::tests::measure_allocations ([&] {
        for (auto &task : baseline_tasks)
            task.emplace (fw::result_t<std::optional<zlink::routing_id_t>>::success (identity));
    });
    const auto measured = fw::tests::measure_allocations ([&] {
        for (auto &task : actual_tasks)
            task.emplace (node.routing_id_async ());
    });
    const bool ready = actual_tasks[0]->await_ready () && actual_tasks[1]->await_ready ();
    cleanup.run ();
    for (auto &task : actual_tasks)
        EXPECT_EQ (task->result ().value (), identity);
    std::printf (
      "local send identity reads (2/message): baseline=%zu/%zu actual=%zu/%zu ready=%d\n",
      baseline.count, baseline.bytes, measured.count, measured.bytes, ready);
    EXPECT_TRUE (ready);
    EXPECT_EQ (measured.count, baseline.count);
    EXPECT_EQ (measured.bytes, baseline.bytes);
    node.stop ();
}

TEST (SendAllocations, KnownTargetForwardsTheNativeTaskWithoutAnotherResult)
{
    detail::mesh_node_runtime_t node (mesh_state ());
    node.start ();
    const auto target = zlink::routing_id_t::from ("target");
    auto peer = node.native_node ().transport ().topology ().local_descriptor ();
    peer.node_routing_id = target.to_bytes ();
    peer.advertised_endpoint = "tcp://127.0.0.1:1";
    ASSERT_EQ (node.native_node ().transport ().topology ().admit (peer, {1}),
               mesh::peer_admission_result_t::admitted);
    std::vector<zlink::message_t> parts;
    parts.push_back (zlink::message_t::from ("payload"));
    const auto native = [&] {
        (void) node.native_node ().transport ().classify_node_target (target.to_bytes ());
        return node.native_node ().send_to_node (target, parts).result ();
    };
    const auto forward = [&] {
        return node.send_to_node (target, parts, std::vector<std::uint8_t>{}).result ();
    };
    ASSERT_TRUE (native ());
    ASSERT_TRUE (forward ());
    std::optional<fw::result_t<zlink::submit_result_t>> expected;
    std::optional<fw::result_t<zlink::submit_result_t>> actual;
    const auto baseline = fw::tests::measure_allocations ([&] { expected.emplace (native ()); });
    const auto measured = fw::tests::measure_allocations ([&] { actual.emplace (forward ()); });
    ASSERT_TRUE (*expected);
    ASSERT_TRUE (*actual);
    EXPECT_EQ (expected->value (), zlink::submit_result_t::not_connected);
    EXPECT_EQ (actual->value (), expected->value ());
    EXPECT_EQ (measured.count, baseline.count);
    EXPECT_EQ (measured.bytes, baseline.bytes);
    std::printf ("known send allocations: native=%zu/%zu forwarded=%zu/%zu\n", baseline.count,
                 baseline.bytes, measured.count, measured.bytes);
    node.stop ();
}

class pending_listing_t final : public fw::runtime::in_memory_location_repository_t
{
  public:
    using page_t = fw::location_page_t<fw::mesh_node_descriptor_t>;
    fw::task_completion_source_t<page_t> completion;
    unsigned calls = 0;
    fw::task_t<page_t> list_mesh_nodes (std::string, fw::location_page_request_t = {}) override
    {
        ++calls;
        return completion.task ();
    }
};

TEST (SendAllocations, UnknownTargetPreservesLocationLookupAndFailureClassification)
{
    for (const bool move_parts : {false, true}) {
        for (const bool failed_listing : {false, true}) {
            detail::mesh_node_runtime_t node (mesh_state ());
            auto store = std::make_shared<pending_listing_t> ();
            node.configure_user_spot_operations (store, {});
            node.start ();
            const auto target = zlink::routing_id_t::from ("unknown");
            std::vector<zlink::message_t> parts;
            parts.push_back (zlink::message_t::from ("payload"));
            auto pending =
              move_parts
                ? node.send_to_node (target, std::move (parts), std::vector<std::uint8_t>{})
                : node.send_to_node (target, parts, std::vector<std::uint8_t>{});
            EXPECT_FALSE (pending.await_ready ());
            EXPECT_EQ (store->calls, 1u);
            EXPECT_TRUE (store->completion.complete (
              failed_listing ? fw::result_t<pending_listing_t::page_t>::failure (
                                 fw::framework_error_kind_t::unavailable, "listing unavailable")
                             : fw::result_t<pending_listing_t::page_t>::success ({})));
            const auto result = pending.result ();
            ASSERT_TRUE (result);
            EXPECT_EQ (result.value (), zlink::submit_result_t::not_connected);
            node.stop ();
        }
    }
}

TEST (SendAllocations, UnknownTargetKeepsValuesAndFailuresAcrossLocationSuspension)
{
    enum class scenario_t
    {
        replaced_target,
        cleared_parts,
        failed_listing
    };
    for (const bool move_parts : {false, true}) {
        for (const auto scenario :
             {scenario_t::replaced_target, scenario_t::cleared_parts, scenario_t::failed_listing}) {
            detail::mesh_node_runtime_t node (mesh_state ());
            auto store = std::make_shared<pending_listing_t> ();
            node.configure_user_spot_operations (store, {});
            node.start ();
            auto target = zlink::routing_id_t::from ("unknown");
            const auto submitted_target = target;
            std::vector<zlink::message_t> parts;
            parts.push_back (zlink::message_t::from ("payload"));
            auto pending =
              move_parts
                ? node.send_to_node (target, std::move (parts), std::vector<std::uint8_t>{})
                : node.send_to_node (target, parts, std::vector<std::uint8_t>{});
            EXPECT_FALSE (pending.await_ready ());
            EXPECT_EQ (store->calls, 1u);
            if (scenario == scenario_t::replaced_target)
                target = zlink::routing_id_t::from ("changed");
            else
                parts.clear ();
            pending_listing_t::page_t page;
            if (scenario == scenario_t::replaced_target) {
                fw::mesh_node_descriptor_t client;
                client.mesh_name = "send-allocation";
                client.rid = submitted_target;
                client.object_role = fw::object_role_t::client;
                page.items.push_back (std::move (client));
            }
            EXPECT_TRUE (store->completion.complete (
              scenario == scenario_t::failed_listing
                ? fw::result_t<pending_listing_t::page_t>::failure (
                    fw::framework_error_kind_t::unavailable, "listing unavailable")
                : fw::result_t<pending_listing_t::page_t>::success (std::move (page))));
            const auto result = pending.result ();
            ASSERT_TRUE (result);
            EXPECT_EQ (result.value (), scenario == scenario_t::replaced_target
                                          ? zlink::submit_result_t::not_found
                                          : zlink::submit_result_t::not_connected);
            node.stop ();
        }
    }
}

TEST (SendAllocations, StoppedNodeKeepsItsTaskFailureBoundary)
{
    detail::mesh_node_runtime_t node (mesh_state ());
    const auto target = zlink::routing_id_t::from ("target");
    const std::vector<zlink::message_t> parts;
    const auto result = node.send_to_node (target, parts, std::vector<std::uint8_t>{}).result ();
    EXPECT_FALSE (result);
    ASSERT_NE (result.error (), nullptr);
    EXPECT_EQ (result.error ()->kind (), fw::framework_error_kind_t::protocol_error);
}
TEST (SendAllocations, ReceiveTurnForwardsTheNativeTaskWithoutAnotherResult)
{
    detail::mesh_node_runtime_t node (mesh_state ());
    node.start ();
    const auto native = [&] {
        return node.native_node ()
          .dispatch_ready ([] (const auto &, const auto &, auto, auto) {}, false)
          .result ();
    };
    const auto forward = [&] {
        return node.dispatch_ready ([] (const auto &, const auto &, auto, auto) {}, false)
          .result ();
    };
    ASSERT_TRUE (native ());
    ASSERT_TRUE (forward ());
    std::optional<fw::result_t<std::size_t>> expected;
    std::optional<fw::result_t<std::size_t>> actual;
    const auto baseline = fw::tests::measure_allocations ([&] { expected.emplace (native ()); });
    const auto measured = fw::tests::measure_allocations ([&] { actual.emplace (forward ()); });
    ASSERT_TRUE (*expected);
    ASSERT_TRUE (*actual);
    EXPECT_EQ (expected->value (), 0u);
    EXPECT_EQ (actual->value (), expected->value ());
    EXPECT_EQ (measured.count, baseline.count);
    EXPECT_EQ (measured.bytes, baseline.bytes);
    std::printf ("receive forwarding allocations: native=%zu/%zu forwarded=%zu/%zu\n",
                 baseline.count, baseline.bytes, measured.count, measured.bytes);
    node.stop ();
}

TEST (SendAllocations, ReceiveTurnValidationKeepsItsTaskFailureBoundary)
{
    detail::mesh_node_runtime_t node (mesh_state ());
    node.start ();
    auto invalid = node.dispatch_ready ({}, false);
    EXPECT_TRUE (invalid.await_ready ());
    const auto &result = invalid.result ();
    ASSERT_FALSE (result);
    ASSERT_NE (result.error (), nullptr);
    EXPECT_EQ (result.error ()->kind (), fw::framework_error_kind_t::protocol_error);
    EXPECT_STREQ (result.error ()->what (), "MeshNode dispatch callback is required");
    node.stop ();
}

TEST (SendAllocations, ReceiveTurnRetainsCallbacksAcrossNativeSuspension)
{
    namespace host = fw::runtime::host;
    namespace stateful = fw::runtime::stateful;
    namespace wire = fw::runtime::protocol;
    for (const bool forwarded : {false, true}) {
        detail::mesh_node_runtime_t node (mesh_state ());
        fw::task_completion_source_t<std::optional<fw::location_owner_token_t>> owner;
        unsigned owner_calls = 0;
        unsigned bind_calls = 0;
        node.configure_bound_session_operations (
          {.session_owner =
             [&] (const auto &, auto) {
                 ++owner_calls;
                 return owner.task ();
             },
           .bind =
             [&] (const auto &, const auto &, auto, const auto &, auto) {
                 ++bind_calls;
                 return host::bound_session_bind_operation_result_t{
                   stateful::stateful_error_t::none, std::nullopt};
             },
           .send = [] (const auto &, auto) { return stateful::stateful_error_t::conflict; },
           .replaced = [] (const auto &) { ADD_FAILURE () << "unexpected Session replacement"; },
           .commit_relocation_route =
             [] (const auto &, const auto &, const auto &) {
                 ADD_FAILURE () << "unexpected relocation commit";
                 return false;
             },
           .prepare_relocation_target_route =
             [] (const auto &, auto) {
                 ADD_FAILURE () << "unexpected relocation preparation";
                 return false;
             }});
        node.start ();
        auto &runtime = node.native_node ();
        const auto descriptor = runtime.transport ().topology ().local_descriptor ();
        const stateful::object_ref_t actor{
          stateful::object_kind_t::actor,
          "actor",
          1,
          1,
          descriptor.mesh_name,
          zlink::routing_id_t::from (descriptor.node_routing_id).to_string ()};
        const auto created =
          fw::test::begin_reserved_object (runtime.objects (), actor, "actor", {});
        ASSERT_EQ (created.error, stateful::stateful_error_t::none);
        ASSERT_EQ (runtime.objects ().commit_create (created.attempt),
                   stateful::stateful_error_t::none);
        const wire::bound_session_bind_t bind{
          .correlation = 1,
          .actor = {.actor_id = "actor",
                    .object_generation = 1,
                    .target_node_routing_id = descriptor.node_routing_id,
                    .target_node_generation = descriptor.lifecycle_generation,
                    .authority_owner_generation = 1,
                    .owner_lease_generation = 1},
          .session_routing_id = {'s'},
          .binding = {.generation = 1}};
        mesh::service_mailbox_record_t record;
        record.owner = "control";
        record.domain = mesh::service_mailbox_domain_t::infrastructure;
        record.parts.push_back (zlink::message_t::from (wire::encode_bound_session_bind (bind)));
        record.source_routing_id = {'s'};
        record.source_node_generation = 1;
        ASSERT_TRUE (runtime.transport ().mailbox ().try_enqueue (std::move (record)));
        auto lifetime = std::make_shared<int> (1);
        std::weak_ptr<int> held = lifetime;
        auto callback = [lifetime] (const auto &, const auto &, auto, auto) {
            ADD_FAILURE () << "unexpected application callback";
        };
        auto next = [lifetime] { return false; };
        auto running = forwarded
                         ? node.dispatch_ready (std::move (callback), false, std::move (next))
                         : runtime.dispatch_ready (std::move (callback), false, std::move (next));
        lifetime.reset ();
        EXPECT_FALSE (running.await_ready ());
        EXPECT_EQ (owner_calls, 1u);
        EXPECT_EQ (bind_calls, 0u);
        EXPECT_FALSE (held.expired ());
        EXPECT_TRUE (
          owner.complete (fw::result_t<std::optional<fw::location_owner_token_t>>::success (
            fw::location_owner_token_t{"session-owner", 1})));
        const auto settled = running.result ();
        ASSERT_TRUE (settled);
        EXPECT_EQ (settled.value (), 1u);
        EXPECT_EQ (bind_calls, 1u);
        EXPECT_TRUE (runtime.transport ().mailbox ().try_claim_owner (
                       mesh::service_mailbox_domain_t::infrastructure, "control", 1,
                       std::numeric_limits<std::size_t>::max ())
                     == nullptr);
        node.stop ();
    }
}
} // namespace

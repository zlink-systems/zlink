/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <gtest/gtest.h>
#include "allocation_counter.hpp"
#include "creation_counter.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/foundation/operation_registry.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/locations/spot_address_resolvers.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include <zlink/framework.hpp>
#include <thread>
#include <atomic>
#include "runtime_failure_fixture.hpp"
#include <array>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <latch>
#include <optional>
#include <string>
#include <span>
#include <variant>

namespace zlink::framework::runtime::host
{
struct public_host_runtime_test_access_t
{
    static void register_local_actor_request (
      public_host_runtime_t &host,
      pending_operation_t &operation,
      const mesh::raw_mesh_node_owner_t::local_operation_reservation_t &reservation)
    {
        (void) host.register_local_completion (operation, reservation, std::chrono::seconds (5), {},
                                               {}, mesh_request_surface_t::actor);
    }
};
}

#if defined(ZLINK_TEST_COUNT_REPLY_NOTIFICATIONS)
namespace
{
thread_local std::size_t *reply_notifications = nullptr;
}
extern "C" void __real__ZNSt18condition_variable10notify_allEv (void *condition);
extern "C" void __wrap__ZNSt18condition_variable10notify_allEv (void *condition)
{
    if (reply_notifications != nullptr)
        ++*reply_notifications;
    __real__ZNSt18condition_variable10notify_allEv (condition);
}
#endif

namespace
{
namespace fw = zlink::framework;
using parts_t = fw::runtime::messaging::message_parts_t;
using transport_reply_t = fw::result_t<parts_t>;

struct payload_t
{
    static constexpr const char *packet_name = "p";
    int value = 7;
};

void add_payload (fw::serializer_registry_t &serializers)
{
    serializers.add<payload_t> (
      [] (const payload_t &payload) {
          return fw::encoded_payload_t::from_string (std::to_string (payload.value));
      },
      [] (const fw::encoded_payload_t &payload) {
          return payload_t{std::stoi (payload.to_string ())};
      },
      "x/y");
}

struct allocation_sample_t
{
    std::array<fw::tests::allocation_event_t, 256> events{};
    std::size_t count = 0;
    std::size_t total = 0;
    std::string diagnostics;
};

template <typename TAction> allocation_sample_t sample_allocations (TAction &&action)
{
    allocation_sample_t sample;
    fw::tests::allocation_event_log_t log{sample.events.data (), sample.events.size ()};
    {
        fw::tests::allocation_event_scope_t scope (log);
        sample.total = fw::tests::count_allocations (std::forward<TAction> (action));
    }
    EXPECT_FALSE (log.overflow);
    sample.count = log.count;
    sample.diagnostics = fw::tests::describe_allocations (log);
    return sample;
}

void print_allocation_sizes (const char *label, const allocation_sample_t &sample)
{
    std::printf ("%s: total=%zu sizes=", label, sample.total);
    for (std::size_t i = 0; i < sample.count; ++i)
        std::printf ("%s%zu", i == 0 ? "" : ",", sample.events[i].size);
    std::printf ("\n");
}

// Locate a live result in the last recorded allocation containing its address.
// Earlier temporary allocations may have reused the same storage. No object
// representation or allocator size class is used to identify the reply.
std::optional<std::size_t> allocation_containing (const allocation_sample_t &sample,
                                                  const void *address)
{
    const std::less<const void *> before;
    for (std::size_t i = sample.count; i > 0; --i) {
        const auto &event = sample.events[i - 1];
        const auto *end = static_cast<const unsigned char *> (event.memory) + event.size;
        if (!before (address, event.memory) && before (address, end))
            return i - 1;
    }
    return std::nullopt;
}

void expect_public_and_physical_reply_allocations (const allocation_sample_t &sample,
                                                   const fw::request_task_t<payload_t> &pending,
                                                   bool coallocated = false)
{
    auto physical = fw::detail::request_task_access_t::physical_reply (pending);
    ASSERT_TRUE (physical.await_ready ());
    const auto public_allocation =
      allocation_containing (sample, std::addressof (pending.result ()));
    const auto physical_allocation =
      allocation_containing (sample, std::addressof (physical.result ()));
    ASSERT_TRUE (public_allocation.has_value ());
    ASSERT_TRUE (physical_allocation.has_value ());
    EXPECT_NE (std::addressof (pending.result ()), std::addressof (physical.result ()));
    if (coallocated) {
        EXPECT_EQ (*public_allocation, *physical_allocation);
        const auto admission = fw::detail::request_task_access_t::admission_source (pending);
        EXPECT_EQ (allocation_containing (sample, admission.get ()), public_allocation);
    } else {
        EXPECT_NE (*public_allocation, *physical_allocation);
    }
}

TEST (RequestCallAllocations, PreparedReceiverAndCodecCapturesUseOneOwnershipBlock)
{
    std::optional<fw::detail::request_receiver_t<int>> terminal;
    std::optional<fw::request_task_t<int>> pending;
    const auto allocation = sample_allocations ([&] {
        pending.emplace (
          fw::detail::request_task_access_t::receive_public<int> ([&] (auto receiver) {
              auto first = receiver.template project<int> ([] (const fw::result_t<int> &value) {
                  return fw::result_t<int>::success (value.value () + 1);
              });
              terminal.emplace (first.template project<int> ([] (const fw::result_t<int> &value) {
                  return fw::result_t<int>::success (value.value () * 2);
              }));
          }));
    });
    EXPECT_EQ (allocation.total, 1u) << allocation.diagnostics;
    terminal->admitted->complete (fw::result_t<void>::success ());
    terminal->complete (fw::result_t<int>::success (20));
    EXPECT_EQ (pending->result ().value (), 41);
    const std::weak_ptr<fw::task_completion_source_t<void>> ownership =
      fw::detail::request_task_access_t::admission_source (*pending);
    {
        auto physical = fw::detail::request_task_access_t::physical_reply (*pending);
        EXPECT_EQ (physical.result ().value (), 41);
        auto admitted = pending->admitted ();
        const auto public_block = allocation_containing (allocation, &pending->result ());
        EXPECT_EQ (public_block, allocation_containing (allocation, &physical.result ()));
        EXPECT_EQ (public_block, allocation_containing (allocation, &admitted.result ()));
    }
    terminal.reset ();
    pending.reset ();
    EXPECT_TRUE (ownership.expired ());
    std::printf ("prepared request ownership allocations=%zu\n", allocation.total);
}

TEST (RequestCallAllocations, LocalActorRequestCompletionRegistrationDoesNotExceedBaselineHeapCalls)
{
    using host_t = fw::runtime::host::public_host_runtime_t;
    using operation_t = fw::runtime::host::pending_operation_t;
    using parts_reply_t = fw::result_t<parts_t>;

    fw::runtime::host::host_options_t options;
    options.mesh = fw::test::runtime_failure_options (options.mesh);
    options.mesh.descriptor = {"request-allocation",
                               {'r'},
                               1,
                               1,
                               "tcp://127.0.0.1:0",
                               {{"actor", 100}},
                               fw::runtime::mesh::service_node_state_t::preparing};
    options.mesh.descriptor.lifecycle_generation = 1;
    auto host = std::make_shared<host_t> (std::move (options));
    host->start ();

    const auto prepared = host->transport ().prepare_local_operation_async ().result ();
    ASSERT_TRUE (prepared);
    ASSERT_TRUE (prepared.value ());
    operation_t operation;
    std::optional<fw::request_task_t<parts_reply_t>> pending;
    const auto sample = sample_allocations ([&] {
        pending.emplace (fw::detail::request_task_access_t::receive_public<parts_reply_t> (
          [&operation] (auto receiver) {
              operation.receiver = [receiver] (const auto &) mutable {
                  receiver.complete (fw::result_t<parts_reply_t>::success (parts_reply_t::failure (
                    fw::framework_error_kind_t::unavailable, "allocation probe terminal")));
              };
          }));
        fw::runtime::host::public_host_runtime_test_access_t::register_local_actor_request (
          *host, operation, *prepared.value ());
    });
    print_allocation_sizes ("local Actor request completion registration", sample);
    // 5ecae5de8b used eight heap calls for this measured local Actor path.
    EXPECT_LE (sample.total, 8u) << sample.diagnostics;
    EXPECT_FALSE (operation.receiver);

    EXPECT_TRUE (host->transport ().fail_local_operation (
      prepared.value ()->first, fw::runtime::foundation::operation_terminal_t::transport_failed));
    const auto terminal_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (!pending->await_ready () && std::chrono::steady_clock::now () < terminal_deadline)
        std::this_thread::yield ();
    ASSERT_TRUE (pending->await_ready ());
    ASSERT_TRUE (pending->result ());
    EXPECT_FALSE (pending->result ().value ());
    host->close ();
}

TEST (RequestCallAllocations, PhysicalProjectionReleasesConstCapturesWithTaskHandlesRetained)
{
    std::weak_ptr<int> lifetime;
    std::optional<fw::detail::request_receiver_t<int>> terminal;
    std::optional<fw::request_task_t<int>> pending;
    {
        const auto marker = std::make_shared<int> (42);
        lifetime = marker;
        pending.emplace (
          fw::detail::request_task_access_t::receive_public<int> ([&] (auto receiver) {
              terminal.emplace (receiver.template project<int> (
                [marker] (const auto &) { return fw::result_t<int>::success (*marker); }));
          }));
    }
    ASSERT_FALSE (lifetime.expired ());
    terminal->admitted->complete (fw::result_t<void>::failure (
      fw::framework_error_kind_t::unavailable, "public request ended first"));
    ASSERT_FALSE (lifetime.expired ());
    terminal->complete (fw::result_t<int>::success (0));
    EXPECT_TRUE (lifetime.expired ());
    EXPECT_EQ (pending->result ().error_kind (), fw::framework_error_kind_t::unavailable);
    EXPECT_EQ (fw::detail::request_task_access_t::physical_reply (*pending).result ().value (), 42);
}

TEST (RequestCallAllocations, ThrowBeforeRegistrationReleasesUnusedProjectionWithHandlesRetained)
{
    std::weak_ptr<int> lifetime;
    auto pending = [&] {
        const auto marker = std::make_shared<int> (42);
        lifetime = marker;
        return fw::detail::request_task_access_t::receive_public<int, int> (
          [&] (auto receiver) {
              auto physical = receiver.template project<long> (
                [marker] (const auto &) { return fw::result_t<int>::success (*marker); });
              throw std::runtime_error ("before registration");
          },
          [marker] (const auto &) { return fw::result_t<int>::success (*marker); });
    }();
    auto physical = fw::detail::request_task_access_t::physical_reply (pending);
    auto public_reply = fw::detail::request_task_access_t::public_reply (pending);
    EXPECT_FALSE (pending.result ());
    EXPECT_FALSE (public_reply.result ());
    EXPECT_FALSE (physical.result ());
    EXPECT_TRUE (lifetime.expired ());
}

TEST (RequestCallAllocations, SubmissionExceptionKeepsEscapedPhysicalProjectionAlive)
{
    std::weak_ptr<int> lifetime;
    std::optional<fw::detail::request_receiver_t<long>> terminal;
    auto pending = [&] {
        const auto marker = std::make_shared<int> (42);
        lifetime = marker;
        return fw::detail::request_task_access_t::receive_public<int> ([&] (auto receiver) {
            terminal.emplace (receiver.template project<long> (
              [marker] (const auto &) { return fw::result_t<int>::success (*marker); }));
            throw std::runtime_error ("physical producer already owns receiver");
        });
    }();
    EXPECT_FALSE (pending.result ());
    EXPECT_FALSE (lifetime.expired ());
    terminal->complete (fw::result_t<long>::success (0));
    EXPECT_TRUE (lifetime.expired ());
}

TEST (RequestCallAllocations, LastCallbackOwnerReleasesUnusedProjectionWithPublicHandleRetained)
{
    std::weak_ptr<int> lifetime;
    std::optional<fw::detail::request_receiver_t<long>> terminal;
    auto pending = [&] {
        const auto marker = std::make_shared<int> (42);
        lifetime = marker;
        return fw::detail::request_task_access_t::receive_public<int> ([&] (auto receiver) {
            terminal.emplace (receiver.template project<long> (
              [marker] (const auto &) { return fw::result_t<int>::success (*marker); }));
        });
    }();
    terminal->admitted->complete (fw::result_t<void>::failure (
      fw::framework_error_kind_t::unavailable, "no physical producer remains"));
    EXPECT_FALSE (pending.result ());
    EXPECT_FALSE (lifetime.expired ());
    auto copy = *terminal;
    terminal.reset ();
    EXPECT_FALSE (lifetime.expired ());
    copy = {};
    EXPECT_TRUE (lifetime.expired ());
}

TEST (RequestCallAllocations, PreparedReceiverKeepsSeveralIndependentObservers)
{
    fw::completion_owner_t owner;
    std::optional<fw::detail::request_receiver_t<int>> terminal;
    auto pending = fw::detail::request_task_access_t::receive_public<int> (
      [&] (auto receiver) { terminal.emplace (std::move (receiver)); });
    auto first = fw::detail::request_task_access_t::public_reply (pending);
    auto second = fw::detail::request_task_access_t::public_reply (pending);
    std::latch delivered (2);
    std::atomic<unsigned> calls{0};
    std::atomic<unsigned> failures{0};
    const auto register_observer = [&] (auto &task) {
        fw::detail::observe_task_completion (task, [&] (const auto &result) {
            if (!result || result.value () != 42)
                ++failures;
            ++calls;
            delivered.count_down ();
        });
    };
    register_observer (first);
    register_observer (second);
    terminal->admitted->complete (fw::result_t<void>::success ());
    terminal->complete (fw::result_t<int>::success (42));
    delivered.wait ();
    owner.close ();
    EXPECT_EQ (calls.load (), 2u);
    EXPECT_EQ (failures.load (), 0u);
    EXPECT_EQ (pending.result ().value (), 42);
    EXPECT_EQ (first.result ().value (), 42);
    EXPECT_EQ (second.result ().value (), 42);
}

TEST (RequestCallAllocations, ShutdownDrainsPendingPreparedReceiversAndTheirObservers)
{
    fw::completion_owner_t owner;
    fw::runtime::foundation::operation_registry_t registry;
    std::vector<fw::request_task_t<int>> requests;
    std::latch delivered (16);
    std::atomic<unsigned> observed{0};
    std::atomic<unsigned> wrong_turn{0};
    const auto notification_thread = std::this_thread::get_id ();
    for (unsigned index = 0; index < 16; ++index) {
        requests.push_back (
          fw::detail::request_task_access_t::receive_public<int> ([&] (auto receiver) {
              const fw::runtime::foundation::call_id_t id{77, index + 1};
              auto completion =
                receiver.complete.template capture<fw::runtime::foundation::operation_terminal_t,
                                                   fw::result_t<zlink::message_t>> (
                  [receiver = receiver.borrow ()] (auto terminal, auto) {
                      EXPECT_EQ (terminal, fw::runtime::foundation::operation_terminal_t::shutdown);
                      receiver.complete (fw::result_t<int>::failure (
                        fw::framework_error_kind_t::shutting_down, "request owner stopped"));
                  });
              ASSERT_TRUE (registry.register_operation (
                id, fw::runtime::foundation::operation_registry_t::clock_t::time_point::max (),
                fw::runtime::foundation::operation_registry_t::callback_t (std::move (completion))
                  .with_admission (receiver.admitted)));
          }));
        fw::detail::observe_task_completion (
          requests.back (), [&, notification_thread] (const auto &result) {
              EXPECT_EQ (result.error_kind (), fw::framework_error_kind_t::shutting_down);
              if (std::this_thread::get_id () == notification_thread)
                  ++wrong_turn;
              ++observed;
              delivered.count_down ();
          });
    }
    EXPECT_EQ (registry.shutdown (), 16u);
    delivered.wait ();
    owner.close ();
    EXPECT_EQ (observed.load (), 16u);
    EXPECT_EQ (wrong_turn.load (), 0u);
    for (auto &pending : requests) {
        EXPECT_EQ (pending.result ().error_kind (), fw::framework_error_kind_t::shutting_down);
        EXPECT_TRUE (fw::detail::request_task_access_t::physical_reply (pending).await_ready ());
        EXPECT_TRUE (pending.admitted ().await_ready ());
    }
    EXPECT_EQ (registry.shutdown (), 0u);
}

using node_transport_t =
  std::variant<std::monostate,
               std::shared_ptr<fw::detail::route_channel_runtime_t>,
               fw::detail::channel_runtime_state_t::mesh_node_transport_ptr_t>;
using channel_transport_t =
  std::variant<fw::detail::channel_runtime_state_t::client_server_request_t,
               fw::detail::channel_runtime_state_t::mesh_channel_request_t>;

// Minimal references compose existing request primitives. Their value argument
// boundaries and capture types match the owning runtime layers so the current
// standard library measures its own map heads and std::function storage. They
// do not duplicate routing, decoding, admission or completion decisions.
void encode_request_primitive (fw::serializer_registry_t &serializers,
                               std::string channel_name,
                               std::string packet_name,
                               std::chrono::milliseconds timeout,
                               fw::channel_request_call_t::metadata_map_t metadata)
{
    fw::runtime::messaging::client_call_codec_t codec;
    auto header = codec.create_envelope (fw::runtime::messaging::message_kind_t::request,
                                         channel_name, packet_name, timeout);
    header.metadata = std::move (metadata);
    (void) codec.encode_payload_parts (
      std::move (header), serializers.get<payload_t> ().serialize_with_content_type (payload_t{}));
}

allocation_sample_t encode_allocations (fw::serializer_registry_t &serializers,
                                        const std::string &channel_name)
{
    const fw::channel_request_call_t::metadata_map_t metadata;
    const std::string packet_name = payload_t::packet_name;
    return sample_allocations ([&] {
        encode_request_primitive (serializers, channel_name, packet_name,
                                  fw::runtime::configuration::default_request_timeout, metadata);
    });
}

allocation_sample_t channel_projection_allocations (channel_transport_t channel_transport)
{
    fw::task_completion_source_t<transport_reply_t> source;
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    admitted->complete (fw::result_t<void>::success ());
    // The transport fixture has already registered its public-reply observer.
    // Preserve that vector population before measuring the next registration.
    fw::request_task_t<transport_reply_t> selected (source.task (), admitted);
    std::optional<fw::request_task_t<zlink::message_t>> projected;
    const std::shared_ptr<fw::detail::route_client_state_t> state;
    std::string packet_name = payload_t::packet_name;
    std::string channel_name = "c";
    return sample_allocations ([&] {
        projected.emplace (fw::detail::project_request_reply<zlink::message_t> (
          selected,
          [state, transport = std::move (channel_transport), packet_name = std::move (packet_name),
           channel_name = std::move (channel_name)] (
            const fw::result_t<transport_reply_t> &) -> fw::result_t<zlink::message_t> {
              (void) state;
              (void) transport;
              (void) packet_name;
              (void) channel_name;
              return fw::result_t<zlink::message_t>::success (zlink::message_t{});
          }));
    });
}

allocation_sample_t public_projection_allocations (fw::serializer_registry_t &registry)
{
    fw::task_completion_source_t<zlink::message_t> source;
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    admitted->complete (fw::result_t<void>::success ());
    auto selected =
      fw::detail::request_task_access_t::from_admitted_reply (source.task (), admitted);
    std::optional<fw::request_task_t<payload_t>> projected;
    return sample_allocations ([&] {
        projected.emplace (fw::detail::public_request_reply<payload_t> (
          selected,
          [serializers =
             &registry] (const fw::result_t<zlink::message_t> &) -> fw::result_t<payload_t> {
              (void) serializers;
              return fw::result_t<payload_t>::success (payload_t{});
          }));
    });
}

allocation_sample_t node_receiver_allocations (fw::serializer_registry_t &registry)
{
    fw::task_completion_source_t<transport_reply_t> physical;
    std::optional<fw::request_task_t<payload_t>> pending;
    const std::shared_ptr<fw::detail::route_client_state_t> state;
    node_transport_t transport;
    std::string packet_name = payload_t::packet_name;
    std::string router_channel_id = "m";
    const auto target_owner = zlink::routing_id_t::from ("n");
    return sample_allocations ([&] {
        pending.emplace (
          fw::detail::request_task_access_t::receive_public<payload_t, zlink::message_t> (
            [&] (auto receiver) {
                auto parts = receiver.template project<transport_reply_t> (
                  [state, transport, packet_name = std::move (packet_name),
                   router_channel_id = std::move (router_channel_id),
                   target_owner] (const fw::result_t<transport_reply_t> &) {
                      (void) state;
                      (void) transport;
                      (void) packet_name;
                      (void) router_channel_id;
                      (void) target_owner;
                      return fw::result_t<zlink::message_t>::success (zlink::message_t{});
                  });
                parts.admitted->complete (fw::result_t<void>::success ());
                auto reply = physical.task ();
                fw::detail::observe_task_terminal (reply, std::move (parts.complete));
            },
            [serializers = &registry] (const fw::result_t<zlink::message_t> &) {
                (void) serializers;
                return fw::result_t<payload_t>::success (payload_t{});
            }));
    });
}

allocation_sample_t channel_selection_allocations (
  const fw::detail::channel_runtime_state_t::mesh_channel_request_t &requester)
{
    fw::runtime::offload_executor_t executor (1, "request-allocation-reference");
    fw::runtime::state_lane_t lane (executor);
    const std::string channel_name = "c";
    const std::map<std::string, fw::detail::channel_runtime_state_t::client_server_request_t>
      client_server_transports;
    const std::map<std::string, fw::detail::channel_runtime_state_t::mesh_channel_request_t>
      route_mesh_transports{{channel_name, requester}};
    return sample_allocations ([&] {
        (void) lane
          .run_checked ([&] () -> std::optional<channel_transport_t> {
              const auto client_server_transport = client_server_transports.find (channel_name);
              if (client_server_transport != client_server_transports.end ())
                  return channel_transport_t (std::in_place_index<0>,
                                              client_server_transport->second);
              const auto route_mesh_transport = route_mesh_transports.find (channel_name);
              if (route_mesh_transport != route_mesh_transports.end ())
                  return channel_transport_t (std::in_place_index<1>, route_mesh_transport->second);
              return std::nullopt;
          })
          .get ();
    });
}

parts_t reply_parts (int value)
{
    fw::runtime::messaging::client_call_codec_t codec;
    auto header = codec.create_envelope (fw::runtime::messaging::message_kind_t::response, "m", "p",
                                         std::chrono::milliseconds (1000));
    return codec.encode_payload_parts (
      std::move (header),
      fw::serialized_payload_t{fw::encoded_payload_t::from_string (std::to_string (value)), "x/y"});
}

/* Submit/completion §9 and hot path E4·E5: a request is one operation with one
 * terminal. Above the transport the call decodes that terminal into the
 * envelope body and then into the reply type, once each, and shares the
 * transport's admission source. The public reply adds its own state, which
 * ends at an admission failure before the physical terminal, and the
 * observer of that admission. No coroutine or task wraps a layer again. */
TEST (RequestCallAllocations, NodeReplyConstructsOnlyFinalPublicAndPhysicalResultStates)
{
    fw::serializer_registry_t serializers;
    add_payload (serializers);
    auto builder = fw::test::runtime_failure_builder ();
    auto channel = fw::detail::channel_runtime_t::from (builder.message_bus ());
    channel.bind_serializers (serializers);
    auto state = fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> (
      "result-chain-count");
    fw::detail::mesh_node_runtime_t mesh (state);
    fw::runtime::host::pending_operation_t operation;
    auto submitted = fw::task_t<zlink::submit_result_t> (
      fw::result_t<zlink::submit_result_t>::success (zlink::submit_result_t::ok));
    const zlink::routing_id_t *borrowed_target = nullptr;
    channel.bind_mesh_node_transport ("m", {}, [&] (const auto &target, auto, auto, auto receiver) {
        borrowed_target = &target;
        mesh.compose_node_request_reply (operation, std::move (submitted), std::move (receiver));
    });
    auto client = builder.route_client (serializers);
    auto call = client.request_to_node ("m", zlink::routing_id_t::from ("n"), payload_t{});
    const auto before = fw::tests::creation_counts ();
    std::optional<fw::request_task_t<payload_t>> pending;
    const auto sample = sample_allocations ([&] { pending.emplace (call.async<payload_t> ()); });
    const auto created = fw::tests::creation_counts () - before;
    const auto only_request_stages = [] (const auto &counts) {
        // Independent admission, public reply and physical reply; no conversion state.
        return counts.shared_states == 3 && counts.coroutine_states == 0;
    };
    EXPECT_TRUE (only_request_stages (created)) << sample.diagnostics;
    ASSERT_NE (borrowed_target, nullptr);
    const auto target_allocation = allocation_containing (sample, borrowed_target);
    ASSERT_TRUE (target_allocation.has_value ());
    EXPECT_EQ (borrowed_target->to_string (), "n");
    // The creation guard must reject a genuine additional conversion state,
    // even when existing request stages share their backing allocation.
    const auto injected =
      std::make_shared<fw::detail::application_task_shared_state_t<transport_reply_t>> ();
    const auto with_conversion = fw::tests::creation_counts () - before;
    EXPECT_FALSE (only_request_stages (with_conversion));
    std::printf ("node request stage objects=%zu injected=%zu allocations=%zu\n",
                 created.shared_states, with_conversion.shared_states, sample.total);
    fw::runtime::host::operation_completion_t reply;
    reply.record.terminal_result = static_cast<int> (zlink::request_result_t::ok);
    reply.parts = std::move (reply_parts (41)).take_items ();
#if defined(ZLINK_TEST_COUNT_REPLY_NOTIFICATIONS)
    std::size_t notifications = 0;
    reply_notifications = &notifications;
#endif
    operation.completion->complete (
      fw::result_t<fw::runtime::host::operation_completion_t>::success (std::move (reply)));
#if defined(ZLINK_TEST_COUNT_REPLY_NOTIFICATIONS)
    reply_notifications = nullptr;
    std::printf ("node terminal notify_all calls/request=%zu\n", notifications);
    EXPECT_EQ (notifications, 3u); // Host terminal, final physical, final public.
#endif
    ASSERT_TRUE (pending->result ());
    EXPECT_EQ (pending->result ().value ().value, 41);
    expect_public_and_physical_reply_allocations (sample, *pending, true);
    EXPECT_EQ (target_allocation, allocation_containing (sample, &pending->result ()));
}

TEST (RequestCallAllocations, NodeProjectionKeepsSubmitTargetWhenInlineReplyReleasesDecoder)
{
    fw::serializer_registry_t serializers;
    add_payload (serializers);
    auto builder = fw::test::runtime_failure_builder ();
    auto channel = fw::detail::channel_runtime_t::from (builder.message_bus ());
    channel.bind_serializers (serializers);
    unsigned submissions = 0;
    channel.bind_mesh_node_transport ("m", {}, [&] (const auto &target, auto, auto, auto receiver) {
        EXPECT_EQ (target.to_string (), "n");
        receiver.admitted->complete (fw::result_t<void>::success ());
        receiver.complete (
          fw::result_t<transport_reply_t>::success (transport_reply_t::success (reply_parts (41))));
        // Completion destroys the request decoder while submit is still active.
        EXPECT_EQ (target.to_string (), "n");
        ++submissions;
    });
    auto pending = builder.route_client (serializers)
                     .request_to_node ("m", zlink::routing_id_t::from ("n"), payload_t{})
                     .async<payload_t> ();
    EXPECT_EQ (pending.result ().value ().value, 41);
    EXPECT_EQ (submissions, 1u);
}

TEST (RequestCallAllocations, RouteNodeProjectionDecodesPhysicalReplyAfterEarlyPublicFailure)
{
    fw::serializer_registry_t serializers;
    unsigned decoded = 0;
    serializers.add<payload_t> (
      [] (const payload_t &payload) {
          return fw::encoded_payload_t::from_string (std::to_string (payload.value));
      },
      [&] (const fw::encoded_payload_t &payload) {
          ++decoded;
          return payload_t{std::stoi (payload.to_string ())};
      },
      "x/y");
    auto builder = fw::test::runtime_failure_builder ();
    builder.route_channel ("m");
    auto manager = fw::detail::channel_runtime_manager_t::from (builder);
    manager.initialize_route_channels (builder);
    auto &route = manager.get_route_channel ("m");
    fw::task_completion_source_t<parts_t> terminal;
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    route.set_request_backend ([&] (const auto &target, const auto &, const auto &, auto) {
        EXPECT_EQ (target.to_string (), "n");
        return fw::request_task_t<parts_t> (terminal.task (), admitted);
    });
    auto pending = builder.route_client (serializers)
                     .request_to_node ("m", zlink::routing_id_t::from ("n"), payload_t{})
                     .async<payload_t> ();
    auto physical = fw::detail::request_task_access_t::physical_reply (pending);
    admitted->complete (fw::result_t<void>::failure (fw::framework_error_kind_t::unavailable,
                                                     "admission ended first"));
    EXPECT_EQ (pending.result ().error_kind (), fw::framework_error_kind_t::unavailable);
    EXPECT_FALSE (physical.await_ready ());
    terminal.complete (fw::result_t<parts_t>::success (reply_parts (51)));
    EXPECT_EQ (physical.result ().value ().value, 51);
    EXPECT_EQ (decoded, 1u);
    EXPECT_EQ (pending.result ().error_kind (), fw::framework_error_kind_t::unavailable);
    route.stop ();
}

TEST (RequestCallAllocations, NodeRequestUsesOnlyItsPreparedReceiverAboveTheTransport)
{
    fw::serializer_registry_t serializers;
    add_payload (serializers);
    auto builder = fw::test::runtime_failure_builder ();
    auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    fw::task_completion_source_t<transport_reply_t> physical;
    std::shared_ptr<fw::task_completion_source_t<void>> admitted;
    unsigned submitted = 0;
    runtime.bind_mesh_node_transport ("m", {}, [&] (const auto &, auto, auto, auto receiver) {
        ++submitted;
        admitted = receiver.admitted;
        admitted->complete (fw::result_t<void>::success ());
        auto reply = physical.task ();
        fw::detail::observe_task_terminal (reply, std::move (receiver.complete));
    });
    auto client = builder.route_client (serializers);
    const auto encode_node = [&] {
        return sample_allocations ([&] {
            fw::runtime::messaging::client_call_codec_t codec;
            auto header = codec.create_envelope (
              fw::runtime::messaging::message_kind_t::request, "m", payload_t::packet_name,
              fw::runtime::configuration::default_request_timeout, std::nullopt, std::nullopt, {});
            const auto serializer = serializers.get<payload_t> ();
            (void) codec.encode_payload_parts (
              std::move (header), serializer.serialize (payload_t{}), serializer.content_type ());
        });
    };
    const auto first_encode = encode_node ().total;
    const auto encode = encode_node ();
    std::printf ("encode calibration: first=%zu steady=%zu\n", first_encode, encode.total);
    // The receiver's projection owns the fixed target through the physical terminal.
    const auto target_owner = sample_allocations ([] {
        const auto target = zlink::routing_id_t::from ("n");
        const auto copy = target;
        (void) copy;
    });
    const auto receiver = node_receiver_allocations (serializers);
    print_allocation_sizes ("node encode reference", encode);
    print_allocation_sizes ("node target reference", target_owner);
    print_allocation_sizes ("node receiver reference", receiver);
    SCOPED_TRACE ("node encode reference\n" + encode.diagnostics);
    SCOPED_TRACE ("node target reference\n" + target_owner.diagnostics);
    SCOPED_TRACE ("node receiver reference\n" + receiver.diagnostics);

    auto call = client.request_to_node ("m", zlink::routing_id_t::from ("n"), payload_t{});
    std::optional<fw::request_task_t<payload_t>> pending;
    const auto actual = sample_allocations ([&] { pending.emplace (call.async<payload_t> ()); });
    print_allocation_sizes ("node request above transport", actual);
    SCOPED_TRACE ("node request above transport\n" + actual.diagnostics);
    EXPECT_EQ (submitted, 1u);
    EXPECT_EQ (actual.total, encode.total + target_owner.total + receiver.total);

    ASSERT_TRUE (pending.has_value ());
    EXPECT_EQ (fw::detail::request_task_access_t::admission_source (*pending), admitted);
    EXPECT_FALSE (pending->await_ready ());
    EXPECT_TRUE (pending->admitted ().result ());
    EXPECT_TRUE (physical.complete (
      fw::result_t<transport_reply_t>::success (transport_reply_t::success (reply_parts (41)))));
    const auto &reply = pending->result ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (reply.value ().value, 41);
    expect_public_and_physical_reply_allocations (actual, *pending, true);
}

TEST (RequestCallAllocations, ChannelRequestAddsOneReplyPerReplyTypeAboveTheTransport)
{
    fw::serializer_registry_t serializers;
    add_payload (serializers);
    auto builder = fw::test::runtime_failure_builder ();
    auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    fw::task_completion_source_t<transport_reply_t> physical;
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    admitted->complete (fw::result_t<void>::success ());
    std::optional<fw::request_task_t<transport_reply_t>> prepared;
    fw::detail::channel_runtime_state_t::mesh_channel_request_t requester = [&] (auto, auto) {
        auto selected = std::move (*prepared);
        prepared.reset ();
        return selected;
    };
    runtime.bind_mesh_channel_transport ("c", {}, requester);
    auto client = builder.route_client (serializers);
    (void) encode_allocations (serializers, "c");
    const auto encode = encode_allocations (serializers, "c");
    const auto selection = channel_selection_allocations (requester);
    const auto internal_projection =
      channel_projection_allocations (channel_transport_t (std::in_place_index<1>, requester));
    const auto public_projection = public_projection_allocations (serializers);
    print_allocation_sizes ("channel encode reference", encode);
    print_allocation_sizes ("channel selection reference", selection);
    print_allocation_sizes ("channel internal projection reference", internal_projection);
    print_allocation_sizes ("channel public projection reference", public_projection);
    SCOPED_TRACE ("channel encode reference\n" + encode.diagnostics);
    SCOPED_TRACE ("channel selection reference\n" + selection.diagnostics);
    SCOPED_TRACE ("channel internal projection reference\n" + internal_projection.diagnostics);
    SCOPED_TRACE ("channel public projection reference\n" + public_projection.diagnostics);
    prepared.emplace (physical.task (), admitted);
    auto call = client.request_to_channel ("c", payload_t{});
    std::optional<fw::request_task_t<payload_t>> pending;
    const auto actual = sample_allocations ([&] { pending.emplace (call.async<payload_t> ()); });
    print_allocation_sizes ("channel request above transport", actual);
    SCOPED_TRACE ("channel request above transport\n" + actual.diagnostics);
    EXPECT_EQ (actual.total, encode.total + selection.total + internal_projection.total
                               + public_projection.total);

    ASSERT_TRUE (pending.has_value ());
    EXPECT_EQ (fw::detail::request_task_access_t::admission_source (*pending), admitted);
    EXPECT_TRUE (physical.complete (
      fw::result_t<transport_reply_t>::success (transport_reply_t::success (reply_parts (43)))));
    const auto &reply = pending->result ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (reply.value ().value, 43);
    expect_public_and_physical_reply_allocations (actual, *pending);
}

class ready_spot_resolver_t final : public fw::runtime::spot_address_resolver_t
{
  public:
    fw::task_t<std::optional<fw::runtime::spot_address_t>>
    resolve_spot_address (std::string, std::string spot_id) override
    {
        fw::runtime::spot_address_t address;
        address.mesh_name = "s";
        address.node_rid = zlink::routing_id_t::from ("n");
        address.spot_id = std::move (spot_id);
        address.spot_generation = 1;
        return fw::task_t<std::optional<fw::runtime::spot_address_t>> (
          fw::result_t<std::optional<fw::runtime::spot_address_t>>::success (std::move (address)));
    }
    void invalidate_spot_address (std::string_view) override {}
    void invalidate_all_routes_after_store_recovery () override {}
};

// Retain the displaced operation and projection primitives as an allocation
// ceiling. Compare allocation counts, rather than compiler-owned frame sizes.
fw::request_task_t<zlink::message_t>
spot_operation_primitive (fw::route_client_t::payload_encoder_t encode_payload,
                          fw::channel_request_call_t::metadata_map_t metadata,
                          fw::request_task_t<zlink::message_t> selected,
                          fw::runtime::spot_address_resolver_t &resolver)
{
    (void) encode_payload;
    (void) metadata;
    (void) co_await resolver.resolve_spot_address ({}, "spot", false);
    co_return co_await fw::detail::request_submission (std::move (selected));
}

allocation_sample_t spot_operation_allocations (fw::runtime::spot_address_resolver_t &resolver)
{
    fw::task_completion_source_t<zlink::message_t> source;
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    admitted->complete (fw::result_t<void>::success ());
    auto selected =
      fw::detail::request_task_access_t::from_admitted_reply (source.task (), admitted);
    auto request_value = std::make_shared<payload_t> ();
    const fw::channel_request_call_t::metadata_map_t metadata;
    std::optional<fw::request_task_t<zlink::message_t>> operation;
    const auto sample = sample_allocations ([&] {
        operation.emplace (spot_operation_primitive (
          [request_value] (fw::serializer_registry_t &serializers) {
              return serializers.get<payload_t> ().serialize_with_content_type (*request_value);
          },
          metadata, std::move (selected), resolver));
    });
    EXPECT_TRUE (source.complete (fw::result_t<zlink::message_t>::success (zlink::message_t{})));
    EXPECT_TRUE (operation->result ());
    return sample;
}

allocation_sample_t spot_selection_allocations (
  const fw::detail::channel_runtime_state_t::spot_mesh_request_t &requester)
{
    fw::runtime::offload_executor_t executor (1, "request-allocation-reference");
    fw::runtime::state_lane_t lane (executor);
    const std::string mesh_name = "s";
    const std::map<std::string, fw::detail::channel_runtime_state_t::spot_mesh_request_t>
      requesters{{mesh_name, requester}};
    return sample_allocations ([&] {
        (void) lane
          .run_checked ([&] {
              const auto found = requesters.find (mesh_name);
              return found == requesters.end ()
                       ? std::nullopt
                       : std::optional<fw::detail::channel_runtime_state_t::spot_mesh_request_t> (
                           found->second);
          })
          .get ();
    });
}

allocation_sample_t spot_projection_allocations (
  const fw::detail::channel_runtime_state_t::spot_mesh_request_t &requester)
{
    fw::task_completion_source_t<transport_reply_t> source;
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    admitted->complete (fw::result_t<void>::success ());
    fw::request_task_t<transport_reply_t> selected (source.task (), admitted);
    std::optional<fw::request_task_t<zlink::message_t>> projected;
    const std::shared_ptr<fw::detail::route_client_state_t> state;
    std::optional<fw::detail::channel_runtime_state_t::spot_mesh_request_t> mesh_requester (
      requester);
    std::string packet_name = payload_t::packet_name;
    std::string router_channel_id = "s";
    const std::shared_ptr<const std::pair<zlink::routing_id_t, fw::spot_id_t>> target;
    const auto sample = sample_allocations ([&] {
        projected.emplace (fw::detail::project_request_reply<zlink::message_t> (
          selected,
          [state, requester = std::move (mesh_requester), packet_name = std::move (packet_name),
           router_channel_id = std::move (router_channel_id),
           target] (const fw::result_t<transport_reply_t> &) -> fw::result_t<zlink::message_t> {
              (void) state;
              (void) requester;
              (void) packet_name;
              (void) router_channel_id;
              (void) target;
              return fw::result_t<zlink::message_t>::success (zlink::message_t{});
          }));
    });
    EXPECT_EQ (fw::detail::request_task_access_t::admission_source (*projected), admitted);
    return sample;
}

#if defined(ZLINK_TEST_COUNT_NATIVE_COPIES)
std::size_t spot_transport_payload_copies ()
{
    // Match the selected transport fixture exactly, including its public reply
    // observer. Its own retained header/body list is outside typed conversion.
    fw::task_completion_source_t<transport_reply_t> physical;
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    admitted->complete (fw::result_t<void>::success ());
    fw::request_task_t<transport_reply_t> selected (physical.task (), admitted);
    auto frames = reply_parts (47);
    fw::tests::native_call_scope_t native;
    EXPECT_TRUE (physical.complete (
      fw::result_t<transport_reply_t>::success (transport_reply_t::success (std::move (frames)))));
    EXPECT_TRUE (selected.result ());
    return native[6];
}
#endif

/* Submit/completion §9 removes conversion-only message result states. The
 * old primitive chain remains the cost ceiling: a prepared typed receiver
 * must use fewer allocations and still own both final reply states. */
TEST (RequestCallAllocations, SpotFinalReceiverUsesFewerAllocationsThanTheConversionChain)
{
    fw::tests::verify_creation_counter_controls ();
    fw::serializer_registry_t serializers;
    add_payload (serializers);
    auto builder = fw::test::runtime_failure_builder ();
    auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    auto resolver = std::make_shared<ready_spot_resolver_t> ();
    runtime.bind_spot_address_resolver (resolver);
    fw::task_completion_source_t<transport_reply_t> physical;
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    admitted->complete (fw::result_t<void>::success ());
    std::optional<fw::request_task_t<transport_reply_t>> prepared;
    unsigned submitted = 0;
    fw::detail::channel_runtime_state_t::spot_mesh_request_t requester =
      [&] (const zlink::routing_id_t &, const std::string &, std::uint64_t, auto, auto) {
          ++submitted;
          auto selected = std::move (*prepared);
          prepared.reset ();
          return selected;
      };
    runtime.bind_spot_mesh_transport ("s", {}, requester);
    auto client = builder.route_client (serializers);
    (void) encode_allocations (serializers, "s");
    const auto encode = encode_allocations (serializers, "s");
    const auto operation = spot_operation_allocations (*resolver);
    const auto selection = spot_selection_allocations (requester);
    const auto target_owner = sample_allocations ([] {
        (void) std::make_shared<const std::pair<zlink::routing_id_t, fw::spot_id_t>> (
          zlink::routing_id_t::from ("n"), "spot");
    });
    const auto projection = spot_projection_allocations (requester);
    const auto public_projection = public_projection_allocations (serializers);
    print_allocation_sizes ("spot encode reference", encode);
    print_allocation_sizes ("spot operation reference", operation);
    print_allocation_sizes ("spot selection reference", selection);
    print_allocation_sizes ("spot target reference", target_owner);
    print_allocation_sizes ("spot internal projection reference", projection);
    print_allocation_sizes ("spot public projection reference", public_projection);
    SCOPED_TRACE ("spot encode reference\n" + encode.diagnostics);
    SCOPED_TRACE ("spot operation reference\n" + operation.diagnostics);
    SCOPED_TRACE ("spot selection reference\n" + selection.diagnostics);
    SCOPED_TRACE ("spot target reference\n" + target_owner.diagnostics);
    SCOPED_TRACE ("spot internal projection reference\n" + projection.diagnostics);
    SCOPED_TRACE ("spot public projection reference\n" + public_projection.diagnostics);
#if defined(ZLINK_TEST_COUNT_NATIVE_COPIES)
    const auto transport_copies = spot_transport_payload_copies ();
    std::printf ("spot physical transport fixture copies=%zu\n", transport_copies);
#endif
    fw::tests::creation_counts_t single{};
    for (const unsigned records : {1u, 4u, 7u}) {
        fw::tests::creation_counts_t created{};
        for (unsigned record = 0; record != records; ++record) {
            physical = fw::task_completion_source_t<transport_reply_t> ();
            prepared.emplace (physical.task (), admitted);
            auto call = client.request_to_spot (fw::spot_id_t ("spot"), payload_t{});
            auto reply_frames = reply_parts (47);
            std::optional<fw::request_task_t<payload_t>> pending;
#if defined(ZLINK_TEST_COUNT_NATIVE_COPIES)
            fw::tests::native_call_scope_t native;
#endif
            const auto before = fw::tests::creation_counts ();
            const auto actual =
              sample_allocations ([&] { pending.emplace (call.async<payload_t> ()); });
            // Compare the public call with its components in the same process.
            EXPECT_LT (actual.total, encode.total + operation.total + selection.total
                                       + target_owner.total + projection.total
                                       + public_projection.total);
            ASSERT_TRUE (pending.has_value ());
            const auto operation_admission =
              fw::detail::request_task_access_t::admission_source (*pending);
            ASSERT_NE (operation_admission, admitted);
            EXPECT_TRUE (allocation_containing (actual, operation_admission.get ()).has_value ());
            auto operation_admission_task = operation_admission->task ();
            EXPECT_TRUE (
              allocation_containing (actual, std::addressof (operation_admission_task.result ()))
                .has_value ());
            EXPECT_TRUE (pending->admitted ().result ());
            EXPECT_FALSE (pending->await_ready ());
            EXPECT_TRUE (physical.complete (fw::result_t<transport_reply_t>::success (
              transport_reply_t::success (std::move (reply_frames)))));
            const auto &reply = pending->result ();
            ASSERT_TRUE (reply);
            EXPECT_EQ (reply.value ().value, 47);
            const auto delta = fw::tests::creation_counts () - before;
            created.shared_states += delta.shared_states;
            created.coroutine_states += delta.coroutine_states;
            created.payload_copies += delta.payload_copies;
#if defined(ZLINK_TEST_COUNT_NATIVE_COPIES)
            // The typed Framework path adds no copy above this same transport.
            EXPECT_EQ (native[6], transport_copies);
#endif
            expect_public_and_physical_reply_allocations (actual, *pending, true);
        }
        if (records == 1) {
            single = created;
            // Final public reply, physical typed reply, operation admission and
            // the Spot submit coroutine. A ready resolver adds no shared state.
            EXPECT_EQ (single.shared_states, 4u);
            EXPECT_EQ (single.coroutine_states, 1u);
        }
        EXPECT_EQ (created.payload_copies, 0u);
        std::printf ("spot creations: records=%u shared=%zu coroutine=%zu copies=%zu\n", records,
                     created.shared_states, created.coroutine_states, created.payload_copies);
        EXPECT_EQ (created.shared_states, single.shared_states + 4u * (records - 1));
        EXPECT_EQ (created.coroutine_states, single.coroutine_states + 1u * (records - 1));
    }
    EXPECT_EQ (submitted, 12u);
}

/* Hot path E4: the MeshNode request submit of a target the topology already
 * decides is the host runtime's own task. No coroutine wraps it to forward the
 * same result; a failure before submit is that task's result, as before. */
TEST (RequestCallAllocations, MeshNodeRequestSubmitDoesNotWrapTheHostTask)
{
    auto state =
      fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> ("request-call");
    fw::detail::mesh_node_runtime_t runtime (state);
    const auto failed_task = fw::tests::count_allocations ([] {
        try {
            throw fw::framework_exception_t (fw::framework_error_kind_t::protocol_error,
                                             "MeshNode has not started");
        }
        catch (...) {
            (void) fw::task_t<zlink::submit_result_t> (
              fw::detail::current_exception_result<zlink::submit_result_t> ());
        }
    });
    fw::detail::host::pending_operation_t operation;
    std::optional<fw::task_t<zlink::submit_result_t>> submitted;
    const auto target = zlink::routing_id_t::from ("n");
    const auto measured = fw::tests::count_allocations ([&] {
        submitted.emplace (runtime.request_to_node (target, std::vector<zlink::message_t>{},
                                                    operation, std::chrono::milliseconds (1000),
                                                    std::vector<std::uint8_t>{}));
    });
    std::printf ("mesh node request submit: measured=%zu failed_task=%zu\n", measured, failed_task);
    EXPECT_EQ (measured, failed_task);
    ASSERT_TRUE (submitted.has_value ());
    ASSERT_TRUE (submitted->await_ready ());
    EXPECT_EQ (submitted->result ().error_kind (), fw::framework_error_kind_t::protocol_error);
    ASSERT_NE (submitted->result ().error (), nullptr);
    EXPECT_STREQ (submitted->result ().error ()->what (), "MeshNode has not started");
}

/* Submit/completion §9: an admission failure is the caller's terminal even
 * while the transport has not reached its physical terminal; the later
 * physical terminal does not replace it. */
TEST (RequestCallAllocations, AdmissionFailureEndsThePublicReplyBeforeThePhysicalTerminal)
{
    for (const bool channel_request : {false, true}) {
        fw::serializer_registry_t serializers;
        add_payload (serializers);
        auto builder = fw::test::runtime_failure_builder ();
        auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
        runtime.bind_serializers (serializers);
        fw::task_completion_source_t<transport_reply_t> physical;
        auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
        const auto request = [&] (auto, auto) {
            return fw::request_task_t<transport_reply_t> (physical.task (), admitted);
        };
        runtime.bind_mesh_channel_transport ("c", {}, request);
        runtime.bind_mesh_node_transport (
          "m", {}, [&] (const auto &, auto parts, auto timeout, auto receiver) {
              receiver.observe (request (std::move (parts), timeout));
          });
        auto client = builder.route_client (serializers);
        auto pending =
          (channel_request
             ? client.request_to_channel ("c", payload_t{})
             : client.request_to_node ("m", zlink::routing_id_t::from ("n"), payload_t{}))
            .async<payload_t> ();
        auto admission = pending.admitted ();
        EXPECT_FALSE (pending.await_ready ());
        EXPECT_FALSE (admission.await_ready ());
        EXPECT_TRUE (admitted->complete (fw::result_t<void>::failure (
          fw::framework_error_kind_t::unavailable, "admission refused")));
        ASSERT_TRUE (pending.await_ready ());
        EXPECT_EQ (pending.result ().error_kind (), fw::framework_error_kind_t::unavailable);
        ASSERT_NE (pending.result ().error (), nullptr);
        EXPECT_STREQ (pending.result ().error ()->what (), "admission refused");
        EXPECT_EQ (admission.result ().error_kind (), fw::framework_error_kind_t::unavailable);
        EXPECT_TRUE (physical.complete (
          fw::result_t<transport_reply_t>::success (transport_reply_t::success (reply_parts (5)))));
        EXPECT_EQ (pending.result ().error_kind (), fw::framework_error_kind_t::unavailable);
    }
}
// A rejected raw request has no pending admission and returns only its
// ready result state. Neither the node entry nor header submission wraps it.
fw::task_t<bool> one_ready_admission_result ()
{
    return fw::task_t<bool> (fw::result_t<bool>::success (false));
}

TEST (RequestCallAllocations, ReceiverPreparationFailureDoesNotSubmitOrEscapeAsync)
{
    fw::serializer_registry_t serializers;
    add_payload (serializers);
    auto builder = fw::test::runtime_failure_builder ();
    auto channel = fw::detail::channel_runtime_t::from (builder.message_bus ());
    channel.bind_serializers (serializers);
    unsigned submitted = 0;
    channel.bind_mesh_node_transport ("m", {},
                                      [&] (const auto &, auto, auto, auto) { ++submitted; });
    auto call = builder.route_client (serializers)
                  .request_to_node ("m", zlink::routing_id_t::from ("n"), payload_t{});
    std::optional<fw::request_task_t<payload_t>> pending;
    fw::tests::allocations_before_failure = 0;
    EXPECT_NO_THROW (pending.emplace (call.async<payload_t> ()));
    fw::tests::allocations_before_failure.reset ();
    ASSERT_TRUE (pending.has_value ());
    EXPECT_EQ (submitted, 0u);
    ASSERT_TRUE (pending->await_ready ());
    EXPECT_THROW (pending->result ().value (), std::bad_alloc);
    auto physical = fw::detail::request_task_access_t::physical_reply (*pending);
    ASSERT_TRUE (physical.await_ready ());
    EXPECT_THROW (physical.result ().value (), std::bad_alloc);
    ASSERT_TRUE (pending->admitted ().await_ready ());
    EXPECT_THROW (pending->admitted ().result ().value (), std::bad_alloc);
}

TEST (RequestCallAllocations, PreparedReceiverDecodesAndReleasesPhysicalReplyAfterAdmissionFailure)
{
    // Success, typed decode failure and malformed envelope retain the first public failure.
    for (const int mode : {0, 1, 2}) {
        fw::serializer_registry_t serializers;
        unsigned decoded = 0;
        serializers.add<payload_t> (
          [] (const payload_t &payload) {
              return fw::encoded_payload_t::from_string (std::to_string (payload.value));
          },
          [&] (const fw::encoded_payload_t &payload) {
              ++decoded;
              if (mode == 1)
                  throw fw::framework_exception_t (fw::framework_error_kind_t::protocol_error,
                                                   "typed reply decode failed");
              return payload_t{std::stoi (payload.to_string ())};
          },
          "x/y");
        auto builder = fw::test::runtime_failure_builder ();
        auto channel = fw::detail::channel_runtime_t::from (builder.message_bus ());
        channel.bind_serializers (serializers);
        std::optional<fw::detail::request_receiver_t<transport_reply_t>> receiver;
        channel.bind_mesh_node_transport ("m", {}, [&] (const auto &, auto, auto, auto selected) {
            receiver.emplace (std::move (selected));
        });
        auto pending = builder.route_client (serializers)
                         .request_to_node ("m", zlink::routing_id_t::from ("n"), payload_t{})
                         .async<payload_t> ();
        auto physical = fw::detail::request_task_access_t::physical_reply (pending);
        ASSERT_TRUE (receiver.has_value ());
        ASSERT_FALSE (pending.await_ready ());
        receiver->admitted->complete (fw::result_t<void>::failure (
          fw::framework_error_kind_t::unavailable, "admission refused"));
        ASSERT_TRUE (pending.await_ready ());
        EXPECT_EQ (pending.result ().error_kind (), fw::framework_error_kind_t::unavailable);
        EXPECT_FALSE (physical.await_ready ());
        unsigned released = 0;
        struct body_storage_t
        {
            std::vector<std::uint8_t> bytes;
            unsigned *released;
        };
        auto storage = std::make_unique<body_storage_t> (body_storage_t{{'5', '1'}, &released});
        auto parts = std::move (reply_parts (51)).take_items ();
        parts[1] = zlink::advanced::external_message_t::from (
          std::span<std::uint8_t> (storage->bytes),
          [] (void *, void *hint) {
              std::unique_ptr<body_storage_t> body (static_cast<body_storage_t *> (hint));
              ++*body->released;
          },
          storage.get ());
        ASSERT_TRUE (parts[1].valid ());
        storage.release ();
        if (mode == 2)
            parts[0] = zlink::message_t::from (std::string ("{"));
        receiver->complete (fw::result_t<transport_reply_t>::success (
          transport_reply_t::success (parts_t (std::move (parts)))));
        receiver.reset ();
        ASSERT_TRUE (physical.await_ready ());
        if (mode == 0) {
            ASSERT_TRUE (physical.result ());
            EXPECT_EQ (physical.result ().value ().value, 51);
        } else {
            EXPECT_EQ (physical.result ().error_kind (),
                       fw::framework_error_kind_t::protocol_error);
        }
        EXPECT_EQ (decoded, mode == 2 ? 0u : 1u);
        EXPECT_EQ (released, 1u);
        EXPECT_EQ (pending.result ().error_kind (), fw::framework_error_kind_t::unavailable);
        EXPECT_STREQ (pending.result ().error ()->what (), "admission refused");
    }
}

TEST (RequestCallAllocations, RawNodeRequestDoesNotWrapItsAdmissionTask)
{
    fw::runtime::mesh::raw_mesh_node_options_t options;
    options.descriptor.mesh_name = "allocation-node";
    options.descriptor.node_routing_id = {'a'};
    options.descriptor.lifecycle_generation = options.descriptor.descriptor_revision = 1;
    options.descriptor.advertised_endpoint = "tcp://127.0.0.1:0";
    fw::runtime::mesh::raw_mesh_node_owner_t node (
      fw::test::runtime_failure_options (std::move (options)));
    fw::runtime::protocol::application_payload_t payload;
    std::vector<std::uint8_t> target{'b'};
    std::optional<fw::task_t<bool>> pending;
    const auto baseline =
      fw::tests::count_allocations ([&] { pending.emplace (one_ready_admission_result ()); });
    pending.reset ();
    const auto measured = fw::tests::count_allocations ([&] {
        pending.emplace (node.request_to_node (target, payload, std::chrono::seconds (1), {}));
    });
    std::printf ("raw node request: measured=%zu ready_admission=%zu\n", measured, baseline);
    EXPECT_EQ (measured, baseline);
    ASSERT_TRUE (pending->await_ready ());
    ASSERT_TRUE (pending->result ());
    EXPECT_FALSE (pending->result ().value ());
}
TEST (RequestCallAllocations, RawNodeRequestKeepsInvalidTimeoutInTheReturnedTask)
{
    fw::runtime::mesh::raw_mesh_node_options_t options;
    options.descriptor.mesh_name = "invalid-timeout-node";
    options.descriptor.node_routing_id = {'a'};
    options.descriptor.lifecycle_generation = options.descriptor.descriptor_revision = 1;
    options.descriptor.advertised_endpoint = "tcp://127.0.0.1:0";
    fw::runtime::mesh::raw_mesh_node_owner_t node (
      fw::test::runtime_failure_options (std::move (options)));
    fw::runtime::protocol::application_payload_t payload;
    std::optional<fw::task_t<bool>> pending;
    const std::array<std::uint8_t, 1> target{'b'};
    EXPECT_NO_THROW (pending.emplace (
      node.request_to_node (target, payload, std::chrono::milliseconds::zero (), {})));
    ASSERT_TRUE (pending.has_value ());
    ASSERT_TRUE (pending->await_ready ());
    const auto &result = pending->result ();
    ASSERT_FALSE (result);
    try {
        (void) result.value ();
        FAIL () << "invalid timeout did not remain a task failure";
    }
    catch (const std::invalid_argument &error) {
        EXPECT_STREQ (error.what (), "raw mesh request timeout must be positive");
    }
}

TEST (RequestCallAllocations, HostNodeRequestAddsNoCoroutineAboveTheRawTask)
{
    fw::completion_owner_t completion_owner;
    auto state =
      fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> ("host-request");
    state->core_context = std::make_shared<zlink::context_t> ();
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->routing_id = zlink::routing_id_t::from ("host-source");
    state->object_role = fw::object_role_t::none;
    fw::detail::mesh_node_runtime_t runtime (state);
    runtime.start ().result ().value ();
    struct stop_t
    {
        fw::detail::mesh_node_runtime_t &runtime;
        ~stop_t () { runtime.stop (); }
    } stop{runtime};
    auto host = runtime.native_node ().shared_from_this ();
    const auto target = zlink::routing_id_t::from ("missing-host-target");
    const auto target_bytes = target.to_bytes ();
    fw::runtime::host::pending_operation_t operation;
    fw::runtime::protocol::application_payload_t payload;
    std::optional<fw::task_t<bool>> raw;
    const auto raw_count = fw::tests::count_allocations ([&] {
        raw.emplace (
          host->transport ().request_to_node (target_bytes, payload, std::chrono::seconds (1), {}));
    });
    const auto target_count = fw::tests::count_allocations ([&] {
        const std::span<const std::uint8_t> bytes (target.data (), target.size ());
        (void) bytes;
    });
    const auto callback_count = fw::tests::count_allocations ([&] {
        fw::runtime::foundation::operation_registry_t::callback_t callback =
          [host, operation] (fw::runtime::foundation::operation_terminal_t,
                             ::zlink::framework::result_t<zlink::message_t>) {
              (void) host;
              (void) operation;
          };
    });
    const auto result_count = fw::tests::count_allocations ([] {
        (void) fw::task_t<zlink::submit_result_t> (
          fw::result_t<zlink::submit_result_t>::success (zlink::submit_result_t::not_found));
    });
    std::vector<zlink::message_t> calibrated_parts;
    calibrated_parts.push_back (zlink::message_t::from (std::string ("request")));
    const auto payload_count = fw::tests::count_allocations ([&] {
        (void) fw::runtime::protocol::application_payload_t::from_parts (
          std::move (calibrated_parts));
    });
    std::vector<zlink::message_t> parts;
    parts.push_back (zlink::message_t::from (std::string ("request")));
    std::optional<fw::task_t<zlink::submit_result_t>> pending;
    const auto measured = fw::tests::count_allocations ([&] {
        pending.emplace (
          host->request_to_node (target, std::move (parts), operation, std::chrono::seconds (1)));
    });
    const auto baseline = raw_count + target_count + callback_count + result_count + payload_count;
    std::printf (
      "host request: measured=%zu baseline=%zu raw=%zu target=%zu callback=%zu result=%zu\n",
      measured, baseline, raw_count, target_count, callback_count, result_count);
    EXPECT_EQ (measured, baseline);
    ASSERT_TRUE (pending->result ());
    EXPECT_EQ (pending->result ().value (), zlink::submit_result_t::not_found);
}

struct allocation_codec_t
{
    void register_framework_codecs (fw::codec_registration_context_t &context) const
    {
        context.add_serializer<payload_t> (
          [] (const payload_t &payload) {
              return fw::encoded_payload_t::from_string (std::to_string (payload.value));
          },
          [] (const fw::encoded_payload_t &payload) {
              return payload_t{std::stoi (payload.to_string ())};
          },
          "x/y");
    }
};

struct allocation_host_capture_t final : fw::hosted_service_t
{
    fw::route_client_t *client = nullptr;
    fw::route_mesh_runtime_t *mesh = nullptr;
    fw::framework_runtime_t *runtime = nullptr;
    fw::task_t<void> start (fw::service_provider_t &services) override
    {
        client = &services.get_required<fw::route_client_t> ();
        mesh = &services.get_required<fw::route_mesh_runtime_t> ();
        runtime = &services.get_required<fw::framework_runtime_t> ();
        co_return;
    }
    void stop () noexcept override {}
};

// The real app transport must not allocate an operation or a coroutine when
// topology has already classified an Object Client as an invalid Node target.
// Compare the same route layer over an immediate transport, plus the real
// topology classifier's borrowed routing-id bytes. No allocation size identifies
// a reply state or an admission source.
TEST (RequestCallAllocations, AppClassifiedNodeFailureAddsNoTransportReplyState)
{
    struct app_host_t
    {
        fw::app_t app = fw::app_t::create ();
        allocation_host_capture_t *capture;
        std::thread runner;
        std::atomic<bool> exited{false};
        app_host_t ()
        {
            auto service = std::make_unique<allocation_host_capture_t> ();
            capture = service.get ();
            app.add_hosted_service (std::move (service));
        }
        void start ()
        {
            runner = std::thread ([this] {
                app.run (0, nullptr);
                exited = true;
            });
            const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
            while (!app.is_ready () && !exited.load ()
                   && std::chrono::steady_clock::now () < deadline)
                std::this_thread::yield ();
            if (!app.is_ready ())
                throw std::runtime_error ("allocation host failed to start");
        }
        ~app_host_t ()
        {
            if (runner.joinable ()) {
                app.request_stop ();
                runner.join ();
            }
        }
    } target_host, source_host;
    const auto target = zlink::routing_id_t::from ("allocation-client");
    auto &target_options = target_host.app.add_zlink_framework ();
    target_options.configure_locations ().owner_lease_renew_interval =
      std::chrono::milliseconds (10);
    target_options.configure_locations ().owner_lease_fencing_margin =
      std::chrono::milliseconds (1);
    target_options.add_location_store (
      std::make_shared<fw::runtime::in_memory_location_store_t> ());
    target_options.add_route_mesh ("m")
      .set_object_role (fw::object_role_t::client)
      .listen ("tcp://127.0.0.1:0")
      .set_routing_id (target);
    target_host.start ();
    const auto endpoint =
      target_host.capture->runtime->listener_status (fw::listener_kind_t::route_mesh, "m").endpoint;
    auto &options = source_host.app.add_zlink_framework ();
    options.codecs ().use (allocation_codec_t{});
    options.add_route_mesh ("m")
      .set_object_role (fw::object_role_t::none)
      .listen ("tcp://127.0.0.1:0")
      .set_routing_id (zlink::routing_id_t::from ("allocation-source"))
      .peer_connections ()
      .connect (target, endpoint);
    source_host.start ();
    bool known_client = false;
    const auto peer_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (std::chrono::steady_clock::now () < peer_deadline) {
        const auto snapshot = source_host.capture->mesh->snapshot ("m");
        if (std::any_of (snapshot.peers.begin (), snapshot.peers.end (), [&] (const auto &peer) {
                return peer.node_rid == target && peer.state == fw::peer_state_t::ready;
            })) {
            known_client = true;
            break;
        }
        std::this_thread::yield ();
    }
    ASSERT_TRUE (known_client);
    auto *host = source_host.capture;
    fw::serializer_registry_t serializers;
    add_payload (serializers);
    auto builder = fw::test::runtime_failure_builder ();
    auto channel = fw::detail::channel_runtime_t::from (builder.message_bus ());
    channel.bind_serializers (serializers);
    channel.bind_mesh_node_transport ("m", {}, [] (const auto &, auto, auto, auto receiver) {
        auto failure = transport_reply_t::failure (fw::framework_error_kind_t::not_found,
                                                   "MeshNode request target was not found");
        receiver.admitted->complete (
          fw::detail::result_access_t::failure<void> (failure.exception ()));
        receiver.complete (fw::result_t<transport_reply_t>::success (std::move (failure)));
    });
    auto reference = builder.route_client (serializers);
    const auto target_bytes = sample_allocations ([&] {
        const std::span<const std::uint8_t> bytes (target.data (), target.size ());
        (void) bytes;
    });
    std::optional<fw::request_task_t<payload_t>> pending;
    auto measure = [&] (fw::route_client_t &client) {
        auto call = client.request_to_node ("m", target, payload_t{});
        auto sample = sample_allocations ([&] { pending.emplace (call.async<payload_t> ()); });
        EXPECT_EQ (pending->result ().error_kind (), fw::framework_error_kind_t::not_found);
        expect_public_and_physical_reply_allocations (sample, *pending, true);
        const auto admission = fw::detail::request_task_access_t::admission_source (*pending);
        EXPECT_TRUE (allocation_containing (sample, admission.get ()).has_value ());
        auto admission_task = admission->task ();
        EXPECT_TRUE (
          allocation_containing (sample, std::addressof (admission_task.result ())).has_value ());
        return sample;
    };
    (void) measure (reference);
    (void) measure (*host->client);
    const auto baseline = measure (reference);
    const auto actual = measure (*host->client);
    SCOPED_TRACE ("app classified reference\n" + baseline.diagnostics);
    SCOPED_TRACE ("app classified actual\n" + actual.diagnostics);
    print_allocation_sizes ("app classified reference", baseline);
    print_allocation_sizes ("app classified target bytes reference", target_bytes);
    print_allocation_sizes ("app classified actual", actual);
    EXPECT_EQ (actual.total, baseline.total + target_bytes.total);
}
} // namespace

TEST (RequestCallAllocations, CsitesRequestCompletionKeepsStorageUntilEveryTerminalTurn)
{
    namespace foundation = fw::runtime::foundation;
    using terminal_t = foundation::operation_terminal_t;
    for (int mode = 0; mode != 5; ++mode) {
        fw::completion_owner_t owner;
        std::atomic_int deliveries{0};
        std::weak_ptr<int> lifetime;
        std::optional<fw::request_task_t<int>> pending;
        std::stop_source cancel;
        std::promise<void> capture_released;
        auto capture_release = capture_released.get_future ();
        {
            foundation::operation_registry_t registry;
            auto marker = std::shared_ptr<int> (new int (42), [&] (int *value) {
                delete value;
                capture_released.set_value ();
            });
            lifetime = marker;
            auto wire = fw::runtime::protocol::encode_application_payload_message (
              fw::runtime::protocol::application_payload_t::from_parts (
                std::vector<zlink::message_t>{zlink::message_t::from ("header"),
                                              zlink::message_t::from (std::string (1024, 'x'))}));
            pending.emplace (
              fw::detail::request_task_access_t::receive_public<int> ([&] (auto receiver) {
                  auto callback =
                    receiver.complete.template capture<terminal_t, fw::result_t<zlink::message_t>> (
                      [complete = receiver.complete.borrow (), marker, wire, &deliveries,
                       mode] (terminal_t terminal, auto) {
                          EXPECT_EQ (*marker, 42);
                          EXPECT_EQ (terminal, mode == 1   ? terminal_t::timed_out
                                               : mode == 2 ? terminal_t::cancelled
                                               : mode == 3 ? terminal_t::shutdown
                                                           : terminal_t::completed);
                          auto descriptors = fw::runtime::protocol::decode_application_part_views (
                            fw::runtime::protocol::message_bytes (wire), false,
                            complete.resource ());
                          EXPECT_EQ (descriptors.size (), 2u);
                          EXPECT_EQ (descriptors.at (1).size (), 1024u);
                          ++deliveries;
                          complete (fw::result_t<int>::success (42));
                          if (mode == 4)
                              throw std::runtime_error ("completion consumer failure");
                      });
                  ASSERT_TRUE (callback.completion_storage ());
                  if (mode == 0 || mode == 4)
                      receiver.admitted->complete (fw::result_t<void>::success ());
                  ASSERT_TRUE (registry.register_operation (
                    {3867, 1}, std::chrono::steady_clock::now () + std::chrono::seconds (30),
                    std::move (foundation::completion_sink_t (std::move (callback)))
                      .with_admission (receiver.admitted),
                    {}, {}, {}, cancel.get_token ()));
              }));
            marker.reset ();
            EXPECT_FALSE (lifetime.expired ());
            EXPECT_EQ (deliveries.load (), 0);
            if (mode == 1)
                registry.expire (std::chrono::steady_clock::now () + std::chrono::seconds (31))
                  .result ()
                  .value ();
            else if (mode == 2)
                cancel.request_stop ();
            else if (mode == 3)
                EXPECT_EQ (registry.shutdown (), 1u);
            else
                EXPECT_TRUE (registry.complete ({3867, 1}, zlink::message_t::from ("reply")));
            EXPECT_FALSE (registry.complete ({3867, 1}, zlink::message_t::from ("late reply")));
            EXPECT_FALSE (registry.cancel ({3867, 1}));
            ASSERT_EQ (capture_release.wait_for (std::chrono::seconds (1)),
                       std::future_status::ready);
            EXPECT_TRUE (lifetime.expired ());
            EXPECT_EQ (deliveries.load (), 1);
            if (mode == 0 || mode == 2 || mode == 4)
                EXPECT_GT (registry.index_size (), 0u);
        }
        cancel.request_stop ();
        EXPECT_EQ (deliveries.load (), 1);
        ASSERT_TRUE (pending->await_ready ());
        EXPECT_EQ (static_cast<bool> (pending->result ()), mode == 0 || mode == 4);
        auto physical = fw::detail::request_task_access_t::physical_reply (*pending);
        EXPECT_EQ (physical.result ().value (), 42);
        EXPECT_TRUE (lifetime.expired ());
    }
}

TEST (RequestCallAllocations, CsitesReplyDescriptorsUseRequestStorageUntilProjectionReturns)
{
    namespace protocol = fw::runtime::protocol;
    std::vector<zlink::message_t> parts;
    parts.push_back (zlink::message_t::from ("header"));
    parts.push_back (zlink::message_t::from (std::string (1024, 'x')));
    auto encoded = protocol::encode_application_payload_message (
      protocol::application_payload_t::from_parts (std::move (parts)));
    auto pending = fw::detail::request_task_access_t::receive_public<int> ([&] (auto receiver) {
        const auto before = sample_allocations ([&] {
            auto descriptors = protocol::decode_application_part_views (
              protocol::message_bytes (encoded), false, receiver.complete.resource ());
            ASSERT_EQ (descriptors.size (), 2u);
            EXPECT_EQ (descriptors[1].size (), 1024u);
            EXPECT_EQ (static_cast<char> (descriptors[1].front ()), 'x');
            auto projected = receiver.template project<int> ([&] (const auto &value) {
                EXPECT_EQ (descriptors[1].size (), 1024u);
                return value;
            });
            projected.complete (fw::result_t<int>::success (7));
        });
        EXPECT_EQ (before.total, 0u) << before.diagnostics;
        const auto ordinary = fw::tests::count_allocations ([&] {
            auto descriptors =
              protocol::decode_application_part_views (protocol::message_bytes (encoded), false);
            EXPECT_EQ (descriptors.size (), 2u);
        });
        EXPECT_EQ (ordinary, 1u);
        std::printf ("csites descriptors request=%zu ordinary=%zu\n", before.total, ordinary);
    });
    EXPECT_EQ (pending.result ().value (), 7);
}

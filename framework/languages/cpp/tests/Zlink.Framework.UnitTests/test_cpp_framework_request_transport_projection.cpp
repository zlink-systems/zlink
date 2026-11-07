/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <gtest/gtest.h>
#include "allocation_counter.hpp"
#include "creation_counter.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include <zlink/framework.hpp>
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime_failure_fixture.hpp"
#include <cstdio>
#include <array>
#include <atomic>
#include <optional>
#include <thread>

namespace zlink::framework::runtime::mesh
{
struct raw_mesh_node_owner_test_access_t
{
    static void close_registry (raw_mesh_node_owner_t &owner) { owner._operations->shutdown (); }
};
}

namespace
{
namespace fw = zlink::framework;
using reply_t = fw::result_t<fw::runtime::messaging::message_parts_t>;

// Exercise the real host/raw-owner bridge: registration and admission can
// finish on different turns, but only the registry owns a registered delivery.
struct request_receiver_mesh_t
{
    static auto configure (const std::string &rid)
    {
        auto state = fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> (
          "receiver-terminal");
        state->core_context = std::make_shared<zlink::context_t> ();
        state->listen_endpoint = "tcp://127.0.0.1:0";
        state->routing_id = zlink::routing_id_t::from (rid);
        state->object_role = fw::object_role_t::server;
        state->channels.emplace ("work",
                                 fw::detail::mesh_channel_registration_t{100, {}, true, true});
        return state;
    }
    explicit request_receiver_mesh_t (const std::string &rid,
                                      std::shared_ptr<fw::location_repository_t> store = {}) :
        runtime (configure (rid))
    {
        if (store)
            runtime.configure_user_spot_operations (std::move (store), {});
        runtime.start ().result ().value ();
    }
    ~request_receiver_mesh_t () { runtime.stop (); }
    fw::request_task_t<int> request (const zlink::routing_id_t &target,
                                     std::atomic<unsigned> &conversions,
                                     std::chrono::milliseconds timeout = std::chrono::seconds (5))
    {
        return fw::detail::request_task_access_t::receive_public<int, reply_t> (
          [&] (auto receiver) {
              std::vector<zlink::message_t> parts{zlink::message_t::from (std::string ("request"))};
              runtime.request_node_reply (
                target, fw::runtime::messaging::message_parts_t (std::move (parts)), timeout,
                std::move (receiver));
          },
          [&conversions] (const fw::result_t<reply_t> &terminal) {
              ++conversions;
              if (!terminal)
                  return fw::detail::result_access_t::failure<int> (terminal.exception ());
              if (!terminal.value ())
                  return fw::detail::result_access_t::failure<int> (terminal.value ().exception ());
              return fw::result_t<int>::success (42);
          });
    }
    void pump ()
    {
        runtime.dispatch_ready ([] (const auto &, const auto &, auto, auto) {}).result ().value ();
    }
    fw::detail::mesh_node_runtime_t runtime;
};

TEST (RequestCallAllocations, MeshReceiverImmediateRejectionHasNoRegistryDelivery)
{
    request_receiver_mesh_t source ("reject-source");
    std::atomic<unsigned> conversions{0};
    auto pending = source.request (zlink::routing_id_t::from ("unknown-target"), conversions);
    auto physical = fw::detail::request_task_access_t::physical_reply (pending);
    EXPECT_EQ (pending.result ().error_kind (), fw::framework_error_kind_t::not_found);
    EXPECT_EQ (physical.result ().error_kind (), fw::framework_error_kind_t::not_found);
    source.runtime.stop ();
    EXPECT_EQ (conversions.load (), 1u);
}

TEST (RequestCallAllocations, MeshReceiverNativeSubmitRejectionUsesRegistryDelivery)
{
    request_receiver_mesh_t source ("native-reject-source");
    auto &raw = source.runtime.native_node ().transport ();
    auto descriptor = raw.topology ().local_descriptor ();
    const auto target = zlink::routing_id_t::from ("no-native-route");
    descriptor.node_routing_id = target.to_bytes ();
    descriptor.advertised_endpoint = "tcp://127.0.0.1:1";
    ASSERT_EQ (raw.admit_peer (descriptor, target.to_bytes (), std::chrono::steady_clock::now ()),
               fw::runtime::mesh::peer_admission_result_t::admitted);
    // A known logical peer with no native route is an unavailable transport,
    // rather than an unknown logical target (Framework error model section 3).
    std::atomic<unsigned> conversions{0};
    auto pending = source.request (target, conversions);
    auto physical = fw::detail::request_task_access_t::physical_reply (pending);
    EXPECT_EQ (pending.result ().error_kind (), fw::framework_error_kind_t::unavailable);
    EXPECT_EQ (physical.result ().error_kind (), fw::framework_error_kind_t::unavailable);
    source.runtime.stop ();
    EXPECT_EQ (conversions.load (), 1u);
}

struct capture_payload_t
{
    static constexpr const char *packet_name = "p";
    int value = 42;
};

TEST (RequestCallAllocations, NodeAndChannelNameCaptureStorageBytes)
{
    namespace probe = fw::detail::test_creation_probe;
    request_receiver_mesh_t source ("capture-source");
    auto &raw = source.runtime.native_node ().transport ();
    auto descriptor = raw.topology ().local_descriptor ();
    const auto target = zlink::routing_id_t::from ("capture-target");
    descriptor.node_routing_id = target.to_bytes ();
    descriptor.advertised_endpoint = "tcp://127.0.0.1:1";
    ASSERT_EQ (raw.admit_peer (descriptor, target.to_bytes (), std::chrono::steady_clock::now ()),
               fw::runtime::mesh::peer_admission_result_t::admitted);
    fw::serializer_registry_t serializers;
    serializers.add<capture_payload_t> (
      [] (const capture_payload_t &value) {
          return fw::encoded_payload_t::from_string (std::to_string (value.value));
      },
      [] (const fw::encoded_payload_t &value) {
          return capture_payload_t{std::stoi (value.to_string ())};
      },
      "x/y");
    auto builder = fw::test::runtime_failure_builder ();
    auto channel = fw::detail::channel_runtime_t::from (builder.message_bus ());
    channel.bind_serializers (serializers);
    channel.bind_mesh_node_transport (
      "m", {}, [&] (const auto &rid, auto parts, auto timeout, auto receiver) {
          source.runtime.request_node_reply (rid, std::move (parts), timeout, std::move (receiver));
      });
    channel.bind_mesh_channel_transport ("work", {}, [&] (auto, auto) {
        return fw::request_task_t<reply_t> (fw::result_t<reply_t>::success (
          reply_t::failure (fw::framework_error_kind_t::unavailable, "capture probe")));
    });
    auto client = builder.route_client (serializers);
    for (const bool node : {true, false}) {
        const auto captures = probe::request_capture_bytes.load ();
        const auto values = probe::request_capture_value_bytes.load ();
        const auto blocks = probe::request_block_bytes.load ();
        const auto owners = probe::request_callback_owner_bytes.load ();
        auto pending = (node ? client.request_to_node ("m", target, capture_payload_t{})
                             : client.request_to_channel ("work", capture_payload_t{}))
                         .async<capture_payload_t> ();
        EXPECT_FALSE (pending.result ());
        EXPECT_FALSE (fw::detail::request_task_access_t::physical_reply (pending).result ());
        std::printf ("%s receiver captures=%zu values=%zu block=%zu callback-owner=%zu\n",
                     node ? "node" : "channel-name",
                     probe::request_capture_bytes.load () - captures,
                     probe::request_capture_value_bytes.load () - values,
                     probe::request_block_bytes.load () - blocks,
                     probe::request_callback_owner_bytes.load () - owners);
    }
    source.runtime.stop ();
}

TEST (RequestCallAllocations, MeshReceiverDeadlineBeforeNativeStartDeliversOnce)
{
    request_receiver_mesh_t source ("deadline-source");
    const auto target = zlink::routing_id_t::from ("not-ready");
    source.runtime.expect_peer (target, "tcp://127.0.0.1:1", 1, "default");
    std::atomic<unsigned> conversions{0};
    auto pending = source.request (target, conversions);
    auto physical = fw::detail::request_task_access_t::physical_reply (pending);
    EXPECT_FALSE (pending.admitted ().await_ready ());
    EXPECT_FALSE (physical.await_ready ());
    EXPECT_EQ (source.runtime.native_node ()
                 .transport ()
                 .expire_requests (std::chrono::steady_clock::now () + std::chrono::seconds (6))
                 .result ()
                 .value (),
               1u);
    EXPECT_EQ (pending.result ().error_kind (), fw::framework_error_kind_t::deadline_exceeded);
    EXPECT_EQ (physical.result ().error_kind (), fw::framework_error_kind_t::deadline_exceeded);
    source.runtime.stop ();
    EXPECT_EQ (conversions.load (), 1u);
}

TEST (RequestCallAllocations, MeshReceiverShutdownPendingRetainsDeliveryAfterPublicHandleDrops)
{
    request_receiver_mesh_t source ("shutdown-source");
    const auto target = zlink::routing_id_t::from ("not-ready");
    source.runtime.expect_peer (target, "tcp://127.0.0.1:1", 1, "default");
    std::atomic<unsigned> conversions{0};
    auto physical = [&] {
        auto pending = source.request (target, conversions);
        EXPECT_FALSE (pending.admitted ().await_ready ());
        return fw::detail::request_task_access_t::physical_reply (pending);
    }();
    source.runtime.stop ();
    EXPECT_EQ (physical.result ().error_kind (), fw::framework_error_kind_t::shutting_down);
    EXPECT_EQ (conversions.load (), 1u);
}

class held_mesh_nodes_t final : public fw::runtime::in_memory_location_repository_t
{
  public:
    bool hold = false;
    fw::task_completion_source_t<fw::location_page_t<fw::mesh_node_descriptor_t>> listed;
    fw::task_t<fw::location_page_t<fw::mesh_node_descriptor_t>>
    list_mesh_nodes (std::string mesh, fw::location_page_request_t page = {}) override
    {
        return hold ? listed.task ()
                    : in_memory_location_repository_t::list_mesh_nodes (std::move (mesh),
                                                                        std::move (page));
    }
};

void unknown_node_submission_terminal (bool shutdown, bool native_rejection = false)
{
    auto store = std::make_shared<held_mesh_nodes_t> ();
    request_receiver_mesh_t source (shutdown ? "unknown-shutdown" : "unknown-deadline", store);
    store->hold = true;
    const auto target = zlink::routing_id_t::from ("pending-classification");
    std::atomic<unsigned> conversions{0};
    auto pending = source.request (target, conversions);
    auto physical = fw::detail::request_task_access_t::physical_reply (pending);
    EXPECT_FALSE (pending.admitted ().await_ready ());
    if (native_rejection) {
        auto &raw = source.runtime.native_node ().transport ();
        auto descriptor = raw.topology ().local_descriptor ();
        descriptor.node_routing_id = target.to_bytes ();
        descriptor.advertised_endpoint = "tcp://127.0.0.1:1";
        ASSERT_EQ (
          raw.admit_peer (descriptor, target.to_bytes (), std::chrono::steady_clock::now ()),
          fw::runtime::mesh::peer_admission_result_t::admitted);
    } else
        source.runtime.expect_peer (target, "tcp://127.0.0.1:1", 1, "default");
    std::atomic<unsigned> public_results{0};
    auto public_reply = fw::detail::request_task_access_t::public_reply (pending);
    fw::detail::observe_task_terminal (public_reply, [&] (const auto &) { ++public_results; });
    store->listed.complete (
      fw::result_t<fw::location_page_t<fw::mesh_node_descriptor_t>>::success ({}));
    if (native_rejection) {
        // The classification continuation submits asynchronously after list completion.
        ASSERT_FALSE (pending.result ());
        ASSERT_TRUE (pending.admitted ().await_ready ());
        ASSERT_FALSE (physical.result ());
    } else {
        auto &raw = source.runtime.native_node ().transport ();
        const auto registration_deadline =
          std::chrono::steady_clock::now () + std::chrono::seconds (2);
        while (raw.pending_operation_count () == 0 && !pending.admitted ().await_ready ()
               && std::chrono::steady_clock::now () < registration_deadline) {
            source.pump ();
            std::this_thread::yield ();
        }
        ASSERT_EQ (raw.pending_operation_count (), 1u);
        EXPECT_FALSE (pending.admitted ().await_ready ());
        EXPECT_FALSE (physical.await_ready ());
        if (shutdown)
            source.runtime.stop ();
        else
            EXPECT_EQ (
              raw.expire_requests (std::chrono::steady_clock::now () + std::chrono::seconds (6))
                .result ()
                .value (),
              1u);
    }
    // Binding owns native rejection classification; all stages retain that failure.
    EXPECT_FALSE (pending.result ());
    EXPECT_FALSE (pending.admitted ().result ());
    EXPECT_FALSE (physical.result ());
    const auto expected = native_rejection ? pending.admitted ().result ().error_kind ()
                          : shutdown       ? fw::framework_error_kind_t::shutting_down
                                           : fw::framework_error_kind_t::deadline_exceeded;
    EXPECT_EQ (pending.result ().error_kind (), expected);
    EXPECT_EQ (pending.admitted ().result ().error_kind (), expected);
    EXPECT_EQ (physical.result ().error_kind (), expected);
    source.runtime.stop ();
    EXPECT_EQ (conversions.load (), 1u);
    EXPECT_EQ (public_results.load (), 1u);
}

TEST (RequestCallAllocations, UnknownNodeNativeRejectionDeliversOnce)
{
    unknown_node_submission_terminal (false, true);
}

TEST (RequestCallAllocations, UnknownNodeDeadlineDeliversOnce)
{
    unknown_node_submission_terminal (false);
}

TEST (RequestCallAllocations, UnknownNodeShutdownDeliversOnce)
{
    unknown_node_submission_terminal (true);
}

TEST (RequestCallAllocations, UnknownNodeRegistryCloseDeliversOnce)
{
    const auto deliveries = fw::detail::test_creation_probe::request_deliveries.load ();
    auto store = std::make_shared<held_mesh_nodes_t> ();
    request_receiver_mesh_t source ("unknown-registry-close", store);
    store->hold = true;
    const auto target = zlink::routing_id_t::from ("closed-registry-classification");
    std::atomic<unsigned> conversions{0};
    auto pending = source.request (target, conversions);
    auto physical = fw::detail::request_task_access_t::physical_reply (pending);
    EXPECT_FALSE (pending.admitted ().await_ready ());

    source.runtime.expect_peer (target, "tcp://127.0.0.1:1", 1, "default");
    fw::runtime::mesh::raw_mesh_node_owner_test_access_t::close_registry (
      source.runtime.native_node ().transport ());
    std::atomic<unsigned> public_results{0};
    auto public_reply = fw::detail::request_task_access_t::public_reply (pending);
    fw::detail::observe_task_terminal (public_reply, [&] (const auto &) { ++public_results; });
    store->listed.complete (
      fw::result_t<fw::location_page_t<fw::mesh_node_descriptor_t>>::success ({}));

    const auto public_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (!pending.await_ready () && std::chrono::steady_clock::now () < public_deadline)
        std::this_thread::yield ();
    ASSERT_TRUE (pending.await_ready ());
    EXPECT_FALSE (pending.result ());
    EXPECT_EQ (pending.admitted ().result ().error_kind (),
               fw::framework_error_kind_t::shutting_down);

    const auto physical_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (!physical.await_ready () && std::chrono::steady_clock::now () < physical_deadline)
        std::this_thread::yield ();
    const auto physical_ready = physical.await_ready ();
    EXPECT_TRUE (physical_ready);
    if (physical_ready)
        EXPECT_FALSE (physical.result ());
    source.runtime.stop ();
    EXPECT_EQ (fw::detail::test_creation_probe::request_deliveries.load () - deliveries, 1u);
    EXPECT_EQ (conversions.load (), 1u);
    EXPECT_EQ (public_results.load (), 1u);
}

TEST (RequestCallAllocations, MeshReceiverNormalReplyDeliversOnceAndIgnoresLaterExpiry)
{
    request_receiver_mesh_t source ("reply-source"), target ("reply-target");
    source.runtime.connect_peer (target.runtime.status ().routing_id (),
                                 target.runtime.status ().local_endpoint ());
    const auto ready_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (
      (source.runtime.admitted_peer_count () == 0 || target.runtime.admitted_peer_count () == 0)
      && std::chrono::steady_clock::now () < ready_deadline) {
        source.pump ();
        target.pump ();
        std::this_thread::yield ();
    }
    ASSERT_GT (source.runtime.admitted_peer_count (), 0u);
    ASSERT_GT (target.runtime.admitted_peer_count (), 0u);
    std::atomic<unsigned> conversions{0};
    auto pending = source.request (target.runtime.status ().routing_id (), conversions);
    auto physical = fw::detail::request_task_access_t::physical_reply (pending);
    bool replied = false;
    const auto reply_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (!physical.await_ready () && std::chrono::steady_clock::now () < reply_deadline) {
        target.runtime
          .dispatch_ready ([&] (const auto &, const auto &record, auto, auto) {
              const std::vector<zlink::message_t> parts{
                zlink::message_t::from (std::string ("reply"))};
              EXPECT_EQ (fw::runtime::host::reply (record.reply_token, parts),
                         zlink::submit_result_t::ok);
              replied = true;
          })
          .result ()
          .value ();
        source.pump ();
        std::this_thread::yield ();
    }
    ASSERT_TRUE (replied);
    ASSERT_TRUE (physical.await_ready ());
    EXPECT_EQ (physical.result ().value (), 42);
    EXPECT_EQ (pending.result ().value (), 42);
    EXPECT_TRUE (pending.admitted ().result ());
    EXPECT_EQ (source.runtime.native_node ()
                 .transport ()
                 .expire_requests (std::chrono::steady_clock::now () + std::chrono::seconds (6))
                 .result ()
                 .value (),
               0u);
    source.runtime.stop ();
    target.runtime.stop ();
    EXPECT_EQ (conversions.load (), 1u);
}

TEST (RequestCallAllocations, RejectedRegistryRegistrationKeepsUndeliveredCallback)
{
    using registry_t = fw::runtime::foundation::operation_registry_t;
    fw::completion_owner_t owner;
    registry_t registry;
    EXPECT_EQ (registry.shutdown (), 0u);
    unsigned deliveries = 0;
    registry_t::callback_t callback = [&] (auto, auto) { ++deliveries; };
    EXPECT_FALSE (registry.register_operation ({88, 1}, registry_t::clock_t::time_point::max (),
                                               std::move (callback)));
    ASSERT_TRUE (callback);
    callback (fw::runtime::foundation::operation_terminal_t::transport_failed,
              fw::result_t<zlink::message_t>::success ({}));
    EXPECT_EQ (deliveries, 1u);
}

TEST (RequestCallAllocations, RegistryPublicationExceptionLeavesOnlyC1Delivery)
{
    using registry_t = fw::runtime::foundation::operation_registry_t;
    fw::completion_owner_t owner;
    registry_t registry (&registry_t::clock_t::now, [] { throw std::bad_alloc (); });
    std::atomic<unsigned> conversions{0};
    auto pending = fw::detail::request_task_access_t::receive_public<int, int> (
      [&] (auto receiver) {
          registry_t::callback_t callback (
            receiver.complete.template capture<fw::runtime::foundation::operation_terminal_t,
                                               fw::result_t<zlink::message_t>> (
              [receiver = receiver.borrow ()] (auto, auto) {
                  receiver.complete (fw::result_t<int>::failure (
                    fw::framework_error_kind_t::unavailable, "registration publication failed"));
              }));
          EXPECT_THROW (registry.register_operation (
                          {88, 2}, registry_t::clock_t::now () + std::chrono::seconds (5),
                          std::move (callback).with_admission (receiver.admitted)),
                        std::bad_alloc);
          EXPECT_FALSE (callback);
          EXPECT_EQ (registry.size (), 1u);
          // The raw owner's exception path can fail C1, but has no receiver left
          // to deliver independently after successful publication.
          EXPECT_TRUE (registry.fail (
            {88, 2}, fw::runtime::foundation::operation_terminal_t::transport_failed));
      },
      [&] (const fw::result_t<int> &terminal) {
          ++conversions;
          return terminal;
      });
    auto physical = fw::detail::request_task_access_t::physical_reply (pending);
    EXPECT_EQ (physical.result ().error_kind (), fw::framework_error_kind_t::unavailable);
    EXPECT_EQ (pending.result ().error_kind (), fw::framework_error_kind_t::unavailable);
    EXPECT_EQ (registry.shutdown (), 0u);
    EXPECT_EQ (conversions.load (), 1u);
}

TEST (RequestCallAllocations, ReadyNodeReplyRegistersWithoutAConversionResultState)
{
    auto state =
      fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> ("ready-projection");
    fw::detail::mesh_node_runtime_t runtime (state);
    std::array<fw::tests::allocation_event_t, 64> events{};
    fw::tests::allocation_event_log_t log{events.data (), events.size ()};
    std::size_t conversion_state_size = 0;
    {
        fw::tests::allocation_event_scope_t scope (log);
        const auto calibration = fw::tests::count_allocations (
          [] { (void) std::make_shared<fw::detail::application_task_shared_state_t<reply_t>> (); });
        ASSERT_EQ (calibration, 1u);
        ASSERT_EQ (log.count, 1u);
        conversion_state_size = events[0].size;
    }
    fw::runtime::host::pending_operation_t operation;
    auto submitted = fw::task_t<zlink::submit_result_t> (
      fw::result_t<zlink::submit_result_t>::success (zlink::submit_result_t::ok));
    std::optional<fw::detail::request_receiver_t<reply_t>> receiver;
    auto pending = fw::detail::request_task_access_t::receive_internal<reply_t> (
      [&] (auto prepared) { receiver.emplace (std::move (prepared)); });
    log.count = 0;
    std::size_t measured = 0;
    {
        fw::tests::allocation_event_scope_t scope (log);
        measured = fw::tests::count_allocations ([&] {
            runtime.compose_node_request_reply (operation, std::move (submitted),
                                                std::move (*receiver));
        });
    }
    ASSERT_FALSE (log.overflow);
    std::size_t conversion_states = 0;
    for (std::size_t index = 0; index < log.count; ++index)
        conversion_states += events[index].size == conversion_state_size;
    std::printf ("ready node reply: conversion_states=%zu state_bytes=%zu allocations=%zu\n",
                 conversion_states, conversion_state_size, measured);
    EXPECT_EQ (conversion_states, 0u) << fw::tests::describe_allocations (log);
    EXPECT_FALSE (pending.await_ready ());
    ASSERT_TRUE (pending.admitted ().await_ready ());
    EXPECT_TRUE (pending.admitted ().result ());
    fw::runtime::host::operation_completion_t reply;
    reply.record.terminal_result = static_cast<int> (zlink::request_result_t::ok);
    reply.parts.push_back (zlink::message_t::from (std::string ("reply")));
    operation.completion->complete (
      fw::result_t<fw::runtime::host::operation_completion_t>::success (std::move (reply)));
    const auto &result = pending.result ();
    ASSERT_TRUE (result);
    ASSERT_TRUE (result.value ());
    EXPECT_EQ (result.value ().value ().items ().size (), 1u);
}

TEST (RequestCallAllocations, ReadyAdmissionKeepsALateReplyFailureSeparate)
{
    auto state = fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> (
      "late-projection-failure");
    fw::detail::mesh_node_runtime_t runtime (state);
    fw::runtime::host::pending_operation_t operation;
    auto pending =
      fw::detail::request_task_access_t::receive_internal<reply_t> ([&] (auto receiver) {
          runtime.compose_node_request_reply (
            operation,
            fw::task_t<zlink::submit_result_t> (
              fw::result_t<zlink::submit_result_t>::success (zlink::submit_result_t::ok)),
            std::move (receiver));
      });
    ASSERT_TRUE (pending.admitted ().result ());
    operation.completion->complete (
      fw::result_t<fw::runtime::host::operation_completion_t>::failure (
        fw::framework_error_kind_t::deadline_exceeded, "late reply timeout"));
    const auto &result = pending.result ();
    ASSERT_TRUE (result);
    EXPECT_EQ (result.value ().error_kind (), fw::framework_error_kind_t::deadline_exceeded);
    EXPECT_TRUE (pending.admitted ().result ());
}
fw::task_t<int> read_projected_reply (fw::task_t<int> reply)
{
    co_return co_await reply;
}

TEST (RequestCallAllocations, AdoptedReplyPreservesBothResumeSchedulers)
{
    fw::task_completion_source_t<int> completion;
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    admitted->complete (fw::result_t<void>::success ());
    std::atomic<unsigned> scheduled{0};
    auto reply = fw::detail::with_task_resume_scheduler (
      completion.task (), [&] (std::function<void ()> &&continuation) {
          ++scheduled;
          continuation ();
      });
    auto pending = fw::detail::request_task_access_t::from_admitted_reply (std::move (reply),
                                                                           std::move (admitted));
    auto physical = fw::detail::request_task_access_t::physical_reply (pending);
    auto physical_wait = read_projected_reply (std::move (physical));
    auto public_wait = read_projected_reply (std::move (static_cast<fw::task_t<int> &> (pending)));
    completion.complete (fw::result_t<int>::success (42));
    ASSERT_TRUE (physical_wait.result ());
    ASSERT_TRUE (public_wait.result ());
    EXPECT_EQ (physical_wait.result ().value (), 42);
    EXPECT_EQ (public_wait.result ().value (), 42);
    EXPECT_EQ (scheduled.load (), 2u);
}
TEST (RequestCallAllocations, ReceiverCopiesRetainPhysicalProjectionWithoutAllocating)
{
    std::optional<fw::detail::request_receiver_t<long>> selected;
    auto lifetime = std::make_shared<int> (42);
    std::weak_ptr<int> weak = lifetime;
    int projections = 0;
    auto pending = fw::detail::request_task_access_t::receive_public<int> ([&] (auto receiver) {
        selected.emplace (receiver.template project<long> (
          [lifetime, &projections] (const fw::result_t<long> &result) {
              ++projections;
              return fw::result_t<int>::success (static_cast<int> (result.value ()) + *lifetime);
          }));
    });
    lifetime.reset ();
    std::optional<fw::detail::request_receiver_t<long>> copy;
    const auto allocations = fw::tests::count_allocations ([&] { copy.emplace (*selected); });
    EXPECT_EQ (allocations, 0u);
    selected.reset ();
    ASSERT_FALSE (weak.expired ());
    const auto failure = fw::result_t<void>::failure (fw::framework_error_kind_t::unavailable,
                                                      "admission failed before physical reply");
    copy->admitted->complete (failure);
    ASSERT_TRUE (pending.await_ready ());
    EXPECT_EQ (pending.result ().exception (), failure.exception ());
    auto physical = fw::detail::request_task_access_t::physical_reply (pending);
    ASSERT_FALSE (physical.await_ready ());
    copy->complete (fw::result_t<long>::success (1));
    EXPECT_EQ (physical.result ().value (), 43);
    EXPECT_EQ (pending.result ().exception (), failure.exception ());
    EXPECT_EQ (projections, 1);
    copy.reset ();
    EXPECT_TRUE (weak.expired ());
}

TEST (RequestCallAllocations, TerminalObserversKeepRegistrationOrderDuringReentry)
{
    fw::task_completion_source_t<int> source;
    auto task = source.task ();
    std::vector<int> order;
    fw::detail::observe_task_terminal (task, [&] (const auto &) {
        order.push_back (1);
        fw::detail::observe_task_terminal (task, [&] (const auto &) { order.push_back (9); });
    });
    fw::detail::observe_task_terminal (task, [&] (const auto &) { order.push_back (2); });
    source.complete (fw::result_t<int>::success (42));
    EXPECT_EQ (order, (std::vector<int>{1, 9, 2}));
}

TEST (RequestCallAllocations, SharedAdmissionSourceUsesOneAllocationAndReleasesItsState)
{
    std::shared_ptr<fw::task_completion_source_t<int>> source;
    const auto allocations = fw::tests::count_allocations (
      [&] { source = fw::detail::make_task_completion_source<int> (); });
    EXPECT_EQ (allocations, 1u);
    std::optional<fw::task_t<int>> pending (source->task ());
    std::weak_ptr<fw::task_completion_source_t<int>> state = source;
    source->complete (fw::result_t<int>::success (42));
    source.reset ();
    ASSERT_FALSE (state.expired ());
    EXPECT_EQ (pending->result ().value (), 42);
    pending.reset ();
    EXPECT_TRUE (state.expired ());
}

TEST (RequestCallAllocations, CompletionObserverReleasesCapturesBeforeDeliveryStorage)
{
    fw::task_completion_source_t<int> source;
    std::function<void ()> delivery;
    auto task = fw::detail::with_task_resume_scheduler (
      source.task (), [&] (std::function<void ()> work) { delivery = std::move (work); });
    auto captured = std::make_shared<int> (42);
    std::weak_ptr<int> weak = captured;
    int calls = 0;
    fw::detail::observe_task_completion (task, [captured, &calls] (const auto &result) {
        EXPECT_EQ (result.value (), *captured);
        ++calls;
    });
    captured.reset ();
    source.complete (fw::result_t<int>::success (42));
    ASSERT_TRUE (delivery);
    ASSERT_FALSE (weak.expired ());
    delivery ();
    EXPECT_EQ (calls, 1);
    EXPECT_TRUE (weak.expired ());
    EXPECT_TRUE (delivery);
    delivery ();
    EXPECT_EQ (calls, 1);
}

TEST (RequestCallAllocations, SharedAdmissionSourceCannotMoveAfterRegistration)
{
    auto source = fw::detail::make_task_completion_source<int> ();
    auto task = source->task ();
    fw::detail::observe_task_completion (task, [] (const auto &) {});
    EXPECT_DEATH ({ fw::task_completion_source_t<int> moved (std::move (*source)); }, "");
    EXPECT_DEATH (
      {
          fw::task_completion_source_t<int> destination;
          destination = std::move (*source);
      },
      "");
    EXPECT_DEATH (
      {
          fw::task_completion_source_t<int> replacement;
          *source = std::move (replacement);
      },
      "");
    EXPECT_TRUE (source->complete (fw::result_t<int>::success (42)));
    EXPECT_EQ (task.result ().value (), 42);
}

TEST (RequestCallAllocations, InlineRawCallbackCopiesAndMovesWithoutAllocating)
{
    using callback_t = fw::detail::backend::raw_request_callback_t;
    using raw_result_t = fw::result_t<fw::detail::backend::raw_request_completion_t>;
    auto captured = std::make_shared<int> (42);
    std::weak_ptr<int> weak = captured;
    int calls = 0;
    callback_t original = [captured, &calls,
                           padding = std::array<char, 64>{}] (raw_result_t result) {
        EXPECT_TRUE (result);
        EXPECT_EQ (*captured, 42);
        EXPECT_EQ (padding.front (), 0);
        ++calls;
    };
    captured.reset ();
    callback_t copy;
    const auto allocations = fw::tests::count_allocations ([&] { copy = original; });
    EXPECT_EQ (allocations, 0u);
    callback_t moved (std::move (copy));
    EXPECT_FALSE (copy);
    original = nullptr;
    EXPECT_FALSE (weak.expired ());
    moved (raw_result_t::success ({}));
    EXPECT_EQ (calls, 1);
    moved = nullptr;
    EXPECT_TRUE (weak.expired ());
}

TEST (RequestCallAllocations, InlineRawCallbackCopyFailureKeepsTheDestination)
{
    using callback_t = fw::detail::backend::raw_request_callback_t;
    using raw_result_t = fw::result_t<fw::detail::backend::raw_request_completion_t>;
    struct throwing_copy_t
    {
        bool *throw_copy;
        int *calls;
        throwing_copy_t (bool *flag, int *count) : throw_copy (flag), calls (count) {}
        throwing_copy_t (throwing_copy_t &&) noexcept = default;
        throwing_copy_t (const throwing_copy_t &other) :
            throw_copy (other.throw_copy), calls (other.calls)
        {
            if (*throw_copy)
                throw std::runtime_error ("callback copy failed");
        }
        void operator() (raw_result_t) const { ++*calls; }
    };
    bool throw_copy = false;
    int source_calls = 0;
    int destination_calls = 0;
    callback_t source = throwing_copy_t (&throw_copy, &source_calls);
    callback_t destination = [&] (raw_result_t) { ++destination_calls; };
    throw_copy = true;
    EXPECT_THROW (destination = source, std::runtime_error);
    destination (raw_result_t::success ({}));
    EXPECT_EQ (destination_calls, 1);
    EXPECT_EQ (source_calls, 0);
    throw_copy = false;
    destination = source;
    destination (raw_result_t::success ({}));
    EXPECT_EQ (source_calls, 1);
    callback_t empty = std::function<void (raw_result_t)>{};
    EXPECT_FALSE (empty);
    EXPECT_THROW (empty (raw_result_t::success ({})), std::bad_function_call);
}

} // namespace

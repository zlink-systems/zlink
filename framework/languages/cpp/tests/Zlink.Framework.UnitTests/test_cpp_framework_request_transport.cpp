/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <gtest/gtest.h>
#include "allocation_counter.hpp"
#include <array>
#include <cstdio>
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime_failure_fixture.hpp"
#include <optional>
namespace
{
namespace fw = zlink::framework;
using transport_reply_t = fw::result_t<fw::runtime::messaging::message_parts_t>;
fw::task_t<transport_reply_t> await_node_reply (fw::task_t<transport_reply_t> &reply)
{
    co_return co_await reply;
}

TEST (RequestCallAllocations, PendingAdmissionOwnsTheOperationAndPrecedesSuccessfulReply)
{
    auto state =
      fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> ("pending-request");
    fw::detail::mesh_node_runtime_t runtime (state);
    fw::task_completion_source_t<zlink::submit_result_t> submitted;
    std::shared_ptr<fw::task_completion_source_t<fw::runtime::host::operation_completion_t>>
      completion;
    std::optional<fw::request_task_t<transport_reply_t>> pending;
    {
        fw::runtime::host::pending_operation_t operation;
        completion = operation.completion;
        pending.emplace (fw::detail::request_task_access_t::receive_public<transport_reply_t> (
          [&] (auto receiver) {
              runtime.compose_node_request_reply (std::move (operation), submitted.task (),
                                                  std::move (receiver));
          }));
    }
    fw::runtime::host::operation_completion_t reply;
    reply.record.terminal_result = static_cast<int> (zlink::request_result_t::ok);
    completion->complete (
      fw::result_t<fw::runtime::host::operation_completion_t>::success (std::move (reply)));
    EXPECT_FALSE (pending->await_ready ());
    EXPECT_FALSE (pending->admitted ().await_ready ());
    submitted.complete (
      fw::result_t<zlink::submit_result_t>::success (zlink::submit_result_t::not_found));
    const auto &result = pending->result ();
    ASSERT_FALSE (result);
    EXPECT_EQ (result.error_kind (), fw::framework_error_kind_t::not_found);
    EXPECT_EQ (pending->admitted ().result ().error_kind (), fw::framework_error_kind_t::not_found);
}

TEST (RequestCallAllocations, ReadySubmissionPreservesAnEarlierTerminalFailure)
{
    auto state =
      fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> ("ready-request");
    fw::detail::mesh_node_runtime_t runtime (state);
    fw::runtime::host::pending_operation_t operation;
    operation.completion->complete (
      fw::result_t<fw::runtime::host::operation_completion_t>::failure (
        fw::framework_error_kind_t::deadline_exceeded, "reply already timed out"));
    auto pending =
      fw::detail::request_task_access_t::receive_internal<transport_reply_t> ([&] (auto receiver) {
          runtime.compose_node_request_reply (
            std::move (operation),
            fw::task_t<zlink::submit_result_t> (
              fw::result_t<zlink::submit_result_t>::success (zlink::submit_result_t::ok)),
            std::move (receiver));
      });
    ASSERT_TRUE (pending.await_ready ());
    ASSERT_TRUE (pending.result ());
    EXPECT_EQ (pending.result ().value ().error_kind (),
               fw::framework_error_kind_t::deadline_exceeded);
    EXPECT_EQ (pending.admitted ().result ().error_kind (),
               fw::framework_error_kind_t::deadline_exceeded);
}


TEST (RequestCallAllocations, ReadyRejectedSubmissionPrecedesAnUnobservedHostFailure)
{
    for (const bool exceptional_submission : {false, true}) {
        auto state = fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> (
          "rejected-ready-request");
        fw::detail::mesh_node_runtime_t runtime (state);
        fw::runtime::host::pending_operation_t operation;
        operation.completion->complete (
          fw::result_t<fw::runtime::host::operation_completion_t>::failure (
            fw::framework_error_kind_t::deadline_exceeded, "host reply already timed out"));
        const auto expected = exceptional_submission ? fw::framework_error_kind_t::unavailable
                                                     : fw::framework_error_kind_t::not_found;
        auto submitted = fw::task_t<zlink::submit_result_t> (
          exceptional_submission
            ? fw::result_t<zlink::submit_result_t>::failure (expected, "submission failed")
            : fw::result_t<zlink::submit_result_t>::success (zlink::submit_result_t::not_found));
        auto pending = fw::detail::request_task_access_t::receive_public<int, transport_reply_t> (
          [&] (auto receiver) {
              runtime.compose_node_request_reply (operation, std::move (submitted),
                                                  std::move (receiver));
          },
          [] (const fw::result_t<transport_reply_t> &terminal) {
              const auto &reply = terminal.value ();
              return reply ? fw::result_t<int>::success (1)
                           : fw::detail::result_access_t::failure<int> (reply.exception ());
          });
        ASSERT_TRUE (pending.await_ready ());
        ASSERT_FALSE (pending.result ());
        EXPECT_EQ (pending.result ().error_kind (), expected);
        EXPECT_EQ (pending.admitted ().result ().error_kind (), expected);
        auto physical = fw::detail::request_task_access_t::physical_reply (pending);
        ASSERT_TRUE (physical.await_ready ());
        EXPECT_EQ (physical.result ().error_kind (), expected);
    }
}

TEST (RequestCallAllocations, PendingAdmissionRetainsTheExistingWaitOwnerCancellation)
{
    auto state = fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> (
      "pending-request-owner");
    fw::detail::mesh_node_runtime_t runtime (state);
    fw::runtime::host::pending_operation_t operation;
    fw::task_completion_source_t<zlink::submit_result_t> submission;
    std::stop_source owner;
    auto pending = [&] {
        const fw::detail::ambient_context_scope_t scope (nullptr, nullptr, owner.get_token ());
        return fw::detail::request_task_access_t::receive_public<transport_reply_t> (
          [&] (auto receiver) {
              runtime.compose_node_request_reply (operation, submission.task (),
                                                  std::move (receiver));
          });
    }();
    EXPECT_FALSE (pending.await_ready ());
    owner.request_stop ();
    const auto &cancelled = pending.result ();
    ASSERT_FALSE (cancelled);
    EXPECT_EQ (cancelled.error_kind (), fw::framework_error_kind_t::shutting_down);
    EXPECT_EQ (pending.admitted ().result ().error_kind (),
               fw::framework_error_kind_t::shutting_down);
    submission.complete (
      fw::result_t<zlink::submit_result_t>::success (zlink::submit_result_t::ok));
    fw::runtime::host::operation_completion_t reply;
    reply.record.terminal_result = static_cast<int> (zlink::request_result_t::ok);
    operation.completion->complete (
      fw::result_t<fw::runtime::host::operation_completion_t>::success (std::move (reply)));
    EXPECT_EQ (pending.result ().error_kind (), fw::framework_error_kind_t::shutting_down);
}

class deferred_node_lookup_t final : public fw::runtime::in_memory_location_repository_t
{
  public:
    fw::task_completion_source_t<fw::location_page_t<fw::mesh_node_descriptor_t>> lookup;
    unsigned calls = 0;
    fw::task_t<fw::location_page_t<fw::mesh_node_descriptor_t>>
    list_mesh_nodes (std::string, fw::location_page_request_t = {}) override
    {
        ++calls;
        if (calls == 1)
            return lookup.task ();
        return fw::task_t<fw::location_page_t<fw::mesh_node_descriptor_t>> (
          fw::result_t<fw::location_page_t<fw::mesh_node_descriptor_t>>::success ({}));
    }
};

TEST (RequestCallAllocations, UnknownLookupRetainsTheTargetAfterTheCallerReturns)
{
    fw::completion_owner_t completion_owner;
    auto state =
      fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> ("lookup-request");
    state->core_context = std::make_shared<zlink::context_t> ();
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->routing_id = zlink::routing_id_t::from ("lookup-source");
    state->object_role = fw::object_role_t::none;
    auto store = std::make_shared<deferred_node_lookup_t> ();
    fw::detail::mesh_node_runtime_t runtime (state);
    runtime.configure_user_spot_operations (store, {}, {});
    runtime.start ();
    struct stop_t
    {
        fw::detail::mesh_node_runtime_t &runtime;
        ~stop_t () { runtime.stop (); }
    } stop{runtime};
    std::optional<fw::request_task_t<transport_reply_t>> pending;
    {
        auto target = zlink::routing_id_t::from ("lookup-client");
        pending.emplace (fw::detail::request_task_access_t::receive_public<transport_reply_t> (
          [&] (auto receiver) {
              runtime.request_node_reply (target, {}, std::chrono::seconds (1),
                                          std::move (receiver));
          }));
        target = zlink::routing_id_t::from ("changed-after-submit");
    }
    ASSERT_EQ (store->calls, 1u);
    EXPECT_FALSE (pending->await_ready ());
    fw::location_page_t<fw::mesh_node_descriptor_t> page;
    fw::mesh_node_descriptor_t descriptor;
    descriptor.rid = zlink::routing_id_t::from ("lookup-client");
    descriptor.object_role = fw::object_role_t::client;
    page.items.push_back (std::move (descriptor));
    page.continuation_token = "next-page";
    store->lookup.complete (
      fw::result_t<fw::location_page_t<fw::mesh_node_descriptor_t>>::success (std::move (page)));
    const auto &result = pending->result ();
    ASSERT_FALSE (result);
    EXPECT_EQ (result.error_kind (), fw::framework_error_kind_t::not_found);
    EXPECT_EQ (pending->admitted ().result ().error_kind (), fw::framework_error_kind_t::not_found);
    EXPECT_EQ (store->calls, 1u);
}
TEST (RequestCallAllocations, UnknownLookupRetainsOwnerCancellationAcrossRuntimeClose)
{
    auto state = fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> (
      "lookup-request-owner");
    state->core_context = std::make_shared<zlink::context_t> ();
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->routing_id = zlink::routing_id_t::from ("lookup-source");
    state->object_role = fw::object_role_t::none;
    auto store = std::make_shared<deferred_node_lookup_t> ();
    fw::detail::mesh_node_runtime_t runtime (state);
    runtime.configure_user_spot_operations (store, {}, {});
    runtime.start ();
    struct stop_t
    {
        fw::detail::mesh_node_runtime_t &runtime;
        ~stop_t () { runtime.stop (); }
    } stop{runtime};
    fw::runtime::messaging::client_call_codec_t codec;
    auto header = codec.create_envelope (fw::runtime::messaging::message_kind_t::request, "m", "p",
                                         std::chrono::seconds (1));
    auto parts = codec.encode_payload_parts (
      std::move (header),
      fw::serialized_payload_t{fw::encoded_payload_t::from_string ("request"), "x/y"});
    const auto target = zlink::routing_id_t::from ("lookup-target");
    // Cancellation §5.1 ends this owner's wait, without replacing the producer's result.
    auto pending =
      fw::detail::request_task_access_t::receive_public<transport_reply_t> ([&] (auto receiver) {
          runtime.request_node_reply (target, std::move (parts).take_items (),
                                      std::chrono::seconds (1), std::move (receiver));
      });
    auto physical = fw::detail::request_task_access_t::physical_reply (pending);
    ASSERT_EQ (store->calls, 1u);
    ASSERT_FALSE (pending.await_ready ());
    std::stop_source owner;
    auto observed = [&] {
        const fw::detail::ambient_context_scope_t scope (nullptr, nullptr, owner.get_token ());
        return await_node_reply (pending);
    }();
    owner.request_stop ();
    ASSERT_FALSE (observed.result ());
    EXPECT_EQ (observed.result ().error_kind (), fw::framework_error_kind_t::shutting_down);
    EXPECT_FALSE (pending.await_ready ());
    EXPECT_FALSE (physical.await_ready ());
    runtime.stop ();
    fw::location_page_t<fw::mesh_node_descriptor_t> page;
    fw::mesh_node_descriptor_t descriptor;
    descriptor.rid = target;
    descriptor.object_role = fw::object_role_t::client;
    page.items.push_back (std::move (descriptor));
    store->lookup.complete (
      fw::result_t<fw::location_page_t<fw::mesh_node_descriptor_t>>::success (std::move (page)));
    ASSERT_FALSE (pending.result ());
    EXPECT_EQ (pending.result ().error_kind (), fw::framework_error_kind_t::not_found);
    EXPECT_EQ (pending.admitted ().result ().error_kind (), fw::framework_error_kind_t::not_found);
    ASSERT_TRUE (physical.result ());
    ASSERT_FALSE (physical.result ().value ());
    EXPECT_EQ (physical.result ().value ().error_kind (), fw::framework_error_kind_t::not_found);
    EXPECT_EQ (store->calls, 1u);
    EXPECT_EQ (observed.result ().error_kind (), fw::framework_error_kind_t::shutting_down);
}

TEST (RequestCallAllocations, PendingNodeLookupDoesNotCreateAnOperationBeforeResolution)
{
    std::array<fw::tests::allocation_event_t, 64> events{};
    fw::tests::allocation_event_log_t log{events.data (), events.size ()};
    fw::tests::allocation_event_log = &log;
    const auto calibration = fw::tests::count_allocations (
      [] { (void) fw::task_completion_source_t<fw::runtime::host::operation_completion_t> (); });
    fw::tests::allocation_event_log = nullptr;
    ASSERT_EQ (calibration, 1u);
    const auto operation_state_size = events[0].size;
    auto state = fw::test::runtime_failure_fixture<fw::detail::mesh_node_builder_state_t> (
      "lookup-operation-cost");
    state->core_context = std::make_shared<zlink::context_t> ();
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->routing_id = zlink::routing_id_t::from ("lookup-cost-source");
    state->object_role = fw::object_role_t::none;
    auto store = std::make_shared<deferred_node_lookup_t> ();
    fw::detail::mesh_node_runtime_t runtime (state);
    runtime.configure_user_spot_operations (store, {}, {});
    runtime.start ();
    struct stop_t
    {
        fw::detail::mesh_node_runtime_t &runtime;
        ~stop_t () { runtime.stop (); }
    } stop{runtime};
    const auto target = zlink::routing_id_t::from ("lookup-cost-client");
    std::optional<fw::request_task_t<transport_reply_t>> pending;
    log.count = 0;
    fw::tests::allocation_event_log = &log;
    const auto allocations = fw::tests::count_allocations ([&] {
        pending.emplace (fw::detail::request_task_access_t::receive_public<transport_reply_t> (
          [&] (auto receiver) {
              runtime.request_node_reply (target, {}, std::chrono::seconds (1),
                                          std::move (receiver));
          }));
    });
    fw::tests::allocation_event_log = nullptr;
    ASSERT_FALSE (log.overflow);
    std::size_t operation_states = 0;
    for (std::size_t index = 0; index < log.count; ++index)
        operation_states += events[index].size == operation_state_size;
    std::printf ("pending lookup: operation_states=%zu state_bytes=%zu total=%zu\n",
                 operation_states, operation_state_size, allocations);
    EXPECT_EQ (operation_states, 0u);
    fw::location_page_t<fw::mesh_node_descriptor_t> page;
    fw::mesh_node_descriptor_t descriptor;
    descriptor.rid = target;
    descriptor.object_role = fw::object_role_t::client;
    page.items.push_back (std::move (descriptor));
    store->lookup.complete (
      fw::result_t<fw::location_page_t<fw::mesh_node_descriptor_t>>::success (std::move (page)));
    ASSERT_FALSE (pending->result ());
    EXPECT_EQ (pending->result ().error_kind (), fw::framework_error_kind_t::not_found);
}
} // namespace

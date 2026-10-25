/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <gtest/gtest.h>
#include "allocation_counter.hpp"
#include <array>
#include <cstdio>
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime_failure_fixture.hpp"
#include <optional>
namespace
{
namespace fw = zlink::framework;
using transport_reply_t = fw::result_t<fw::runtime::messaging::message_parts_t>;
TEST (RequestCallAllocations, ReadySubmissionPreservesAnEarlierTerminalFailure)
{
    fw::task_completion_source_t<zlink::message_t> completion;
    completion.complete (fw::result_t<zlink::message_t>::failure (
      fw::framework_error_kind_t::deadline_exceeded, "reply already timed out"));
    fw::channel_request_call_t call ("request", nullptr, [&] (const auto &, auto, const auto &) {
        auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
        fw::request_task_t<zlink::message_t> selected (completion.task (), admitted);
        admitted->complete (fw::result_t<void>::success ());
        return selected;
    });
    auto pending = call.async<int> ();
    ASSERT_TRUE (pending.await_ready ());
    ASSERT_FALSE (pending.result ());
    EXPECT_EQ (pending.result ().error_kind (), fw::framework_error_kind_t::deadline_exceeded);
    EXPECT_EQ (pending.admitted ().result ().error_kind (),
               fw::framework_error_kind_t::deadline_exceeded);
}

TEST (RequestCallAllocations, ReturnedPhysicalReplyPreservesAnEarlierAdmissionFailure)
{
    fw::serializer_registry_t serializers;
    unsigned decoded = 0;
    serializers.add<int> (
      [] (const int &value) { return fw::encoded_payload_t::from_string (std::to_string (value)); },
      [&] (const fw::encoded_payload_t &) {
          ++decoded;
          return 42;
      },
      "text/plain");
    fw::task_completion_source_t<zlink::message_t> physical;
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    const auto failure =
      fw::result_t<void>::failure (fw::framework_error_kind_t::unavailable,
                                   "admission failed before the returned physical reply");
    fw::channel_request_call_t call (
      "request", &serializers, [&] (const auto &, auto, const auto &) {
          fw::request_task_t<zlink::message_t> selected (physical.task (), admitted);
          admitted->complete (failure);
          physical.complete (
            fw::result_t<zlink::message_t>::success (zlink::message_t::from ("42")));
          return selected;
      });
    auto pending = call.async<int> ();
    ASSERT_TRUE (pending.await_ready ());
    ASSERT_FALSE (pending.result ());
    EXPECT_EQ (pending.result ().exception (), failure.exception ());
    EXPECT_EQ (pending.admitted ().result ().exception (), failure.exception ());
    EXPECT_EQ (decoded, 1u);
}

TEST (RequestCallAllocations, ReadyAdmissionKeepsALateReplyFailureSeparate)
{
    fw::task_completion_source_t<zlink::message_t> physical;
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    fw::channel_request_call_t call ("request", nullptr, [&] (const auto &, auto, const auto &) {
        admitted->complete (fw::result_t<void>::success ());
        return fw::request_task_t<zlink::message_t> (physical.task (), admitted);
    });
    auto pending = call.async<int> ();
    ASSERT_TRUE (pending.admitted ().result ());
    EXPECT_FALSE (pending.await_ready ());
    const auto failure = fw::result_t<zlink::message_t>::failure (
      fw::framework_error_kind_t::deadline_exceeded, "late reply timeout");
    ASSERT_TRUE (physical.complete (failure));
    ASSERT_FALSE (pending.result ());
    EXPECT_EQ (pending.result ().exception (), failure.exception ());
    EXPECT_EQ (pending.result ().error_kind (), fw::framework_error_kind_t::deadline_exceeded);
    EXPECT_TRUE (pending.admitted ().result ());
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
    auto prepared =
      fw::detail::request_task_access_t::prepare<fw::runtime::messaging::message_parts_t,
                                                 fw::runtime::messaging::message_parts_t> (
        [] (const auto &result) { return result; });
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    prepared.second.admitted = admitted;
    prepared.second.fail.bind_admission (admitted);
    fw::detail::request_task_access_t::connect_admission (prepared.first, admitted);
    auto pending = std::move (prepared.first);
    {
        auto target = zlink::routing_id_t::from ("lookup-client");
        runtime.submit_node_request_reply (target, {}, std::chrono::seconds (1),
                                           std::move (prepared.second));
        target = zlink::routing_id_t::from ("changed-after-submit");
    }
    ASSERT_EQ (store->calls, 1u);
    EXPECT_FALSE (pending.await_ready ());
    fw::location_page_t<fw::mesh_node_descriptor_t> page;
    fw::mesh_node_descriptor_t descriptor;
    descriptor.rid = zlink::routing_id_t::from ("lookup-client");
    descriptor.object_role = fw::object_role_t::client;
    page.items.push_back (std::move (descriptor));
    page.continuation_token = "next-page";
    store->lookup.complete (
      fw::result_t<fw::location_page_t<fw::mesh_node_descriptor_t>>::success (std::move (page)));
    const auto &result = pending.result ();
    ASSERT_FALSE (result);
    EXPECT_EQ (result.error_kind (), fw::framework_error_kind_t::not_found);
    EXPECT_EQ (pending.admitted ().result ().error_kind (), fw::framework_error_kind_t::not_found);
    EXPECT_EQ (store->calls, 1u);
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
    auto prepared =
      fw::detail::request_task_access_t::prepare<fw::runtime::messaging::message_parts_t,
                                                 fw::runtime::messaging::message_parts_t> (
        [] (const auto &result) { return result; });
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    prepared.second.admitted = admitted;
    prepared.second.fail.bind_admission (admitted);
    fw::detail::request_task_access_t::connect_admission (prepared.first, admitted);
    auto pending = std::move (prepared.first);
    log.count = 0;
    fw::tests::allocation_event_log = &log;
    const auto allocations = fw::tests::count_allocations ([&] {
        runtime.submit_node_request_reply (target, {}, std::chrono::seconds (1),
                                           std::move (prepared.second));
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
    ASSERT_FALSE (pending.result ());
    EXPECT_EQ (pending.result ().error_kind (), fw::framework_error_kind_t::not_found);
}
} // namespace

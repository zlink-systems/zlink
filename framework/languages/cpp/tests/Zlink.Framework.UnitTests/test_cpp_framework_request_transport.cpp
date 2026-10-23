/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <gtest/gtest.h>
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime_failure_fixture.hpp"
#include <optional>
namespace
{
namespace fw = zlink::framework;
using transport_reply_t = fw::result_t<fw::runtime::messaging::message_parts_t>;
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
        pending.emplace (
          runtime.compose_node_request_reply (std::move (operation), submitted.task ()));
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
    auto pending = runtime.compose_node_request_reply (
      std::move (operation),
      fw::task_t<zlink::submit_result_t> (
        fw::result_t<zlink::submit_result_t>::success (zlink::submit_result_t::ok)));
    ASSERT_TRUE (pending.await_ready ());
    ASSERT_TRUE (pending.result ());
    EXPECT_EQ (pending.result ().value ().error_kind (),
               fw::framework_error_kind_t::deadline_exceeded);
    EXPECT_EQ (pending.admitted ().result ().error_kind (),
               fw::framework_error_kind_t::deadline_exceeded);
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
    state->listen_endpoint = "tcp://127.0.0.1:7484";
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
        pending.emplace (runtime.request_node_reply (target, {}, std::chrono::seconds (1)));
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
} // namespace

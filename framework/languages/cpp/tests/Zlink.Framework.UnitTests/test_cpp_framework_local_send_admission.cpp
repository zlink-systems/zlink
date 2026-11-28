/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/messaging/submit_result_mapper.hpp"
#include "runtime/stateful/public_host_runtime.hpp"

#include <gtest/gtest.h>

#include <latch>
#include <thread>

namespace
{
namespace fw = zlink::framework;
namespace rt = fw::runtime;
namespace host = rt::host;
namespace mesh = rt::mesh;
namespace stateful = rt::stateful;

class LocalSendAdmission : public testing::TestWithParam<bool>
{
  protected:
    void SetUp () override
    {
        rt::application_job_queue_configuration_t capacity;
        capacity.effective_max_queued_application_jobs = 1;
        jobs = std::make_shared<rt::application_job_queue_t> (capacity);
        native = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
          .mesh = {.descriptor = {.mesh_name = "local-admission",
                                  .node_routing_id = node_rid.to_bytes (),
                                  .lifecycle_generation = 1,
                                  .descriptor_revision = 1,
                                  .advertised_endpoint = "tcp://127.0.0.1:0"},
                   .application_jobs = jobs,
                   .runtime_failures = std::make_shared<rt::runtime_failure_collector_t> ()},
          .object_stable_types = {"framework.spot", "player"}});
        native->configure_stateful_dispatch (
          [] (const stateful::accepted_record_authority_query_t &)
            -> fw::task_t<std::optional<stateful::accepted_record_authority_t>> {
              co_return std::nullopt;
          });
        native->configure_spot_route_fence_resolver (
          [] (auto, auto, auto) -> fw::task_t<std::optional<host::route_fence_t>> {
              co_return host::route_fence_t{1, 1, 1};
          });
        native->start ();
        spot = native->get_or_create_spot (spot_id);
        actor = native->create_actor ("player", "local-actor");
        held = jobs->try_reserve_supply ();
        ASSERT_TRUE (held);
    }

    void TearDown () override
    {
        jobs->stop ();
        held.reset ();
        auto &mailbox = native->transport ().mailbox ();
        if (auto claim = mailbox.try_claim (mesh::service_mailbox_domain_t::application, 1,
                                            rt::dispatch_limits::receive_batch_bytes))
            mailbox.release (*claim);
        native->close ();
        EXPECT_EQ (0u, jobs->snapshot ().permits_in_use);
    }

    fw::task_t<zlink::submit_result_t> send ()
    {
        if (GetParam ())
            return actor->send_to (actor->ref (), parts, zlink::send_flags_t::none);
        return spot.send_to_spot (node_rid, spot_id, 1, parts, zlink::send_flags_t::none);
    }

    std::size_t mailbox_size () const
    {
        return native->transport ().mailbox ().pending_messages (
          mesh::service_mailbox_domain_t::application);
    }

    void expect_waiting (const fw::task_t<zlink::submit_result_t> &caller)
    {
        EXPECT_EQ (1u, jobs->snapshot ().capacity_waiters);
        EXPECT_EQ (0u, mailbox_size ());
        if (caller.await_ready ()) {
            const auto &result = caller.result ();
            ADD_FAILURE () << (result && result.value () == zlink::submit_result_t::ok
                                 ? "SEND returned Ok"
                                 : "SEND failed")
                           << " while waiting for mailbox admission";
        }
    }

    static fw::framework_error_kind_t
    error_kind (const fw::result_t<zlink::submit_result_t> &result)
    {
        return result ? rt::messaging::map_submit_result_error_kind (result.value ())
                      : result.error_kind ();
    }

    const zlink::routing_id_t node_rid = zlink::routing_id_t::from ("local");
    const std::string spot_id = "local-spot";
    const std::vector<zlink::message_t> parts{zlink::message_t::from ("payload")};
    std::shared_ptr<rt::application_job_queue_t> jobs;
    std::shared_ptr<host::public_host_runtime_t> native;
    host::spot_handle_t spot;
    std::optional<host::actor_handle_t> actor;
    std::optional<rt::application_job_queue_t::permit_t> held;
};

TEST_P (LocalSendAdmission, CompletesAfterMailboxAccepts)
{
    std::latch publication_entered (1), publish (1);
    native->transport ().mailbox ().bind_application_dispatch (
      [&] (auto &) {
          publication_entered.count_down ();
          publish.wait ();
      },
      {});
    auto caller = send ();
    expect_waiting (caller);

    std::jthread return_permit ([&] { held.reset (); });
    publication_entered.wait ();
    EXPECT_FALSE (caller.await_ready ()) << "SEND completed before mailbox publication";
    publish.count_down ();
    const auto result = caller.result ();
    native->transport ().mailbox ().bind_application_dispatch ({}, {});
    ASSERT_TRUE (result);
    EXPECT_EQ (zlink::submit_result_t::ok, result.value ());
    EXPECT_EQ (1u, mailbox_size ());
    EXPECT_EQ (0u, jobs->snapshot ().capacity_waiters);
}

TEST_P (LocalSendAdmission, ShutdownTerminatesPendingSend)
{
    auto caller = send ();
    expect_waiting (caller);

    jobs->stop ();
    native->close ();
    EXPECT_EQ (fw::framework_error_kind_t::shutting_down, error_kind (caller.result ()));
    EXPECT_EQ (0u, mailbox_size ());
    EXPECT_EQ (0u, jobs->snapshot ().capacity_waiters);
}

TEST_P (LocalSendAdmission, ClosedMailboxRejectsPendingSend)
{
    auto caller = send ();
    expect_waiting (caller);

    native->transport ().mailbox ().close ();
    held.reset ();
    EXPECT_EQ (fw::framework_error_kind_t::rejected, error_kind (caller.result ()));
    EXPECT_EQ (0u, mailbox_size ());
    EXPECT_EQ (0u, jobs->snapshot ().capacity_waiters);
}

INSTANTIATE_TEST_SUITE_P (SpotAndActor,
                          LocalSendAdmission,
                          testing::Bool (),
                          [] (const auto &info) { return info.param ? "Actor" : "Spot"; });
} // namespace

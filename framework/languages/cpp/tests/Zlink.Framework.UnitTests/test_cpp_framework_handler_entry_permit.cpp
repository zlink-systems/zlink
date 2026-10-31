/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <gtest/gtest.h>
#include <zlink/json.hpp>
#include "runtime/channels/route_handler_invoker.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/mesh/mesh_record_dispatcher.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime_failure_fixture.hpp"
#include "scope_exit.hpp"
#include "stateful_ingress_fixture.hpp"

namespace zlink::framework::runtime::host
{
struct public_host_runtime_test_access_t
{
    static task_t<std::size_t>
    dispatch_claim (public_host_runtime_t &host,
                    std::shared_ptr<mesh::service_mailbox_claim_t> claim,
                    mesh::service_mailbox_t::application_dispatch_t dispatch)
    {
        return host.dispatch_application_claim (std::move (claim), std::move (dispatch));
    }
};
}

namespace
{
using namespace zlink::framework;
namespace host = runtime::host;
namespace mesh = runtime::mesh;
namespace stateful = runtime::stateful;
namespace protocol = runtime::protocol;
using queue_t = runtime::application_job_queue_t;
constexpr auto domain = mesh::service_mailbox_domain_t::application;

struct invocation_t
{
    task_completion_source_t<void> resume;
    std::shared_ptr<queue_t> jobs;
    bool filter_entered = false;
    bool call_next = true;
    int handler_calls = 0;
    int permit_returns = 0;

    void handle (const int &value, const route_message_context_t &)
    {
        EXPECT_EQ (17, value);
        EXPECT_EQ (0u, jobs->snapshot ().permits_in_use);
        EXPECT_EQ (1, permit_returns);
        ++handler_calls;
    }
};

struct permit_handler_t
{
    invocation_t &invocation;
    void handle (const int &value, const route_message_context_t &context)
    {
        invocation.handle (value, context);
    }
};

class HandlerEntryPermit : public ::testing::Test
{
  protected:
    HandlerEntryPermit () :
        jobs (std::make_shared<queue_t> (runtime::application_job_queue_configuration_t{
          application_job_queue_profile_t::balanced, std::uint32_t{1}, 1, 1}))
    {
        invocation.jobs = jobs;
        services.add_factory<permit_handler_t> ([this] (service_provider_t &) {
            return std::make_unique<permit_handler_t> (permit_handler_t{invocation});
        });
        provider.emplace (services.build_provider ());
        handlers.on_send<permit_handler_t, int> ("permit-mesh", "packet",
                                                 &permit_handler_t::handle);
        filters.add_filter ([this] (service_provider_t &, serializer_registry_t &,
                                    const handler_filter_context_t &,
                                    handler_next_t next) -> task_t<void> {
            invocation.filter_entered = true;
            co_await invocation.resume.task ();
            if (invocation.call_next)
                co_await next ();
        });
    }

    mesh::service_mailbox_record_t queued (const std::string &owner)
    {
        mesh::service_mailbox_record_t record;
        record.owner = owner;
        record.domain = domain;
        record.application_permit = jobs->try_reserve_supply ();
        EXPECT_TRUE (record.application_permit);
        record.application_permit->mark_queued ();
        return record;
    }

    void check_duplicate_entry (std::function<void ()> &before)
    {
        auto next = jobs->try_reserve_supply ();
        ASSERT_TRUE (next);
        before ();
        EXPECT_EQ (1u, jobs->snapshot ().permits_in_use);
        before = {};
        next.reset ();
        EXPECT_EQ (0u, jobs->snapshot ().permits_in_use);
        EXPECT_EQ (1, invocation.permit_returns);
    }

    void node_case (bool call_next)
    {
        invocation.call_next = call_next;
        auto options = test::runtime_failure_options (
          mesh::raw_mesh_node_options_t{.descriptor = {"permit-mesh",
                                                       {'p', 'e', 'r', 'm', 'i', 't'},
                                                       1,
                                                       1,
                                                       "tcp://127.0.0.1:0",
                                                       {},
                                                       mesh::service_node_state_t::preparing}});
        host::public_host_runtime_t target (host::host_options_t{std::move (options)});
        target.start ();
        test::scope_exit_t cleanup ([&] {
            invocation.call_next = false;
            invocation.resume.complete (result_t<void>::success ());
            target.close ();
        });
        auto &mailbox = target.transport ().mailbox ();
        const auto owner = mesh::service_mailbox_t::application_owner (host::owner_kind_t::node);
        runtime::messaging::envelope_header_t header;
        header.kind = runtime::messaging::message_kind_t::command;
        header.channel_name = "permit-mesh";
        header.message_name = "packet";
        header.content_type = "application/json";
        auto parts = runtime::messaging::envelope_codec_t{}
                       .encode_raw_body_parts (header, zlink::message_t::from ("17"))
                       .take_items ();
        auto record = queued (owner);
        record.parts.push_back (zlink::message_t::from (protocol::encode_node_send_header ()));
        record.parts.push_back (zlink::message_t::from (protocol::encode_application_payload (
          protocol::application_payload_t::from_parts (std::move (parts)))));
        record.source_routing_id = {'p', 'e', 'e', 'r'};
        ASSERT_TRUE (mailbox.try_enqueue (std::move (record)));
        auto waiter = jobs->wait_for_supply (
          [&] (auto supplied) {
              EXPECT_TRUE (supplied);
              ++invocation.permit_returns;
          },
          queue_t::origin_t::remote);
        auto claim =
          mailbox.try_claim_owner (domain, owner, 1, runtime::dispatch_limits::receive_batch_bytes);
        ASSERT_TRUE (claim);
        std::weak_ptr<mesh::service_mailbox_claim_t> claim_lifetime = claim;
        std::optional<task_t<std::optional<detail::route_dispatch_reply_t>>> pending;
        std::function<void ()> duplicate;
        auto dispatched = host::public_host_runtime_test_access_t::dispatch_claim (
          target, std::move (claim), [&] (const auto &, const auto &received, auto payload) {
              EXPECT_EQ (host::record_kind_t::node_send, received.kind);
              EXPECT_EQ ("peer", received.source_node_rid.to_string ());
              // This is the hosted Node pipeline handoff. Its ordinary prefix
              // credit settles when this callback returns, before filter resume.
              detail::mesh_record_dispatcher_t dispatcher (
                *provider, serializers, handlers, filters, {}, received.before_application_handler);
              if (call_next)
                  duplicate = received.before_application_handler;
              pending.emplace (dispatcher.dispatch (received, std::move (payload), header));
          });
        ASSERT_TRUE (dispatched.await_ready ());
        ASSERT_EQ (1u, dispatched.result ().value ());
        EXPECT_TRUE (claim_lifetime.expired ());
        RecordProperty ("claim_destroyed_before_resume", claim_lifetime.expired ());
        EXPECT_EQ (0u, mailbox.pending_messages (domain));
        ASSERT_TRUE (invocation.filter_entered);
        ASSERT_TRUE (pending && !pending->await_ready ());
        RecordProperty ("permits_before_handler", jobs->snapshot ().permits_in_use);
        ASSERT_EQ (1u, jobs->snapshot ().permits_in_use)
          << "claim credit settlement returned the pre-handler permit";
        EXPECT_EQ (0, invocation.permit_returns);
        invocation.resume.complete (result_t<void>::success ());
        ASSERT_TRUE (pending->result ());
        pending.reset ();
        EXPECT_EQ (call_next ? 1 : 0, invocation.handler_calls);
        EXPECT_EQ (0u, jobs->snapshot ().permits_in_use);
        EXPECT_EQ (1, invocation.permit_returns);
        RecordProperty ("permit_returns", invocation.permit_returns);
        if (call_next)
            check_duplicate_entry (duplicate);
    }

    std::shared_ptr<queue_t> jobs;
    invocation_t invocation;
    service_collection_t services;
    std::optional<service_provider_t> provider;
    serializer_registry_t serializers;
    detail::route_handler_registry_t handlers;
    handler_registry_t filters;
};

TEST_F (HandlerEntryPermit, RemoteNodeAsyncFilterOutlivesClaim)
{
    node_case (true);
}

TEST_F (HandlerEntryPermit, RemoteNodeFilterTerminalWithoutHandlerReturnsPermit)
{
    node_case (false);
}

TEST_F (HandlerEntryPermit, StatefulAcceptedRecordAsyncFilterOutlivesClaim)
{
    auto options = test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{.descriptor = {"permit-mesh",
                                                   {'s', 't', 'a', 't', 'e'},
                                                   1,
                                                   1,
                                                   "tcp://127.0.0.1:0",
                                                   {},
                                                   mesh::service_node_state_t::preparing}});
    mesh::raw_mesh_node_owner_t transport (std::move (options));
    transport.start ();
    test::scope_exit_t cleanup ([&] {
        invocation.call_next = false;
        invocation.resume.complete (result_t<void>::success ());
        transport.close ();
    });
    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates ({{"permit-mesh", "state", {"player"}, 100, 16, 0, 8, 0}});
    const auto reserved = objects.begin_create (
      {stateful::object_kind_t::actor, "actor", "player", std::nullopt, {}, false, false});
    ASSERT_EQ (stateful::create_status_t::reserved, reserved.status);
    ASSERT_EQ (stateful::stateful_error_t::none, objects.commit_create (reserved.attempt));
    const auto actor = objects.find (stateful::object_kind_t::actor, "actor");
    ASSERT_TRUE (actor);
    stateful::raw_stateful_dispatch_t dispatch (objects, transport);
    const auto descriptor = transport.topology ().local_descriptor ();
    protocol::actor_message_header_t header;
    header.operation = {1, 17};
    header.target = {actor->key,
                     actor->object_generation,
                     descriptor.node_routing_id,
                     descriptor.lifecycle_generation,
                     actor->authority_owner_generation,
                     1};
    auto accepted = std::make_shared<runtime::serial_application_record_t> ();
    accepted->payload ().emplace ();
    accepted->payload ()->application_record = stateful::accepted_actor_record (
      header, {"source", 1, {'p', 'e', 'e', 'r'}, 1},
      protocol::application_payload_t{"packet", "application/json", {'1', '7'}}, false);
    auto original =
      queued (mesh::service_mailbox_t::application_owner (host::owner_kind_t::actor, actor->key));
    original.application.emplace ();
    auto slot = test::publish_stateful_claim (transport, *actor, std::move (original));
    auto waiter = jobs->wait_for_supply (
      [&] (auto supplied) {
          EXPECT_TRUE (supplied);
          ++invocation.permit_returns;
      },
      queue_t::origin_t::remote);
    std::weak_ptr<mesh::service_mailbox_claim_t> claim_lifetime = slot.claim;
    std::optional<mesh::service_mailbox_record_t> taken;
    ASSERT_EQ (stateful::stateful_error_t::none,
               dispatch.accept_local (*actor, std::move (accepted), std::move (slot), taken, {})
                 .result ()
                 .value ());
    taken.reset ();
    EXPECT_TRUE (claim_lifetime.expired ());
    RecordProperty ("claim_destroyed_before_resume", claim_lifetime.expired ());
    auto delivery = dispatch.try_claim_async (*actor).result ().value ();
    ASSERT_EQ (stateful::stateful_error_t::none, delivery.error);
    ASSERT_TRUE (delivery.delivery && delivery.before_application_handler);
    auto duplicate = delivery.before_application_handler;
    const auto *handler =
      handlers.resolve ("permit-mesh", runtime::messaging::message_kind_t::command, "packet");
    ASSERT_NE (nullptr, handler);
    route_message_context_t context{{}, zlink::routing_id_t::from ("source")};
    context.content_type = "application/json";
    auto pending = detail::route_handler_invoker_t::invoke (
      *handler, filters, handler_dispatch_kind_t::node_direct_send, *provider, serializers,
      zlink::message_t::from ("17"), std::move (context),
      std::move (delivery.before_application_handler));
    ASSERT_TRUE (invocation.filter_entered);
    ASSERT_FALSE (pending.await_ready ());
    RecordProperty ("permits_before_handler", jobs->snapshot ().permits_in_use);
    ASSERT_EQ (1u, jobs->snapshot ().permits_in_use)
      << "stateful accept settled the permit with its mailbox credit";
    EXPECT_EQ (0, invocation.permit_returns);
    invocation.resume.complete (result_t<void>::success ());
    ASSERT_TRUE (pending.result ());
    EXPECT_EQ (1, invocation.handler_calls);
    EXPECT_EQ (0u, jobs->snapshot ().permits_in_use);
    EXPECT_EQ (1, invocation.permit_returns);
    RecordProperty ("permit_returns", invocation.permit_returns);
    check_duplicate_entry (duplicate);
    EXPECT_EQ (stateful::stateful_error_t::none,
               dispatch.complete_async (*delivery.delivery).result ().value ());
}
}

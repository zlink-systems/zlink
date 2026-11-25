/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_observations.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "../support/actor_maintenance_join_native_fixture.hpp"
#include "../support/actor_maintenance_join_host_access.hpp"
#include "../support/stateful_object_sync.hpp"
#include "../support/stateful_ingress_fixture.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/foundation/operation_registry.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/execution/task_result.hpp"
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace zlink::framework::test::actor_maintenance_join
{
using zlink::framework::test::begin_reserved_object;

TEST (NativeIntake, RawAuthorityFailurePreservesTerminalKind)
{
    namespace rt = fw::runtime;
    namespace st = rt::stateful;
    namespace wire = rt::protocol;
    using kind = fw::framework_error_kind_t;
    for (const auto expected :
         {kind::internal_failure, kind::deadline_exceeded, kind::shutting_down, kind::rejected}) {
        SCOPED_TRACE (static_cast<int> (expected));
        int queries = 0;
        relay_peer_pair_t pair (
          rt::mesh::service_object_role_t::server, rt::mesh::service_object_role_t::server,
          [&] (st::accepted_record_authority_query_t query)
            -> fw::task_t<std::optional<st::accepted_record_authority_t>> {
              ++queries;
              if (expected == kind::rejected)
                  co_return st::accepted_record_authority_t{
                    {"source", 1, query.source_node_routing_id, query.source_node_generation}, 2};
              if (expected == kind::internal_failure)
                  throw std::runtime_error ("authority read failed");
              if (expected == kind::shutting_down)
                  std::rethrow_exception (
                    fw::detail::make_cancellation_exception ("authority read cancelled"));
              throw fw::framework_exception_t (expected, "authority read failed");
          });
        const auto deadline = std::chrono::steady_clock::now () + wait_budget;
        while (!pair.ready () && std::chrono::steady_clock::now () < deadline)
            pair.pump ();
        ASSERT_TRUE (pair.ready ());
        const st::object_ref_t object{
          st::object_kind_t::actor, actor_id, 1, 1, mesh_name, "relay-target"};
        const auto created =
          begin_reserved_object (pair.target->objects (), object, actor_type, {});
        ASSERT_EQ (created.error, st::stateful_error_t::none);
        ASSERT_EQ (pair.target->objects ().commit_create (created.attempt),
                   st::stateful_error_t::none);
        const wire::actor_route_fence_t fence{
          actor_id, 1, pair.target_wire.node_routing_id, pair.target_wire.lifecycle_generation,
          1,        1};
        using reply_t =
          std::pair<rt::foundation::operation_terminal_t, fw::result_t<zlink::message_t>>;
        std::promise<reply_t> completion;
        auto reply = completion.get_future ();
        ASSERT_TRUE (
          pair.source->transport ()
            .request_to_actor (pair.target_wire.node_routing_id, std::nullopt, fence,
                               {"Probe", "application/json", {'{', '}'}}, wait_budget,
                               [&] (auto terminal, fw::result_t<zlink::message_t> payload) {
                                   completion.set_value ({terminal, std::move (payload)});
                               })
            .result ()
            .value ());
        while (reply.wait_for (0ms) != std::future_status::ready
               && std::chrono::steady_clock::now () < deadline)
            pair.pump ();
        ASSERT_EQ (reply.wait_for (0ms), std::future_status::ready);
        const auto [terminal, payload] = reply.get ();
        EXPECT_EQ (terminal, rt::foundation::operation_terminal_t::transport_failed);
        ASSERT_TRUE (payload);
        const auto header = wire::decode_reply_header (payload.value ().to_bytes ());
        EXPECT_EQ (
          rt::messaging::request_failure_mapper_t{}
            .reply_header_exception (header.terminal_result, header.failure_code, "authority")
            .kind (),
          expected);
        EXPECT_EQ (queries, 1);
        EXPECT_EQ (pair.target->objects ().pending (object, st::turn_domain_t::application), 0u);
    }
}

class ReceiveClaimAuthorityFailure : public ::testing::TestWithParam<std::tuple<bool, int, bool>>
{
};

TEST_P (ReceiveClaimAuthorityFailure, UnexpectedFailureKeepsTerminalAndProcessesOtherRecords)
{
    namespace rt = fw::runtime;
    namespace st = rt::stateful;
    namespace wire = rt::protocol;
    using authority_t = std::optional<st::accepted_record_authority_t>;
    using reply_t = std::pair<rt::foundation::operation_terminal_t, fw::result_t<zlink::message_t>>;
    const auto [spot, failed_index, suspended] = GetParam ();
    constexpr int record_count = 4;
    fw::task_completion_source_t<authority_t> authority;
    std::promise<void> authority_entered;
    auto authority_started = authority_entered.get_future ();
    int queries = 0;
    relay_peer_pair_t pair (
      rt::mesh::service_object_role_t::server, rt::mesh::service_object_role_t::server,
      [&] (st::accepted_record_authority_query_t query) -> fw::task_t<authority_t> {
          const auto fail = spot
                              ? query.source_spot_id
                                  == std::optional{"claim-source-" + std::to_string (failed_index)}
                              : queries == failed_index;
          ++queries;
          if (fail) {
              authority_entered.set_value ();
              if (suspended)
                  return authority.task ();
              throw std::runtime_error ("authority read failed within receive claim");
          }
          return fw::task_t<authority_t> (
            fw::result_t<authority_t>::success (st::accepted_record_authority_t{
              {"source", 1, query.source_node_routing_id, query.source_node_generation}, 1}));
      });
    ASSERT_TRUE (pair.await_ready ());
    const st::object_ref_t object{spot ? st::object_kind_t::user_spot : st::object_kind_t::actor,
                                  actor_id,
                                  1,
                                  1,
                                  mesh_name,
                                  "relay-target"};
    const auto created = begin_reserved_object (pair.target->objects (), object, actor_type, {});
    ASSERT_EQ (created.error, st::stateful_error_t::none);
    ASSERT_EQ (pair.target->objects ().commit_create (created.attempt), st::stateful_error_t::none);
    auto completions = std::make_shared<std::array<std::promise<reply_t>, record_count>> ();
    std::array<std::future<reply_t>, record_count> replies;
    for (int index = 0; index < record_count; ++index) {
        replies[index] = (*completions)[index].get_future ();
        const auto complete = [completions, index] (auto terminal,
                                                    fw::result_t<zlink::message_t> payload) {
            (*completions)[index].set_value ({terminal, std::move (payload)});
        };
        const wire::wire_operation_id_t operation{1, static_cast<std::uint64_t> (index + 1)};
        rt::messaging::envelope_header_t envelope;
        envelope.kind = rt::messaging::message_kind_t::request;
        envelope.channel_name = mesh_name;
        envelope.message_name = probe_t::packet_name;
        envelope.correlation_id = std::to_string (index + 1);
        auto parts =
          fw::detail::encode_actor_packet (envelope, zlink::message_t::from (std::string ("{}")));
        const auto payload =
          wire::application_payload_t::from_parts (std::move (parts).take_items ());
        const auto sent =
          spot ? pair.source->transport ().request_to_spot (
                   pair.target_wire.node_routing_id, "claim-source-" + std::to_string (index),
                   {actor_id, 1, pair.target_wire.node_routing_id,
                    pair.target_wire.lifecycle_generation, 1, 1},
                   payload, wait_budget, complete, operation)
               : pair.source->transport ().request_to_actor (
                   pair.target_wire.node_routing_id, std::nullopt,
                   {actor_id, 1, pair.target_wire.node_routing_id,
                    pair.target_wire.lifecycle_generation, 1, 1},
                   payload, wait_budget, complete, operation);
        ASSERT_TRUE (sent.result ().value ());
    }
    auto &mailbox = pair.target->transport ().mailbox ();
    constexpr auto domain = rt::mesh::service_mailbox_domain_t::application;
    const auto deadline = std::chrono::steady_clock::now () + wait_budget;
    while (mailbox.pending_messages (domain) != record_count
           && std::chrono::steady_clock::now () < deadline) {
        pair.target->transport ().pump_one (std::chrono::steady_clock::now ()).result ().value ();
        std::this_thread::sleep_for (1ms);
    }
    ASSERT_EQ (mailbox.pending_messages (domain), record_count);
    const auto owner = rt::mesh::service_mailbox_t::application_owner (
      spot ? rt::host::owner_kind_t::spot : rt::host::owner_kind_t::actor, actor_id);
    auto claim = mailbox.try_claim_owner (domain, owner, record_count,
                                          rt::dispatch_limits::receive_batch_bytes);
    ASSERT_TRUE (claim);
    ASSERT_EQ (claim->claimed_messages, record_count);
    std::vector<std::uint64_t> delivered;
    auto pending = rt::host::public_host_runtime_test_access_t::dispatch_claim (
      *pair.target, claim, [&] (const auto &, const auto &record, auto, auto) {
          delivered.push_back (record.operation_id.low);
          ASSERT_TRUE (record.complete_stateful_dispatch);
          record.complete_stateful_dispatch ();
      });
    if (suspended) {
        ASSERT_EQ (authority_started.wait_for (wait_budget), std::future_status::ready);
        ASSERT_FALSE (pending.await_ready ());
        EXPECT_GE (queries, failed_index + 1);
        authority.complete (
          fw::detail::result_access_t::failure<authority_t> (std::make_exception_ptr (
            std::runtime_error ("authority read failed within receive claim"))));
    }
    const auto dispatch_result = pending.result ();
    ASSERT_TRUE (dispatch_result);
    if (spot)
        EXPECT_GE (queries, record_count);
    else
        EXPECT_EQ (queries, record_count);
    std::vector<std::uint64_t> expected;
    for (int index = 0; index < record_count; ++index)
        if (index != failed_index)
            expected.push_back (static_cast<std::uint64_t> (index + 1));
    EXPECT_EQ (delivered, expected);
    EXPECT_EQ (claim->outstanding (), 0u);
    EXPECT_EQ (mailbox.pending_messages (domain), 0u);
    EXPECT_EQ (pair.target->objects ().pending (object, st::turn_domain_t::application), 0u);
    while (replies[failed_index].wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now () < deadline)
        pair.source->dispatch_ready ([] (const auto &, const auto &, auto, auto) {})
          .result ()
          .value ();
    ASSERT_EQ (replies[failed_index].wait_for (0ms), std::future_status::ready);
    const auto [terminal, payload] = replies[failed_index].get ();
    EXPECT_EQ (terminal, rt::foundation::operation_terminal_t::transport_failed);
    ASSERT_TRUE (payload);
    const auto header = wire::decode_reply_header (payload.value ().to_bytes ());
    EXPECT_EQ (header.terminal_result,
               static_cast<std::uint32_t> (wire::request_terminal_result::internalError));
    EXPECT_EQ (header.failure_code,
               static_cast<std::uint32_t> (wire::framework_error_code::requestFailed));
    EXPECT_EQ (rt::messaging::request_failure_mapper_t{}
                 .reply_header_exception (header.terminal_result, header.failure_code, "authority")
                 .kind (),
               fw::framework_error_kind_t::internal_failure);
}

INSTANTIATE_TEST_SUITE_P (NativeIntake,
                          ReceiveClaimAuthorityFailure,
                          ::testing::Combine (::testing::Bool (),
                                              ::testing::Values (0, 1, 3),
                                              ::testing::Bool ()));

TEST (NativeIntake, RawAuthorityFailureAfterSealKeepsClaimSuffix)
{
    namespace rt = fw::runtime;
    namespace st = rt::stateful;
    for (const bool seal_first : {false, true}) {
        SCOPED_TRACE (seal_first);
        fw::task_completion_source_t<std::optional<st::accepted_record_authority_t>> authority;
        int queries = 0;
        native_intake_fixture_t fixture ({}, [&] (st::accepted_record_authority_query_t query) {
            if (++queries == 1)
                return authority.task ();
            return fw::task_t<std::optional<st::accepted_record_authority_t>> (
              fw::result_t<std::optional<st::accepted_record_authority_t>>::success (
                st::accepted_record_authority_t{
                  {"source", 1, query.source_node_routing_id, query.source_node_generation}, 1}));
        });
        auto host = fixture.host;
        auto actor = host->create_actor (actor_type, actor_id);
        const auto object = host->objects ().find (st::object_kind_t::actor, actor_id);
        ASSERT_TRUE (object);
        std::vector<rt::mesh::service_mailbox_record_t> records;
        records.push_back (fixture.actor_record (1));
        records.push_back (fixture.actor_record (2));
        auto claim =
          fw::test::publish_stateful_claims (host->transport (), *object, std::move (records));
        int dispatched = 0;
        auto pending = rt::host::public_host_runtime_test_access_t::dispatch_claim (
          *host, claim, [&] (const auto &, const auto &record, auto, auto) {
              ++dispatched;
              ASSERT_TRUE (record.complete_stateful_dispatch);
              record.complete_stateful_dispatch ();
          });
        ASSERT_EQ (queries, 1);
        ASSERT_FALSE (pending.await_ready ());
        std::optional<rt::mesh::service_mailbox_record_t> sealed;
        if (seal_first) {
            sealed = host->transport ().mailbox ().take ({claim, 0});
            EXPECT_TRUE (sealed);
        }
        authority.complete (fw::result_t<std::optional<st::accepted_record_authority_t>>::failure (
          fw::framework_error_kind_t::internal_failure, "authority read failed after seal"));
        const auto result = pending.result ();
        EXPECT_TRUE (result);
        EXPECT_EQ (queries, 2);
        EXPECT_EQ (dispatched, 1);
        EXPECT_EQ (claim->outstanding (), seal_first ? 1u : 0u);
        if (sealed)
            sealed->transferred ();
        EXPECT_EQ (claim->outstanding (), 0u);
        host->close ();
    }
}

TEST (NativeIntake, RawAuthorityOwnedOriginalSettlesFailure)
{
    namespace rt = fw::runtime;
    namespace st = rt::stateful;
    for (const auto kind : {fw::framework_error_kind_t::internal_failure,
                            fw::framework_error_kind_t::deadline_exceeded,
                            fw::framework_error_kind_t::shutting_down}) {
        SCOPED_TRACE (static_cast<int> (kind));
        native_intake_fixture_t fixture (
          {},
          [kind] (st::accepted_record_authority_query_t)
            -> fw::task_t<std::optional<st::accepted_record_authority_t>> {
              if (kind == fw::framework_error_kind_t::internal_failure)
                  throw std::runtime_error ("cut source authority failed");
              if (kind == fw::framework_error_kind_t::shutting_down)
                  std::rethrow_exception (
                    fw::detail::make_cancellation_exception ("cut source authority cancelled"));
              throw fw::framework_exception_t (kind, "cut source authority failed");
              co_return std::nullopt;
          });
        auto host = fixture.host;
        auto actor = host->create_actor (actor_type, actor_id);
        const auto object = host->objects ().find (st::object_kind_t::actor, actor_id);
        ASSERT_TRUE (object);
        auto slot =
          fw::test::publish_stateful_claim (host->transport (), *object, fixture.actor_record (1));
        auto original = host->transport ().mailbox ().take (slot);
        ASSERT_TRUE (original);
        const auto credit = original->transferred;
        const auto result = rt::host::public_host_runtime_test_access_t::ingest_cut (
                              *host, *object, std::move (*original))
                              .result ();
        EXPECT_TRUE (result);
        EXPECT_EQ (slot.claim->outstanding (), 0u);
        EXPECT_EQ (host->objects ().pending (*object, st::turn_domain_t::application), 0u);
        credit ();
        host->close ();
    }
}

TEST (NativeIntake, RawAuthorityConflictRemainsRejected)
{
    namespace st = fw::runtime::stateful;
    const st::accepted_record_authority_resolver_t resolver =
      [] (st::accepted_record_authority_query_t)
      -> fw::task_t<std::optional<st::accepted_record_authority_t>> {
        co_return st::accepted_record_authority_t{{"source", 1, {}, 1}, 2};
    };
    native_intake_fixture_t fixture ({}, resolver);
    auto actor = fixture.host->create_actor (actor_type, actor_id);
    const auto object = fixture.host->objects ().find (st::object_kind_t::actor, actor_id);
    ASSERT_TRUE (object);
    st::raw_stateful_dispatch_t raw (fixture.host->objects (), fixture.host->transport (),
                                     resolver);
    auto slot = fw::test::publish_stateful_claim (fixture.host->transport (), *object,
                                                  fixture.actor_record (1));
    EXPECT_EQ (raw.ingest_claimed (*object, slot, std::nullopt).result ().value (),
               st::stateful_error_t::conflict);
    EXPECT_EQ (fixture.host->objects ().pending (*object, st::turn_domain_t::application), 0u);
    EXPECT_EQ (slot.claim->outstanding (), 0u);
    fixture.host->close ();
}

TEST (NativeIntake, LocalSourceRejectionSettlesOnlyOwnedOriginal)
{
    namespace rt = fw::runtime;
    namespace st = rt::stateful;
    native_intake_fixture_t fixture ({}, [] (st::accepted_record_authority_query_t) {
        return fw::task_t<std::optional<st::accepted_record_authority_t>> (
          fw::result_t<std::optional<st::accepted_record_authority_t>>::success (std::nullopt));
    });
    auto host = fixture.host;
    auto actor = host->create_actor (actor_type, actor_id);
    auto wire = fixture.actor_record (1);
    auto parts = rt::protocol::decode_application_parts (std::move (wire.body_parts ().back ()));
    rt::host::pending_operation_t operation;
    ASSERT_EQ (
      actor.request_to (actor.ref (), parts, operation, zlink::send_flags_t::none, wait_budget)
        .result ()
        .value (),
      zlink::submit_result_t::ok);
    auto &mailbox = host->transport ().mailbox ();
    auto claim = mailbox.try_claim_owner (
      rt::mesh::service_mailbox_domain_t::application,
      rt::mesh::service_mailbox_t::application_owner (rt::host::owner_kind_t::actor, actor_id), 1,
      4096);
    ASSERT_TRUE (claim);
    const auto old = claim;
    int dispatched = 0;
    ASSERT_EQ (rt::host::public_host_runtime_test_access_t::dispatch_claim (
                 *host, claim, [&] (const auto &, const auto &, auto, auto) { ++dispatched; })
                 .result ()
                 .value (),
               0u);
    EXPECT_EQ (dispatched, 0);
    EXPECT_FALSE (host->transport ().operation_pending (operation.id));
    auto terminal = operation.completion->task ();
    auto observed = std::make_shared<std::promise<bool>> ();
    auto ready = observed->get_future ();
    fw::detail::observe_task_completion (terminal, [observed] (const auto &result) {
        observed->set_value (static_cast<bool> (result));
    });
    ASSERT_EQ (ready.wait_for (wait_budget), std::future_status::ready);
    EXPECT_FALSE (ready.get ());
    EXPECT_FALSE (mailbox.take ({old, 0}));
    EXPECT_FALSE (mailbox.release (*old));
    host->close ();
}
} // namespace zlink::framework::test::actor_maintenance_join

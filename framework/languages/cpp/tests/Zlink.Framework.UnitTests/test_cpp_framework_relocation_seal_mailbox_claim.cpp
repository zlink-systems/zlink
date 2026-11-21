/* SPDX-License-Identifier: Elastic-2.0 */
#include "mailbox_claim_fixture.hpp"
#include "../support/telemetry_observation.hpp"
#include "../support/stateful_object_sync.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/exception_payload.hpp"
#include "../support/manual_internal_scheduler.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "../support/stateful_ingress_fixture.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/timers/timer_runtime.hpp"
#include <gtest/gtest.h>
#include <service_wire_pilot_codec.hpp>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <string>
#include <utility>
#include <type_traits>
#include <variant>
#include <vector>
#include "../support/relocation_seal_execution_fixture.hpp"

using namespace framework_relocation_seal_fixture;

using namespace zlink::framework;

using namespace zlink::framework::detail;

namespace host = zlink::framework::runtime::host;

namespace mesh = zlink::framework::runtime::mesh;

namespace protocol = zlink::framework::runtime::protocol;

namespace stateful = zlink::framework::runtime::stateful;

// Location §7.3 and relocation §10 require native Follow without a fresh Store read.
// A raw ingress or claim waits for the object state lane without holding the raw dispatch
// mutex, so pending-entry work for another owner continues meanwhile.
// Relocation flow §4.2 producer cut at the mailbox: a claimed original not taken yet leaves with
// its claim credit and an unclaimed one leaves the FIFO; a late take finds nothing, a refused
// original leaves the owner FIFO untouched, and a record enqueued afterwards waits for a claim.
TEST (FrameworkRelocationSealContract, mailbox_cut_takes_each_untaken_original_once)
{
    const auto domain = mesh::service_mailbox_domain_t::application;
    mesh::service_mailbox_t mailbox;
    const auto record = [domain] (std::uint8_t id) {
        auto parts = detail::backend::materialize_binding_parts ({{id}});
        return mesh::service_mailbox_record_t::from_parts ("spot:cut", domain, std::move (parts));
    };
    const auto id_of = [] (const mesh::service_mailbox_record_t &value) {
        return value.parts ().front ().to_bytes ().front ();
    };
    for (const std::uint8_t id : {1, 2, 3})
        ASSERT_TRUE (mailbox.try_enqueue (record (id)));
    const auto claim = mailbox.try_claim_owner (domain, "spot:cut", 2, 4096);
    ASSERT_TRUE (claim);
    ASSERT_EQ (claim->claimed_messages, 2u);
    auto first = mailbox.take ({claim, 0});
    ASSERT_TRUE (first);
    bool cut = false;
    EXPECT_FALSE (mailbox.cut_owners (
      {"spot:cut"}, [&] (const auto &value) { return id_of (value) != 3; }, [&] { cut = true; }));
    EXPECT_FALSE (cut);
    EXPECT_EQ (mailbox.pending_messages (domain), 1u);
    auto taken = mailbox.cut_owners ({"spot:cut", "spot:other"}, {}, [&] { cut = true; });
    ASSERT_TRUE (taken);
    EXPECT_TRUE (cut);
    ASSERT_EQ (taken->size (), 2u);
    ASSERT_EQ ((*taken)[0].size (), 2u);
    EXPECT_TRUE ((*taken)[1].empty ());
    EXPECT_EQ (id_of ((*taken)[0][0]), 2);
    EXPECT_EQ (id_of ((*taken)[0][1]), 3);
    EXPECT_TRUE ((*taken)[0][0].transferred);
    EXPECT_FALSE ((*taken)[0][1].transferred);
    EXPECT_FALSE (mailbox.take ({claim, 1}));
    EXPECT_EQ (mailbox.pending_messages (domain), 0u);
    ASSERT_TRUE (mailbox.try_enqueue (record (4)));
    EXPECT_FALSE (mailbox.try_claim_owner (domain, "spot:cut", 8, 4096));
    EXPECT_TRUE (first->transferred ());
    EXPECT_TRUE ((*taken)[0][0].transferred ());
    EXPECT_FALSE ((*taken)[0][0].transferred ());
    EXPECT_EQ (claim->outstanding (), 0u);
    const auto next = mailbox.try_claim_owner (domain, "spot:cut", 8, 4096);
    ASSERT_TRUE (next);
    ASSERT_EQ (next->claimed_messages, 1u);
    EXPECT_EQ (id_of (*next->records.front ()), 4);
}

TEST (FrameworkRelocationSealContract, ordinary_claim_transfers_batch_before_cut)
{
    const auto domain = mesh::service_mailbox_domain_t::application;
    mesh::service_mailbox_t mailbox;
    std::vector<mesh::service_mailbox_record_t> batch;
    batch.reserve (64);
    for (std::uint8_t index = 0; index < 64; ++index) {
        auto original = mesh::service_mailbox_record_t::from_parts (
          "ordinary", domain, detail::backend::materialize_binding_parts ({{index}}));
        ASSERT_TRUE (mailbox.try_enqueue (std::move (original)));
    }
    const auto claim = mailbox.try_claim (domain, 64, 65536, [&] (const auto &claimed) noexcept {
        for (const auto &original : claimed->records)
            batch.push_back (std::move (*original));
    });
    ASSERT_TRUE (claim);
    ASSERT_EQ (batch.size (), 64u);
    const auto cut = mailbox.cut_owners ({"ordinary"}, {}, {});
    ASSERT_TRUE (cut);
    EXPECT_TRUE (cut->front ().empty ());
    for (std::size_t index = 0; index < batch.size (); ++index) {
        EXPECT_EQ (batch[index].parts ().front ().to_bytes ().front (), index);
        EXPECT_FALSE (mailbox.take ({claim, index}));
        EXPECT_TRUE (batch[index].transferred ());
        EXPECT_FALSE (batch[index].transferred ());
    }
    EXPECT_EQ (claim->outstanding (), 0u);
    EXPECT_FALSE (mailbox.release (*claim));
}

TEST (FrameworkRelocationSealContract, throwing_claim_consumer_returns_all_credits)
{
    const auto domain = mesh::service_mailbox_domain_t::application;
    for (const bool by_owner : {false, true}) {
        mesh::service_mailbox_t mailbox;
        const auto enqueue = [&] {
            auto original = mesh::service_mailbox_record_t::from_parts (
              "ordinary", domain, detail::backend::materialize_binding_parts ({{1}}));
            ASSERT_TRUE (mailbox.try_enqueue (std::move (original)));
        };
        enqueue ();
        enqueue ();
        std::optional<mesh::service_mailbox_record_t> transferred;
        const mesh::service_mailbox_t::claimed_t fail = [&] (const auto &claim) {
            transferred.emplace (std::move (*claim->records.front ()));
            throw std::bad_alloc ();
        };
        if (by_owner)
            EXPECT_THROW (mailbox.try_claim_owner (domain, "ordinary", 2, 4096, fail),
                          std::bad_alloc);
        else
            EXPECT_THROW (mailbox.try_claim (domain, 2, 4096, fail), std::bad_alloc);
        ASSERT_TRUE (transferred);
        EXPECT_FALSE (transferred->transferred ());
        EXPECT_EQ (mailbox.pending_messages (domain), 0u);
        enqueue ();
        const auto next = mailbox.try_claim (domain, 1, 4096);
        ASSERT_TRUE (next);
        EXPECT_TRUE (mailbox.release (*next));
    }
}

TEST (FrameworkRelocationSealContract, ordinary_dispatch_failure_settles_each_record)
{
    auto failures = std::make_shared<runtime::runtime_failure_collector_t> ();
    const auto marker = std::make_shared<const int> (369303);
    using failure_t = zlink::framework::test::marked_exception_t<std::runtime_error>;
    const auto injected = std::make_exception_ptr (failure_t (marker, "dispatch failed"));
    unsigned reported = 0;
    failures->bind_runtime_reporter ([&] (auto error) {
        EXPECT_TRUE (zlink::framework::test::exception_payload_matches<failure_t> (
          error, [&] (const auto &cause) {
              return cause.marker == marker && std::string (cause.what ()) == "dispatch failed";
          }));
        ++reported;
    });
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("ordinary-batch"), .runtime_failures = failures}});
    auto &mailbox = host->transport ().mailbox ();
    const auto enqueue = [&] {
        mesh::service_mailbox_record_t original;
        original.owner = "node";
        original.domain = mesh::service_mailbox_domain_t::application;
        original.application.emplace ();
        original.application->owner.owner_kind = host::owner_kind_t::node;
        original.application->record.kind = host::record_kind_t::channel_send;
        original.application->parts = detail::backend::materialize_binding_parts ({{1}});
        ASSERT_TRUE (mailbox.try_enqueue (std::move (original)));
    };
    for (int index = 0; index < 64; ++index)
        enqueue ();
    unsigned started = 0, rejected = 0, calls = 0;
    for (unsigned index = 0; index != 64; ++index) {
        auto failed = host->dispatch_application_owner (
          "node",
          [&] (const auto &, const auto &, auto, auto) {
              ++calls;
              std::rethrow_exception (injected);
          },
          [&] { ++started; }, [&] { ++rejected; });
        EXPECT_TRUE (zlink::framework::test::application_owner_result (failed).value ());
        EXPECT_EQ (calls, index + 1);
        EXPECT_EQ (mailbox.pending_messages (mesh::service_mailbox_domain_t::application),
                   63 - index);
    }
    EXPECT_EQ (calls, 64u);
    EXPECT_EQ (reported, 64u);
    EXPECT_EQ (started, 64u);
    EXPECT_EQ (rejected, 64u);
    EXPECT_EQ (mailbox.pending_messages (mesh::service_mailbox_domain_t::application), 0u);
    enqueue ();
    unsigned dispatched = 0;
    auto next = host->dispatch_application_owner (
      "node", [&] (const auto &, const auto &, auto, auto) { ++dispatched; }, [] {}, [] {});
    EXPECT_TRUE (zlink::framework::test::application_owner_result (next).value ());
    EXPECT_EQ (dispatched, 1u);
    host->close ();
}

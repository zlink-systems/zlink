/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "allocation_counter.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime_failure_fixture.hpp"

#include <atomic>
#include <iostream>
#include <stdexcept>

namespace
{
using namespace zlink::framework;
namespace host = runtime::host;
namespace mesh = runtime::mesh;
namespace protocol = runtime::protocol;
constexpr std::uint64_t request_correlation = 17;
const std::vector<std::uint8_t> request_source = {'p', 'e', 'e', 'r'};
constexpr auto domain = mesh::service_mailbox_domain_t::application;
const auto owner = mesh::service_mailbox_t::application_owner (host::owner_kind_t::node);
using dispatch_t = std::function<void (
  const host::ready_record_t &, const host::receive_record_t &, std::vector<zlink::message_t>)>;
const std::function<void ()> noop = [] {};

void require (bool value, const char *message)
{
    if (!value)
        throw std::runtime_error (message);
}

std::shared_ptr<host::public_host_runtime_t> make_host ()
{
    auto options = test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{.descriptor = {"claim-cost",
                                                   {'c', 'l', 'a', 'i', 'm'},
                                                   1,
                                                   1,
                                                   "tcp://127.0.0.1:0",
                                                   {},
                                                   mesh::service_node_state_t::preparing}});
    auto result =
      std::make_shared<host::public_host_runtime_t> (host::host_options_t{std::move (options)});
    result->start ();
    return result;
}

void enqueue (mesh::service_mailbox_t &mailbox, bool raw_request = false)
{
    mesh::service_mailbox_record_t record;
    record.owner = owner;
    record.domain = domain;
    if (raw_request) {
        record.parts.push_back (
          zlink::message_t::from (protocol::encode_node_request_header (request_correlation)));
        protocol::application_payload_t::multipart_t parts;
        parts.push_back (zlink::message_t::from (std::vector<std::uint8_t>{'o', 'k'}));
        record.parts.push_back (zlink::message_t::from (protocol::encode_application_payload (
          protocol::application_payload_t::from_parts (std::move (parts)))));
        record.source_routing_id = request_source;
        record.correlation = request_correlation;
    } else {
        record.application = std::make_shared<host::local_application_dispatch_t> ();
        record.application->owner.owner_kind = host::owner_kind_t::node;
        record.application->owner.domain = host::ready_domain_t::application;
        record.application->record.domain = host::ready_domain_t::application;
    }
    require (mailbox.try_enqueue (std::move (record)), "fixture enqueue failed");
}

bool dispatch (host::public_host_runtime_t &target, const dispatch_t &callback)
{
    return target.dispatch_application_owner (owner, callback, noop, noop).result ().value ();
}

// Measure the standard-library representation of exactly one owner, record-state
// array and two shared-owner callbacks. This calibrates both count and byte
// bounds for the current STL, without fixing its control-block size or SBO size.
struct record_state_t
{
    std::atomic_bool retained{false};
    std::atomic_bool released{false};
};
struct owner_layout_t
{
    mesh::service_mailbox_claim_t claim;
    std::weak_ptr<host::public_host_runtime_t> host;
    std::atomic_size_t remaining{1};
    std::vector<record_state_t> records{1};
};

auto owner_baseline ()
{
    return tests::measure_allocations ([] {
        const auto shared = std::make_shared<owner_layout_t> ();
        const std::size_t index = 0;
        const std::function<void ()> retain = [shared, index] {
            shared->records[index].retained.store (true, std::memory_order_release);
        };
        const std::function<void ()> release = [shared, index] {
            shared->records[index].released.store (true, std::memory_order_release);
        };
        retain ();
        release ();
    });
}

void verify_cost (host::public_host_runtime_t &target)
{
    mesh::service_mailbox_t baseline_mailbox;
    enqueue (baseline_mailbox);
    const auto claim_cost = tests::measure_allocations ([&] {
        auto claim = baseline_mailbox.try_claim_owner (
          domain, owner, 1, runtime::dispatch_limits::receive_batch_bytes);
        require (claim.has_value (), "baseline claim failed");
        require (baseline_mailbox.release (*claim), "baseline release failed");
    });
    const dispatch_t callback = [] (const auto &, const auto &, auto) {};
    enqueue (target.transport ().mailbox ());
    const auto actual = tests::measure_allocations (
      [&] { require (dispatch (target, callback), "measured dispatch failed"); });
    const auto expected = owner_baseline ();
    std::cout << "claim-owner actual-count=" << actual.count << " actual-bytes=" << actual.bytes
              << " mailbox-count=" << claim_cost.count << " mailbox-bytes=" << claim_cost.bytes
              << " owner-count=" << expected.count << " owner-bytes=" << expected.bytes << '\n';
    require (actual.count <= claim_cost.count + expected.count,
             "claim dispatch allocates more than one shared owner and two callbacks");
    require (actual.bytes <= claim_cost.bytes + expected.bytes,
             "claim dispatch exceeds the calibrated owner storage bound");
}

void verify_retained_batch (host::public_host_runtime_t &target)
{
    auto &mailbox = target.transport ().mailbox ();
    enqueue (mailbox);
    enqueue (mailbox);
    std::vector<std::function<void ()>> releases;
    const dispatch_t callback = [&] (const auto &, const auto &record, auto) {
        record.retain_mailbox_reservation ();
        releases.push_back (record.release_mailbox_reservation);
    };
    require (dispatch (target, callback) && releases.size () == 2, "batch dispatch changed");
    enqueue (mailbox);
    const dispatch_t next = [] (const auto &, const auto &, auto) {};
    require (!dispatch (target, next), "retained claim released early");
    releases[0]();
    releases[0]();
    require (!dispatch (target, next), "duplicate release ended another child");
    releases[1]();
    require (dispatch (target, next), "last child did not release claim");
    require (mailbox.pending_messages (domain) == 0, "next record was lost");
}

void verify_exception (host::public_host_runtime_t &target)
{
    auto &mailbox = target.transport ().mailbox ();
    enqueue (mailbox);
    enqueue (mailbox);
    bool caught = false;
    try {
        dispatch (target, [] (const auto &, const auto &, auto) {
            throw std::runtime_error ("handler fixture");
        });
    }
    catch (const std::runtime_error &error) {
        caught = std::string_view (error.what ()) == "handler fixture";
    }
    require (caught && mailbox.pending_messages (domain) == 0,
             "exception did not return current and unstarted record reservations");
    enqueue (mailbox);
    require (dispatch (target, [] (const auto &, const auto &, auto) {}),
             "exception left the owner claim active");
}

void verify_prestart_terminal (host::public_host_runtime_t &target)
{
    auto &mailbox = target.transport ().mailbox ();
    mesh::service_mailbox_record_t record;
    record.owner = owner;
    record.domain = domain;
    record.parts.push_back (zlink::message_t::from (std::vector<std::uint8_t>{0}));
    require (mailbox.try_enqueue (std::move (record)), "malformed fixture enqueue failed");
    std::size_t started = 0;
    std::size_t rejected = 0;
    const dispatch_t callback = [] (const auto &, const auto &, auto) {
        throw std::runtime_error ("malformed request reached handler");
    };
    require (
      target.dispatch_application_owner (
              owner, callback, [&] { ++started; }, [&] { ++rejected; })
        .result ()
        .value (),
      "malformed request was not claimed");
    require (started == 1 && rejected == 1 && mailbox.pending_messages (domain) == 0,
             "decode rejection did not terminalize the unstarted record");
    enqueue (mailbox);
    require (dispatch (target, [] (const auto &, const auto &, auto) {}),
             "pre-start rejection left the owner claim active");
}

void verify_reply_alias ()
{
    auto target = make_host ();
    auto &mailbox = target->transport ().mailbox ();
    enqueue (mailbox, true);
    host::reply_token_t reply;
    const dispatch_t callback = [&] (const auto &, const auto &record, auto parts) {
        require (record.kind == host::record_kind_t::node_request && parts.size () == 1,
                 "raw request dispatch changed");
        reply = record.reply_token;
    };
    require (dispatch (*target, callback), "raw request was not dispatched");
    require (mailbox.pending_messages (domain) == 0, "reply alias retained mailbox reservation");
    require (reply.request && reply.request->correlation == request_correlation
               && reply.request->source_routing_id == request_source,
             "reply alias lost routing storage after dispatch");
    enqueue (mailbox);
    require (dispatch (*target, [] (const auto &, const auto &, auto) {}),
             "reply alias retained the owner claim");
    target->close ();
    target.reset ();
    require (reply.host.expired () && reply.request->correlation == request_correlation
               && reply.request->source_routing_id == request_source,
             "shutdown invalidated retained reply routing storage");
}

void verify_late_release ()
{
    auto target = make_host ();
    enqueue (target->transport ().mailbox ());
    std::function<void ()> release;
    dispatch (*target, [&] (const auto &, const auto &record, auto) {
        record.retain_mailbox_reservation ();
        release = record.release_mailbox_reservation;
    });
    target->close ();
    target.reset ();
    release ();
    release ();
}
}

int main ()
{
    try {
        auto target = make_host ();
        verify_retained_batch (*target);
        verify_exception (*target);
        verify_prestart_terminal (*target);
        target->close ();
        verify_reply_alias ();
        verify_late_release ();
        target = make_host ();
        verify_cost (*target);
        target->close ();
        std::cout << "claim owner cost and lifetime passed\n";
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}

/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "allocation_counter.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime_failure_fixture.hpp"

#include <atomic>
#include <array>
#include <cstdint>
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

struct allocation_events_t
{
    std::array<tests::allocation_event_t, 64> events{};
    tests::allocation_event_log_t log{events.data (), events.size ()};

    template <typename TAction> auto measure (TAction &&action)
    {
        struct restore_log_t
        {
            tests::allocation_event_log_t *previous;
            ~restore_log_t () { tests::allocation_event_log = previous; }
        } restore{std::exchange (tests::allocation_event_log, &log)};
        const auto result = tests::measure_allocations (std::forward<TAction> (action));
        require (!log.overflow, "allocation attribution event buffer overflowed");
        require (log.count == result.count, "allocation attribution lost an event");
        return result;
    }

    std::size_t containing (const void *ready, const void *record, std::size_t begin) const
    {
        const auto contains = [] (const auto &event, const void *address) {
            const auto base = reinterpret_cast<std::uintptr_t> (event.memory);
            const auto value = reinterpret_cast<std::uintptr_t> (address);
            return value >= base && value - base < event.size;
        };
        auto found = log.count;
        for (auto index = begin; index < log.count; ++index) {
            if (contains (events[index], ready) || contains (events[index], record)) {
                require (found == log.count, "dispatch locals identify multiple allocations");
                found = index;
            }
        }
        require (found != log.count,
                 "compiler did not place a dispatch local in an allocated coroutine frame");
        return found;
    }
};

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
    struct observation_t
    {
        allocation_events_t allocations;
        std::size_t started_at = 0;
        std::size_t inner_frame = 0;
        std::size_t started_count = 0;
        std::size_t dispatched_count = 0;
        bool inject_allocation = false;
        std::shared_ptr<int> injected;
    } observation;
    const std::function<void ()> started = [&observation] {
        ++observation.started_count;
        observation.started_at = observation.allocations.log.count;
    };
    const dispatch_t callback = [&observation] (const auto &ready, const auto &record, auto) {
        ++observation.dispatched_count;
        observation.inner_frame =
          observation.allocations.containing (&ready, &record, observation.started_at);
        if (observation.inject_allocation)
            observation.injected = std::make_shared<int> (1);
    };
    // An empty owner takes the same outer coroutine path without creating a claim
    // owner. Keep callbacks and argument copies identical to the measured call.
    const auto outer = tests::measure_allocations ([&] {
        require (
          !target.dispatch_application_owner (owner, callback, started, noop).result ().value (),
          "outer coroutine baseline unexpectedly claimed a record");
    });
    allocation_events_t completion_events;
    const auto completion = completion_events.measure ([] {
        const task_t<std::size_t> completed (result_t<std::size_t>::success (0));
        require (completed.result ().value () == 0, "task completion calibration failed");
    });
    require (completion_events.log.count != 0,
             "task completion calibration did not allocate storage");
    const auto expected = owner_baseline ();
    const auto measure_dispatch = [&] (bool inject_allocation) {
        auto &events = observation.allocations;
        events.log.count = 0;
        observation.started_count = 0;
        observation.dispatched_count = 0;
        observation.inject_allocation = inject_allocation;
        enqueue (target.transport ().mailbox ());
        const auto actual = events.measure ([&] {
            require (
              target.dispatch_application_owner (owner, callback, started, noop).result ().value (),
              "measured dispatch failed");
        });
        const auto inner_frame = observation.inner_frame;
        require (observation.started_count == 1 && observation.dispatched_count == 1,
                 "allocation attribution requires exactly one dispatched record");
        require (inner_frame == observation.started_at,
                 "unexpected allocation before the inner coroutine frame");
        require (inner_frame + 1 + completion_events.log.count <= events.log.count,
                 "inner coroutine completion allocation is missing");
        // The moved claim and std::function parameters do not allocate. Promise
        // construction follows the frame allocation and precedes the claim owner.
        // Frame size comes from the live ready/record addresses, not a compiler ABI.
        // A compiler placing both locals elsewhere fails attribution explicitly.
        for (std::size_t index = 0; index < completion_events.log.count; ++index)
            require (events.events[inner_frame + 1 + index].size
                       == completion_events.events[index].size,
                     "inner coroutine promise differs from task completion calibration");
        const auto overhead_count = outer.count + 1 + completion.count;
        const auto overhead_bytes =
          outer.bytes + events.events[inner_frame].size + completion.bytes;
        require (actual.count >= overhead_count && actual.bytes >= overhead_bytes,
                 "coroutine attribution exceeds the measured dispatch allocations");
        std::cout << "claim-owner injected=" << inject_allocation
                  << " actual-count=" << actual.count << " actual-bytes=" << actual.bytes
                  << " mailbox-count=" << claim_cost.count << " mailbox-bytes=" << claim_cost.bytes
                  << " owner-count=" << expected.count << " owner-bytes=" << expected.bytes
                  << " coroutine-count=" << overhead_count << " coroutine-bytes=" << overhead_bytes
                  << '\n';
        std::cout << "claim-owner outer-count=" << outer.count << " outer-bytes=" << outer.bytes
                  << " inner-frame-event=" << inner_frame
                  << " inner-frame-bytes=" << events.events[inner_frame].size
                  << " completion-count=" << completion.count
                  << " completion-bytes=" << completion.bytes << '\n';
        for (std::size_t index = 0; index < events.log.count; ++index)
            std::cout << "claim-owner allocation-event=" << index
                      << " bytes=" << events.events[index].size << '\n';
        return tests::allocation_measurement_t{actual.count - overhead_count,
                                               actual.bytes - overhead_bytes, 0};
    };
    const auto actual = measure_dispatch (false);
    require (actual.count <= claim_cost.count + expected.count,
             "claim dispatch allocates more than one shared owner and two callbacks");
    require (actual.bytes <= claim_cost.bytes + expected.bytes,
             "claim dispatch exceeds the calibrated owner storage bound");
    const auto injected = measure_dispatch (true);
    require (injected.count > claim_cost.count + expected.count
               && injected.bytes > claim_cost.bytes + expected.bytes,
             "allocation attribution hid an injected shared allocation");
    std::cout << "claim-owner injected allocation rejected by count and byte bounds\n";
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

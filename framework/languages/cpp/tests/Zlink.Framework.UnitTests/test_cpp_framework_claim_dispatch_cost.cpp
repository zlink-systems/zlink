/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "allocation_counter.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime_failure_fixture.hpp"
#include <service_wire_codec.hpp>
#include <future>

#include <atomic>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace zlink::framework::runtime::stateful
{
struct stateful_object_runtime_test_access_t
{
    static state_lane_t &lane (stateful_object_runtime_t &objects) { return objects._lane; }
};
}
namespace zlink::framework::runtime::host
{
struct public_host_runtime_test_access_t
{
    static task_t<std::size_t> dispatch_claim (public_host_runtime_t &host,
                                               std::shared_ptr<mesh::service_mailbox_claim_t> claim)
    {
        return host.dispatch_application_claim (
          std::move (claim), [] (const auto &, const auto &, auto) {
              throw std::runtime_error ("multicast source reached application callback");
          });
    }
    static task_t<void> forward_multicast (public_host_runtime_t &host,
                                           task_t<std::vector<stateful::object_ref_t>> targets,
                                           std::function<void ()> before)
    {
        return host.enqueue_local_multicast (std::move (targets), "c", "topic", {}, {},
                                             host.status ().routing_id (), std::move (before));
    }
};
}

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
std::string claim_owner (bool channel)
{
    return channel ? mesh::service_mailbox_t::application_owner (host::owner_kind_t::channel, "c")
                   : owner;
}

void require (bool value, const char *message)
{
    if (!value)
        throw std::runtime_error (message);
}

std::shared_ptr<host::public_host_runtime_t>
make_host (std::shared_ptr<runtime::application_job_queue_t> jobs = {},
           std::shared_ptr<runtime::runtime_failure_collector_t> failures = {})
{
    auto options = test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{.descriptor = {"claim-cost",
                                                   {'c', 'l', 'a', 'i', 'm'},
                                                   1,
                                                   1,
                                                   "tcp://127.0.0.1:0",
                                                   {},
                                                   mesh::service_node_state_t::preparing}});
    if (failures)
        options.runtime_failures = std::move (failures);
    if (jobs) {
        options.application_jobs = std::move (jobs);
        options.descriptor.channels = {{"c", 0}};
    }
    host::host_options_t settings{std::move (options)};
    settings.object_stable_types = {"framework.spot"};
    auto result = std::make_shared<host::public_host_runtime_t> (std::move (settings));
    result->start ();
    return result;
}

void enqueue (mesh::service_mailbox_t &mailbox,
              bool raw_request = false,
              bool channel = false,
              std::function<void ()> before = {})
{
    mesh::service_mailbox_record_t record;
    record.owner = claim_owner (channel);
    record.domain = domain;
    record.before_application_handler = std::move (before);
    if (raw_request) {
        record.parts.push_back (zlink::message_t::from (
          channel ? protocol::encode_channel_request_header (request_correlation, "c")
                  : protocol::encode_node_request_header (request_correlation)));
        protocol::application_payload_t::multipart_t parts;
        parts.push_back (zlink::message_t::from (std::vector<std::uint8_t>{'o', 'k'}));
        record.parts.push_back (zlink::message_t::from (protocol::encode_application_payload (
          protocol::application_payload_t::from_parts (std::move (parts)))));
        record.source_routing_id = request_source;
        record.correlation = request_correlation;
    } else {
        record.application.emplace ();
        record.application->owner.owner_kind =
          channel ? host::owner_kind_t::channel : host::owner_kind_t::node;
        record.application->record.kind =
          channel ? host::record_kind_t::channel_send : host::record_kind_t::node_send;
        record.application->owner.domain = host::ready_domain_t::application;
        record.application->record.domain = host::ready_domain_t::application;
    }
    require (mailbox.try_enqueue (std::move (record)), "fixture enqueue failed");
}

bool dispatch (host::public_host_runtime_t &target,
               const dispatch_t &callback,
               bool channel = false)
{
    return zlink::framework::test::application_owner_result (
             target.dispatch_application_owner (claim_owner (channel), callback, noop, noop))
      .value ();
}

bool verify_cost (host::public_host_runtime_t &target, bool channel, bool owning_callback = false)
{
    mesh::service_mailbox_t mailbox;
    enqueue (mailbox, false, channel);
    // The original moves once, inside the claim, exactly as the dispatcher takes it;
    // a record move allocates on some standard libraries (MSVC std::map).
    const auto claim_cost = tests::measure_allocations ([&] {
        std::optional<mesh::service_mailbox_record_t> original;
        auto claim = mailbox.try_claim_owner (domain, claim_owner (channel), 1,
                                              runtime::dispatch_limits::receive_batch_bytes);
        original = mailbox.take ({claim, 0});
        require (static_cast<bool> (claim), "baseline claim failed");
        require (original.has_value (), "baseline original transfer failed");
        require (original->transferred (), "baseline release failed");
    });

    // Measure the steady path after an idle worker has acquired its reusable buffer.
    enqueue (target.transport ().mailbox (), false, channel);
    require (dispatch (
               target, [] (const auto &, const auto &, auto) {}, channel),
             "ordinary claim warmup failed");
    struct observation_t
    {
        std::size_t calls = 0, starts = 0, rejects = 0;
        bool inject = false;
        bool channel;
        std::shared_ptr<int> injected;
        std::thread::id caller = std::this_thread::get_id ();
    } state{.channel = channel};
    const dispatch_t invoke = [&state] (const auto &, const auto &record, auto) {
        require (std::this_thread::get_id () == state.caller,
                 "ordinary claim left the allocation measurement thread");
        ++state.calls;
        require (
          record.kind
            == (state.channel ? host::record_kind_t::channel_send : host::record_kind_t::node_send),
          "record classification changed");
        if (state.inject)
            state.injected = std::make_shared<int> (1);
    };
    // A real drain callback may own heap storage. Permit exactly the API's
    // input copy; moving that argument into the claim must not clone it again.
    const dispatch_t callback =
      owning_callback ? dispatch_t ([invoke, padding = std::array<std::uint64_t, 8>{}] (
                                      const auto &ready, const auto &record, auto parts) {
          require (padding.front () == 0, "callback capture changed");
          invoke (ready, record, std::move (parts));
      })
                      : invoke;
    const auto input_copy = tests::measure_allocations ([&] {
        const auto copy = callback;
        require (static_cast<bool> (copy), "callback copy calibration failed");
    });
    const auto limit_count = claim_cost.count + input_copy.count;
    const auto limit_bytes = claim_cost.bytes + input_copy.bytes;
    const std::function<void ()> started = [&state] { ++state.starts; };
    const std::function<void ()> rejected = [&state] { ++state.rejects; };
    const auto measure = [&] {
        enqueue (target.transport ().mailbox (), false, channel);
        return tests::measure_allocations ([&] {
            auto pending = target.dispatch_application_owner (claim_owner (channel), callback,
                                                              started, rejected);
            require (zlink::framework::test::application_owner_ready (pending),
                     "ordinary claim did not complete synchronously");
            require (zlink::framework::test::application_owner_result (pending).value (),
                     "dispatch failed");
        });
    };
    const auto actual = measure ();
    std::cout << "ordinary-claim channel=" << channel << " owning=" << owning_callback
              << " count=" << actual.count << " bytes=" << actual.bytes
              << " limit-count=" << limit_count << " limit-bytes=" << limit_bytes << std::endl;
    require (state.calls == 1 && state.starts == 1 && state.rejects == 0,
             "ordinary claim accounting changed");
    require (target.transport ().mailbox ().pending_messages (domain) == 0,
             "claim was not returned");
    state.inject = true;
    const auto bad = measure ();
    std::cout << "injected-claim channel=" << channel << " owning=" << owning_callback
              << " count=" << bad.count << " bytes=" << bad.bytes << std::endl;
    require (bad.count > limit_count && bad.bytes > limit_bytes,
             "allocation guard accepted injected ownership cost");
    std::cout << "allocation-guard channel=" << channel << " owning=" << owning_callback
              << " injected-rejected=true" << std::endl;
    return actual.count <= limit_count && actual.bytes <= limit_bytes;
}
void verify_batch_record_cost (host::public_host_runtime_t &target)
{
    const auto measure = [&] (std::size_t size, bool dispatch_records) {
        mesh::service_mailbox_t baseline;
        auto &mailbox = dispatch_records ? target.transport ().mailbox () : baseline;
        for (std::size_t index = 0; index < size; ++index)
            enqueue (mailbox);
        const dispatch_t callback = [] (const auto &, const auto &, auto) {};
        return tests::measure_allocations ([&] {
            if (dispatch_records)
                require (dispatch (target, callback), "batch cost dispatch failed");
            else {
                auto claim = mailbox.try_claim_owner (
                  domain, owner, size, runtime::dispatch_limits::receive_batch_bytes);
                require (static_cast<bool> (claim), "batch cost claim failed");
                const auto count = claim->claimed_messages;
                for (std::size_t index = 0; index < count; ++index) {
                    auto original = mailbox.take ({claim, index});
                    require (original.has_value (), "batch baseline take failed");
                    original->transferred ();
                }
            }
        });
    };
    const auto one = measure (1, true);
    const auto batch = measure (32, true);
    const auto baseline_one = measure (1, false);
    const auto baseline_batch = measure (32, false);
    const auto extra = static_cast<long long> (batch.count) - one.count
                       - static_cast<long long> (baseline_batch.count) + baseline_one.count;
    std::cout << "batch-record additional-allocations=" << extra
              << " records=31 allocations-per-record=" << extra / 31.0 << std::endl;
    require (extra <= 0, "dispatch allocated callback storage per claimed record");
}

void verify_retained_batch (host::public_host_runtime_t &target)
{
    auto &mailbox = target.transport ().mailbox ();
    enqueue (mailbox);
    enqueue (mailbox);
    std::vector<host::mailbox_callback_t> releases;
    const dispatch_t callback = [&] (const auto &, const auto &record, auto) {
        record.retain_mailbox_reservation ();
        releases.push_back (record.release_mailbox_reservation);
    };
    require (dispatch (target, callback) && releases.size () == 2, "batch dispatch changed");
    enqueue (mailbox, false, true);
    const dispatch_t independent = [] (const auto &, const auto &, auto) {};
    require (dispatch (target, independent, true),
             "independent owner could not drain while first batch retained");
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

void verify_original_transfer (host::public_host_runtime_t &target)
{
    auto &mailbox = target.transport ().mailbox ();
    enqueue (mailbox);
    enqueue (mailbox);
    auto claim =
      mailbox.try_claim_owner (domain, owner, 2, runtime::dispatch_limits::receive_batch_bytes);
    require (claim && claim->claimed_messages == 2, "transfer fixture claim failed");
    auto first = mailbox.take ({claim, 0});
    require (first && !claim->records[0]->application && !mailbox.take ({claim, 0}),
             "local original remained present after transfer");
    auto cut = mailbox.cut_owners ({owner}, [] (const auto &) { return true; }, [] {});
    require (cut && cut->size () == 1 && cut->front ().size () == 1,
             "relocation cut included an already transferred original");
    require (!mailbox.take ({claim, 1}), "relocation cut left its original in the slot");
    first->transferred ();
    cut->front ().front ().transferred ();
    require (mailbox.pending_messages (domain) == 0, "transferred originals lost claim credit");
}

void verify_nested_batch (host::public_host_runtime_t &target)
{
    auto &mailbox = target.transport ().mailbox ();
    auto lifetime = std::make_shared<int> (1);
    std::weak_ptr<int> alive = lifetime;
    enqueue (mailbox, true, true, [lifetime] {});
    lifetime.reset ();
    enqueue (mailbox, true);
    host::reply_token_t outer_reply, nested_reply;
    const dispatch_t outer = [&] (const auto &, const auto &record, auto) {
        outer_reply = record.reply_token;
        const dispatch_t inner = [&] (const auto &, const auto &nested, auto) {
            nested_reply = nested.reply_token;
        };
        require (dispatch (target, inner, true), "nested independent owner failed to drain");
    };
    require (dispatch (target, outer), "outer owner failed to drain");
    require (!alive.expired () && nested_reply.request && outer_reply.request,
             "outer drain recycled a nested batch with a retained reply original");
    require (nested_reply.request->correlation == request_correlation,
             "nested reply original changed after outer drain returned");
}

void measure_permit_ownership_cost ()
{
    constexpr std::size_t count = 32;
    const auto measure = [] (bool admitted) {
        runtime::application_job_queue_t jobs (runtime::application_job_queue_configuration_t{
          application_job_queue_profile_t::balanced, std::uint32_t{count}, 1, count});
        mesh::service_mailbox_t mailbox;
        return tests::measure_allocations ([&] {
            for (std::size_t i = 0; i < count; ++i) {
                mesh::service_mailbox_record_t record;
                record.owner = owner;
                record.domain = domain;
                record.application.emplace ();
                if (admitted) {
                    record.application_permit = jobs.try_reserve_supply ();
                    require (record.application_permit.has_value (), "cost permit unavailable");
                    record.application_permit->mark_queued ();
                }
                require (mailbox.try_enqueue (std::move (record)), "cost publication failed");
            }
            auto claim = mailbox.try_claim_owner (domain, owner, count,
                                                  runtime::dispatch_limits::receive_batch_bytes);
            require (claim && claim->claimed_messages == count, "cost claim failed");
            for (std::size_t i = 0; i < count; ++i) {
                auto original = mailbox.take ({claim, i});
                if (original->before_application_handler)
                    original->before_application_handler ();
                original->transferred ();
            }
        });
    };
    const auto baseline = measure (false);
    const auto admitted = measure (true);
    std::cout << "permit-ownership records=" << count << " baseline-count=" << baseline.count
              << " admitted-count=" << admitted.count << " extra-per-record="
              << (admitted.count - baseline.count) / static_cast<double> (count) << std::endl;
}

void verify_inline_permit (bool bulk_release, bool enter_handler = true)
{
    runtime::application_job_queue_t jobs (runtime::application_job_queue_configuration_t{
      application_job_queue_profile_t::balanced, std::uint32_t{1}, 1, 1});
    mesh::service_mailbox_t mailbox;
    auto permit = jobs.try_reserve_supply ();
    require (permit.has_value (), "inline permit reservation failed");
    mesh::service_mailbox_record_t record;
    record.owner = owner;
    record.domain = domain;
    record.application.emplace ();
    record.application_permit.emplace (std::move (*permit));
    record.application_permit->mark_queued ();
    require (mailbox.try_enqueue (std::move (record)), "inline permit publication failed");
    auto claim =
      mailbox.try_claim_owner (domain, owner, 1, runtime::dispatch_limits::receive_batch_bytes);
    auto original = mailbox.take ({claim, 0});
    require (original && original->before_application_handler,
             "claim did not bind inline permit handler entry");
    std::size_t delivered = 0;
    auto waiter = jobs.wait_for_supply (
      [&] (auto supplied) {
          require (supplied.has_value (), "inline permit did not return capacity");
          (void) mailbox.pending_messages (domain);
          ++delivered;
      },
      runtime::application_job_queue_t::origin_t::remote);
    require (!delivered, "inline permit returned capacity before terminal");
    if (bulk_release)
        require (mailbox.release (*claim), "bulk terminal did not settle claim");
    else
        require (original->transferred (), "individual terminal did not settle claim");
    auto before = std::move (original->before_application_handler);
    original->transferred = {};
    original.reset ();
    std::weak_ptr<mesh::service_mailbox_claim_t> lifetime = claim;
    claim.reset ();
    require (lifetime.expired (), "late handler fixture retained the claim");
    std::cout << "permit-lifetime bulk=" << bulk_release << " handler=" << enter_handler
              << " claim-destroyed=" << lifetime.expired ()
              << " permits-before-handler=" << jobs.snapshot ().permits_in_use
              << " returns-before-handler=" << delivered << std::endl;
    require (delivered == 0 && jobs.snapshot ().permits_in_use == 1,
             "claim credit settlement released admission before handler entry");
    if (enter_handler) {
        before ();
        require (delivered == 1 && jobs.snapshot ().permits_in_use == 0,
                 "late handler entry did not return admission exactly once");
        before ();
    }
    before = {};
    require (delivered == 1 && jobs.snapshot ().permits_in_use == 0,
             "handler or no-handler terminal returned admission incorrectly");
    std::cout << "permit-lifetime returns=" << delivered << std::endl;
}

void verify_cut_permit ()
{
    runtime::application_job_queue_t jobs (runtime::application_job_queue_configuration_t{
      application_job_queue_profile_t::balanced, std::uint32_t{1}, 1, 1});
    mesh::service_mailbox_t mailbox;
    mesh::service_mailbox_record_t record;
    record.owner = owner;
    record.domain = domain;
    record.application.emplace ();
    record.application_permit = jobs.try_reserve_supply ();
    require (record.application_permit.has_value (), "cut permit reservation failed");
    record.application_permit->mark_queued ();
    require (mailbox.try_enqueue (std::move (record)), "cut permit publication failed");
    auto cut = mailbox.cut_owners ({owner}, [] (const auto &) { return true; }, [] {});
    require (cut && cut->front ().size () == 1, "unclaimed cut lost original");
    auto &original = cut->front ().front ();
    require (!original.application_permit && original.before_application_handler,
             "unclaimed cut did not preserve admission callback ownership");
    require (jobs.snapshot ().queued_application_jobs == 1,
             "unclaimed cut released admission before handler entry");
    original.before_application_handler ();
    require (jobs.snapshot ().permits_in_use == 0, "unclaimed cut did not return admission");
}

void verify_reply_source_snapshot ()
{
    mesh::service_mailbox_record_t original;
    original.owner = owner;
    original.domain = domain;
    original.application.emplace ();
    original.parts.push_back (zlink::message_t::from ("request"));
    original.source_routing_id = request_source;
    original.correlation = request_correlation;
    original.before_application_handler = [] {};
    const auto source = original.reply_source ();
    require (source->source_routing_id == request_source
               && source->correlation == request_correlation
               && source->reply_token.has_value () == original.reply_token.has_value (),
             "reply source snapshot changed routing metadata");
    require (!source->application && source->parts.empty () && !source->transferred
               && !source->application_permit && !source->before_application_handler
               && !source->source_original,
             "reply source snapshot retained owned application work");
    require (original.application && original.parts.size () == 1,
             "reply source snapshot consumed the owned original");
}

void verify_multicast_admission (bool has_target)
{
    using queue_t = runtime::application_job_queue_t;
    auto jobs = std::make_shared<queue_t> (runtime::application_job_queue_configuration_t{
      application_job_queue_profile_t::balanced, std::uint32_t{1}, 1, 1});
    auto target = make_host (jobs);
    if (has_target)
        target->get_or_create_spot ("multicast-target").set_subscription ("c", "topic");
    auto permit = jobs->try_reserve_supply ();
    require (permit.has_value (), "multicast fixture admission failed");
    mesh::service_mailbox_record_t original;
    original.owner = owner;
    original.domain = domain;
    original.source_routing_id = request_source;
    original.application_permit.emplace (std::move (*permit));
    original.application_permit->mark_queued ();
    namespace wire = runtime::protocol::generated::detail;
    wire::logicalMulticast_23_t header;
    header.channelName.value = "c";
    header.topic.value = "topic";
    header.sourceSpotId.value = "source";
    wire::writer_t writer;
    require (wire::encode_head_logicalMulticast_23 (writer, header) == wire::error_code::ok,
             "multicast fixture header failed");
    original.parts.push_back (zlink::message_t::from (std::move (writer.data)));
    original.parts.push_back (zlink::message_t::from (protocol::encode_application_payload (
      protocol::application_payload_t::from_parts ({zlink::message_t::from ("payload")}))));
    auto &mailbox = target->transport ().mailbox ();
    require (mailbox.try_enqueue (std::move (original)), "multicast fixture publication failed");
    auto claim =
      mailbox.try_claim_owner (domain, owner, 1, runtime::dispatch_limits::receive_batch_bytes);
    std::weak_ptr<mesh::service_mailbox_claim_t> source_claim = claim;
    std::promise<void> entered, release;
    auto resume = release.get_future ().share ();
    auto blocked = std::async (std::launch::async, [&] {
        runtime::stateful::stateful_object_runtime_test_access_t::lane (target->objects ())
          .run_sync ([&] {
              entered.set_value ();
              resume.wait ();
          });
    });
    entered.get_future ().wait ();
    bool source_destroyed = false;
    std::uint64_t queued = 0;
    try {
        auto pending =
          host::public_host_runtime_test_access_t::dispatch_claim (*target, std::move (claim));
        require (pending.await_ready () && pending.result ().value () == 1,
                 "multicast source did not finish while target snapshot was suspended");
        source_destroyed = source_claim.expired ();
        queued = jobs->snapshot ().queued_application_jobs;
    }
    catch (...) {
        release.set_value ();
        blocked.get ();
        throw;
    }
    release.set_value ();
    blocked.get ();
    require (source_destroyed && queued == 1,
             "multicast source settlement released admission before first target entry");
    if (has_target) {
        std::size_t calls = 0;
        const dispatch_t handler = [&] (const auto &, const auto &record, auto parts) {
            require (record.kind == host::record_kind_t::spot_multicast && parts.size () == 1,
                     "forwarded multicast record changed");
            require (jobs->snapshot ().queued_application_jobs == 1,
                     "multicast admission was released before handler entry");
            record.before_application_handler ();
            record.before_application_handler ();
            require (jobs->snapshot ().permits_in_use == 0,
                     "multicast first handler did not return admission exactly once");
            ++calls;
        };
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
        while (calls == 0) {
            target->dispatch_ready (handler).result ().value ();
            require (std::chrono::steady_clock::now () < deadline,
                     "forwarded multicast did not reach its target");
            std::this_thread::yield ();
        }
        require (calls == 1, "multicast target ran more than once");
    } else {
        std::promise<std::optional<queue_t::permit_t>> supplied;
        auto available = supplied.get_future ();
        auto waiter = jobs->wait_for_supply (
          [&] (auto value) { supplied.set_value (std::move (value)); }, queue_t::origin_t::remote);
        require (available.wait_for (std::chrono::seconds (5)) == std::future_status::ready,
                 "empty multicast did not return admission");
        auto returned = available.get ();
        require (returned.has_value (), "empty multicast stopped admission queue");
        returned.reset ();
        require (jobs->snapshot ().permits_in_use == 0, "empty multicast retained admission");
    }
    target->close ();
}

void verify_multicast_failure_admission ()
{
    using queue_t = runtime::application_job_queue_t;
    auto jobs = std::make_shared<queue_t> (runtime::application_job_queue_configuration_t{
      application_job_queue_profile_t::balanced, std::uint32_t{1}, 1, 1});
    auto target = make_host (jobs);
    auto permit = jobs->try_reserve_supply ();
    require (permit.has_value (), "failed multicast fixture admission failed");
    permit->mark_queued ();
    auto before = permit->defer_handler_entry ();
    task_completion_source_t<std::vector<runtime::stateful::object_ref_t>> snapshot;
    std::optional<task_t<void>> pending;
    pending.emplace (host::public_host_runtime_test_access_t::forward_multicast (
      *target, snapshot.task (), std::move (before)));
    require (!pending->await_ready () && jobs->snapshot ().queued_application_jobs == 1,
             "failed multicast did not retain suspended admission");
    snapshot.complete (result_t<std::vector<runtime::stateful::object_ref_t>>::failure (
      framework_error_kind_t::unavailable, "multicast snapshot fixture"));
    require (!pending->result (), "multicast snapshot failure was lost");
    pending.reset ();
    require (jobs->snapshot ().permits_in_use == 0,
             "failed multicast did not return pending admission");
    target->close ();
}

void verify_retained_local_original ()
{
    mesh::service_mailbox_record_t original;
    original.owner = owner;
    original.domain = domain;
    original.application.emplace ();
    original.application->parts.push_back (zlink::message_t::from ("original"));
    const auto before = original.application.operator->();
    tests::allocations_before_failure = 0;
    bool failed = false;
    try {
        (void) original.application.retain ();
    }
    catch (const std::bad_alloc &) {
        failed = true;
    }
    tests::allocations_before_failure.reset ();
    require (failed && original.application.operator->() == before
               && original.application->parts.front ().to_string () == "original",
             "failed promotion consumed the inline original");
    const auto retained = original.application.retain ();
    require (original.application.operator->() == retained.get ()
               && original.application.retain () == retained,
             "promotion did not retain the canonical original");
    mesh::service_mailbox_t mailbox;
    require (mailbox.try_enqueue (std::move (original)), "retained original publication failed");
    auto claim =
      mailbox.try_claim_owner (domain, owner, 1, runtime::dispatch_limits::receive_batch_bytes);
    auto cut = mailbox.cut_owners ({owner}, [] (const auto &) { return true; }, [] {});
    require (cut && cut->front ().size () == 1,
             "retained original was not transferred by relocation cut");
    auto &moved = cut->front ().front ();
    require (!claim->records.front ()->application
               && moved.application.operator->() == retained.get ()
               && moved.application.retain () == retained
               && retained->parts.front ().to_string () == "original",
             "relocation cut cloned or invalidated the retained original");
    moved.transferred ();
}

void verify_exception ()
{
    auto failures = std::make_shared<runtime::runtime_failure_collector_t> ();
    auto owned = make_host ({}, failures);
    auto &target = *owned;
    auto &mailbox = target.transport ().mailbox ();
    enqueue (mailbox);
    enqueue (mailbox);
    enqueue (mailbox);
    std::size_t starts = 0, rejects = 0, calls = 0;
    bool caught = false;
    auto pending = target.dispatch_application_owner (
      owner,
      [&] (const auto &, const auto &, auto) {
          if (++calls == 2)
              throw std::runtime_error ("handler fixture");
      },
      [&] { ++starts; }, [&] { ++rejects; });
    require (zlink::framework::test::application_owner_result (pending).value (),
             "record failure aborted claim continuation");
    try {
        failures->rethrow_if_failed ();
    }
    catch (const std::runtime_error &error) {
        caught = std::string_view (error.what ()) == "handler fixture";
    }
    require (caught && mailbox.pending_messages (domain) == 0,
             "exception did not return record reservations");
    require (starts == 3 && rejects == 1 && calls == 3,
             "exception did not settle started and rejected exactly once");
    enqueue (mailbox);
    require (dispatch (target, [] (const auto &, const auto &, auto) {}),
             "exception left the owner claim active");
    owned->close ();
}

void verify_standalone (host::public_host_runtime_t &target)
{
    auto &mailbox = target.transport ().mailbox ();
    enqueue (mailbox);
    std::size_t calls = 0;
    const auto count = target.dispatch_ready ([&] (const auto &, const auto &, auto) { ++calls; })
                         .result ()
                         .value ();
    require (count == 1 && calls == 1 && mailbox.pending_messages (domain) == 0,
             "standalone collect did not consume and return ordinary claim");
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
    require (zlink::framework::test::application_owner_result (
               target.dispatch_application_owner (
                 owner, callback, [&] { ++started; }, [&] { ++rejected; }))
               .value (),
             "malformed request was not claimed");
    require (started == 1 && rejected == 1 && mailbox.pending_messages (domain) == 0,
             "decode rejection did not terminalize the unstarted record");
    enqueue (mailbox);
    require (dispatch (target, [] (const auto &, const auto &, auto) {}),
             "pre-start rejection left the owner claim active");
}

void verify_reply_alias (bool channel)
{
    auto target = make_host ();
    auto &mailbox = target->transport ().mailbox ();
    auto sentinel = std::make_shared<int> (1);
    std::weak_ptr<int> alive = sentinel;
    enqueue (mailbox, true, channel, [sentinel] {});
    sentinel.reset ();
    host::reply_token_t reply;
    const dispatch_t callback = [&] (const auto &, const auto &record, auto parts) {
        require (record.kind
                     == (channel ? host::record_kind_t::channel_request
                                 : host::record_kind_t::node_request)
                   && parts.size () == 1,
                 "raw request dispatch changed");
        reply = record.reply_token;
    };
    require (dispatch (*target, callback, channel), "raw request was not dispatched");
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
    require (!alive.expired (), "retained reply lost its original callback storage");
    reply = {};
    require (alive.expired (), "idle cache retained an original after its final reply alias");
}

void verify_late_release ()
{
    auto target = make_host ();
    std::weak_ptr<host::public_host_runtime_t> alive = target;
    mesh::service_mailbox_record_t original;
    original.owner = owner;
    original.domain = domain;
    original.application.emplace ();
    original.source_original = mesh::local_request_source_t{
      {}, std::shared_ptr<const mesh::service_node_descriptor_t> (target, nullptr)};
    require (target->transport ().mailbox ().try_enqueue (std::move (original)),
             "late release fixture enqueue failed");
    host::mailbox_callback_t retained;
    host::mailbox_callback_t release;
    dispatch (*target, [&] (const auto &, const auto &record, auto) {
        retained = record.retain_mailbox_reservation;
        retained ();
        release = record.release_mailbox_reservation;
    });
    target->close ();
    target.reset ();
    require (alive.expired (), "completed cached batch retained its source host");
    release ();
    release ();
}
void verify_suspended_actor (bool local)
{
    namespace stateful = runtime::stateful;
    using authority_t = std::optional<stateful::accepted_record_authority_t>;
    task_completion_source_t<authority_t> authority;
    auto admission_entered = std::make_shared<task_completion_source_t<void>> ();
    task_completion_source_t<bool> admission;
    auto options = test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{.descriptor = {"claim-pending",
                                                   {'c', 'l', 'a', 'i', 'm'},
                                                   1,
                                                   1,
                                                   "tcp://127.0.0.1:0",
                                                   {},
                                                   mesh::service_node_state_t::preparing}});
    host::host_options_t settings{std::move (options)};
    settings.object_stable_types = {"claim_actor"};
    auto target = std::make_shared<host::public_host_runtime_t> (std::move (settings));
    target->configure_stateful_dispatch (
      [&] (stateful::accepted_record_authority_query_t) { return authority.task (); });
    target->start ();
    const auto actor = target->create_actor ("claim_actor", "pending-actor");
    auto &mailbox = target->transport ().mailbox ();
    mailbox.bind_application_dispatch ({}, {}, [&] (const auto &, auto) {
        admission_entered->complete (result_t<void>::success ());
        return admission.task ();
    });
    const auto actor_owner =
      mesh::service_mailbox_t::application_owner (host::owner_kind_t::actor, "pending-actor");
    const protocol::actor_route_fence_t fence{
      "pending-actor", 1, {'c', 'l', 'a', 'i', 'm'}, 1, 1, 1};
    mesh::service_mailbox_record_t first;
    first.owner = actor_owner;
    first.domain = domain;
    first.source_routing_id = {'c', 'l', 'a', 'i', 'm'};
    first.source_node_generation = 1;
    if (local) {
        first.application.emplace ();
        first.application->owner.owner_kind = host::owner_kind_t::actor;
        first.application->owner.actor = actor.ref ();
        first.application->record.kind = host::record_kind_t::actor_send;
        first.application->record.actor_route = fence;
        first.application->record.source_node_rid = target->status ().routing_id ();
        first.application->record.operation_id = {1, 1};
        first.application->parts.push_back (zlink::message_t::from (std::string ("ok")));
    } else {
        first.parts.push_back (zlink::message_t::from (protocol::encode_actor_message_header (
          protocol::command::actorSend, std::nullopt, fence, {1, 1})));
        protocol::application_payload_t::multipart_t parts;
        parts.push_back (zlink::message_t::from (std::string ("ok")));
        first.parts.push_back (zlink::message_t::from (protocol::encode_application_payload (
          protocol::application_payload_t::from_parts (std::move (parts)))));
    }
    require (mailbox.try_enqueue (std::move (first)), "suspended Actor enqueue failed");
    mesh::service_mailbox_record_t suffix;
    suffix.owner = actor_owner;
    suffix.domain = domain;
    suffix.application.emplace ();
    suffix.application->owner.owner_kind = host::owner_kind_t::node;
    suffix.application->record.kind = host::record_kind_t::node_send;
    require (mailbox.try_enqueue (std::move (suffix)), "suspended suffix enqueue failed");
    std::size_t starts = 0, rejects = 0, calls = 0;
    auto pending = target->dispatch_application_owner (
      actor_owner,
      [&] (const auto &, const auto &record, auto parts) {
          ++calls;
          require (calls == 1 ? record.kind == host::record_kind_t::actor_send
                              : record.kind == host::record_kind_t::node_send,
                   "claim suffix overtook suspended Actor");
          if (record.complete_stateful_dispatch) {
              require (parts.size () == 1 && parts.front ().to_string () == "ok",
                       "suspended Actor lost its payload");
              record.complete_stateful_dispatch ();
          }
      },
      [&] { ++starts; }, [&] { ++rejects; });
    auto &pending_task = std::get<task_t<bool>> (pending);
    require (!pending_task.await_ready () && starts == 2 && calls == 0 && rejects == 0,
             "authority preparation did not suspend the claim");
    detail::observe_task_completion (pending_task, [admission_entered] (const auto &) {
        admission_entered->complete (result_t<void>::failure (
          framework_error_kind_t::invalid_operation, "claim terminated before Actor admission"));
    });
    authority.complete (result_t<authority_t>::success (
      stateful::accepted_record_authority_t{{"claim-source", 1, {'c', 'l', 'a', 'i', 'm'}, 1}, 1}));
    admission_entered->task ().result ().value ();
    require (!pending_task.await_ready () && calls == 0 && rejects == 0,
             "claim bypassed pending Actor admission");
    admission.complete (result_t<bool>::success (false));
    require (pending_task.result ().value () && calls == 2 && starts == 2 && rejects == 0,
             "suspended claim did not resume and account its suffix exactly once");
    require (mailbox.pending_messages (domain) == 0, "suspended claim was not returned");
    target->close ();
}

} // namespace
int main (int argc, char **argv)
{
    try {
        if (argc == 2 && std::string_view (argv[1]) == "--permit-cost-only") {
            measure_permit_ownership_cost ();
            return 0;
        }
        if (argc == 2 && std::string_view (argv[1]) == "--permit-lifetime-only") {
            verify_inline_permit (false);
            verify_inline_permit (true);
            verify_inline_permit (false, false);
            verify_inline_permit (true, false);
            return 0;
        }
        measure_permit_ownership_cost ();
        auto target = make_host ();
        verify_retained_batch (*target);
        verify_original_transfer (*target);
        verify_nested_batch (*target);
        verify_inline_permit (false);
        verify_inline_permit (true);
        verify_inline_permit (false, false);
        verify_inline_permit (true, false);
        verify_cut_permit ();
        verify_reply_source_snapshot ();
        verify_retained_local_original ();
        verify_multicast_admission (true);
        verify_multicast_admission (false);
        verify_multicast_failure_admission ();
        verify_exception ();
        verify_prestart_terminal (*target);
        verify_reply_alias (false);
        verify_reply_alias (true);
        verify_late_release ();
        verify_standalone (*target);
        verify_suspended_actor (false);
        verify_suspended_actor (true);
        const auto node_cost = verify_cost (*target, false);
        const auto channel_cost = verify_cost (*target, true);
        const auto owning_node_cost = verify_cost (*target, false, true);
        const auto owning_channel_cost = verify_cost (*target, true, true);
        verify_batch_record_cost (*target);
        require (node_cost && channel_cost && owning_node_cost && owning_channel_cost,
                 "ordinary claim exceeded synchronous ownership count/byte bounds");
        target->close ();
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << std::endl;
        return 1;
    }
}

/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "allocation_counter.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime_failure_fixture.hpp"

#include <atomic>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
using dispatch_completion_t =
  std::optional<zlink::framework::task_t<zlink::framework::result_t<void>>>;

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

std::shared_ptr<host::public_host_runtime_t> make_host (bool start = true)
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
    if (start)
        result->start ();
    return result;
}

void enqueue (mesh::service_mailbox_t &mailbox, bool raw_request = false, bool channel = false)
{
    mesh::service_mailbox_record_t record;
    record.owner = claim_owner (channel);
    record.domain = domain;
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
        record.application = std::make_shared<host::local_application_dispatch_t> ();
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
    auto result = target.dispatch_application_owner (claim_owner (channel),
      [&] (const auto &ready, const auto &record, auto parts) -> dispatch_completion_t {
          callback (ready, record, std::move (parts));
          return std::nullopt;
      }, noop, noop);
    if (const auto ready = std::get_if<bool> (&result))
        return *ready;
    std::get<task_t<std::size_t>> (result).result ().value ();
    return true;
}

// Empty claims have no result storage owner. Compare against the value itself,
// and reject equal-cost comparisons so the guard cannot claim a reduction by
// measuring the same implementation twice.
bool verify_empty_cost (host::public_host_runtime_t &target)
{
    const dispatch_t callback = [] (const auto &, const auto &, auto) {
        throw std::runtime_error ("empty claim reached handler");
    };
    const auto measure = [&] {
        return tests::measure_allocations ([&] {
            require (!dispatch (target, callback), "empty claim returned work");
        });
    };
    (void) measure ();
    const auto actual = measure ();
    const auto same = measure ();
    const auto reduced = [] (const auto &before, const auto &after) {
        return after.count < before.count && after.bytes < before.bytes;
    };
    require (!reduced (actual, same), "equal implementation accepted as a reduction");
    std::cout << "empty-owner count=" << actual.count << " bytes=" << actual.bytes
              << " same-count=" << same.count << " same-bytes=" << same.bytes
              << " equal-reduction-rejected=true expected-count=0 expected-bytes=0" << std::endl;
    return actual.count == 0 && actual.bytes == 0;
}

// Calibrate the canonical batch: originals and retain marks share one buffer,
// while the producer claim owns credit settlement and the owner FIFO.
// No fixed frame, control-block or std::function SBO byte size is assumed.
struct record_state_t
{
    std::optional<mesh::service_mailbox_record_t> original;
    std::atomic_bool retained{false};
};
struct owner_layout_t
{
    std::vector<record_state_t> records{1};
    std::shared_ptr<mesh::service_mailbox_claim_t> claim;
    dispatch_t dispatch;
    std::function<void ()> rejected;
    std::size_t index = 0, count = 0, handed_off = 0;
};
auto ownership_baseline ()
{
    return tests::measure_allocations ([] {
        auto shared = std::make_shared<owner_layout_t> ();
        const std::size_t index = 0;
        const std::function<void ()> retain = [shared, index] {
            shared->records[index].retained = true;
        };
        const std::function<void ()> finish = [shared, index] {
            shared->records[index].original.reset ();
        };
        retain ();
        finish ();

    });
}
bool verify_cost (host::public_host_runtime_t &target, bool channel, bool owning_callback = false)
{
    mesh::service_mailbox_t mailbox;
    enqueue (mailbox, false, channel);
    // The original moves once, inside the claim, exactly as the dispatcher takes it;
    // a record move allocates on some standard libraries (MSVC std::map).
    const auto claim_cost = tests::measure_allocations ([&] {
        std::optional<mesh::service_mailbox_record_t> original;
        const auto transfer = [&original] (const auto &claim) {
            original.emplace (std::move (*claim->records[0]));
        };
        auto claim = mailbox.try_claim_owner (domain, claim_owner (channel), 1,
                                              runtime::dispatch_limits::receive_batch_bytes,
                                              std::ref (transfer));
        require (static_cast<bool> (claim), "baseline claim failed");
        require (original.has_value (), "baseline original transfer failed");
        require (original->transferred (), "baseline release failed");
    });
    const auto ownership = ownership_baseline ();
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
    const auto limit_count = claim_cost.count + ownership.count + input_copy.count;
    const auto limit_bytes = claim_cost.bytes + ownership.bytes + input_copy.bytes;
    const std::function<void ()> started = [&state] { ++state.starts; };
    const std::function<void ()> rejected = [&state] { ++state.rejects; };
    const auto measure = [&] {
        enqueue (target.transport ().mailbox (), false, channel);
        return tests::measure_allocations ([&] {
            auto pending = target.dispatch_application_owner (claim_owner (channel),
      [&] (const auto &ready, const auto &record, auto parts) -> dispatch_completion_t {
          callback (ready, record, std::move (parts));
          return std::nullopt;
      },
                                                              started, rejected);
            require (std::holds_alternative<bool> (pending),
                     "ordinary claim did not return its ready value");
            require (std::get<bool> (pending), "dispatch failed");
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
    enqueue (mailbox);
    std::size_t starts = 0, rejects = 0, calls = 0;
    bool caught = false;
    auto pending = target.dispatch_application_owner (
      owner,
      [&] (const auto &, const auto &, auto) -> dispatch_completion_t {
          if (++calls == 2)
              throw std::runtime_error ("handler fixture");

          return std::nullopt;
      },
      [&] { ++starts; }, [&] { ++rejects; });
    try {
        std::get<task_t<std::size_t>> (pending).result ().value ();
    }
    catch (const std::runtime_error &error) {
        caught = std::string_view (error.what ()) == "handler fixture";
    }
    require (caught && mailbox.pending_messages (domain) == 0,
             "exception did not return current and unstarted record reservations");
    require (starts == 3 && rejects == 2 && calls == 2,
             "exception did not settle started and rejected exactly once");
    enqueue (mailbox);
    require (dispatch (target, [] (const auto &, const auto &, auto) {}),
             "exception left the owner claim active");
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
    require (
      std::get<bool> (target.dispatch_application_owner (
        owner,
      [&] (const auto &ready, const auto &record, auto parts) -> dispatch_completion_t {
          callback (ready, record, std::move (parts));
          return std::nullopt;
      }, [&] { ++started; }, [&] { ++rejected; })),
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
    enqueue (mailbox, true, channel);
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
enum class terminal_case_t { retained_success, failed, cancelled, shutdown, dropped_success };

void verify_suspended_actor (
  bool local, terminal_case_t terminal = terminal_case_t::retained_success)
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
        first.application = std::make_shared<host::local_application_dispatch_t> ();
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
    suffix.application = std::make_shared<host::local_application_dispatch_t> ();
    suffix.application->owner.owner_kind = host::owner_kind_t::node;
    suffix.application->record.kind = host::record_kind_t::node_send;
    require (mailbox.try_enqueue (std::move (suffix)), "suspended suffix enqueue failed");
    std::size_t starts = 0, rejects = 0, calls = 0;
    const dispatch_t callback = [&] (const auto &, const auto &record, auto parts) {
          ++calls;
          require (calls == 1 ? record.kind == host::record_kind_t::actor_send
                              : record.kind == host::record_kind_t::node_send,
                   "claim suffix overtook suspended Actor");
          if (record.complete_stateful_dispatch) {
              require (parts.size () == 1 && parts.front ().to_string () == "ok",
                       "suspended Actor lost its payload");
              record.complete_stateful_dispatch ();
          }
      };
    using owner_result_t = decltype (target->dispatch_application_owner (
      actor_owner,
      [&] (const auto &ready, const auto &record, auto parts) -> dispatch_completion_t {
          callback (ready, record, std::move (parts));
          return std::nullopt;
      }, noop, noop));
    std::optional<owner_result_t> measured;
    const auto cost = tests::measure_allocations ([&] {
        measured.emplace (target->dispatch_application_owner (
          actor_owner,
      [&] (const auto &ready, const auto &record, auto parts) -> dispatch_completion_t {
          callback (ready, record, std::move (parts));
          return std::nullopt;
      }, [&] { ++starts; }, [&] { ++rejects; }));
    });
    require (std::holds_alternative<task_t<std::size_t>> (*measured),
             "authority preparation returned a ready value");
    auto pending = std::get<task_t<std::size_t>> (std::move (*measured));
    std::cout << "suspended-owner-allocation local=" << local << " terminal=" << static_cast<int> (terminal)
              << " count=" << cost.count << " bytes=" << cost.bytes << std::endl;
    require (!pending.await_ready () && starts == 2 && calls == 0 && rejects == 0,
             "authority preparation did not suspend the claim");
    if (terminal != terminal_case_t::retained_success) {
        auto failures = std::make_shared<runtime::runtime_failure_collector_t> ();
        std::atomic_size_t reports{0};
        std::atomic_bool report_has_error{false};
        auto observed = std::make_shared<task_completion_source_t<void>> ();
        auto released = std::make_shared<task_completion_source_t<void>> ();
        failures->bind_runtime_reporter ([&] (std::exception_ptr error) {
            report_has_error = static_cast<bool> (error);
            ++reports;
        });
        std::weak_ptr<int> observer_lifetime;
        {
            auto lifetime = std::shared_ptr<int> (new int (0), [released] (int *value) mutable {
                delete value;
                released->complete (result_t<void>::success ());
            });
            observer_lifetime = lifetime;
            // The infrastructure observer owns no task. Discarding the returned
            // task must leave its coroutine promise and this observer alive.
            detail::observe_task_terminal (pending,
              [failures, observed, lifetime] (const auto &result) mutable {
                  failures->capture ([&] { result.value (); });
                  observed->complete (result_t<void>::success ());
              });
        }
        { auto discarded = std::move (pending); }
        require (!observer_lifetime.expired (), "pending terminal observer died early");
        if (terminal == terminal_case_t::shutdown)
            target->close ();
        const auto error = terminal == terminal_case_t::cancelled ? detail::make_cancellation_exception ("claim cancelled")
                                        : std::make_exception_ptr (
                                            std::runtime_error ("claim authority failed"));
        if (terminal == terminal_case_t::dropped_success) {
            authority.complete (result_t<authority_t>::success (
              stateful::accepted_record_authority_t{
                {"claim-source", 1, {'c', 'l', 'a', 'i', 'm'}, 1}, 1}));
            admission_entered->task ().result ().value ();
            admission.complete (result_t<bool>::success (false));
        } else
            authority.complete (detail::result_access_t::failure<authority_t> (error));
        observed->task ().result ().value ();
        released->task ().result ().value ();
        require (observer_lifetime.expired (), "terminal observer retained its capture");
        target->close ();
        require (reports == (terminal == terminal_case_t::dropped_success ? 0u : 1u)
                   && report_has_error == (terminal != terminal_case_t::dropped_success) && starts == 2
                   && calls == (terminal == terminal_case_t::dropped_success ? 2u : 0u)
                   && rejects == (terminal == terminal_case_t::dropped_success ? 0u : 2u),
                 ("suspended claim terminal accounting changed: local="
                  + std::to_string (local) + " terminal="
                  + std::to_string (static_cast<int> (terminal))
                  + " reports=" + std::to_string (reports.load ())
                  + " expected_reports="
                  + std::to_string (terminal == terminal_case_t::dropped_success ? 0u : 1u)
                  + " report_has_error=" + std::to_string (report_has_error.load ())
                  + " expected_report_has_error="
                  + std::to_string (terminal != terminal_case_t::dropped_success)
                  + " starts=" + std::to_string (starts) + " expected_starts=2"
                  + " calls=" + std::to_string (calls) + " expected_calls="
                  + std::to_string (terminal == terminal_case_t::dropped_success ? 2u : 0u)
                  + " rejects=" + std::to_string (rejects) + " expected_rejects="
                  + std::to_string (terminal == terminal_case_t::dropped_success ? 0u : 2u))
                   .c_str ());
        require (mailbox.pending_messages (domain) == 0,
                 "failed/cancelled/shutdown claim leaked reservations");
        std::cout << "suspended-owner local=" << local << " terminal=" << static_cast<int> (terminal)
                  << " failure-reports=" << reports << " starts=" << starts
                  << " rejects=" << rejects << std::endl;
        return;
    }
    detail::observe_task_completion (pending, [admission_entered] (const auto &) {
        admission_entered->complete (result_t<void>::failure (
          framework_error_kind_t::invalid_operation, "claim terminated before Actor admission"));
    });
    authority.complete (result_t<authority_t>::success (
      stateful::accepted_record_authority_t{{"claim-source", 1, {'c', 'l', 'a', 'i', 'm'}, 1}, 1}));
    admission_entered->task ().result ().value ();
    require (!pending.await_ready () && calls == 0 && rejects == 0,
             "claim bypassed pending Actor admission");
    admission.complete (result_t<bool>::success (false));
    pending.result ().value ();
    require (calls == 2 && starts == 2 && rejects == 0,
             "suspended claim did not resume and account its suffix exactly once");
    require (mailbox.pending_messages (domain) == 0, "suspended claim was not returned");
    target->close ();
}

} // namespace
int main (int argc, char **argv)
{
    try {
        const bool ready_only = argc == 2 && std::string_view (argv[1]) == "--ready-only";
        auto target = make_host (!ready_only);
        const auto empty_cost = verify_empty_cost (*target);
        verify_retained_batch (*target);
        verify_exception (*target);
        if (!ready_only) {
            verify_prestart_terminal (*target);
            verify_reply_alias (false);
            verify_reply_alias (true);
            verify_late_release ();
            verify_standalone (*target);
            verify_suspended_actor (false);
            verify_suspended_actor (true);
            for (const auto terminal : {terminal_case_t::failed, terminal_case_t::cancelled,
                                        terminal_case_t::shutdown, terminal_case_t::dropped_success}) {
                verify_suspended_actor (false, terminal);
                verify_suspended_actor (true, terminal);
            }
        }
        const auto node_cost = verify_cost (*target, false);
        const auto channel_cost = verify_cost (*target, true);
        const auto owning_node_cost = verify_cost (*target, false, true);
        const auto owning_channel_cost = verify_cost (*target, true, true);
        require (empty_cost && node_cost && channel_cost && owning_node_cost && owning_channel_cost,
                 "ordinary claim exceeded synchronous ownership count/byte bounds");
        target->close ();
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << std::endl;
        return 1;
    }
}

/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_relocation_store_fixture.hpp"
#include "../support/m6c_runtime_cases.hpp"
#include "../support/m6c_stateful_fixture.hpp"
#include "../support/m6c_task.hpp"
#include "runtime/timers/async_delay.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace m6c_test
{

using namespace zlink::framework::runtime::stateful;

void test_source_relocation_settles_by_authority (test_context_t &test,
                                                  bool cancel_management_delay,
                                                  bool cancel_cutover_submit,
                                                  bool cancel_pending_wait,
                                                  bool expired_teardown)
{
    using namespace std::chrono_literals;
    namespace framework = zlink::framework;
    namespace protocol = zlink::framework::runtime::protocol;

    struct outcome_t
    {
        relocation_result_t result;
        std::optional<zlink::framework::framework_error_kind_t> operation_error;
        int data_sends = 0;
        int cutover_sends = 0;
        bool late_accepted = false;
        bool late_relayed = false;
        bool dispatch_reopened = false;
        std::vector<object_ref_t> unavailable;
        std::optional<relocation_authority_fence_t> observed;
        int preserve_calls = 0;
        bool registered_wait = false;
    };
    /* One unit whose cutover submit first misses the connection; `settle`
     * scripts the Store after that submit terminal. */
    const auto run = [cancel_management_delay, cancel_cutover_submit, cancel_pending_wait,
                      expired_teardown] (
                       std::chrono::milliseconds restore_after,
                       const std::function<void (settlement_authority_t &)> &settle,
                       bool expire_source_lease = false, bool inject_late = false,
                       bool submitted_ok = false, bool change_route = false) {
        stateful_object_runtime_t objects;
        objects.configure_relocation_state (
          [] (const object_ref_t &, const std::string &, std::stop_token)
            -> zlink::framework::task_t<
              zlink::framework::runtime::stateful::frozen_object_state_t> {
              frozen_object_state_t captured;
              captured.application_state = std::vector<std::uint8_t> (16, 0x5a);
              co_return captured;
          },
          [] (const frozen_object_state_t &, const object_ref_t &,
              std::stop_token) -> zlink::framework::task_t<bool> { co_return true; });
        const auto actor = create_actor (objects, "settled-actor");
        auto roots = std::make_shared<memory_relocation_repository_t> ();
        auto authority = std::make_shared<settlement_authority_t> ();
        settle (*authority);
        if (expire_source_lease) {
            authority->observed_authority = relocation_authority_t::source_lease_expired;
            authority->settle_after_observes = 3;
        }
        const auto requested_authority = authority->observed_authority;
        if (cancel_pending_wait) {
            authority->observed_authority = relocation_authority_t::unsettled;
            authority->settle_after_observes = 0;
        }
        std::mutex delay_mutex;
        zlink::framework::detail::delay_owner_t delays (delay_mutex);
        maintenance_runtime_t relocation (objects, authority, roots);
        relocation.bind_delay_owner (delays);
        outcome_t outcome;
        relocation.attach_unavailable_terminal (
          [&outcome] (const object_ref_t &owner) { outcome.unavailable.push_back (owner); });
        const auto teardown_deadline = std::chrono::steady_clock::now () + restore_after;
        eligible_relocation_unit_t::canonical_wire_context_t wire{
          .relocation = {0x61, 0x62},
          .target_attempt_generation = 7,
          .coordinator = {"source-owner", 1, {'s', 'r', 'c'}, 3, "authority-v1"},
          .target_node_routing_id = {'d', 's', 't'},
          .target_node_generation = 7,
          .application_version = 1,
          .prepare_target = [] (const std::vector<frozen_object_state_t> &,
                                const relocation_payload_manifest_t &)
            -> framework::task_t<relocation_reason_t> { co_return relocation_reason_t::none; },
          .send_state_chunk = [] (const protocol::relocation_state_t &) -> framework::task_t<bool> {
              co_return true;
          },
          .send_relocation_data =
            [&outcome] (const std::vector<protocol::relocation_data_t> &,
                        const relocation_ingress_batch_t &) -> framework::task_t<bool> {
              ++outcome.data_sends;
              co_return true;
          },
          .send_application =
            [&outcome] (const object_ref_t &, const turn_record_t &record,
                        std::optional<std::uint64_t>) {
                if (record.application_record
                    && record.application_record->operation
                         == protocol::wire_operation_id_t{0x1087, 0x44}
                    && record.application_record->reply_route_id == 77)
                    outcome.late_relayed = true;
            },
          .send_cutover = [&] (const protocol::relocation_cutover_t &) -> framework::task_t<bool> {
              if (++outcome.cutover_sends == 1) {
                  if (inject_late) {
                      protocol::frozen_application_record_t late;
                      late.kind = protocol::frozen_record_kind_t::actor_request;
                      late.source_kind = protocol::frozen_source_kind_t::node;
                      late.source = {"source-owner", 1, {'s', 'r', 'c'}, 3};
                      late.operation = {0x1087, 0x44};
                      late.operation_kind = 4;
                      late.reply_route_id = 77;
                      late.body = protocol::frozen_actor_application_body_t{
                        {actor.key,
                         actor.object_generation,
                         {'d', 's', 't'},
                         7,
                         actor.authority_owner_generation,
                         9},
                        {"ActorPacket", "application/json", {0x42}}};
                      outcome.late_accepted = objects.enqueue (actor, turn_domain_t::application,
                                                               {1, {}, 1, std::move (late)})
                                              == stateful_error_t::none;
                  }
                  if (cancel_management_delay && !cancel_pending_wait)
                      delays.request_stop (expired_teardown ? std::chrono::steady_clock::now ()
                                                            : teardown_deadline);
                  if (cancel_cutover_submit)
                      throw framework::framework_exception_t (
                        framework::framework_error_kind_t::shutting_down,
                        "source cutover submission cancelled during host shutdown");
                  co_return submitted_ok;
              }
              co_return true;
          },
          .restore_deadline = std::chrono::steady_clock::now () + restore_after,
          .target_route =
            [&] {
                const auto changed =
                  outcome.cutover_sends != 0
                  && (!submitted_ok || (change_route && authority->observe_calls != 0));
                return std::pair{std::uint64_t{changed ? 2u : 1u}, true};
            },
          .abort_target_before_cutover = [] { return true; }};
        auto moving = relocation.relocate (actor, "target-node", {"target-owner", 9}, 1024 * 1024,
                                           digest_with (0x41), wire);
        if (cancel_pending_wait) {
            {
                std::lock_guard lock (authority->mutex);
                outcome.registered_wait = authority->observed.has_value ()
                                          && delays.worker_count () >= 1 && !moving.await_ready ();
            }
            delays.request_stop (teardown_deadline);
            std::lock_guard lock (authority->mutex);
            authority->observed_authority = requested_authority;
        }
        const auto result = moving.result ();
        if (!result && (cancel_management_delay || cancel_cutover_submit)) {
            outcome.operation_error = result.error_kind ();
            std::cerr << "cancelled source relocation operation_error="
                      << static_cast<int> (result.error_kind ())
                      << " message=" << (result.error () ? result.error ()->what () : "") << '\n';
        } else
            outcome.result = result.value ();
        {
            std::lock_guard lock (authority->mutex);
            outcome.observed = authority->observed;
            outcome.preserve_calls = authority->preserve_calls;
        }
        (void) objects.enqueue (actor, turn_domain_t::application, {1, {0x42}});
        const auto [claim_error, claimed] = objects.try_claim (actor, turn_domain_t::application);
        outcome.dispatch_reopened = claim_error == stateful_error_t::none && claimed.has_value ();
        return outcome;
    };

    if (expired_teardown) {
        const auto unknown = run (10s, [] (settlement_authority_t &) {}, false, false, true);
        test.require (!unknown.operation_error
                        && unknown.result.settlement.outcome == relocation_authority_t::unsettled
                        && unknown.result.terminal == relocation_terminal_t::recovery_required
                        && !unknown.dispatch_reopened && unknown.unavailable.size () == 1,
                      "UNKNOWN at the existing host teardown deadline must remain unavailable "
                      "and apply the source terminal exactly once");
        return;
    }
    if (cancel_management_delay || cancel_cutover_submit) {
        // Cancelling the host delay owner in the cutover callback makes its
        // first management wait fail synchronously. The scripted second read
        // must still decide the source terminal, without a timer or sleep.
        for (const auto terminal :
             {relocation_authority_t::target_committed, relocation_authority_t::source_preserved,
              relocation_authority_t::source_lease_expired}) {
            const auto cancelled = run (
              10s,
              [terminal] (settlement_authority_t &authority) {
                  authority.observed_authority = terminal;
                  authority.settle_after_observes = 2;
              },
              false, false, true);
            if (cancel_pending_wait)
                test.require (cancelled.registered_wait,
                              "pending cancellation must own an actual suspended management wait");
            test.require (!cancelled.operation_error,
                          "management cancellation must reach the authority-owned source terminal");
            test.require (cancelled.result.settlement.outcome == terminal,
                          "management cancellation must preserve the authority settlement outcome");
            test.require (
              cancelled.result.terminal
                == (terminal == relocation_authority_t::target_committed
                      ? relocation_terminal_t::completed
                    : terminal == relocation_authority_t::source_preserved
                      ? relocation_terminal_t::blocked
                      : relocation_terminal_t::recovery_required),
              "management cancellation must use the normal relocation terminal classification");
            test.require (
              cancelled.dispatch_reopened == (terminal == relocation_authority_t::source_preserved),
              "only confirmed source Preserve may reopen dispatch after management cancellation");
        }
        return;
    }

    // Target commit confirmed by the Store is the only evidence of Relocated.
    const auto committed = run (
      10s,
      [] (settlement_authority_t &authority) {
          authority.observed_authority = relocation_authority_t::target_committed;
          authority.settle_after_observes = 2;
      },
      false, true);
    test.require (
      committed.result.terminal == relocation_terminal_t::completed && committed.preserve_calls == 0
        && committed.cutover_sends >= 2 && committed.data_sends >= 2 && committed.observed
        && committed.observed->key == "settled-actor"
        && committed.observed->expected_store_version == "authority-v1"
        && committed.observed->source_owner.owner_id == "source-owner"
        && committed.observed->target_owner.owner_id == "target-owner" && committed.late_accepted
        && committed.late_relayed && !committed.dispatch_reopened && committed.unavailable.empty (),
      "a relocation unit completes only once the Store shows the target commit; a "
      "cutover that missed the connection is resent after the whole batch");

    const auto replaced = run (
      10s,
      [] (settlement_authority_t &authority) {
          authority.observed_authority = relocation_authority_t::target_committed;
          authority.settle_after_observes = 4;
      },
      false, false, true, true);
    test.require (replaced.cutover_sends == 2 && replaced.data_sends == 2
                    && replaced.result.terminal == relocation_terminal_t::completed,
                  "selected route replacement resends the whole batch and cutover once without a "
                  "disconnected edge");
    const auto unchanged = run (
      10s,
      [] (settlement_authority_t &authority) {
          authority.observed_authority = relocation_authority_t::target_committed;
          authority.settle_after_observes = 4;
      },
      false, false, true, false);
    test.require (unchanged.cutover_sends == 1 && unchanged.data_sends == 1,
                  "the same selected route must not cause periodic cutover submissions");

    // Restore deadline without a target commit: the Preserve fence wins.
    const auto preserved = run (0ms, [] (settlement_authority_t &authority) {
        authority.preserved_authority = relocation_authority_t::source_preserved;
    });
    test.require (preserved.result.terminal == relocation_terminal_t::blocked
                    && preserved.preserve_calls >= 1 && preserved.dispatch_reopened
                    && preserved.unavailable.empty (),
                  "a winning source Preserve fence returns the unit to source dispatch");

    // Source owner lease expiry before Preserve succeeds: permanent stop.
    const auto expired = run (0ms, [] (settlement_authority_t &authority) {
        authority.preserved_authority = relocation_authority_t::source_lease_expired;
    });
    test.require (expired.result.terminal == relocation_terminal_t::recovery_required
                    && expired.result.reason == relocation_reason_t::owner_lease_expired
                    && !expired.dispatch_reopened && expired.unavailable.size () == 1
                    && expired.unavailable.front ().key == "settled-actor",
                  "source lease expiry stops the unit for good and fails its pending requests "
                  "with Unavailable");

    const auto stopped = run (10s, [] (settlement_authority_t &) {}, true);
    test.require (
      stopped.result.terminal == relocation_terminal_t::recovery_required
        && stopped.unavailable.size () == 1 && !stopped.dispatch_reopened,
      "a Store-confirmed expired source lease ends settlement and fails retained requests");
}

} // namespace m6c_test

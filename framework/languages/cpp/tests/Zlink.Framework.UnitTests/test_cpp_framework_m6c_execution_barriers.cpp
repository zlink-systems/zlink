/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_runtime_cases.hpp"
#include "../support/m6c_stateful_fixture.hpp"
#include "../support/m6c_task.hpp"
#include "runtime/stateful/maintenance_runtime.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

namespace m6c_test
{

using namespace zlink::framework::runtime::stateful;

void test_generation_barrier_quiesces_yield_spot_and_timer (test_context_t &test)
{
    stateful_object_runtime_t objects;
    const auto actor = create_actor (objects, "barrier-actor");
    const auto spot = create_spot (objects, object_kind_t::user_spot, "barrier-spot");
    test.require (objects.register_timer (actor, {9, 1000, 1000, 30}) == stateful_error_t::none,
                  "barrier test timer must register");
    test.require (
      objects.enqueue (actor, turn_domain_t::application, {10, {10}}) == stateful_error_t::none
        && objects.enqueue (actor, turn_domain_t::application, {20, {20}}) == stateful_error_t::none
        && objects.enqueue (spot, turn_domain_t::application, {40, {40}}) == stateful_error_t::none,
      "barrier test turns must enqueue");

    const auto [actor_claim_error, actor_claim] =
      objects.try_claim (actor, turn_domain_t::application);
    const auto [spot_claim_error, spot_claim] =
      objects.try_claim (spot, turn_domain_t::application);
    test.require (actor_claim_error == stateful_error_t::none && actor_claim
                    && actor_claim->sequence == 10 && spot_claim_error == stateful_error_t::none
                    && spot_claim && spot_claim->sequence == 40,
                  "Actor and Spot lanes must both be active before sealing");
    test.require (objects.yield_claim (actor, {11, {11}}) == stateful_error_t::none,
                  "yield must retain the Actor claim until its continuation completes");

    std::atomic<bool> seal_completed = false;
    stateful_error_t seal_error = stateful_error_t::conflict;
    aggregate_relocation_seal_t seal;
    std::thread sealing ([&] {
        auto result = await_task (objects.try_seal_relocation_aggregate ({actor, spot}));
        seal_error = result.error;
        seal = std::move (result.seal);
        seal_completed.store (true, std::memory_order_release);
    });

    const bool sealed = wait_until_bounded (
      [&] { return objects.cancel_timer (actor, 999) == stateful_error_t::moving; },
      std::chrono::seconds (5));
    test.require (sealed && !seal_completed.load (std::memory_order_acquire),
                  "seal must close timer admission and wait for active lanes");
    test.require (objects.enqueue_timer_tick (actor, 9, {30}) == stateful_error_t::moving,
                  "timer dispatch must not mutate the sealed generation");

    const auto [continuation_error, continuation] =
      objects.try_claim (actor, turn_domain_t::application);
    test.require (continuation_error == stateful_error_t::none && continuation
                    && continuation->sequence == 11,
                  "yielded continuation must reacquire its Actor lane while sealed");
    test.require (objects.complete_claim (actor, turn_domain_t::application)
                      == stateful_error_t::none
                    && !seal_completed.load (std::memory_order_acquire),
                  "Spot lane must also quiesce before capture");
    test.require (objects.complete_claim (spot, turn_domain_t::application)
                    == stateful_error_t::none,
                  "active Spot lane must complete");
    sealing.join ();

    test.require (seal_error == stateful_error_t::none && seal.participants.size () == 2
                    && seal.participants[0].pending_application.size () == 1
                    && seal.participants[0].pending_application[0].sequence == 20
                    && seal.participants[0].timers
                         == std::vector<logical_timer_t> ({{9, 1000, 1000, 30}}),
                  "capture must occur after quiescence and preserve queued work and timers");

    test.require (objects.enqueue (actor, turn_domain_t::application, {21, {21}})
                    == stateful_error_t::none,
                  "sealed ingress must be retained for same-generation abort");
    test.require (objects.abort_relocation_before_cutover (seal.token).result ().value ()
                    == stateful_error_t::none,
                  "same-generation abort must reopen the seal");
    test.require (objects.finalize_relocation_cutover (seal.token).result ().value ()
                    == stateful_error_t::not_found,
                  "stale commit must not mutate an aborted generation");

    const auto [first_error, first] = objects.try_claim (actor, turn_domain_t::application);
    test.require (first_error == stateful_error_t::none && first && first->sequence == 20,
                  "abort must restore the captured queue before held ingress");
    test.require (objects.complete_claim (actor, turn_domain_t::application)
                    == stateful_error_t::none,
                  "restored captured turn must complete");
    const auto [held_error, held] = objects.try_claim (actor, turn_domain_t::application);
    test.require (held_error == stateful_error_t::none && held && held->sequence == 21,
                  "abort must restore held ingress in FIFO order");
    test.require (objects.complete_claim (actor, turn_domain_t::application)
                    == stateful_error_t::none,
                  "restored held turn must complete");

    const auto [second_error, second_seal] =
      await_task (objects.try_seal_relocation_aggregate ({actor, spot}));
    test.require (second_error == stateful_error_t::none
                    && objects.abort_relocation_before_cutover (seal.token).result ().value ()
                         == stateful_error_t::not_found,
                  "stale abort must not reopen a newer generation");
    test.require (objects.abort_relocation_before_cutover (second_seal.token).result ().value ()
                      == stateful_error_t::none
                    && objects.enqueue (actor, turn_domain_t::application, {22, {22}})
                         == stateful_error_t::none,
                  "the current pre-Cutover abort must reopen application ingress");
}

void test_close_eligibility_preserves_execution_and_deletion_is_fenced (test_context_t &test)
{
    stateful_object_runtime_t objects;
    const auto spot = create_spot (objects, object_kind_t::user_spot, "closing-spot");
    test.require (objects.enqueue (spot, turn_domain_t::application, {1, {1}})
                    == stateful_error_t::none,
                  "eligibility test turn must enqueue");
    const auto [claim_error, claim] = objects.try_claim (spot, turn_domain_t::application);
    test.require (claim_error == stateful_error_t::none && claim && claim->sequence == 1,
                  "Spot lane must be active before eligibility is read");
    test.require (objects.can_close_spot_async (spot).result ().value ()
                    == std::pair{stateful_error_t::none, true},
                  "eligibility must return without waiting for the active application turn");
    test.require (objects.find (object_kind_t::user_spot, spot.key) == spot
                    && objects.register_timer (spot, {1, 1000, 1000, 1}) == stateful_error_t::none
                    && objects.cancel_timer (spot, 1) == stateful_error_t::none,
                  "eligibility must preserve the exact object and timer admission");
    test.require (
      objects.enqueue (spot, turn_domain_t::application, {2, {2}}) == stateful_error_t::none
        && objects.complete_claim (spot, turn_domain_t::application) == stateful_error_t::none,
      "eligibility must preserve application ingress and current turn completion");
    const auto [next_error, next] = objects.try_claim (spot, turn_domain_t::application);
    test.require (next_error == stateful_error_t::none && next && next->sequence == 2,
                  "eligibility must not hold the next accepted application turn");
    test.require (objects.complete_claim (spot, turn_domain_t::application)
                    == stateful_error_t::none,
                  "the execution owner completes application work before local release");
    auto stale = spot;
    ++stale.object_generation;
    test.require (objects.can_close_spot_async (stale).result ().value ().first
                      == stateful_error_t::generation_stale
                    && objects.close_spot (stale).first == stateful_error_t::generation_stale
                    && objects.find (object_kind_t::user_spot, spot.key) == spot,
                  "stale eligibility and deletion must preserve the current generation");
    const auto actor = create_actor (objects, "closing-member");
    const auto [join_error, join] =
      objects.begin_membership_move_async (actor, spot).result ().value ();
    const auto [membership_error, member] =
      objects.commit_membership_move_async (join).result ().value ();
    test.require (join_error == stateful_error_t::none && membership_error == stateful_error_t::none
                    && objects.can_close_spot_async (spot).result ().value ()
                         == std::pair{stateful_error_t::none, false}
                    && objects.close_spot (spot) == std::pair{stateful_error_t::none, false}
                    && objects.find (object_kind_t::user_spot, spot.key) == spot,
                  "active Actor membership keeps eligibility false and preserves the Spot");
    test.require (objects.destroy_actor (member) == stateful_error_t::none
                    && objects.can_close_spot_async (spot).result ().value ()
                         == std::pair{stateful_error_t::none, true},
                  "removing the last member makes the same exact Spot eligible");
    test.require (objects.close_spot (spot) == std::pair{stateful_error_t::none, true}
                    && !objects.find (object_kind_t::user_spot, spot.key)
                    && objects.close_spot (spot).first == stateful_error_t::not_found,
                  "exact local release deletes once and never retains a Close token");
}

} // namespace m6c_test

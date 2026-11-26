/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_runtime_cases.hpp"
#include "../support/m6c_stateful_fixture.hpp"
#include "../support/m6c_task.hpp"
#include "runtime/stateful/maintenance_runtime.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace m6c_test
{

using namespace zlink::framework::runtime::stateful;

void test_restore_validates_generation_before_spot_publication (test_context_t &test)
{
    stateful_object_runtime_t single;
    const auto old_spot = create_spot (single, object_kind_t::user_spot, "stale-single");
    test.require (single.close_spot (old_spot) == std::pair{stateful_error_t::none, true},
                  "single stale-generation setup must close the old Spot");
    int single_restore_count = 0;
    single.configure_relocation_state (
      [] (const object_ref_t &, const std::string &, std::stop_token)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          frozen_object_state_t captured;
          captured.application_state = std::vector<std::uint8_t>{};
          co_return captured;
      },
      [&] (const frozen_object_state_t &, const object_ref_t &,
           std::stop_token) -> zlink::framework::task_t<bool> {
          ++single_restore_count;
          co_return true;
      });
    auto single_frozen = frozen_object_state_t{.owner = {.kind = object_kind_t::user_spot,
                                                         .key = "stale-single",
                                                         .object_generation = 1,
                                                         .authority_owner_generation = 1,
                                                         .mesh_name = "mesh",
                                                         .node_id = "source"},
                                               .stable_type = "spot",
                                               .application_state = {1},
                                               .pending_application = {},
                                               .timers = {}};
    auto single_target = single_frozen.owner;
    single_target.authority_owner_generation = 2;
    single_target.node_id = "target";
    const relocation_restore_identity_t single_identity{"single-root", 1, digest_with (1)};
    test.require (
      single.restore_relocation (single_frozen, single_target, single_identity).result ().value ()
          == stateful_error_t::generation_stale
        && single_restore_count == 0 && single.inventory_async ().result ().value ().empty (),
      "stale single restore must not publish application Spot state");

    single_frozen.owner.object_generation = 2;
    single_target.object_generation = 2;
    const auto fresh_single =
      single.restore_relocation (single_frozen, single_target, single_identity).result ().value ();
    const auto single_inventory = single.inventory_async ().result ().value ();
    test.require (fresh_single == stateful_error_t::none && single_restore_count == 1
                    && single_inventory.size () == 1
                    && single_inventory.front ().owner == single_target,
                  "fresh single retry must succeed after stale validation");

    stateful_object_runtime_t aggregate;
    const auto holder = create_spot (aggregate, object_kind_t::user_spot, "entry:holder");
    const auto old_actor = create_actor (aggregate, "stale-actor");
    const auto [join_error, join] =
      aggregate.begin_membership_move_async (old_actor, holder).result ().value ();
    const auto [commit_error, joined_actor] =
      aggregate.commit_membership_move_async (join).result ().value ();
    test.require (join_error == stateful_error_t::none && commit_error == stateful_error_t::none
                    && aggregate.destroy_actor (joined_actor) == stateful_error_t::none
                    && aggregate.close_spot (holder) == std::pair{stateful_error_t::none, true},
                  "aggregate stale-generation setup must retain Actor history");
    int aggregate_restore_count = 0;
    aggregate.configure_relocation_state (
      [] (const object_ref_t &, const std::string &, std::stop_token)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          frozen_object_state_t captured;
          captured.application_state = std::vector<std::uint8_t>{};
          co_return captured;
      },
      [&] (const frozen_object_state_t &, const object_ref_t &,
           std::stop_token) -> zlink::framework::task_t<bool> {
          ++aggregate_restore_count;
          co_return true;
      });
    std::vector<frozen_object_state_t> participants{{.owner = {.kind = object_kind_t::user_spot,
                                                               .key = "aggregate-spot",
                                                               .object_generation = 1,
                                                               .authority_owner_generation = 1,
                                                               .mesh_name = "mesh",
                                                               .node_id = "source"},
                                                     .stable_type = "spot",
                                                     .application_state = {2},
                                                     .pending_application = {},
                                                     .timers = {}},
                                                    {.owner = {.kind = object_kind_t::actor,
                                                               .key = "stale-actor",
                                                               .object_generation = 1,
                                                               .authority_owner_generation = 1,
                                                               .mesh_name = "mesh",
                                                               .node_id = "source"},
                                                     .stable_type = "actor",
                                                     .application_state = {},
                                                     .pending_application = {},
                                                     .timers = {}}};
    std::vector<object_ref_t> aggregate_targets{participants[0].owner, participants[1].owner};
    for (auto &target : aggregate_targets) {
        target.authority_owner_generation = 2;
        target.node_id = "target";
    }
    const relocation_restore_identity_t aggregate_identity{"aggregate-root", 2, digest_with (2)};
    test.require (
      aggregate.restore_relocation_aggregate (participants, aggregate_targets, aggregate_identity)
            .result ()
            .value ()
          == stateful_error_t::generation_stale
        && aggregate_restore_count == 0 && aggregate.inventory_async ().result ().value ().empty (),
      "one stale Actor must reject the aggregate before Spot publication");

    participants[1].owner.object_generation = 2;
    aggregate_targets[1].object_generation = 2;
    const auto fresh_aggregate =
      aggregate.restore_relocation_aggregate (participants, aggregate_targets, aggregate_identity)
        .result ()
        .value ();
    const auto aggregate_inventory = aggregate.inventory_async ().result ().value ();
    test.require (fresh_aggregate == stateful_error_t::none && aggregate_restore_count == 1
                    && aggregate_inventory.size () == 2
                    && std::any_of (aggregate_inventory.begin (), aggregate_inventory.end (),
                                    [&] (const object_inventory_t &entry) {
                                        return entry.owner == aggregate_targets[0];
                                    })
                    && std::any_of (aggregate_inventory.begin (), aggregate_inventory.end (),
                                    [&] (const object_inventory_t &entry) {
                                        return entry.owner == aggregate_targets[1];
                                    }),
                  "fresh aggregate retry must succeed without leaked state");
}

/* 28 §2/§8 return relocation. A node that hands an object away keeps its
 * record in `moving` forever — `finalize_relocation_cutover` never removes it
 * and no source-cleanup path does either. When the object relocates back, the
 * incoming restore carries a Store-validated authority generation strictly
 * newer than the remnant's, so the remnant must be replaced, never allowed to
 * veto. Failure-first: a restore that fails after replacing the remnant must
 * leave the node clean enough for the next attempt to be admitted on its own
 * (15 §4.2), or the object is wedged off this node permanently. */
void test_return_relocation_replaces_departed_remnant (test_context_t &test)
{
    stateful_object_runtime_t objects;
    bool restore_succeeds = false;
    objects.configure_relocation_state (
      [] (const object_ref_t &, const std::string &, std::stop_token)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          frozen_object_state_t captured;
          captured.application_state = std::vector<std::uint8_t>{};
          co_return captured;
      },
      [&] (const frozen_object_state_t &, const object_ref_t &,
           std::stop_token) -> zlink::framework::task_t<bool> { co_return restore_succeeds; });
    const auto spot = create_spot (objects, object_kind_t::user_spot, "returning-spot");

    auto sealed = await_task (objects.try_seal_relocation_aggregate ({spot}));
    test.require (sealed.error == stateful_error_t::none, "departing Spot must seal");
    test.require (objects.begin_relocation_boundary (sealed.seal.token).first
                    == stateful_error_t::none,
                  "departing Spot must cross the ingress boundary");
    test.require (objects.finalize_relocation_cutover (sealed.seal.token).result ().value ()
                    == stateful_error_t::none,
                  "cutover must finalize and leave the source record behind");
    test.require (objects.inventory_async ().result ().value ().size () == 1,
                  "the departed record stays on the source as a `moving` remnant");

    frozen_object_state_t frozen{.owner = spot,
                                 .stable_type = "spot",
                                 .application_state = {1},
                                 .pending_application = {},
                                 .timers = {}};
    frozen.owner.node_id = "node-b";
    auto returning = spot;
    returning.authority_owner_generation = spot.authority_owner_generation + 2;
    const relocation_restore_identity_t identity{"return-root", 4, digest_with (4)};

    /* A restore that is NOT newer than the remnant is still a real conflict. */
    auto not_newer = returning;
    not_newer.authority_owner_generation = spot.authority_owner_generation;
    auto not_newer_frozen = frozen;
    not_newer_frozen.owner.authority_owner_generation = spot.authority_owner_generation - 1;
    test.require (
      objects.restore_relocation (not_newer_frozen, not_newer, identity).result ().value ()
        == stateful_error_t::conflict,
      "a restore at or behind the remnant's authority must still conflict");

    restore_succeeds = false;
    test.require (objects.restore_relocation (frozen, returning, identity).result ().value ()
                    == stateful_error_t::conflict,
                  "a failing return restore must roll its own staging back");
    restore_succeeds = true;
    const auto retried =
      objects.restore_relocation (frozen, returning, identity).result ().value ();
    const auto returned_inventory = objects.inventory_async ().result ().value ();
    test.require (retried == stateful_error_t::none && returned_inventory.size () == 1
                    && returned_inventory.front ().owner == returning,
                  "the return retry must replace the remnant instead of wedging");
}

void test_pending_restore_holds_ingress_before_rollback (test_context_t &test)
{
    stateful_object_runtime_t target;
    std::mutex callback_mutex;
    std::condition_variable callback_condition;
    bool callback_entered = false;
    bool release_callback = false;
    target.configure_relocation_state (
      [] (const object_ref_t &, const std::string &, std::stop_token)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          frozen_object_state_t captured;
          captured.application_state = std::vector<std::uint8_t>{};
          co_return captured;
      },
      [&] (const frozen_object_state_t &, const object_ref_t &,
           std::stop_token) -> zlink::framework::task_t<bool> {
          std::unique_lock lock (callback_mutex);
          callback_entered = true;
          callback_condition.notify_all ();
          callback_condition.wait (lock, [&] { return release_callback; });
          co_return false;
      });
    const frozen_object_state_t frozen{.owner = {.kind = object_kind_t::user_spot,
                                                 .key = "rollback-spot",
                                                 .object_generation = 1,
                                                 .authority_owner_generation = 1,
                                                 .mesh_name = "mesh",
                                                 .node_id = "source"},
                                       .stable_type = "spot",
                                       .application_state = {1},
                                       .pending_application = {},
                                       .timers = {}};
    auto restored_target = frozen.owner;
    restored_target.authority_owner_generation = 2;
    restored_target.node_id = "target";
    const relocation_restore_identity_t identity{"rollback-root", 3, digest_with (3)};
    stateful_error_t restore_result = stateful_error_t::none;
    std::thread restoring ([&] {
        restore_result =
          target.restore_relocation (frozen, restored_target, identity).result ().value ();
    });
    {
        std::unique_lock lock (callback_mutex);
        callback_condition.wait (lock, [&] { return callback_entered; });
    }
    const auto ingress = target.enqueue (restored_target, turn_domain_t::application, {1, {9}});
    {
        std::lock_guard lock (callback_mutex);
        release_callback = true;
    }
    callback_condition.notify_all ();
    restoring.join ();
    test.require (ingress == stateful_error_t::none && restore_result == stateful_error_t::conflict
                    && target.inventory_async ().result ().value ().empty (),
                  "pending restore must hold ingress until rollback discards it");

    target.configure_relocation_state (
      [] (const object_ref_t &, const std::string &, std::stop_token)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          frozen_object_state_t captured;
          captured.application_state = std::vector<std::uint8_t>{};
          co_return captured;
      },
      [] (const frozen_object_state_t &, const object_ref_t &,
          std::stop_token) -> zlink::framework::task_t<bool> { co_return true; });
    test.require (target.restore_relocation (frozen, restored_target, identity).result ().value ()
                      == stateful_error_t::none
                    && target.inventory_async ().result ().value ().size () == 1,
                  "callback failure rollback must release the fresh retry");
}

} // namespace m6c_test

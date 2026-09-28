/* SPDX-License-Identifier: FSL-1.1-ALv2 */
// MeshNode §5.1 "Pending activation": a relocation target's Restore holds one activation
// admission in the MeshNode's record until the target commit or abort, and the record's limit
// rejects new activations while it is full.

#include "runtime/spots/spot_runtime.hpp"

#include <zlink/framework.hpp>

#include <iostream>
#include <memory>
#include <string>

namespace
{
namespace fw = zlink::framework;
using namespace zlink::framework::runtime::stateful;

class restore_spot_t final : public fw::spot_t<fw::actor_t>
{
  public:
    explicit restore_spot_t (fw::spot_context_t context) : _context (std::move (context)) {}
    fw::spot_context_t &context () noexcept override { return _context; }
    const fw::spot_context_t &context () const noexcept override { return _context; }
    void configure () override {}
    fw::task_t<fw::spot_actor_join_result_t> on_actor_join (std::string_view,
                                                            const fw::message_t &) override
    {
        co_return fw::spot_actor_join_result_t::accept ();
    }
    fw::task_t<void> on_actor_joined (fw::actor_t &) override { co_return; }
    fw::task_t<void> on_leave_actor (fw::actor_t &) override { co_return; }

  private:
    fw::spot_context_t _context;
};

int failures = 0;

void require (bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "[activation-admission] " << message << '\n';
        ++failures;
    }
}

frozen_object_state_t frozen (const std::string &key)
{
    return {.owner = {.kind = object_kind_t::user_spot,
                      .key = key,
                      .object_generation = 1,
                      .authority_owner_generation = 1,
                      .mesh_name = "mesh",
                      .node_id = "source"},
            .stable_type = "restore-spot",
            .application_state = {},
            .pending_application = {},
            .timers = {}};
}

object_ref_t target (const std::string &key)
{
    return {.kind = object_kind_t::user_spot,
            .key = key,
            .object_generation = 1,
            .authority_owner_generation = 2,
            .mesh_name = "mesh",
            .node_id = "target"};
}

void relocation_target_restore_holds_admission_until_commit_or_abort ()
{
    fw::spot_node_builder_t builder;
    builder.add_spot_factory<restore_spot_t> (
      "restore-spot",
      [] (fw::spot_context_t context) {
          return std::make_shared<restore_spot_t> (std::move (context));
      },
      [] (auto &factory) { factory.recreate_on_relocation (); });
    auto runtime = fw::detail::spot_node_runtime_t::from (builder);
    const auto admission = runtime.weak_state ().lock ()->activation_admission;
    admission->set_limit (1);

    require (runtime.restore_spot_relocation_state (frozen ("first"), target ("first")),
             "the first Restore must be admitted");
    require (admission->active () == 1 && !admission->has_headroom (),
             "a restored target holds its admission until the target commit");
    require (!runtime.restore_spot_relocation_state (frozen ("second"), target ("second")),
             "a Restore beyond the limit must be rejected");
    bool creation_rejected = false;
    try {
        (void) runtime.get_or_create_spot ("restore-spot", fw::spot_id_t ("created"));
    }
    catch (const fw::framework_exception_t &error) {
        creation_rejected = error.kind () == fw::framework_error_kind_t::unavailable;
    }
    require (creation_rejected, "a User Spot creation beyond the limit must be unavailable");
    require (admission->active () == 1, "rejected operations must not hold an admission");

    (void) runtime.commit_relocation_materialization ({target ("first")});
    require (admission->active () == 0, "the target commit must end the Restore admission");

    require (runtime.restore_spot_relocation_state (frozen ("second"), target ("second")),
             "a Restore must be admitted again after the commit");
    require (admission->active () == 1, "the second Restore must hold one admission");
    runtime.abort_relocation_materialization ({target ("second")});
    require (admission->active () == 0, "the target abort must end the Restore admission");

    auto unknown = frozen ("unknown");
    unknown.stable_type = "unregistered-spot";
    require (!runtime.restore_spot_relocation_state (unknown, target ("unknown")),
             "a Restore of an unregistered type must fail");
    require (admission->active () == 0, "a failed Restore must end its admission");

    runtime.request_stop ();
    runtime.cancel_pending_dispatch ();
    runtime.cancel_pending_work ();
    runtime.release_native_handles ();
}

} // namespace

int main ()
{
    relocation_target_restore_holds_admission_until_commit_or_abort ();
    if (failures == 0)
        std::cout << "[activation-admission] passed\n";
    return failures == 0 ? 0 : 1;
}

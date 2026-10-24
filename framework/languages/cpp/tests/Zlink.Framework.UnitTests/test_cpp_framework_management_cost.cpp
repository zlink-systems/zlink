/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "allocation_counter.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime_failure_fixture.hpp"
#include <future>
#include <iostream>
#include <stdexcept>

namespace
{
using namespace zlink::framework;
void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}

void verify_pending_management ()
{
    auto state = test::runtime_failure_fixture<detail::spot_node_builder_state_t> ("pending-cost");
    std::promise<void> entered, release;
    auto released = release.get_future ().share ();
    require (state->lane.try_post ([&] {
        entered.set_value ();
        released.wait ();
    }),
             "management fixture lane rejected work");
    entered.get_future ().wait ();
    auto pending = [&] {
        try {
            return detail::spot_node_runtime_t (state).advance_management_async ();
        }
        catch (...) {
            release.set_value ();
            throw;
        }
    }();
    const bool ready_before_release = pending.await_ready ();
    release.set_value ();
    const auto next = pending.result ().value ();
    require (!ready_before_release && !next,
             "due management bypassed its lane or lost its terminal result");
}

void verify_ready_management_cost ()
{
    auto state = test::runtime_failure_fixture<detail::spot_node_builder_state_t> ("ready-cost");
    detail::spot_node_runtime_t runtime (state);
    require (!runtime.advance_management_async ().result ().value (),
             "empty management warmup reported a deadline");
    using activity_t = std::optional<std::chrono::steady_clock::time_point>;
    std::optional<task_t<activity_t>> ready_result;
    // The existing task API always owns a shared result, including ready values.
    // Calibrate that required object and reject every allocation beyond it.
    const auto baseline = tests::measure_allocations (
      [&] { ready_result.emplace (result_t<activity_t>::success (std::nullopt)); });
    bool within_budget = true;
    for (bool include_next : {false, true}) {
        std::optional<task_t<std::optional<std::chrono::steady_clock::time_point>>> pending;
        const auto cost = tests::measure_allocations (
          [&] { pending.emplace (runtime.advance_management_async (include_next)); });
        require (pending->await_ready () && !pending->result ().value (),
                 "management with no due work did not return its ready result");
        std::cout << "management include-next=" << include_next << " count=" << cost.count
                  << " bytes=" << cost.bytes << " baseline-count=" << baseline.count
                  << " baseline-bytes=" << baseline.bytes << '\n';
        within_budget =
          within_budget && cost.count <= baseline.count && cost.bytes <= baseline.bytes;
    }
    require (within_budget, "management with no due work allocates beyond its result object");
    const auto injected = tests::measure_allocations ([&] {
        (void) runtime.advance_management_async ().result ().value ();
        void *(*volatile allocate) (std::size_t) = &::operator new;
        auto *memory = allocate (64);
        ::operator delete (memory);
    });
    require (injected.count > baseline.count && injected.bytes > baseline.bytes,
             "management cost check did not detect an extra allocation");
}

void verify_published_deadline ()
{
    auto state = test::runtime_failure_fixture<detail::spot_node_builder_state_t> ("deadline-cost");
    detail::spot_node_runtime_t runtime (state);
    const auto now = std::chrono::steady_clock::now ();
    const auto deadline =
      now + detail::spot_node_builder_state_t::pending_handoff_request_t::default_timeout;
    state->lane
      .run ([&] {
          detail::spot_node_builder_state_t::pending_handoff_request_t request{
            .actor = detail::actor_ref_access_t::make (node_rid_t::from_string ("n"), "a", "id", 1),
            .deadline = deadline};
          state->pending_handoff_requests.emplace (
            detail::spot_node_builder_state_t::pending_handoff_request_key_t{},
            std::move (request));
      })
      .get ();
    (void) runtime.cleanup_expired_actor_admissions_at (now);
    const auto next = runtime.advance_management_async ().result ().value ();
    require (next && *next == deadline, "management lost its published handoff deadline");
    state->lane.run ([&] { state->pending_handoff_requests.clear (); }).get ();
}
}

int main ()
{
    zlink::framework::completion_owner_t owner;
    try {
        verify_pending_management ();
        verify_published_deadline ();
        verify_ready_management_cost ();
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}

/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil_unity.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
//  Every field the context plan owns. The attach extension must produce the
//  same plan a full recalculation of the same topology produces.
//  `extension_expected_` says whether this attach could take the extension:
//  only then may the applied total stay above the planned total, which is the
//  deferral 06-auto-hwm.ko.md §2 grants the attaching path. A fallback attach
//  ran the full planner and must match in every field.
void assert_plan_equal (const zlink_auto_hwm_budget_snapshot_t &attached_,
                        const zlink_auto_hwm_budget_snapshot_t &replanned_,
                        bool extension_expected_, const char *what_)
{
    UNITY_TEST_ASSERT_EQUAL_UINT64 (replanned_.configured_memory_limit_bytes,
                                    attached_.configured_memory_limit_bytes,
                                    __LINE__, what_);
    UNITY_TEST_ASSERT_EQUAL_UINT64 (replanned_.runtime_memory_limit_bytes,
                                    attached_.runtime_memory_limit_bytes,
                                    __LINE__, what_);
    UNITY_TEST_ASSERT_EQUAL_UINT64 (replanned_.resolved_memory_limit_bytes,
                                    attached_.resolved_memory_limit_bytes,
                                    __LINE__, what_);
    UNITY_TEST_ASSERT_EQUAL_UINT64 (replanned_.configured_core_budget_bytes,
                                    attached_.configured_core_budget_bytes,
                                    __LINE__, what_);
    UNITY_TEST_ASSERT_EQUAL_UINT64 (replanned_.effective_core_budget_bytes,
                                    attached_.effective_core_budget_bytes,
                                    __LINE__, what_);
    UNITY_TEST_ASSERT_EQUAL_UINT64 (replanned_.total_planned_hwm_bytes,
                                    attached_.total_planned_hwm_bytes,
                                    __LINE__, what_);
    UNITY_TEST_ASSERT_EQUAL_UINT64 (replanned_.manual_reserved_hwm_bytes,
                                    attached_.manual_reserved_hwm_bytes,
                                    __LINE__, what_);
    UNITY_TEST_ASSERT_EQUAL_UINT64 (replanned_.active_directional_queue_count,
                                    attached_.active_directional_queue_count,
                                    __LINE__, what_);
    UNITY_TEST_ASSERT_EQUAL_UINT64 (replanned_.active_send_queue_count,
                                    attached_.active_send_queue_count,
                                    __LINE__, what_);
    UNITY_TEST_ASSERT_EQUAL_UINT64 (replanned_.active_receive_queue_count,
                                    attached_.active_receive_queue_count,
                                    __LINE__, what_);
    UNITY_TEST_ASSERT_EQUAL_UINT64 (replanned_.unlimited_manual_queue_count,
                                    attached_.unlimited_manual_queue_count,
                                    __LINE__, what_);
    UNITY_TEST_ASSERT_EQUAL_UINT32 (replanned_.flags, attached_.flags,
                                    __LINE__, what_);
    if (extension_expected_) {
        UNITY_TEST_ASSERT (attached_.total_applied_hwm_bytes
                             >= replanned_.total_applied_hwm_bytes,
                           __LINE__, what_);
    } else {
        UNITY_TEST_ASSERT_EQUAL_UINT64 (replanned_.total_applied_hwm_bytes,
                                        attached_.total_applied_hwm_bytes,
                                        __LINE__, what_);
    }
    //  Every queue here is empty, so a full pass always leaves the published
    //  targets equal to the plan.
    UNITY_TEST_ASSERT_EQUAL_UINT64 (replanned_.total_planned_hwm_bytes,
                                    replanned_.total_applied_hwm_bytes,
                                    __LINE__, what_);
}

void configure (void *ctx_, uint64_t budget_, int debounce_ms_)
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set (ctx_, ZLINK_CTX_OPT_AUTO_HWM_PROFILE,
                     ZLINK_AUTO_HWM_PROFILE_BALANCED));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set (ctx_, ZLINK_CTX_OPT_AUTO_HWM_RECALC_DEBOUNCE_MS,
                     debounce_ms_));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set_data (ctx_, ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES,
                          &budget_, sizeof (budget_)));
}

std::string endpoint_for (const char *prefix_, size_t index_)
{
    return std::string ("inproc://") + prefix_ + "-"
           + std::to_string (index_);
}

//  Creating a socket makes a planning request of its own, so an attach right
//  after it always falls back. Serving that request first is what puts the
//  next attach on the extension.
void settle (void *ctx_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_auto_hwm_recalculate (ctx_));
}

//  One pending connect publishes one staged application pipe and therefore
//  adds two directions. Returns the snapshot the attach left behind, having
//  checked that the attach recorded exactly one plan.
zlink_auto_hwm_budget_snapshot_t attach_pending_connect (void *ctx_,
                                                         void *socket_,
                                                         const std::string &endpoint_)
{
    const uint64_t before =
      read_auto_hwm_budget_snapshot (ctx_).budget_generation;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (socket_, endpoint_.c_str ()));
    const zlink_auto_hwm_budget_snapshot_t attached =
      read_auto_hwm_budget_snapshot (ctx_);
    //  Extension or fallback, an attach records one plan and only one.
    TEST_ASSERT_EQUAL_UINT64 (before + 1, attached.budget_generation);
    return attached;
}

//  Attaches `connects_` pending connects, each from a socket whose creation
//  request has already been served, so every attach after the first can take
//  the extension. Compares what each attach recorded with what a full
//  recalculation of the same topology records.
void compare_settled_attach_steps (void *ctx_, int socket_type_,
                                   const char *prefix_, size_t connects_,
                                   bool extension_expected_)
{
    std::vector<void *> sockets;
    for (size_t i = 0; i != connects_; ++i) {
        void *socket = test_context_socket (socket_type_);
        sockets.push_back (socket);
        settle (ctx_);
        const bool plan_has_automatic_directions =
          read_auto_hwm_budget_snapshot (ctx_).active_directional_queue_count
          != 0;
        const zlink_auto_hwm_budget_snapshot_t attached =
          attach_pending_connect (ctx_, socket, endpoint_for (prefix_, i));
        settle (ctx_);
        //  An empty plan has no water level to extend, so the first attach of
        //  a topology always falls back and must match in every field.
        assert_plan_equal (attached, read_auto_hwm_budget_snapshot (ctx_),
                           extension_expected_
                             && plan_has_automatic_directions,
                           prefix_);
    }
    for (size_t i = 0; i != sockets.size (); ++i)
        test_context_socket_close_zero_linger (sockets[i]);
}
}

//  Unsaturated water level: the budget cannot give every direction its role
//  maximum, so the level and the queue-ID remainder move on every attach.
//  From the second attach on, the extension leaves the directions it did not
//  visit above the new plan — that gap is the observable proof it ran.
void test_extension_runs_and_matches_full_recalculation_unsaturated ()
{
    void *ctx = get_test_context ();
    configure (ctx, 1024ull * 1024ull + 3ull, 3000);
    std::vector<void *> sockets;
    for (size_t i = 0; i != 5; ++i) {
        void *socket = test_context_socket (ZLINK_SOCKET_DEALER);
        sockets.push_back (socket);
        settle (ctx);
        const zlink_auto_hwm_budget_snapshot_t attached =
          attach_pending_connect (
            ctx, socket, endpoint_for ("auto-hwm-extension-unsaturated", i));
        //  A fallback attach replans every direction and can never leave the
        //  applied total above the planned one, so this inequality is only
        //  reachable through the extension. Require it on every attach that
        //  had a water level to extend — the first has none.
        if (i != 0) {
            TEST_ASSERT_GREATER_THAN_UINT64 (
              attached.total_planned_hwm_bytes,
              attached.total_applied_hwm_bytes);
        } else {
            TEST_ASSERT_EQUAL_UINT64 (attached.total_planned_hwm_bytes,
                                      attached.total_applied_hwm_bytes);
        }
        settle (ctx);
        assert_plan_equal (attached, read_auto_hwm_budget_snapshot (ctx),
                           i != 0, "unsaturated");
    }
    for (size_t i = 0; i != sockets.size (); ++i)
        test_context_socket_close_zero_linger (sockets[i]);
}

//  Starts saturated at the role maximum and crosses into the unsaturated
//  regime as directions accumulate.
void test_extension_matches_full_recalculation_across_saturation ()
{
    void *ctx = get_test_context ();
    configure (ctx, 4ull * 1024ull * 1024ull + 7ull, 3000);
    compare_settled_attach_steps (ctx, ZLINK_SOCKET_DEALER,
                                  "auto-hwm-extension-saturation", 6, true);
}

//  A finite manual reservation stays out of the automatic level but inside the
//  budget the level divides.
void test_extension_matches_full_recalculation_with_finite_manual ()
{
    void *ctx = get_test_context ();
    configure (ctx, 2ull * 1024ull * 1024ull + 5ull, 3000);
    void *manual = test_context_socket (ZLINK_SOCKET_DEALER);
    const uint64_t manual_hwm = 128ull * 1024ull;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      manual, ZLINK_OPT_SNDHWM, &manual_hwm, sizeof (manual_hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (manual, "inproc://auto-hwm-extension-manual-anchor"));
    compare_settled_attach_steps (ctx, ZLINK_SOCKET_DEALER,
                                  "auto-hwm-extension-manual", 4, true);
    test_context_socket_close_zero_linger (manual);
}

//  An unlimited manual direction makes the aggregate HWM invalid, so the
//  extension must refuse the plan. The fallback is then exact in every field,
//  applied total included.
void test_unlimited_manual_falls_back_to_full_recalculation ()
{
    void *ctx = get_test_context ();
    configure (ctx, 2ull * 1024ull * 1024ull + 11ull, 3000);
    void *unlimited = test_context_socket (ZLINK_SOCKET_DEALER);
    const uint64_t unlimited_hwm = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      unlimited, ZLINK_OPT_SNDHWM, &unlimited_hwm, sizeof (unlimited_hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (unlimited, "inproc://auto-hwm-extension-unlimited-anchor"));
    settle (ctx);
    TEST_ASSERT_GREATER_THAN_UINT64 (
      0, read_auto_hwm_budget_snapshot (ctx).unlimited_manual_queue_count);

    compare_settled_attach_steps (ctx, ZLINK_SOCKET_DEALER,
                                  "auto-hwm-extension-unlimited", 3, false);
    test_context_socket_close_zero_linger (unlimited);
}

//  ROUTER and DEALER directions resolve to different roles, so the automatic
//  directions no longer share one water level and the extension must refuse.
void test_mixed_roles_fall_back_to_full_recalculation ()
{
    void *ctx = get_test_context ();
    configure (ctx, 3ull * 1024ull * 1024ull + 13ull, 3000);
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (router, "inproc://auto-hwm-extension-role-anchor"));
    compare_settled_attach_steps (ctx, ZLINK_SOCKET_DEALER,
                                  "auto-hwm-extension-role", 4, false);
    test_context_socket_close_zero_linger (router);
}

//  A completed inproc pair attaches its second endpoint to directions the plan
//  already covers. That endpoint cannot be told apart from an unplanned one in
//  O(1), so it falls back — and the plan it records must be exact.
void test_completed_inproc_pair_falls_back_to_full_recalculation ()
{
    void *ctx = get_test_context ();
    configure (ctx, 2ull * 1024ull * 1024ull + 17ull, 3000);
    std::vector<void *> sockets;
    for (size_t i = 0; i != 4; ++i) {
        void *server = test_context_socket (ZLINK_SOCKET_PAIR);
        void *client = test_context_socket (ZLINK_SOCKET_PAIR);
        sockets.push_back (server);
        sockets.push_back (client);
        const std::string endpoint =
          endpoint_for ("auto-hwm-extension-pair", i);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, endpoint.c_str ()));
        settle (ctx);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                               zlink_connect (client, endpoint.c_str ()));
        //  The bound peer attaches its own endpoint when it next runs its
        //  mailbox. Drive that here so the second attach — the one that has
        //  to fall back — is the last plan this iteration records.
        char drained[1];
        TEST_ASSERT_FAILURE_ERRNO (
          EAGAIN,
          zlink_recv (server, drained, sizeof (drained), ZLINK_DONTWAIT));
        const zlink_auto_hwm_budget_snapshot_t attached =
          read_auto_hwm_budget_snapshot (ctx);
        settle (ctx);
        assert_plan_equal (attached, read_auto_hwm_budget_snapshot (ctx),
                           false, "completed inproc pair");
    }
    for (size_t i = 0; i != sockets.size (); ++i)
        test_context_socket_close_zero_linger (sockets[i]);
}

//  The extension defers lowering the directions it did not visit to the same
//  debounce an option change uses. That replan must run on its own, with no
//  further API call to trigger it.
void test_extension_converges_on_the_debounce_without_further_calls ()
{
    void *ctx = get_test_context ();
    const int debounce_ms = 100;
    configure (ctx, 1024ull * 1024ull + 3ull, debounce_ms);
    std::vector<void *> sockets;
    for (size_t i = 0; i != 5; ++i)
        sockets.push_back (test_context_socket (ZLINK_SOCKET_DEALER));
    settle (ctx);
    const uint64_t before =
      read_auto_hwm_budget_snapshot (ctx).budget_generation;
    for (size_t i = 0; i != sockets.size (); ++i) {
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONNECT_OK,
          zlink_connect (sockets[i],
                         endpoint_for ("auto-hwm-extension-converge", i)
                           .c_str ()));
    }

    const zlink_auto_hwm_budget_snapshot_t deferred =
      read_auto_hwm_budget_snapshot (ctx);
    //  One recorded plan per attach and nothing else: the whole burst joined
    //  one wait, so it woke the recalculation task once.
    TEST_ASSERT_EQUAL_UINT64 (before + sockets.size (),
                              deferred.budget_generation);
    //  The extension ran and left the directions it did not visit above the
    //  plan. Without this the convergence below would be vacuous.
    TEST_ASSERT_GREATER_THAN_UINT64 (deferred.total_planned_hwm_bytes,
                                     deferred.total_applied_hwm_bytes);

    //  Nothing below calls into planning: a snapshot neither replans nor
    //  schedules. Only the debounce those attaches armed can close the gap.
    zlink_auto_hwm_budget_snapshot_t settled = deferred;
    for (int waited_ms = 0;
         waited_ms < 5000
         && settled.total_applied_hwm_bytes != settled.total_planned_hwm_bytes;
         waited_ms += 20) {
        std::this_thread::sleep_for (std::chrono::milliseconds (20));
        settled = read_auto_hwm_budget_snapshot (ctx);
    }
    TEST_ASSERT_EQUAL_UINT64 (settled.total_planned_hwm_bytes,
                              settled.total_applied_hwm_bytes);
    TEST_ASSERT_EQUAL_UINT64 (10, settled.active_directional_queue_count);
    //  Exactly one convergence pass for the whole burst: five attaches armed
    //  the wait once between them.
    TEST_ASSERT_EQUAL_UINT64 (before + sockets.size () + 1,
                              settled.budget_generation);

    //  And it does not repeat: the pass released the wait, so no later tick
    //  records another plan.
    std::this_thread::sleep_for (std::chrono::milliseconds (5 * debounce_ms));
    TEST_ASSERT_EQUAL_UINT64 (
      settled.budget_generation,
      read_auto_hwm_budget_snapshot (ctx).budget_generation);

    for (size_t i = 0; i != sockets.size (); ++i)
        test_context_socket_close_zero_linger (sockets[i]);
}

//  06-auto-hwm.ko.md §4 lets a queue that holds more than its new target keep
//  the higher published limit until it drains, so a plan can report an applied
//  total above its planned total with no planning work outstanding. That state
//  must not be read as an obligation to replan, or the pass would repeat for
//  as long as the queue stays full.
void test_deferred_shrink_does_not_keep_replanning ()
{
    void *ctx = get_test_context ();
    const int debounce_ms = 100;
    configure (ctx, 1024ull * 1024ull + 3ull, debounce_ms);
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (receiver, "inproc://auto-hwm-deferred-shrink"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (sender, "inproc://auto-hwm-deferred-shrink"));
    settle (ctx);

    //  Fill the queue past what the level will become once more directions
    //  divide the same budget.
    const size_t message_size = 4096;
    std::vector<char> payload (message_size, 'x');
    int queued = 0;
    while (queued < 4096
           && zlink_send (sender, payload.data (), payload.size (),
                          ZLINK_DONTWAIT)
                == static_cast<int> (payload.size ()))
        ++queued;
    TEST_ASSERT_GREATER_THAN_INT (0, queued);

    //  More directions lower every target; the full queue cannot follow.
    std::vector<void *> extra;
    for (size_t i = 0; i != 2; ++i) {
        void *socket = test_context_socket (ZLINK_SOCKET_PAIR);
        extra.push_back (socket);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONNECT_OK,
          zlink_connect (socket,
                         endpoint_for ("auto-hwm-deferred-shrink-extra", i)
                           .c_str ()));
    }
    settle (ctx);

    const zlink_auto_hwm_budget_snapshot_t shrinking =
      read_auto_hwm_budget_snapshot (ctx);
    //  A full pass has just run and the gap is the queue's, not the planner's.
    TEST_ASSERT_GREATER_THAN_UINT64 (shrinking.total_planned_hwm_bytes,
                                     shrinking.total_applied_hwm_bytes);

    //  No further plan may be recorded: nothing is owed.
    std::this_thread::sleep_for (std::chrono::milliseconds (6 * debounce_ms));
    TEST_ASSERT_EQUAL_UINT64 (
      shrinking.budget_generation,
      read_auto_hwm_budget_snapshot (ctx).budget_generation);

    for (size_t i = 0; i != extra.size (); ++i)
        test_context_socket_close_zero_linger (extra[i]);
    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

//  A planning request made between two attaches — a socket created, an option
//  changed, a pipe detached — must put the next attach back on the full pass.
void test_planning_request_between_attaches_forces_full_recalculation ()
{
    void *ctx = get_test_context ();
    configure (ctx, 1024ull * 1024ull + 3ull, 3000);
    std::vector<void *> sockets;
    for (size_t i = 0; i != 3; ++i)
        sockets.push_back (test_context_socket (ZLINK_SOCKET_DEALER));
    settle (ctx);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (sockets[0],
                     endpoint_for ("auto-hwm-extension-request", 0).c_str ()));

    //  Creating a socket is a planning request; the attach that follows must
    //  replan in full and therefore leave applied equal to planned.
    void *created = test_context_socket (ZLINK_SOCKET_DEALER);
    const zlink_auto_hwm_budget_snapshot_t after_create =
      attach_pending_connect (
        ctx, sockets[1], endpoint_for ("auto-hwm-extension-request", 1));
    TEST_ASSERT_EQUAL_UINT64 (after_create.total_planned_hwm_bytes,
                              after_create.total_applied_hwm_bytes);

    //  A manual HWM is a planning request too.
    settle (ctx);
    const uint64_t manual_hwm = 96ull * 1024ull;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      created, ZLINK_OPT_SNDHWM, &manual_hwm, sizeof (manual_hwm)));
    const zlink_auto_hwm_budget_snapshot_t after_option =
      attach_pending_connect (
        ctx, sockets[2], endpoint_for ("auto-hwm-extension-request", 2));
    TEST_ASSERT_EQUAL_UINT64 (after_option.total_planned_hwm_bytes,
                              after_option.total_applied_hwm_bytes);

    test_context_socket_close_zero_linger (created);
    for (size_t i = 0; i != sockets.size (); ++i)
        test_context_socket_close_zero_linger (sockets[i]);
}

//  Concurrent attaches serialise on the recalculation mutex. With every
//  creation request served first and both threads released from one barrier,
//  each attach records exactly one plan and none is lost.
void test_concurrent_attach_records_one_plan_per_attach ()
{
    void *ctx = get_test_context ();
    configure (ctx, 2ull * 1024ull * 1024ull + 19ull, 3000);
    const size_t per_thread = 4;
    std::vector<void *> sockets;
    for (size_t i = 0; i != per_thread * 2; ++i)
        sockets.push_back (test_context_socket (ZLINK_SOCKET_DEALER));
    settle (ctx);
    const uint64_t before =
      read_auto_hwm_budget_snapshot (ctx).budget_generation;

    std::atomic<int> ready (0);
    std::atomic<bool> go (false);
    const auto connect_range = [&] (size_t first_) {
        //  Both workers announce themselves and then wait for the same
        //  release, so neither can finish its attaches before the other has
        //  started its first.
        ready.fetch_add (1);
        while (!go.load ())
            std::this_thread::yield ();
        for (size_t i = first_; i != first_ + per_thread; ++i) {
            if (zlink_connect (
                  sockets[i],
                  endpoint_for ("auto-hwm-extension-concurrent", i).c_str ())
                != ZLINK_CONNECT_OK)
                return false;
        }
        return true;
    };
    std::future<bool> first =
      std::async (std::launch::async, connect_range, 0);
    std::future<bool> second =
      std::async (std::launch::async, connect_range, per_thread);
    while (ready.load () < 2)
        std::this_thread::yield ();
    go.store (true);
    TEST_ASSERT_TRUE (first.get ());
    TEST_ASSERT_TRUE (second.get ());

    const zlink_auto_hwm_budget_snapshot_t attached =
      read_auto_hwm_budget_snapshot (ctx);
    //  Exactly one recorded plan per attach: the debounce is 3 s and no other
    //  planning request was made, so nothing else could publish.
    TEST_ASSERT_EQUAL_UINT64 (before + per_thread * 2,
                              attached.budget_generation);
    TEST_ASSERT_EQUAL_UINT64 (per_thread * 2 * 2,
                              attached.active_directional_queue_count);
    settle (ctx);
    assert_plan_equal (attached, read_auto_hwm_budget_snapshot (ctx), true,
                       "concurrent attach");

    for (size_t i = 0; i != sockets.size (); ++i)
        test_context_socket_close_zero_linger (sockets[i]);
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_extension_runs_and_matches_full_recalculation_unsaturated);
    RUN_TEST (test_extension_matches_full_recalculation_across_saturation);
    RUN_TEST (test_extension_matches_full_recalculation_with_finite_manual);
    RUN_TEST (test_unlimited_manual_falls_back_to_full_recalculation);
    RUN_TEST (test_mixed_roles_fall_back_to_full_recalculation);
    RUN_TEST (test_completed_inproc_pair_falls_back_to_full_recalculation);
    RUN_TEST (test_extension_converges_on_the_debounce_without_further_calls);
    RUN_TEST (test_deferred_shrink_does_not_keep_replanning);
    RUN_TEST (test_planning_request_between_attaches_forces_full_recalculation);
    RUN_TEST (test_concurrent_attach_records_one_plan_per_attach);
    return UNITY_END ();
}

/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil_unity.hpp"

#include "core/auto_hwm_policy.hpp"
#include "core/ctx_physical_queue_registry.hpp"

#include <atomic>
#include <thread>
#include <unity.h>
#include <vector>

void setUp ()
{
}

void tearDown ()
{
}

void test_budget_input_priority_and_profile_ratio ()
{
    zlink::auto_hwm_budget_input_t input;
    input.enabled = true;
    input.profile = ZLINK_AUTO_HWM_PROFILE_BALANCED;
    input.detected_physical_memory_bytes = 2000;
    input.detected_hard_limit_bytes = 800;
    input.runtime_memory_limit_bytes = 600;

    zlink::auto_hwm_context_plan_t plan;
    zlink::auto_hwm_context_plan_make (input, &plan);
    TEST_ASSERT_EQUAL_UINT64 (600, plan.resolved_memory_limit_bytes);
    //  Balanced is 5%; the fixed cap (512 MiB) is far above the share.
    TEST_ASSERT_EQUAL_UINT64 (30, plan.effective_core_budget_bytes);

    input.configured_memory_limit_bytes = 700;
    zlink::auto_hwm_context_plan_make (input, &plan);
    TEST_ASSERT_EQUAL_UINT64 (700, plan.resolved_memory_limit_bytes);
    TEST_ASSERT_EQUAL_UINT64 (35, plan.effective_core_budget_bytes);

    input.configured_core_budget_bytes = 333;
    zlink::auto_hwm_context_plan_make (input, &plan);
    TEST_ASSERT_EQUAL_UINT64 (333, plan.effective_core_budget_bytes);
}

void test_profile_percent_and_fixed_caps ()
{
    const uint64_t mib = 1024ull * 1024ull;

    TEST_ASSERT_EQUAL_UINT64 (
      2, zlink::auto_hwm_profile_percent (ZLINK_AUTO_HWM_PROFILE_COMPACT));
    TEST_ASSERT_EQUAL_UINT64 (
      3, zlink::auto_hwm_profile_percent (ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY));
    TEST_ASSERT_EQUAL_UINT64 (
      5, zlink::auto_hwm_profile_percent (ZLINK_AUTO_HWM_PROFILE_BALANCED));
    TEST_ASSERT_EQUAL_UINT64 (
      8, zlink::auto_hwm_profile_percent (ZLINK_AUTO_HWM_PROFILE_THROUGHPUT));

    TEST_ASSERT_EQUAL_UINT64 (
      64 * mib,
      zlink::auto_hwm_profile_fixed_cap_bytes (
        ZLINK_AUTO_HWM_PROFILE_COMPACT));
    TEST_ASSERT_EQUAL_UINT64 (
      256 * mib,
      zlink::auto_hwm_profile_fixed_cap_bytes (
        ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY));
    TEST_ASSERT_EQUAL_UINT64 (
      512 * mib,
      zlink::auto_hwm_profile_fixed_cap_bytes (
        ZLINK_AUTO_HWM_PROFILE_BALANCED));
    TEST_ASSERT_EQUAL_UINT64 (
      1024 * mib,
      zlink::auto_hwm_profile_fixed_cap_bytes (
        ZLINK_AUTO_HWM_PROFILE_THROUGHPUT));
}

void test_effective_budget_is_percent_clamped_by_cap_and_queue_floor ()
{
    const uint64_t mib = 1024ull * 1024ull;
    const uint64_t gib = 1024ull * mib;

    //  Small host: the percent share is well under the fixed cap and wins.
    TEST_ASSERT_EQUAL_UINT64 (
      50 * mib,
      zlink::auto_hwm_effective_budget_bytes (
        ZLINK_AUTO_HWM_PROFILE_BALANCED, 1000 * mib, 4));

    //  Large host: 5% of 64 GiB is 3.2 GiB, so the 512 MiB fixed cap clamps.
    TEST_ASSERT_EQUAL_UINT64 (
      512 * mib,
      zlink::auto_hwm_effective_budget_bytes (
        ZLINK_AUTO_HWM_PROFILE_BALANCED, 64 * gib, 4));

    //  Many queues raise the effective cap to the floor they need:
    //  16384 * 64 KiB = 1 GiB > the 512 MiB fixed cap, and 5% of 64 GiB
    //  (3.2 GiB) still exceeds that, so the queue floor becomes the budget.
    TEST_ASSERT_EQUAL_UINT64 (
      1 * gib,
      zlink::auto_hwm_effective_budget_bytes (
        ZLINK_AUTO_HWM_PROFILE_BALANCED, 64 * gib, 16384));

    //  The queue floor never lifts the budget above the percent share.
    TEST_ASSERT_EQUAL_UINT64 (
      512 * mib,
      zlink::auto_hwm_effective_budget_bytes (
        ZLINK_AUTO_HWM_PROFILE_BALANCED, 10 * gib, 16384));
}

void test_finalize_recomputes_budget_from_queue_count ()
{
    const uint64_t kib = 1024ull;
    const uint64_t mib = 1024ull * kib;
    const uint64_t gib = 1024ull * mib;

    zlink::auto_hwm_budget_input_t input;
    input.enabled = true;
    input.profile = ZLINK_AUTO_HWM_PROFILE_BALANCED;
    input.configured_memory_limit_bytes = 64 * gib;

    zlink::auto_hwm_context_plan_t context;
    zlink::auto_hwm_context_plan_make (input, &context);
    //  plan_make cannot know the queue count yet: the fixed cap applies.
    TEST_ASSERT_EQUAL_UINT64 (512 * mib, context.effective_core_budget_bytes);

    //  16384 directional queues need 16384 * 64 KiB = 1 GiB of role minima,
    //  which raises the effective cap above the fixed cap at finalize time.
    std::vector<zlink::auto_hwm_socket_plan_t> plans (8192);
    for (size_t i = 0; i != plans.size (); ++i)
        zlink::auto_hwm_socket_plan_prepare (zlink::auto_hwm_role_routed, 1, 1,
                                             false, 0, false, 0, true,
                                             &plans[i]);
    zlink::auto_hwm_context_finalize (&context, &plans[0], plans.size ());

    TEST_ASSERT_EQUAL_UINT64 (16384, context.active_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (1 * gib, context.effective_core_budget_bytes);
    TEST_ASSERT_FALSE (context.budget_insufficient);
    //  Every queue sits exactly on its 64 KiB role minimum.
    TEST_ASSERT_EQUAL_UINT64 (64 * kib, plans[0].sndhwm);
    TEST_ASSERT_EQUAL_UINT64 (1 * gib, context.total_planned_hwm_bytes);
}

void test_profile_byte_boundaries ()
{
    TEST_ASSERT_EQUAL_UINT64 (
      32ull * 1024ull,
      zlink::auto_hwm_profile_minimum_bytes (
        ZLINK_AUTO_HWM_PROFILE_COMPACT, zlink::auto_hwm_role_routed));
    TEST_ASSERT_EQUAL_UINT64 (
      512ull * 1024ull,
      zlink::auto_hwm_profile_maximum_bytes (
        ZLINK_AUTO_HWM_PROFILE_COMPACT, zlink::auto_hwm_role_routed));
    TEST_ASSERT_EQUAL_UINT64 (
      32ull * 1024ull,
      zlink::auto_hwm_profile_minimum_bytes (
        ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY, zlink::auto_hwm_role_routed));
    TEST_ASSERT_EQUAL_UINT64 (
      2ull * 1024ull * 1024ull,
      zlink::auto_hwm_profile_maximum_bytes (
        ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY, zlink::auto_hwm_role_routed));
    TEST_ASSERT_EQUAL_UINT64 (
      8ull * 1024ull * 1024ull,
      zlink::auto_hwm_profile_maximum_bytes (
        ZLINK_AUTO_HWM_PROFILE_THROUGHPUT, zlink::auto_hwm_role_routed));
    TEST_ASSERT_EQUAL_UINT64 (
      64ull * 1024ull,
      zlink::auto_hwm_profile_minimum_bytes (
        ZLINK_AUTO_HWM_PROFILE_BALANCED, zlink::auto_hwm_role_stream));
    TEST_ASSERT_EQUAL_UINT64 (
      1024ull * 1024ull,
      zlink::auto_hwm_profile_maximum_bytes (
        ZLINK_AUTO_HWM_PROFILE_BALANCED, zlink::auto_hwm_role_routed));
    TEST_ASSERT_EQUAL_UINT64 (
      1ull * 1024ull * 1024ull,
      zlink::auto_hwm_profile_maximum_bytes (
        ZLINK_AUTO_HWM_PROFILE_BALANCED, zlink::auto_hwm_role_fanout));
    TEST_ASSERT_EQUAL_UINT64 (
      2ull * 1024ull * 1024ull,
      zlink::auto_hwm_profile_maximum_bytes (
        ZLINK_AUTO_HWM_PROFILE_BALANCED,
        zlink::auto_hwm_role_recv_ingress));
    TEST_ASSERT_EQUAL_UINT64 (
      512ull * 1024ull,
      zlink::auto_hwm_profile_maximum_bytes (
        ZLINK_AUTO_HWM_PROFILE_THROUGHPUT, zlink::auto_hwm_role_stream));
}

void test_mixed_queue_water_filling_respects_budget_and_caps ()
{
    zlink::auto_hwm_budget_input_t input;
    input.enabled = true;
    input.profile = ZLINK_AUTO_HWM_PROFILE_BALANCED;
    input.configured_core_budget_bytes = 512ull * 1024ull;

    zlink::auto_hwm_context_plan_t context;
    zlink::auto_hwm_context_plan_make (input, &context);

    zlink::auto_hwm_socket_plan_t plans[2];
    zlink::auto_hwm_socket_plan_prepare (
      zlink::auto_hwm_role_routed, 1, 1, false, 0, false, 0, true,
      &plans[0]);
    zlink::auto_hwm_socket_plan_prepare (
      zlink::auto_hwm_role_stream, 1, 1, false, 0, false, 0, true,
      &plans[1]);
    zlink::auto_hwm_context_finalize (&context, plans, 2);

    TEST_ASSERT_FALSE (context.budget_insufficient);
    TEST_ASSERT_EQUAL_UINT64 (512ull * 1024ull,
                              context.total_planned_hwm_bytes);
    TEST_ASSERT_EQUAL_UINT64 (4, context.active_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (128ull * 1024ull, plans[0].sndhwm);
    TEST_ASSERT_EQUAL_UINT64 (128ull * 1024ull, plans[0].rcvhwm);
    TEST_ASSERT_EQUAL_UINT64 (128ull * 1024ull, plans[1].sndhwm);
    TEST_ASSERT_EQUAL_UINT64 (128ull * 1024ull, plans[1].rcvhwm);
}

void test_water_filling_remainder_is_stable ()
{
    zlink::auto_hwm_budget_input_t input;
    input.enabled = true;
    input.profile = ZLINK_AUTO_HWM_PROFILE_BALANCED;
    input.configured_core_budget_bytes = 262147;

    zlink::auto_hwm_context_plan_t first_context;
    zlink::auto_hwm_context_plan_make (input, &first_context);
    zlink::auto_hwm_socket_plan_t first[2];
    zlink::auto_hwm_socket_plan_prepare (
      zlink::auto_hwm_role_routed, 1, 0, false, 0, false, 0, true,
      &first[0]);
    zlink::auto_hwm_socket_plan_prepare (
      zlink::auto_hwm_role_routed, 1, 0, false, 0, false, 0, true,
      &first[1]);
    zlink::auto_hwm_context_finalize (&first_context, first, 2);

    zlink::auto_hwm_context_plan_t second_context;
    zlink::auto_hwm_context_plan_make (input, &second_context);
    zlink::auto_hwm_socket_plan_t second[2];
    zlink::auto_hwm_socket_plan_prepare (
      zlink::auto_hwm_role_routed, 1, 0, false, 0, false, 0, true,
      &second[0]);
    zlink::auto_hwm_socket_plan_prepare (
      zlink::auto_hwm_role_routed, 1, 0, false, 0, false, 0, true,
      &second[1]);
    zlink::auto_hwm_context_finalize (&second_context, second, 2);

    TEST_ASSERT_EQUAL_UINT64 (131074, first[0].sndhwm);
    TEST_ASSERT_EQUAL_UINT64 (131073, first[1].sndhwm);
    TEST_ASSERT_EQUAL_UINT64 (first[0].sndhwm, second[0].sndhwm);
    TEST_ASSERT_EQUAL_UINT64 (first[1].sndhwm, second[1].sndhwm);
    TEST_ASSERT_EQUAL_UINT64 (262147, first_context.total_planned_hwm_bytes);
}

void test_insufficient_budget_keeps_role_minima_visible ()
{
    zlink::auto_hwm_budget_input_t input;
    input.enabled = true;
    input.profile = ZLINK_AUTO_HWM_PROFILE_BALANCED;
    input.configured_core_budget_bytes = 64ull * 1024ull;

    zlink::auto_hwm_context_plan_t context;
    zlink::auto_hwm_context_plan_make (input, &context);
    zlink::auto_hwm_socket_plan_t plan;
    zlink::auto_hwm_socket_plan_prepare (
      zlink::auto_hwm_role_routed, 1, 1, false, 0, false, 0, true,
      &plan);
    zlink::auto_hwm_context_finalize (&context, &plan, 1);

    TEST_ASSERT_TRUE (context.budget_insufficient);
    TEST_ASSERT_EQUAL_UINT64 (64ull * 1024ull, plan.sndhwm);
    TEST_ASSERT_EQUAL_UINT64 (64ull * 1024ull, plan.rcvhwm);
    TEST_ASSERT_EQUAL_UINT64 (128ull * 1024ull,
                              context.total_planned_hwm_bytes);
}

static void release_pipepair_queue_handles (
  zlink::ctx_physical_queue_registry_t *registry_,
  zlink::physical_queue_handle_t *first_,
  zlink::physical_queue_handle_t *second_)
{
    zlink::physical_queue_handle_t first_peer = *first_;
    zlink::physical_queue_handle_t second_peer = *second_;
    registry_->release_endpoint (first_);
    registry_->release_endpoint (&first_peer);
    registry_->release_endpoint (second_);
    registry_->release_endpoint (&second_peer);
}

void test_atomic_pair_minimum_reservation_has_one_linearization_winner ()
{
    zlink::auto_hwm_budget_input_t input;
    input.enabled = true;
    input.profile = ZLINK_AUTO_HWM_PROFILE_BALANCED;
    input.configured_core_budget_bytes = 128ull * 1024ull;
    zlink::auto_hwm_context_plan_t context;
    zlink::auto_hwm_context_plan_make (input, &context);

    zlink::ctx_physical_queue_registry_t registry;
    const size_t contender_count = 8;
    struct result_t
    {
        result_t () : rc (-1), first (), second () {}
        int rc;
        zlink::physical_queue_handle_t first;
        zlink::physical_queue_handle_t second;
    };
    std::vector<result_t> results (contender_count);
    std::vector<std::thread> contenders;
    std::atomic<bool> start (false);
    for (size_t i = 0; i != contender_count; ++i) {
        contenders.push_back (std::thread ([&registry, &context, &results,
                                            &start, i] () {
            while (!start.load (std::memory_order_acquire))
                std::this_thread::yield ();
            results[i].rc = registry.create_pipepair_queues (
              0, 0, zlink::physical_queue_class_application,
              zlink::auto_hwm_role_routed, true, context,
              &results[i].first, &results[i].second);
        }));
    }
    start.store (true, std::memory_order_release);
    for (size_t i = 0; i != contenders.size (); ++i)
        contenders[i].join ();

    size_t success_count = 0;
    for (size_t i = 0; i != results.size (); ++i) {
        if (results[i].rc == 0) {
            ++success_count;
            release_pipepair_queue_handles (&registry, &results[i].first,
                                             &results[i].second);
        } else {
            TEST_ASSERT_NULL (results[i].first.get ());
            TEST_ASSERT_NULL (results[i].second.get ());
        }
    }
    TEST_ASSERT_EQUAL_UINT64 (1, success_count);

    zlink::physical_queue_registry_snapshot_t snapshot;
    registry.snapshot (&snapshot);
    TEST_ASSERT_EQUAL_UINT64 (0, snapshot.active_application_direction_count);

    zlink::physical_queue_handle_t replacement_first;
    zlink::physical_queue_handle_t replacement_second;
    TEST_ASSERT_EQUAL_INT (
      0, registry.create_pipepair_queues (
           0, 0, zlink::physical_queue_class_application,
           zlink::auto_hwm_role_routed, true, context, &replacement_first,
           &replacement_second));
    release_pipepair_queue_handles (&registry, &replacement_first,
                                    &replacement_second);
}

void test_completion_pair_does_not_consume_application_reservation ()
{
    zlink::auto_hwm_budget_input_t input;
    input.enabled = true;
    input.profile = ZLINK_AUTO_HWM_PROFILE_BALANCED;
    input.configured_core_budget_bytes = 1;
    zlink::auto_hwm_context_plan_t context;
    zlink::auto_hwm_context_plan_make (input, &context);

    zlink::ctx_physical_queue_registry_t registry;
    zlink::physical_queue_handle_t first;
    zlink::physical_queue_handle_t second;
    TEST_ASSERT_EQUAL_INT (
      0, registry.create_pipepair_queues (
           0, 0, zlink::physical_queue_class_completion,
           zlink::auto_hwm_role_routed, true, context, &first, &second));
    zlink::physical_queue_registry_snapshot_t snapshot;
    registry.snapshot (&snapshot);
    TEST_ASSERT_EQUAL_UINT64 (0, snapshot.active_application_direction_count);
    TEST_ASSERT_EQUAL_UINT64 (2, snapshot.active_completion_direction_count);
    release_pipepair_queue_handles (&registry, &first, &second);
}

void test_single_lane_reply_is_application_accounting_only ()
{
    zlink::ctx_physical_queue_registry_t registry;
    zlink::physical_queue_handle_t outbound;
    zlink::physical_queue_handle_t inbound;
    zlink::auto_hwm_budget_input_t input;
    input.enabled = false;
    zlink::auto_hwm_context_plan_t context;
    zlink::auto_hwm_context_plan_make (input, &context);
    TEST_ASSERT_EQUAL_INT (
      0, registry.create_pipepair_queues (
           0, 0, zlink::physical_queue_class_application,
           zlink::auto_hwm_role_routed, false, context, &outbound, &inbound));

    const uint64_t first_frame = 91;
    const uint64_t final_frame = 37;
    registry.account_provisional_frame (outbound, first_frame);

    zlink::physical_queue_registry_snapshot_t provisional;
    registry.snapshot (&provisional);
    TEST_ASSERT_EQUAL_UINT64 (2,
                              provisional.active_application_direction_count);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              provisional.active_completion_direction_count);
    TEST_ASSERT_EQUAL_UINT64 (
      first_frame, provisional.application_provisional_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      first_frame, provisional.application_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              provisional.completion_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              provisional.completion_pending_message_count);

    // counted_message=true models a terminal reply. Its record kind does not
    // reclassify a single Application physical queue as Completion.
    registry.commit_message (outbound, final_frame, true, false);

    zlink::physical_queue_registry_snapshot_t committed;
    registry.snapshot (&committed);
    TEST_ASSERT_EQUAL_UINT64 (
      first_frame + final_frame, committed.application_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      0, committed.application_provisional_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              committed.completion_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              committed.completion_peak_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              committed.completion_pending_message_count);

    registry.release_committed_frame (outbound, first_frame + final_frame, 0);
    release_pipepair_queue_handles (&registry, &outbound, &inbound);
}

void test_policy_disabled_pair_does_not_consume_application_reservation ()
{
    zlink::auto_hwm_budget_input_t input;
    input.enabled = true;
    input.profile = ZLINK_AUTO_HWM_PROFILE_BALANCED;
    input.configured_core_budget_bytes = 1;
    zlink::auto_hwm_context_plan_t context;
    zlink::auto_hwm_context_plan_make (input, &context);

    zlink::ctx_physical_queue_registry_t registry;
    zlink::physical_queue_handle_t first;
    zlink::physical_queue_handle_t second;
    TEST_ASSERT_EQUAL_INT (
      0, registry.create_pipepair_queues (
           0, 0, zlink::physical_queue_class_application,
           zlink::auto_hwm_role_routed, false, context, &first, &second));
    release_pipepair_queue_handles (&registry, &first, &second);
}

void test_last_endpoint_retirement_reconciles_record_owned_accounting ()
{
    zlink::auto_hwm_budget_input_t input;
    input.enabled = true;
    input.profile = ZLINK_AUTO_HWM_PROFILE_BALANCED;
    input.configured_core_budget_bytes = 512ull * 1024ull;
    zlink::auto_hwm_context_plan_t context;
    zlink::auto_hwm_context_plan_make (input, &context);

    zlink::ctx_physical_queue_registry_t registry;
    zlink::physical_queue_handle_t application_first;
    zlink::physical_queue_handle_t application_second;
    TEST_ASSERT_EQUAL_INT (
      0, registry.create_pipepair_queues (
           0, 0, zlink::physical_queue_class_application,
           zlink::auto_hwm_role_routed, true, context, &application_first,
           &application_second));
    registry.account_provisional_frame (application_first, 100);
    registry.commit_message (application_first, 50, false, false);
    registry.account_provisional_frame (application_second, 75);
    registry.rollback_provisional (application_second);
    release_pipepair_queue_handles (&registry, &application_first,
                                    &application_second);

    zlink::physical_queue_handle_t completion_first;
    zlink::physical_queue_handle_t completion_second;
    TEST_ASSERT_EQUAL_INT (
      0, registry.create_pipepair_queues (
           0, 0, zlink::physical_queue_class_completion,
           zlink::auto_hwm_role_routed, true, context, &completion_first,
           &completion_second));
    registry.account_provisional_frame (completion_first, 20);
    registry.commit_message (completion_first, 30, true, false);
    release_pipepair_queue_handles (&registry, &completion_first,
                                    &completion_second);

    zlink::physical_queue_registry_snapshot_t snapshot;
    registry.snapshot (&snapshot);
    TEST_ASSERT_EQUAL_UINT64 (0, snapshot.active_application_direction_count);
    TEST_ASSERT_EQUAL_UINT64 (0, snapshot.active_completion_direction_count);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              snapshot.application_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      0, snapshot.application_provisional_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              snapshot.completion_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (0, snapshot.completion_pending_message_count);
}

static zlink::auto_hwm_context_plan_t make_decoder_test_context ()
{
    zlink::auto_hwm_budget_input_t input;
    input.enabled = false;
    zlink::auto_hwm_context_plan_t context;
    zlink::auto_hwm_context_plan_make (input, &context);
    return context;
}

void test_decoder_reservation_enforces_incremental_hwm_and_final_oversize ()
{
    zlink::ctx_physical_queue_registry_t registry;
    zlink::physical_queue_handle_t first;
    zlink::physical_queue_handle_t second;
    const uint64_t metadata = sizeof (zlink::msg_t);
    const uint64_t hwm = metadata * 3;
    const zlink::auto_hwm_context_plan_t context = make_decoder_test_context ();
    TEST_ASSERT_EQUAL_INT (
      0, registry.create_pipepair_queues (
           hwm, hwm, zlink::physical_queue_class_application,
           zlink::auto_hwm_role_none, false, context, &first, &second));

    zlink::decoder_frame_reservation_request_t first_part;
    first_part.payload_bytes = 1;
    first_part.msg_flags = zlink::msg_t::more;
    first_part.multipart_started_empty = true;
    zlink::decoder_frame_reservation_t reservation_storage;
    zlink::decoder_frame_reservation_t *reservation = NULL;
    TEST_ASSERT_EQUAL_INT (
      0, registry.reserve_decoder_frame (
           first, first_part, &reservation_storage, &reservation));
    bool oversize = false;
    TEST_ASSERT_EQUAL_INT (
      0, registry.commit_decoder_frame (
           first, &reservation, first_part.payload_bytes, first_part.msg_flags,
           false, &oversize));
    TEST_ASSERT_FALSE (oversize);

    zlink::decoder_frame_reservation_request_t crossing_more;
    crossing_more.payload_bytes = hwm;
    crossing_more.msg_flags = zlink::msg_t::more;
    crossing_more.multipart_started_empty = true;
    TEST_ASSERT_EQUAL_INT (
      -1, registry.reserve_decoder_frame (
            first, crossing_more, &reservation_storage, &reservation));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_NULL (reservation);

    zlink::decoder_frame_reservation_request_t final_part;
    final_part.payload_bytes = hwm;
    final_part.msg_flags = 0;
    final_part.multipart_started_empty = true;
    TEST_ASSERT_EQUAL_INT (
      0, registry.reserve_decoder_frame (
           first, final_part, &reservation_storage, &reservation));
    TEST_ASSERT_EQUAL_INT (
      0, registry.commit_decoder_frame (
           first, &reservation, final_part.payload_bytes, final_part.msg_flags,
           true, &oversize));
    TEST_ASSERT_FALSE (oversize);

    zlink::physical_queue_registry_snapshot_t snapshot;
    registry.snapshot (&snapshot);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              snapshot.application_provisional_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              snapshot.application_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (0, snapshot.oversize_admission_count);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              snapshot.largest_oversize_message_bytes);
    release_pipepair_queue_handles (&registry, &first, &second);
}

void test_decoder_reservation_isolated_by_origin_and_generation ()
{
    zlink::ctx_physical_queue_registry_t registry;
    zlink::physical_queue_handle_t first;
    zlink::physical_queue_handle_t second;
    const uint64_t frame_bytes = sizeof (zlink::msg_t) + 1;
    const zlink::auto_hwm_context_plan_t context = make_decoder_test_context ();
    TEST_ASSERT_EQUAL_INT (
      0, registry.create_pipepair_queues (
           frame_bytes, frame_bytes,
           zlink::physical_queue_class_application,
           zlink::auto_hwm_role_none, false, context, &first, &second));

    zlink::decoder_frame_reservation_request_t request;
    request.payload_bytes = 1;
    request.msg_flags = zlink::msg_t::more;
    request.multipart_started_empty = true;
    zlink::decoder_frame_reservation_t first_storage;
    zlink::decoder_frame_reservation_t second_storage;
    zlink::decoder_frame_reservation_t *first_token = NULL;
    zlink::decoder_frame_reservation_t *second_token = NULL;
    TEST_ASSERT_EQUAL_INT (
      0, registry.reserve_decoder_frame (
           first, request, &first_storage, &first_token));
    TEST_ASSERT_EQUAL_INT (
      0, registry.reserve_decoder_frame (
           second, request, &second_storage, &second_token));

    registry.advance_generation (first);
    bool oversize = false;
    TEST_ASSERT_EQUAL_INT (
      -1, registry.commit_decoder_frame (
            first, &first_token, request.payload_bytes, request.msg_flags,
            false, &oversize));
    TEST_ASSERT_EQUAL_INT (ETERM, errno);
    registry.release_decoder_frame (&first_token);
    registry.release_decoder_frame (&second_token);
    zlink::physical_queue_registry_snapshot_t snapshot;
    registry.snapshot (&snapshot);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              snapshot.application_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              snapshot.application_provisional_accounted_bytes);
    release_pipepair_queue_handles (&registry, &first, &second);
}

void test_pipe_rollback_preserves_active_decoder_reservation ()
{
    zlink::ctx_physical_queue_registry_t registry;
    zlink::physical_queue_handle_t first;
    zlink::physical_queue_handle_t second;
    const uint64_t frame_bytes = sizeof (zlink::msg_t) + 1;
    const zlink::auto_hwm_context_plan_t context = make_decoder_test_context ();
    TEST_ASSERT_EQUAL_INT (
      0, registry.create_pipepair_queues (
           frame_bytes * 2, frame_bytes * 2,
           zlink::physical_queue_class_application,
           zlink::auto_hwm_role_none, false, context, &first, &second));

    zlink::decoder_frame_reservation_request_t request;
    request.payload_bytes = 1;
    request.msg_flags = zlink::msg_t::more;
    request.multipart_started_empty = true;
    zlink::decoder_frame_reservation_t reservation_storage;
    zlink::decoder_frame_reservation_t *reservation = NULL;
    TEST_ASSERT_EQUAL_INT (
      0, registry.reserve_decoder_frame (
           first, request, &reservation_storage, &reservation));
    registry.account_provisional_frame (first, 7);

    registry.rollback_provisional (first, 7);
    zlink::physical_queue_registry_snapshot_t snapshot;
    registry.snapshot (&snapshot);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              snapshot.application_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              snapshot.application_provisional_accounted_bytes);

    registry.release_decoder_frame (&reservation);
    registry.snapshot (&snapshot);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              snapshot.application_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      0, snapshot.application_provisional_accounted_bytes);
    release_pipepair_queue_handles (&registry, &first, &second);
}

void test_completion_decoder_reservation_never_applies_hwm ()
{
    zlink::ctx_physical_queue_registry_t registry;
    zlink::physical_queue_handle_t first;
    zlink::physical_queue_handle_t second;
    const zlink::auto_hwm_context_plan_t context = make_decoder_test_context ();
    TEST_ASSERT_EQUAL_INT (
      0, registry.create_pipepair_queues (
           1, 1, zlink::physical_queue_class_completion,
           zlink::auto_hwm_role_none, false, context, &first, &second));

    zlink::decoder_frame_reservation_request_t request;
    request.payload_bytes = 1024 * 1024;
    request.msg_flags = 0;
    request.multipart_started_empty = false;
    zlink::decoder_frame_reservation_t reservation_storage;
    zlink::decoder_frame_reservation_t *reservation = NULL;
    TEST_ASSERT_EQUAL_INT (
      0, registry.reserve_decoder_frame (
           first, request, &reservation_storage, &reservation));
    bool oversize = true;
    TEST_ASSERT_EQUAL_INT (
      0, registry.commit_decoder_frame (
           first, &reservation, request.payload_bytes, request.msg_flags, true,
           &oversize));
    TEST_ASSERT_FALSE (oversize);

    const uint64_t frame_bytes = request.payload_bytes + sizeof (zlink::msg_t);
    zlink::physical_queue_registry_snapshot_t snapshot;
    registry.snapshot (&snapshot);
    TEST_ASSERT_EQUAL_UINT64 (frame_bytes,
                              snapshot.completion_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (1, snapshot.completion_pending_message_count);
    registry.release_committed_frame (first, frame_bytes, 1);
    release_pipepair_queue_handles (&registry, &first, &second);
}

void test_admission_ratio_is_registry_owned_exact_and_resettable ()
{
    zlink::ctx_physical_queue_registry_t registry;
    registry.record_admission_attempt (false);
    registry.record_admission_attempt (true);
    registry.record_admission_attempt (false);

    zlink::physical_queue_registry_snapshot_t snapshot;
    registry.snapshot (&snapshot);
    TEST_ASSERT_EQUAL_UINT64 (3, snapshot.total_admission_attempts);
    TEST_ASSERT_EQUAL_UINT64 (1,
                              snapshot.first_blocked_admission_attempts);
    TEST_ASSERT_EQUAL_UINT32 (333333, snapshot.blocked_ratio_ppm);

    registry.reset_metrics ();
    registry.snapshot (&snapshot);
    TEST_ASSERT_EQUAL_UINT64 (0, snapshot.total_admission_attempts);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              snapshot.first_blocked_admission_attempts);
    TEST_ASSERT_EQUAL_UINT32 (0, snapshot.blocked_ratio_ppm);
}

int main ()
{
    UNITY_BEGIN ();
    RUN_TEST (test_budget_input_priority_and_profile_ratio);
    RUN_TEST (test_profile_byte_boundaries);
    RUN_TEST (test_profile_percent_and_fixed_caps);
    RUN_TEST (test_effective_budget_is_percent_clamped_by_cap_and_queue_floor);
    RUN_TEST (test_finalize_recomputes_budget_from_queue_count);
    RUN_TEST (test_mixed_queue_water_filling_respects_budget_and_caps);
    RUN_TEST (test_water_filling_remainder_is_stable);
    RUN_TEST (test_insufficient_budget_keeps_role_minima_visible);
    RUN_TEST (test_atomic_pair_minimum_reservation_has_one_linearization_winner);
    RUN_TEST (test_completion_pair_does_not_consume_application_reservation);
    RUN_TEST (test_single_lane_reply_is_application_accounting_only);
    RUN_TEST (test_policy_disabled_pair_does_not_consume_application_reservation);
    RUN_TEST (test_last_endpoint_retirement_reconciles_record_owned_accounting);
    RUN_TEST (test_decoder_reservation_enforces_incremental_hwm_and_final_oversize);
    RUN_TEST (test_decoder_reservation_isolated_by_origin_and_generation);
    RUN_TEST (test_pipe_rollback_preserves_active_decoder_reservation);
    RUN_TEST (test_completion_decoder_reservation_never_applies_hwm);
    RUN_TEST (test_admission_ratio_is_registry_owned_exact_and_resettable);
    return UNITY_END ();
}

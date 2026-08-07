/* SPDX-License-Identifier: MPL-2.0 */

#pragma once

#include "addon_common_api.h"

inline napi_value create_monitor_status_value (napi_env env,
                                               const zlink_monitor_status_t &snapshot)
{
    napi_value obj;
    napi_create_object (env, &obj);
    set_uint32_property (env, obj, "abiVersion", snapshot.abi_version);
    set_uint32_property (env, obj, "structSize", snapshot.struct_size);
    set_uint32_property (env, obj, "sourceKind", static_cast<uint32_t> (snapshot.source_kind));
    set_uint32_property (env, obj, "stateFlags", snapshot.state_flags);
    set_uint32_property (env, obj, "detailFlags", snapshot.detail_flags);
    set_uint64_bigint_property (env, obj, "sndPendingMsgs", snapshot.snd_pending_msgs);
    set_uint64_bigint_property (env, obj, "rcvPendingMsgs", snapshot.rcv_pending_msgs);
    napi_value auto_hwm_enabled;
    napi_get_boolean (env, snapshot.auto_hwm_enabled != 0, &auto_hwm_enabled);
    napi_set_named_property (env, obj, "autoHwmEnabled", auto_hwm_enabled);
    set_uint32_property (env, obj, "autoHwmProfile", snapshot.auto_hwm_profile);
    set_uint32_property (env, obj, "autoHwmRole", snapshot.auto_hwm_role);
    set_uint32_property (env, obj, "autoHwmPolicyClass", snapshot.auto_hwm_policy_class);
    set_uint64_bigint_property (env, obj, "autoHwmUnitBudgetBytes",
                                snapshot.auto_hwm_unit_budget_bytes);
    set_uint32_property (env, obj, "autoHwmSizeCap", snapshot.auto_hwm_size_cap);
    set_uint64_bigint_property (env, obj, "autoHwmSocketMessageSlots",
                                snapshot.auto_hwm_socket_message_slots);
    napi_value auto_hwm_connection_bucket_enabled;
    napi_get_boolean (env, snapshot.auto_hwm_connection_bucket_enabled != 0,
                      &auto_hwm_connection_bucket_enabled);
    napi_set_named_property (env, obj, "autoHwmConnectionBucketEnabled",
                             auto_hwm_connection_bucket_enabled);
    set_uint32_property (env, obj, "autoHwmConnectionBucketCount",
                         snapshot.auto_hwm_connection_bucket_count);
    set_uint32_property (env, obj, "autoHwmConnectionBucketIndex",
                         snapshot.auto_hwm_connection_bucket_index);
    set_uint32_property (env, obj, "autoHwmConnectionBucketHwm4K",
                         snapshot.auto_hwm_connection_bucket_hwm_4k);
    napi_value auto_hwm_connection_bucket_hysteresis_retained;
    napi_get_boolean (env, snapshot.auto_hwm_connection_bucket_hysteresis_retained != 0,
                      &auto_hwm_connection_bucket_hysteresis_retained);
    napi_set_named_property (env, obj, "autoHwmConnectionBucketHysteresisRetained",
                             auto_hwm_connection_bucket_hysteresis_retained);
    set_uint64_bigint_property (env, obj, "autoHwmEffectiveMessageBytes",
                                snapshot.auto_hwm_effective_message_bytes);
    set_uint64_bigint_property (env, obj, "autoHwmPlannedSndHwmBytes",
                                snapshot.auto_hwm_planned_sndhwm_bytes);
    set_uint64_bigint_property (env, obj, "autoHwmPlannedRcvHwmBytes",
                                snapshot.auto_hwm_planned_rcvhwm_bytes);
    set_uint64_bigint_property (env, obj, "autoHwmAppliedSndHwmBytes",
                                snapshot.auto_hwm_applied_sndhwm_bytes);
    set_uint64_bigint_property (env, obj, "autoHwmAppliedRcvHwmBytes",
                                snapshot.auto_hwm_applied_rcvhwm_bytes);
    set_int64_property (env, obj, "autoHwmEffectiveSndBuf", snapshot.auto_hwm_effective_sndbuf);
    set_int64_property (env, obj, "autoHwmEffectiveRcvBuf", snapshot.auto_hwm_effective_rcvbuf);
    set_uint64_bigint_property (env, obj, "autoHwmLastRecalcMs",
                                snapshot.auto_hwm_last_recalc_ms);
    set_uint32_property (env, obj, "autoHwmLastRecalcReason", snapshot.auto_hwm_last_recalc_reason);
    set_uint32_property (env, obj, "autoHwmSendBlockedRatioPpm",
                         snapshot.auto_hwm_send_blocked_ratio_ppm);
    set_uint64_bigint_property (env, obj, "autoHwmDeferredSndHwmBytes",
                                snapshot.auto_hwm_deferred_sndhwm_bytes);
    set_uint64_bigint_property (env, obj, "autoHwmDeferredRcvHwmBytes",
                                snapshot.auto_hwm_deferred_rcvhwm_bytes);
    napi_value deferred_snd_valid;
    napi_get_boolean (env, snapshot.auto_hwm_deferred_sndhwm_valid != 0, &deferred_snd_valid);
    napi_set_named_property (env, obj, "autoHwmDeferredSndHwmValid", deferred_snd_valid);
    napi_value deferred_rcv_valid;
    napi_get_boolean (env, snapshot.auto_hwm_deferred_rcvhwm_valid != 0, &deferred_rcv_valid);
    napi_set_named_property (env, obj, "autoHwmDeferredRcvHwmValid", deferred_rcv_valid);
    set_uint64_bigint_property (env, obj, "sndBytesInFlight", snapshot.snd_bytes_in_flight);
    set_uint64_bigint_property (env, obj, "rcvBytesInFlight", snapshot.rcv_bytes_in_flight);
    set_uint64_bigint_property (env, obj, "minimumCoreMessageChargeBytes",
                                snapshot.minimum_core_message_charge_bytes);
    set_uint64_bigint_property (env, obj, "oversizeMessageAdmissionCount",
                                snapshot.oversize_message_admission_count);
    set_uint64_bigint_property (env, obj, "oversizeMessageAdmissionMaxBytes",
                                snapshot.oversize_message_admission_max_bytes);
    return obj;
}

/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <array>
#include <cstdint>

namespace zlink
{

/// @brief Immutable value projection of Core's context-wide Auto HWM ABI v1 snapshot.
class core_hwm_budget_snapshot_t
{
  public:
    std::uint32_t abi_version () const noexcept { return _abi_version; }
    std::uint32_t struct_size () const noexcept { return _struct_size; }
    std::uint64_t budget_generation () const noexcept { return _budget_generation; }
    std::uint64_t measurement_epoch () const noexcept { return _measurement_epoch; }
    std::uint64_t configured_memory_limit_bytes () const noexcept
    {
        return _configured_memory_limit_bytes;
    }
    std::uint64_t runtime_memory_limit_bytes () const noexcept
    {
        return _runtime_memory_limit_bytes;
    }
    std::uint64_t resolved_memory_limit_bytes () const noexcept
    {
        return _resolved_memory_limit_bytes;
    }
    std::uint64_t configured_core_budget_bytes () const noexcept
    {
        return _configured_core_budget_bytes;
    }
    std::uint64_t effective_core_budget_bytes () const noexcept
    {
        return _effective_core_budget_bytes;
    }
    std::uint64_t total_planned_hwm_bytes () const noexcept
    {
        return _total_planned_hwm_bytes;
    }
    std::uint64_t total_applied_hwm_bytes () const noexcept
    {
        return _total_applied_hwm_bytes;
    }
    std::uint64_t manual_reserved_hwm_bytes () const noexcept
    {
        return _manual_reserved_hwm_bytes;
    }
    std::uint64_t core_queue_accounted_bytes () const noexcept
    {
        return _core_queue_accounted_bytes;
    }
    std::uint64_t application_accounted_bytes () const noexcept
    {
        return _application_accounted_bytes;
    }
    std::uint64_t current_accounted_bytes () const noexcept
    {
        return _current_accounted_bytes;
    }
    std::uint64_t provisional_accounted_bytes () const noexcept
    {
        return _provisional_accounted_bytes;
    }
    std::uint64_t peak_accounted_bytes () const noexcept
    {
        return _peak_accounted_bytes;
    }
    std::uint64_t completion_current_accounted_bytes () const noexcept
    {
        return _completion_current_accounted_bytes;
    }
    std::uint64_t completion_peak_accounted_bytes () const noexcept
    {
        return _completion_peak_accounted_bytes;
    }
    std::uint64_t completion_pending_message_count () const noexcept
    {
        return _completion_pending_message_count;
    }
    std::uint64_t total_messaging_accounted_bytes () const noexcept
    {
        return _total_messaging_accounted_bytes;
    }
    std::uint64_t monitor_queue_applied_hwm_bytes () const noexcept
    {
        return _monitor_queue_applied_hwm_bytes;
    }
    std::uint64_t monitor_queue_accounted_bytes () const noexcept
    {
        return _monitor_queue_accounted_bytes;
    }
    std::uint64_t total_instance_applied_hwm_bytes () const noexcept
    {
        return _total_instance_applied_hwm_bytes;
    }
    std::uint64_t total_instance_accounted_bytes () const noexcept
    {
        return _total_instance_accounted_bytes;
    }
    std::uint64_t oversize_admission_count () const noexcept
    {
        return _oversize_admission_count;
    }
    std::uint64_t largest_oversize_message_bytes () const noexcept
    {
        return _largest_oversize_message_bytes;
    }
    std::uint64_t active_directional_queue_count () const noexcept
    {
        return _active_directional_queue_count;
    }
    std::uint64_t active_completion_directional_queue_count () const noexcept
    {
        return _active_completion_directional_queue_count;
    }
    std::uint64_t active_send_queue_count () const noexcept
    {
        return _active_send_queue_count;
    }
    std::uint64_t active_receive_queue_count () const noexcept
    {
        return _active_receive_queue_count;
    }
    std::uint64_t outstanding_application_lease_count () const noexcept
    {
        return _outstanding_application_lease_count;
    }
    std::uint64_t retired_queue_count () const noexcept
    {
        return _retired_queue_count;
    }
    std::uint64_t deferred_origin_credit_bytes () const noexcept
    {
        return _deferred_origin_credit_bytes;
    }
    std::uint64_t unlimited_manual_queue_count () const noexcept
    {
        return _unlimited_manual_queue_count;
    }
    std::uint32_t blocked_ratio_ppm () const noexcept { return _blocked_ratio_ppm; }
    std::uint32_t flags () const noexcept { return _flags; }
    bool budget_planning_active () const noexcept { return (_flags & (1u << 0)) != 0; }
    bool budget_insufficient () const noexcept { return (_flags & (1u << 1)) != 0; }
    bool aggregate_hwm_valid () const noexcept { return (_flags & (1u << 2)) != 0; }
    bool aggregate_overflow () const noexcept { return (_flags & (1u << 3)) != 0; }
    const std::array<std::uint64_t, 8> &reserved_u64 () const noexcept
    {
        return _reserved_u64;
    }

    friend bool operator== (const core_hwm_budget_snapshot_t &,
                            const core_hwm_budget_snapshot_t &) = default;

  private:
    friend class context_t;
    core_hwm_budget_snapshot_t () = default;

    std::uint32_t _abi_version = 0;
    std::uint32_t _struct_size = 0;
    std::uint64_t _budget_generation = 0;
    std::uint64_t _measurement_epoch = 0;
    std::uint64_t _configured_memory_limit_bytes = 0;
    std::uint64_t _runtime_memory_limit_bytes = 0;
    std::uint64_t _resolved_memory_limit_bytes = 0;
    std::uint64_t _configured_core_budget_bytes = 0;
    std::uint64_t _effective_core_budget_bytes = 0;
    std::uint64_t _total_planned_hwm_bytes = 0;
    std::uint64_t _total_applied_hwm_bytes = 0;
    std::uint64_t _manual_reserved_hwm_bytes = 0;
    std::uint64_t _core_queue_accounted_bytes = 0;
    std::uint64_t _application_accounted_bytes = 0;
    std::uint64_t _current_accounted_bytes = 0;
    std::uint64_t _provisional_accounted_bytes = 0;
    std::uint64_t _peak_accounted_bytes = 0;
    std::uint64_t _completion_current_accounted_bytes = 0;
    std::uint64_t _completion_peak_accounted_bytes = 0;
    std::uint64_t _completion_pending_message_count = 0;
    std::uint64_t _total_messaging_accounted_bytes = 0;
    std::uint64_t _monitor_queue_applied_hwm_bytes = 0;
    std::uint64_t _monitor_queue_accounted_bytes = 0;
    std::uint64_t _total_instance_applied_hwm_bytes = 0;
    std::uint64_t _total_instance_accounted_bytes = 0;
    std::uint64_t _oversize_admission_count = 0;
    std::uint64_t _largest_oversize_message_bytes = 0;
    std::uint64_t _active_directional_queue_count = 0;
    std::uint64_t _active_completion_directional_queue_count = 0;
    std::uint64_t _active_send_queue_count = 0;
    std::uint64_t _active_receive_queue_count = 0;
    std::uint64_t _outstanding_application_lease_count = 0;
    std::uint64_t _retired_queue_count = 0;
    std::uint64_t _deferred_origin_credit_bytes = 0;
    std::uint64_t _unlimited_manual_queue_count = 0;
    std::uint32_t _blocked_ratio_ppm = 0;
    std::uint32_t _flags = 0;
    std::array<std::uint64_t, 8> _reserved_u64{};
};

} // namespace zlink

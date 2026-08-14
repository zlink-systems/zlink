/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/ctx_auto_hwm_state.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <string.h>

#if defined ZLINK_HAVE_WINDOWS
#include "utils/windows.hpp"
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

#if defined ZLINK_HAVE_LINUX
#include <fstream>
#include <stdlib.h>
#endif

namespace
{
uint64_t detected_physical_memory_bytes ()
{
#if defined ZLINK_HAVE_WINDOWS
    MEMORYSTATUSEX status;
    memset (&status, 0, sizeof (status));
    status.dwLength = sizeof (status);
    return GlobalMemoryStatusEx (&status) ? static_cast<uint64_t> (status.ullTotalPhys) : 0;
#elif defined(_SC_PHYS_PAGES) && defined(_SC_PAGE_SIZE)
    const long pages = sysconf (_SC_PHYS_PAGES);
    const long page_size = sysconf (_SC_PAGE_SIZE);
    if (pages <= 0 || page_size <= 0)
        return 0;
    const uint64_t pages_u64 = static_cast<uint64_t> (pages);
    const uint64_t page_size_u64 = static_cast<uint64_t> (page_size);
    return page_size_u64 > UINT64_MAX / pages_u64 ? UINT64_MAX
                                                   : pages_u64 * page_size_u64;
#else
    return 0;
#endif
}

uint64_t minimum_nonzero (uint64_t left_, uint64_t right_)
{
    if (left_ == 0)
        return right_;
    if (right_ == 0)
        return left_;
    return std::min (left_, right_);
}

#if defined ZLINK_HAVE_LINUX
uint64_t read_cgroup_limit (const char *path_, uint64_t physical_memory_)
{
    std::ifstream input (path_);
    std::string value;
    if (!input.good () || !(input >> value) || value == "max")
        return 0;

    char *end = NULL;
    errno = 0;
    const unsigned long long parsed = strtoull (value.c_str (), &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed == 0)
        return 0;

    const uint64_t limit = static_cast<uint64_t> (parsed);
    // cgroup v1 represents unlimited with a very large sentinel.
    if (physical_memory_ > 0 && limit >= physical_memory_)
        return 0;
    return limit;
}
#endif

uint64_t detected_hard_memory_limit_bytes (uint64_t physical_memory_)
{
    uint64_t hard_limit = 0;
#if defined ZLINK_HAVE_WINDOWS
    BOOL in_job = FALSE;
    if (IsProcessInJob (GetCurrentProcess (), NULL, &in_job) && in_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
        memset (&info, 0, sizeof (info));
        if (QueryInformationJobObject (NULL, JobObjectExtendedLimitInformation,
                                       &info, sizeof (info), NULL)) {
            if ((info.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY) != 0) {
                hard_limit = minimum_nonzero (
                  hard_limit, static_cast<uint64_t> (info.ProcessMemoryLimit));
            }
            if ((info.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY) != 0) {
                hard_limit = minimum_nonzero (
                  hard_limit, static_cast<uint64_t> (info.JobMemoryLimit));
            }
        }
    }
#else
#if defined(RLIMIT_AS)
    struct rlimit address_space;
    if (getrlimit (RLIMIT_AS, &address_space) == 0
        && address_space.rlim_cur != RLIM_INFINITY) {
        hard_limit = static_cast<uint64_t> (address_space.rlim_cur);
    }
#endif
#if defined ZLINK_HAVE_LINUX
    hard_limit = minimum_nonzero (
      hard_limit, read_cgroup_limit ("/sys/fs/cgroup/memory.max", physical_memory_));
    hard_limit = minimum_nonzero (
      hard_limit,
      read_cgroup_limit ("/sys/fs/cgroup/memory/memory.limit_in_bytes",
                         physical_memory_));
#endif
#endif
    return hard_limit;
}
}

zlink::ctx_auto_hwm_state_t::ctx_auto_hwm_state_t () :
    _input (),
    _recalc_debounce_ms (ZLINK_CTX_AUTO_HWM_RECALC_DEBOUNCE_MS_DFLT),
    _recalc_pending (false),
    _recalc_deadline_ms (0),
    _pending_generation (0),
    _last_applied_generation (0),
    _recalc_task_id (0),
    _budget_generation (0),
    _measurement_epoch (1),
    _applied_plan ()
{
    _input.enabled = ZLINK_CTX_AUTO_HWM_ENABLE_DFLT != 0;
    _input.profile = ZLINK_CTX_AUTO_HWM_PROFILE_DFLT;
    _input.configured_memory_limit_bytes =
      ZLINK_CTX_AUTO_HWM_MEMORY_LIMIT_BYTES_DFLT;
    _input.runtime_memory_limit_bytes =
      ZLINK_CTX_AUTO_HWM_RUNTIME_MEMORY_LIMIT_BYTES_DFLT;
    _input.configured_core_budget_bytes =
      ZLINK_CTX_AUTO_HWM_CORE_BUDGET_BYTES_DFLT;
    _input.detected_physical_memory_bytes = detected_physical_memory_bytes ();
    _input.detected_hard_limit_bytes =
      detected_hard_memory_limit_bytes (_input.detected_physical_memory_bytes);
    auto_hwm_context_plan_make (_input, &_applied_plan);
}

void zlink::ctx_auto_hwm_state_t::set_enabled (bool enabled_)
{
    _input.enabled = enabled_;
}

void zlink::ctx_auto_hwm_state_t::set_recalc_debounce_ms (int debounce_ms_)
{
    _recalc_debounce_ms = debounce_ms_;
}

void zlink::ctx_auto_hwm_state_t::set_profile (zlink_auto_hwm_profile_t profile_)
{
    _input.profile = profile_;
}

bool zlink::ctx_auto_hwm_state_t::valid_explicit_limit (uint64_t value_) const
{
    return value_ == 0 || _input.detected_hard_limit_bytes == 0
           || value_ <= _input.detected_hard_limit_bytes;
}

bool zlink::ctx_auto_hwm_state_t::set_memory_limit_bytes (uint64_t memory_limit_bytes_)
{
    if (!valid_explicit_limit (memory_limit_bytes_))
        return false;
    _input.configured_memory_limit_bytes = memory_limit_bytes_;
    return true;
}

void zlink::ctx_auto_hwm_state_t::set_runtime_memory_limit_bytes (uint64_t memory_limit_bytes_)
{
    _input.runtime_memory_limit_bytes = memory_limit_bytes_;
}

bool zlink::ctx_auto_hwm_state_t::set_core_budget_bytes (uint64_t budget_bytes_)
{
    if (!valid_explicit_limit (budget_bytes_))
        return false;
    _input.configured_core_budget_bytes = budget_bytes_;
    return true;
}

bool zlink::ctx_auto_hwm_state_t::enabled () const
{
    return _input.enabled;
}

int zlink::ctx_auto_hwm_state_t::recalc_debounce_ms () const
{
    return _recalc_debounce_ms;
}

zlink_auto_hwm_profile_t zlink::ctx_auto_hwm_state_t::profile () const
{
    return _input.profile;
}

uint64_t zlink::ctx_auto_hwm_state_t::memory_limit_bytes () const
{
    return _input.configured_memory_limit_bytes;
}

uint64_t zlink::ctx_auto_hwm_state_t::runtime_memory_limit_bytes () const
{
    return _input.runtime_memory_limit_bytes;
}

uint64_t zlink::ctx_auto_hwm_state_t::core_budget_bytes () const
{
    return _input.configured_core_budget_bytes;
}

zlink::auto_hwm_budget_input_t zlink::ctx_auto_hwm_state_t::budget_input () const
{
    return _input;
}

uint64_t zlink::ctx_auto_hwm_state_t::recalc_task_id () const
{
    return _recalc_task_id;
}

void zlink::ctx_auto_hwm_state_t::set_recalc_task_id (uint64_t task_id_)
{
    _recalc_task_id = task_id_;
}

uint64_t zlink::ctx_auto_hwm_state_t::clear_recalc_task_id ()
{
    const uint64_t task_id = _recalc_task_id;
    _recalc_task_id = 0;
    return task_id;
}

void zlink::ctx_auto_hwm_state_t::schedule (uint64_t now_ms_, int debounce_ms_)
{
    ++_pending_generation;

    if (debounce_ms_ <= 0) {
        _recalc_pending = false;
        _recalc_deadline_ms = now_ms_;
    } else {
        _recalc_pending = true;
        _recalc_deadline_ms = now_ms_ + static_cast<uint64_t> (debounce_ms_);
    }
}

void zlink::ctx_auto_hwm_state_t::record_applied_plan (
  const auto_hwm_context_plan_t &plan_)
{
    if (plan_.enabled) {
        _applied_plan = plan_;
    } else {
        _applied_plan.enabled = false;
        _applied_plan.profile = plan_.profile;
        _applied_plan.configured_memory_limit_bytes =
          plan_.configured_memory_limit_bytes;
        _applied_plan.runtime_memory_limit_bytes =
          plan_.runtime_memory_limit_bytes;
        _applied_plan.resolved_memory_limit_bytes =
          plan_.resolved_memory_limit_bytes;
        _applied_plan.configured_core_budget_bytes =
          plan_.configured_core_budget_bytes;
        _applied_plan.effective_core_budget_bytes =
          plan_.effective_core_budget_bytes;
    }
    _recalc_pending = false;
    _recalc_deadline_ms = 0;
    _last_applied_generation = _pending_generation;
    ++_budget_generation;
}

bool zlink::ctx_auto_hwm_state_t::recalc_due (uint64_t now_ms_) const
{
    return _recalc_pending && now_ms_ >= _recalc_deadline_ms
           && _pending_generation != _last_applied_generation;
}

void zlink::ctx_auto_hwm_state_t::copy_budget_snapshot (
  zlink_auto_hwm_budget_snapshot_t *out_) const
{
    if (!out_)
        return;

    memset (out_, 0, sizeof (*out_));
    out_->abi_version = ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1;
    out_->struct_size = static_cast<uint32_t> (sizeof (*out_));
    out_->budget_generation = _budget_generation;
    out_->measurement_epoch = _measurement_epoch;
    out_->configured_memory_limit_bytes = _applied_plan.configured_memory_limit_bytes;
    out_->runtime_memory_limit_bytes = _applied_plan.runtime_memory_limit_bytes;
    out_->resolved_memory_limit_bytes = _applied_plan.resolved_memory_limit_bytes;
    out_->configured_core_budget_bytes = _applied_plan.configured_core_budget_bytes;
    out_->effective_core_budget_bytes = _applied_plan.effective_core_budget_bytes;
    out_->total_planned_hwm_bytes = _applied_plan.total_planned_hwm_bytes;
    out_->total_applied_hwm_bytes = _applied_plan.total_applied_hwm_bytes;
    out_->manual_reserved_hwm_bytes = _applied_plan.manual_reserved_hwm_bytes;
    out_->active_directional_queue_count =
      _applied_plan.active_directional_queue_count;
    out_->active_send_queue_count = _applied_plan.active_send_queue_count;
    out_->active_receive_queue_count = _applied_plan.active_receive_queue_count;
    out_->unlimited_manual_queue_count =
      _applied_plan.unlimited_manual_queue_count;
    if (_applied_plan.enabled)
        out_->flags |= ZLINK_AUTO_HWM_BUDGET_FLAG_PLANNING_ACTIVE;
    if (_applied_plan.budget_insufficient)
        out_->flags |= ZLINK_AUTO_HWM_BUDGET_FLAG_INSUFFICIENT;
    if (_applied_plan.aggregate_hwm_valid)
        out_->flags |= ZLINK_AUTO_HWM_BUDGET_FLAG_AGGREGATE_HWM_VALID;
    if (_applied_plan.aggregate_overflow)
        out_->flags |= ZLINK_AUTO_HWM_BUDGET_FLAG_AGGREGATE_OVERFLOW;
}

void zlink::ctx_auto_hwm_state_t::reset_budget_metrics ()
{
    ++_measurement_epoch;
}
